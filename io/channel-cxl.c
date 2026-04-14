/*
 * QEMU I/O channels CXL DAX driver
 *
 * Copyright (c) 2024 CXL Migration Contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "io/channel-cxl.h"
#include "io/channel-util.h"
#include "io/channel-watch.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define CXL_DAX_ALIGN_FALLBACK (2 * 1024 * 1024)

/*
 * Query DAX device alignment via sysfs.
 * Returns alignment in bytes, or 0 on failure.
 */
static uint64_t cxl_dax_get_align(int fd)
{
#if defined(__linux__)
    struct stat st;
    g_autofree char *subsystem_path = NULL;
    g_autofree char *subsystem = NULL;
    g_autofree char *align_path = NULL;
    g_autofree char *align_str = NULL;

    if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode)) {
        return 0;
    }

    subsystem_path = g_strdup_printf("/sys/dev/char/%d:%d/subsystem",
                                     major(st.st_rdev), minor(st.st_rdev));
    subsystem = g_file_read_link(subsystem_path, NULL);
    if (!subsystem || !g_str_has_suffix(subsystem, "/dax")) {
        return 0;
    }

    align_path = g_strdup_printf("/sys/dev/char/%d:%d/align",
                                 major(st.st_rdev), minor(st.st_rdev));
    if (g_file_get_contents(align_path, &align_str, NULL, NULL)) {
        uint64_t align = g_ascii_strtoull(align_str, NULL, 0);
        if (align > 0) {
            return align;
        }
    }
#endif
    return 0;
}

/*
 * Query DAX device size via sysfs.
 * Returns size in bytes, or -1 on failure.
 */
static int64_t cxl_dax_get_size(int fd)
{
#if defined(__linux__)
    struct stat st;
    g_autofree char *subsystem_path = NULL;
    g_autofree char *subsystem = NULL;
    g_autofree char *size_path = NULL;
    g_autofree char *size_str = NULL;

    if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode)) {
        return -1;
    }

    subsystem_path = g_strdup_printf("/sys/dev/char/%d:%d/subsystem",
                                     major(st.st_rdev), minor(st.st_rdev));
    subsystem = g_file_read_link(subsystem_path, NULL);
    if (!subsystem || !g_str_has_suffix(subsystem, "/dax")) {
        return -1;
    }

    size_path = g_strdup_printf("/sys/dev/char/%d:%d/size",
                                major(st.st_rdev), minor(st.st_rdev));
    if (g_file_get_contents(size_path, &size_str, NULL, NULL)) {
        return g_ascii_strtoll(size_str, NULL, 0);
    }
#endif
    return -1;
}

static int64_t cxl_file_get_size(int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0) {
        return -1;
    }

    if (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
        return st.st_size;
    }

    return -1;
}

static bool qio_channel_cxl_ensure_map(QIOChannelCXL *ioc, Error **errp)
{
    int prot;

    if (ioc->map) {
        return true;
    }

    if (ioc->map_size == 0) {
        error_setg(errp, "CXL backing size is unknown");
        return false;
    }

    prot = PROT_READ;
    if ((ioc->open_flags & O_ACCMODE) != O_RDONLY) {
        prot |= PROT_WRITE;
    }

    ioc->map = mmap(NULL, ioc->map_size, prot, MAP_SHARED, ioc->fd, 0);
    if (ioc->map == MAP_FAILED) {
        ioc->map = NULL;
        error_setg_errno(errp, errno, "Could not mmap CXL backing");
        return false;
    }

    return true;
}

static bool qio_channel_cxl_check_range(QIOChannelCXL *ioc, off_t offset,
                                        size_t len, Error **errp)
{
    uint64_t end;

    if (offset < 0) {
        error_setg(errp, "Negative CXL backing offset %" PRId64,
                   (int64_t)offset);
        return false;
    }

    if ((uint64_t)offset > ioc->map_size) {
        error_setg(errp, "CXL backing offset %" PRIu64 " outside mapped range",
                   (uint64_t)offset);
        return false;
    }

    end = (uint64_t)offset + len;
    if (end < (uint64_t)offset || end > ioc->map_size) {
        error_setg(errp, "CXL backing range [%" PRIu64 ", %" PRIu64
                   ") outside mapped range of %zu bytes",
                   (uint64_t)offset, end, ioc->map_size);
        return false;
    }

    return true;
}

