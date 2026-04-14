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

#ifndef QIO_CHANNEL_CXL_H
#define QIO_CHANNEL_CXL_H

#include "io/channel.h"
#include "qom/object.h"

#define TYPE_QIO_CHANNEL_CXL "qio-channel-cxl"
OBJECT_DECLARE_SIMPLE_TYPE(QIOChannelCXL, QIO_CHANNEL_CXL)


/**
 * QIOChannelCXL:
 *
 * The QIOChannelCXL object provides a channel implementation
 * that is able to perform I/O on CXL DAX devices (/dev/dax*).
 * DAX character devices require positional I/O (preadv/pwritev)
 * for RAM page access, while the migration main stream needs a
 * normal seekable byte stream. Use @stream_fd for normal read/write
 * operations and @fd for positional RAM I/O.
 */

struct QIOChannelCXL {
    QIOChannel parent;
    int fd;              /* Positional I/O fd for RAM page access */
    int stream_fd;       /* Stream-like fd for main migration channel */
    int open_flags;      /* Original open flags */
    uint64_t align;      /* DAX alignment requirement in bytes */
    int64_t dev_size;    /* DAX device size in bytes, -1 if unknown */
    void *map;           /* Shared mapping used for positional RAM I/O */
    size_t map_size;     /* Size of @map in bytes */
    off_t offset;        /* Current read/write position */
};


/**
 * qio_channel_cxl_new:
 * @path: the DAX device path (e.g. /dev/dax0.0)
 * @errp: pointer to initialized error object
 *
 * Create a new IO channel object for a CXL DAX device
 * at @path. The device is opened for read-write access.
 *
 * Returns: the new channel object
 */
QIOChannelCXL *
qio_channel_cxl_new(const char *path, Error **errp);

/**
 * qio_channel_cxl_new_path:
 * @path: the DAX device path (e.g. /dev/dax0.0)
 * @flags: the open flags (O_RDONLY, O_WRONLY, O_RDWR, etc)
 * @errp: pointer to initialized error object
 *
 * Create a new IO channel object for a CXL DAX device
 * at @path, with specified open flags.
 *
 * Returns: the new channel object
 */
QIOChannelCXL *
qio_channel_cxl_new_path(const char *path, int flags, Error **errp);

#endif /* QIO_CHANNEL_CXL_H */