static ssize_t qio_channel_cxl_map_transfer(QIOChannelCXL *ioc,
                                            const struct iovec *iov,
                                            size_t niov, off_t offset,
                                            bool is_write, Error **errp)
{
    size_t done = 0;
    size_t i;

    if (is_write && (ioc->open_flags & O_ACCMODE) == O_RDONLY) {
        error_setg(errp, "CXL backing is read-only");
        return -1;
    }

    if (!qio_channel_cxl_ensure_map(ioc, errp)) {
        return -1;
    }

    for (i = 0; i < niov; i++) {
        if (!qio_channel_cxl_check_range(ioc, offset + done,
                                         iov[i].iov_len, errp)) {
            return -1;
        }

        if (is_write) {
            memcpy((uint8_t *)ioc->map + offset + done, iov[i].iov_base,
                   iov[i].iov_len);
        } else {
            memcpy(iov[i].iov_base, (uint8_t *)ioc->map + offset + done,
                   iov[i].iov_len);
        }
        done += iov[i].iov_len;
    }

    return done;
}

QIOChannelCXL *
qio_channel_cxl_new(const char *path, Error **errp)
{
    return qio_channel_cxl_new_path(path, O_RDWR | O_CLOEXEC, errp);
}

QIOChannelCXL *
qio_channel_cxl_new_path(const char *path, int flags, Error **errp)
{
    QIOChannelCXL *ioc;
    int fd;
    uint64_t align;

    fd = qemu_open(path, flags, errp);
    if (fd < 0) {
        return NULL;
    }

    ioc = QIO_CHANNEL_CXL(object_new(TYPE_QIO_CHANNEL_CXL));
    ioc->fd = fd;
    ioc->stream_fd = dup(fd);
    ioc->open_flags = flags;
    ioc->offset = 0;

    if (ioc->stream_fd < 0) {
        error_setg_errno(errp, errno, "Could not dup CXL fd for stream I/O");
        object_unref(OBJECT(ioc));
        return NULL;
    }

    align = cxl_dax_get_align(fd);
    ioc->align = align > 0 ? align : CXL_DAX_ALIGN_FALLBACK;
    ioc->dev_size = cxl_dax_get_size(fd);
    if (ioc->dev_size > 0) {
        ioc->map_size = ioc->dev_size;
    } else {
        int64_t size = cxl_file_get_size(fd);

        if (size > 0) {
            ioc->map_size = size;
        }
    }

    qio_channel_set_feature(QIO_CHANNEL(ioc), QIO_CHANNEL_FEATURE_SEEKABLE);

    return ioc;
}

static void qio_channel_cxl_init(Object *obj)
{
    QIOChannelCXL *ioc = QIO_CHANNEL_CXL(obj);
    ioc->fd = -1;
    ioc->stream_fd = -1;
    ioc->open_flags = 0;
    ioc->align = 0;
    ioc->dev_size = -1;
    ioc->map = NULL;
    ioc->map_size = 0;
    ioc->offset = 0;
}

static void qio_channel_cxl_finalize(Object *obj)
{
    QIOChannelCXL *ioc = QIO_CHANNEL_CXL(obj);
    if (ioc->map) {
        munmap(ioc->map, ioc->map_size);
        ioc->map = NULL;
    }
    if (ioc->stream_fd != -1) {
        qemu_close(ioc->stream_fd);
        ioc->stream_fd = -1;
    }
    if (ioc->fd != -1) {
        qemu_close(ioc->fd);
        ioc->fd = -1;
    }
}

static ssize_t qio_channel_cxl_readv(QIOChannel *ioc,
                                     const struct iovec *iov, size_t niov,
                                     int **fds, size_t *nfds, int flags,
                                     Error **errp)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);
    ssize_t ret;

    if (cioc->dev_size > 0) {
        ret = qio_channel_cxl_map_transfer(cioc, iov, niov, cioc->offset,
                                           false, errp);
        if (ret > 0) {
            cioc->offset += ret;
        }
        return ret;
    }

retry:
    ret = readv(cioc->stream_fd, iov, niov);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return QIO_CHANNEL_ERR_BLOCK;
        }
        if (errno == EINTR) {
            goto retry;
        }
        error_setg_errno(errp, errno, "Unable to read from CXL device stream");
        return -1;
    }

    cioc->offset += ret;
    return ret;
}

static ssize_t qio_channel_cxl_writev(QIOChannel *ioc,
                                      const struct iovec *iov, size_t niov,
                                      int *fds, size_t nfds, int flags,
                                      Error **errp)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);
    ssize_t ret;

    if (cioc->dev_size > 0) {
        ret = qio_channel_cxl_map_transfer(cioc, iov, niov, cioc->offset,
                                           true, errp);
        if (ret > 0) {
            cioc->offset += ret;
        }
        return ret;
    }

retry:
    ret = writev(cioc->stream_fd, iov, niov);
    if (ret <= 0) {
        if (errno == EAGAIN) {
            return QIO_CHANNEL_ERR_BLOCK;
        }
        if (errno == EINTR) {
            goto retry;
        }
        error_setg_errno(errp, errno, "Unable to write to CXL device stream");
        return -1;
    }

    cioc->offset += ret;
    return ret;
}

#ifdef CONFIG_PREADV
static ssize_t qio_channel_cxl_preadv(QIOChannel *ioc,
                                      const struct iovec *iov, size_t niov,
                                      off_t offset, Error **errp)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);
    return qio_channel_cxl_map_transfer(cioc, iov, niov, offset, false, errp);
}

static ssize_t qio_channel_cxl_pwritev(QIOChannel *ioc,
                                       const struct iovec *iov, size_t niov,
                                       off_t offset, Error **errp)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);
    return qio_channel_cxl_map_transfer(cioc, iov, niov, offset, true, errp);
}
#endif /* CONFIG_PREADV */

static int qio_channel_cxl_set_blocking(QIOChannel *ioc, bool enabled,
                                        Error **errp)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);

    if (!qemu_set_blocking(cioc->fd, enabled, errp)) {
        return -1;
    }
    if (!qemu_set_blocking(cioc->stream_fd, enabled, errp)) {
        return -1;
    }
    return 0;
}

static off_t qio_channel_cxl_seek(QIOChannel *ioc, off_t offset,
                                  int whence, Error **errp)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);
    off_t ret;

    if (cioc->dev_size > 0) {
        switch (whence) {
        case SEEK_SET:
            ret = offset;
            break;
        case SEEK_CUR:
            ret = cioc->offset + offset;
            break;
        case SEEK_END:
            if (cioc->map_size == 0) {
                error_setg(errp, "CXL backing size is unknown");
                return -1;
            }
            ret = cioc->map_size + offset;
            break;
        default:
            error_setg(errp, "Invalid seek whence %d for CXL backing", whence);
            return -1;
        }

        if (ret < 0) {
            error_setg(errp,
                       "Unable to seek to negative offset %lld in CXL backing",
                       (long long int)ret);
            return -1;
        }

        cioc->offset = ret;
        return ret;
    }

    ret = lseek(cioc->stream_fd, offset, whence);
    if (ret == (off_t)-1) {
        error_setg_errno(errp, errno,
                         "Unable to seek to offset %lld whence %d in CXL stream",
                         (long long int)offset, whence);
        return -1;
    }

    cioc->offset = ret;
    return ret;
}

static int qio_channel_cxl_close(QIOChannel *ioc, Error **errp)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);

    if (cioc->stream_fd != -1) {
        if (qemu_close(cioc->stream_fd) < 0) {
            error_setg_errno(errp, errno, "Unable to close CXL device stream");
            return -1;
        }
        cioc->stream_fd = -1;
    }

    if (cioc->fd != -1) {
        if (qemu_close(cioc->fd) < 0) {
            error_setg_errno(errp, errno, "Unable to close CXL device");
            return -1;
        }
        cioc->fd = -1;
    }
    return 0;
}

static void qio_channel_cxl_set_aio_fd_handler(QIOChannel *ioc,
                                               AioContext *read_ctx,
                                               IOHandler *io_read,
                                               AioContext *write_ctx,
                                               IOHandler *io_write,
                                               void *opaque)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);
    qio_channel_util_set_aio_fd_handler(cioc->stream_fd, read_ctx, io_read,
                                        cioc->stream_fd, write_ctx, io_write,
                                        opaque);
}

static GSource *qio_channel_cxl_create_watch(QIOChannel *ioc,
                                             GIOCondition condition)
{
    QIOChannelCXL *cioc = QIO_CHANNEL_CXL(ioc);
    return qio_channel_create_fd_watch(ioc, cioc->stream_fd, condition);
}

static void qio_channel_cxl_class_init(ObjectClass *klass,
                                       const void *class_data)
{
    QIOChannelClass *ioc_klass = QIO_CHANNEL_CLASS(klass);

    ioc_klass->io_writev = qio_channel_cxl_writev;
    ioc_klass->io_readv = qio_channel_cxl_readv;
    ioc_klass->io_set_blocking = qio_channel_cxl_set_blocking;
#ifdef CONFIG_PREADV
    ioc_klass->io_pwritev = qio_channel_cxl_pwritev;
    ioc_klass->io_preadv = qio_channel_cxl_preadv;
#endif
    ioc_klass->io_seek = qio_channel_cxl_seek;
    ioc_klass->io_close = qio_channel_cxl_close;
    ioc_klass->io_create_watch = qio_channel_cxl_create_watch;
    ioc_klass->io_set_aio_fd_handler = qio_channel_cxl_set_aio_fd_handler;
}

static const TypeInfo qio_channel_cxl_info = {
    .parent = TYPE_QIO_CHANNEL,
    .name = TYPE_QIO_CHANNEL_CXL,
    .instance_size = sizeof(QIOChannelCXL),
    .instance_init = qio_channel_cxl_init,
    .instance_finalize = qio_channel_cxl_finalize,
    .class_init = qio_channel_cxl_class_init,
};

static void qio_channel_cxl_register_types(void)
{
    type_register_static(&qio_channel_cxl_info);
}

type_init(qio_channel_cxl_register_types);
