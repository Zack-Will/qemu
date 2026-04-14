/*
 * Copyright (c) 2024 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "system/ramblock.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "channel.h"
#include "cxl.h"
#include "migration.h"
#include "ram.h"
#include "io/channel-cxl.h"
#include "options.h"
#include "system/cpus.h"

static struct CXLOutgoingArgs {
    char *devpath;
} outgoing_args;

/*
 * Write-redirect state for CXL migration.
 *
 * After a page is migrated to CXL, we track it in migrated_bmap using global
 * guest RAM offsets. Once all pages within a DAX-aligned region are migrated,
 * we remap the source host VA to CXL via mmap(MAP_FIXED). remapped_bmap keeps
 * region-level remap state, while remapped_pages_bmap tracks the pages covered
 * by successful remaps for fast dirty-bit clearing and cleanup. Subsequent
 * guest writes go directly to CXL, eliminating re-migration of those pages.
 */
static struct CXLMigrationState {
    void *mmap_base;
    size_t mmap_size;
    int fd;
    uint64_t align;
    uint64_t remap_granule;
    QemuMutex sender_sync_mutex;
    QemuCond sender_sync_cond;
    unsigned long *migrated_bmap;
    unsigned long *pending_remap_bmap;
    unsigned long *remapped_bmap;
    unsigned long *remapped_pages_bmap;
    QEMUBH *remap_bh;
    size_t total_pages;
    size_t total_regions;
    uint32_t senders_in_flight;
    uint64_t remapped_regions;
    uint64_t remap_attempts;
    uint64_t remap_successes;
    uint64_t remap_failures;
    uint64_t skip_out_of_range;
    uint64_t skip_already_remapped;
    uint64_t skip_misaligned;
    uint64_t skip_partial_region;
    uint64_t skip_not_fully_migrated;
    uint64_t dirty_clear_calls;
    uint64_t dirty_cleared_pages;
    uint64_t backing_write_calls;
    uint64_t backing_write_bytes;
    uint64_t backing_write_time_ns;
    uint64_t migrated_bitmap_time_ns;
    uint64_t remap_scan_calls;
    uint64_t remap_scan_time_ns;
    uint64_t remap_page_check_calls;
    uint64_t remap_page_check_time_ns;
    uint64_t remap_syscall_time_ns;
    uint64_t remap_copy_bytes;
    uint64_t remap_copy_time_ns;
    uint64_t remap_pause_calls;
    uint64_t remap_pause_time_ns;
    uint64_t dirty_sync_calls;
    uint64_t dirty_sync_time_ns;
    uint64_t dirty_clear_time_ns;
    bool sender_sync_ready;
    bool sender_shutdown;
    bool remap_quiescing;
    bool active;
} cxl_state = {
    .fd = -1,
};

static void cxl_remap_state_init(int fd, uint64_t align, int64_t dev_size);
static void cxl_process_pending_remaps_bh(void *opaque);

#define CXL_BACKING_ALIGN_FALLBACK (2 * 1024 * 1024)
#define CXL_REMAP_GRANULE_DEFAULT (64 * 1024)
#define CXL_ROLLBACK_COPY_CHUNK (2 * 1024 * 1024)
#define CXL_WRITE_REDIRECT_ENV "QEMU_CXL_WRITE_REDIRECT"
#define CXL_REMAP_GRANULE_ENV "QEMU_CXL_REMAP_GRANULE"
/*
 * migration/ram.c currently aligns mapped-ram page offsets to 1MiB (or higher)
 * and places a per-RAMBlock bitmap ahead of the page data. If we mmap the CXL
 * backing for write-redirect, we must cover those bitmap+alignment gaps too.
 */
#define CXL_MAPPED_RAM_FILE_OFFSET_ALIGNMENT (0x100000ULL)

static inline uint64_t cxl_now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static bool cxl_write_redirect_enabled(void)
{
    const char *value = g_getenv(CXL_WRITE_REDIRECT_ENV);

    if (!value || !*value) {
        return true;
    }

    return strcmp(value, "0") != 0 &&
           g_ascii_strcasecmp(value, "off") != 0 &&
           g_ascii_strcasecmp(value, "false") != 0 &&
           g_ascii_strcasecmp(value, "no") != 0;
}

static uint64_t cxl_choose_remap_granule(uint64_t align, uint64_t total_ram)
{
    const char *value = g_getenv(CXL_REMAP_GRANULE_ENV);
    uint64_t granule = MAX(align, (uint64_t)CXL_REMAP_GRANULE_DEFAULT);

    if (value && *value) {
        char *endptr = NULL;
        uint64_t requested = g_ascii_strtoull(value, &endptr, 0);

        if (endptr && *endptr == '\0' && requested > 0) {
            granule = MAX(align, requested);
        } else {
            warn_report("CXL write-redirect: ignoring invalid %s=%s",
                        CXL_REMAP_GRANULE_ENV, value);
        }
    }

    granule = ROUND_UP(granule, align);
    if (total_ram > 0) {
        granule = MIN(granule, ROUND_UP(total_ram, align));
    }
    return granule;
}

bool cxl_use_mapped_ram_backing(void)
{
    return migrate_mapped_ram() && migrate_cxl_path_enabled();
}

static uint64_t cxl_mapped_ram_pages_offset_alignment(uint64_t align)
{
    return MAX((uint64_t)CXL_MAPPED_RAM_FILE_OFFSET_ALIGNMENT, align);
}

static uint64_t cxl_mapped_ram_required_bytes(uint64_t align)
{
    RAMBlock *block;
    uint64_t offset = 0;
    uint64_t pages_align = cxl_mapped_ram_pages_offset_alignment(align);

    /*
     * Mirror mapped_ram_setup_ramblock()'s layout for the mapped-ram backing
     * file: [bitmap][padding to pages_align][page data] for each migratable
     * RAMBlock that isn't ignored.
     */
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_MIGRATABLE(block) {
        long num_pages;
        uint64_t bitmap_size;

        if (migrate_ram_is_ignored(block)) {
            continue;
        }

        num_pages = block->used_length >> TARGET_PAGE_BITS;
        bitmap_size = (uint64_t)BITS_TO_LONGS(num_pages) * sizeof(unsigned long);

        offset = ROUND_UP(offset + bitmap_size, pages_align);
        offset += block->used_length;
    }

    /* Always map whole host pages. */
    return ROUND_UP(offset, (uint64_t)qemu_real_host_page_size());
}

uint64_t cxl_mapped_ram_alignment(void)
{
    return cxl_state.align ? cxl_state.align : CXL_BACKING_ALIGN_FALLBACK;
}

void cxl_populate_migration_info(MigrationInfo *info)
{
    if (!cxl_use_mapped_ram_backing()) {
        return;
    }

    info->x_cxl = g_malloc0(sizeof(*info->x_cxl));
    info->x_cxl->active = cxl_state.active;
    info->x_cxl->align = cxl_mapped_ram_alignment();
    info->x_cxl->remap_granule = cxl_state.remap_granule;
    info->x_cxl->total_regions = cxl_state.total_regions;
    info->x_cxl->remapped_regions = qatomic_read(&cxl_state.remapped_regions);
    info->x_cxl->remap_attempts = qatomic_read(&cxl_state.remap_attempts);
    info->x_cxl->remap_successes = qatomic_read(&cxl_state.remap_successes);
    info->x_cxl->remap_failures = qatomic_read(&cxl_state.remap_failures);
    info->x_cxl->skip_out_of_range =
        qatomic_read(&cxl_state.skip_out_of_range);
    info->x_cxl->skip_already_remapped =
        qatomic_read(&cxl_state.skip_already_remapped);
    info->x_cxl->skip_misaligned =
        qatomic_read(&cxl_state.skip_misaligned);
    info->x_cxl->skip_partial_region =
        qatomic_read(&cxl_state.skip_partial_region);
    info->x_cxl->skip_not_fully_migrated =
        qatomic_read(&cxl_state.skip_not_fully_migrated);
    info->x_cxl->dirty_clear_calls = qatomic_read(&cxl_state.dirty_clear_calls);
    info->x_cxl->dirty_cleared_pages =
        qatomic_read(&cxl_state.dirty_cleared_pages);
    info->x_cxl->backing_write_calls =
        qatomic_read(&cxl_state.backing_write_calls);
    info->x_cxl->backing_write_bytes =
        qatomic_read(&cxl_state.backing_write_bytes);
    info->x_cxl->backing_write_time_ns =
        qatomic_read(&cxl_state.backing_write_time_ns);
    info->x_cxl->migrated_bitmap_time_ns =
        qatomic_read(&cxl_state.migrated_bitmap_time_ns);
    info->x_cxl->remap_scan_calls =
        qatomic_read(&cxl_state.remap_scan_calls);
    info->x_cxl->remap_scan_time_ns =
        qatomic_read(&cxl_state.remap_scan_time_ns);
    info->x_cxl->remap_page_check_calls =
        qatomic_read(&cxl_state.remap_page_check_calls);
    info->x_cxl->remap_page_check_time_ns =
        qatomic_read(&cxl_state.remap_page_check_time_ns);
    info->x_cxl->remap_syscall_time_ns =
        qatomic_read(&cxl_state.remap_syscall_time_ns);
    info->x_cxl->remap_copy_bytes =
        qatomic_read(&cxl_state.remap_copy_bytes);
    info->x_cxl->remap_copy_time_ns =
        qatomic_read(&cxl_state.remap_copy_time_ns);
    info->x_cxl->remap_pause_calls =
        qatomic_read(&cxl_state.remap_pause_calls);
    info->x_cxl->remap_pause_time_ns =
        qatomic_read(&cxl_state.remap_pause_time_ns);
    info->x_cxl->dirty_sync_calls =
        qatomic_read(&cxl_state.dirty_sync_calls);
    info->x_cxl->dirty_sync_time_ns =
        qatomic_read(&cxl_state.dirty_sync_time_ns);
    info->x_cxl->dirty_clear_time_ns =
        qatomic_read(&cxl_state.dirty_clear_time_ns);
}

void cxl_account_dirty_sync_ns(uint64_t ns)
{
    if (!cxl_use_mapped_ram_backing()) {
        return;
    }

    qatomic_inc(&cxl_state.dirty_sync_calls);
    qatomic_add(&cxl_state.dirty_sync_time_ns, ns);
}

static bool cxl_check_dev_size(QIOChannelCXL *cioc, const char *path,
                               Error **errp)
{
    if (cioc->dev_size > 0) {
        uint64_t required = cxl_mapped_ram_required_bytes(cioc->align);

        if ((uint64_t)cioc->dev_size < required) {
            error_setg(errp, "CXL device %s too small: %" PRId64
                       " bytes, need at least %" PRIu64 " bytes for mapped-ram",
                       path, cioc->dev_size, required);
            return false;
        }
    }

    return true;
}

static QIOChannelCXL *cxl_open_channel(const char *path, int flags,
                                       bool init_remap, Error **errp)
{
    QIOChannelCXL *cioc;

    cioc = qio_channel_cxl_new_path(path, flags, errp);
    if (!cioc) {
        return NULL;
    }

    if (!cxl_check_dev_size(cioc, path, errp)) {
        object_unref(OBJECT(cioc));
        return NULL;
    }

    if (init_remap) {
        g_free(outgoing_args.devpath);
        outgoing_args.devpath = g_strdup(path);
        cxl_remap_state_init(cioc->fd, cioc->align, cioc->dev_size);
    }

    return cioc;
}

QIOChannel *cxl_open_mapped_ram_outgoing(Error **errp)
{
    QIOChannelCXL *cioc;
    const char *path = migrate_cxl_path();

    if (!path) {
        error_setg(errp, "CXL mapped-ram backing requested without cxl-path");
        return NULL;
    }

    cioc = cxl_open_channel(path, O_RDWR | O_CLOEXEC, true, errp);
    if (!cioc) {
        return NULL;
    }

    qio_channel_set_name(QIO_CHANNEL(cioc), "migration-cxl-backing-outgoing");
    return QIO_CHANNEL(cioc);
}

QIOChannel *cxl_open_mapped_ram_incoming(Error **errp)
{
    QIOChannelCXL *cioc;
    const char *path = migrate_cxl_path();

    if (!path) {
        error_setg(errp, "CXL mapped-ram backing requested without cxl-path");
        return NULL;
    }

    cioc = cxl_open_channel(path, O_RDONLY | O_CLOEXEC, false, errp);
    if (!cioc) {
        return NULL;
    }

    qio_channel_set_name(QIO_CHANNEL(cioc), "migration-cxl-backing-incoming");
    return QIO_CHANNEL(cioc);
}

bool cxl_create_incoming_mapped_ram_channels(Error **errp)
{
    int i;

    for (i = 0; i < migrate_multifd_channels(); i++) {
        QIOChannel *ioc = cxl_open_mapped_ram_incoming(errp);

        if (!ioc) {
            return false;
        }

        if (!multifd_recv_new_channel(ioc, errp)) {
            object_unref(OBJECT(ioc));
            return false;
        }

        object_unref(OBJECT(ioc));
    }

    return true;
}

static size_t cxl_global_page_index(RAMBlock *block, ram_addr_t block_offset)
{
    return (block->offset + block_offset) >> TARGET_PAGE_BITS;
}

static size_t cxl_global_region_index(ram_addr_t global_offset)
{
    return global_offset / cxl_state.remap_granule;
}

static void cxl_region_page_span(RAMBlock *block, ram_addr_t block_offset,
                                 size_t *first_page, size_t *npages)
{
    size_t pages_per_region =
        DIV_ROUND_UP(cxl_state.remap_granule, TARGET_PAGE_SIZE);

    *first_page = cxl_global_page_index(block, block_offset);
    *npages = pages_per_region;
    if (*first_page + *npages > cxl_state.total_pages) {
        *npages = cxl_state.total_pages - *first_page;
    }
}

static bool cxl_region_all_pages_migrated(size_t first_page, size_t npages)
{
    size_t page;

    for (page = 0; page < npages; page++) {
        if (!test_bit(first_page + page, cxl_state.migrated_bmap)) {
            return false;
        }
    }

    return true;
}

static void cxl_mark_pages_remapped(size_t first_page, size_t npages)
{
    size_t page;

    for (page = 0; page < npages; page++) {
        set_bit_atomic(first_page + page, cxl_state.remapped_pages_bmap);
    }
}

static bool cxl_refresh_region_backing(RAMBlock *block, ram_addr_t block_offset,
                                       size_t len)
{
    ram_addr_t cxl_offset = block->pages_offset + block_offset;
    void *host_addr = block->host + block_offset;
    void *backing_addr;
    uint64_t t_start;

    if (!cxl_state.mmap_base) {
        return false;
    }

    backing_addr = (uint8_t *)cxl_state.mmap_base + cxl_offset;
    t_start = cxl_now_ns();
    memcpy(backing_addr, host_addr, len);
    qatomic_add(&cxl_state.remap_copy_time_ns, cxl_now_ns() - t_start);
    qatomic_add(&cxl_state.remap_copy_bytes, len);
    return true;
}

bool cxl_sender_access_begin(void)
{
    if (!cxl_state.active || !cxl_state.sender_sync_ready) {
        return false;
    }

    qemu_mutex_lock(&cxl_state.sender_sync_mutex);
    while (cxl_state.remap_quiescing && !cxl_state.sender_shutdown) {
        qemu_cond_wait(&cxl_state.sender_sync_cond,
                       &cxl_state.sender_sync_mutex);
    }
    if (cxl_state.sender_shutdown) {
        qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
        return false;
    }
    cxl_state.senders_in_flight++;
    qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
    return true;
}

void cxl_sender_access_end(void)
{
    if (!cxl_state.sender_sync_ready) {
        return;
    }

    qemu_mutex_lock(&cxl_state.sender_sync_mutex);
    assert(cxl_state.senders_in_flight > 0);
    cxl_state.senders_in_flight--;
    if (cxl_state.senders_in_flight == 0) {
        qemu_cond_broadcast(&cxl_state.sender_sync_cond);
    }
    qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
}

void cxl_sender_access_shutdown(void)
{
    if (!cxl_state.sender_sync_ready) {
        return;
    }

    qemu_mutex_lock(&cxl_state.sender_sync_mutex);
    cxl_state.sender_shutdown = true;
    cxl_state.remap_quiescing = false;
    qemu_cond_broadcast(&cxl_state.sender_sync_cond);
    qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
}

static bool cxl_quiesce_senders_begin(void)
{
    if (!cxl_state.active || !cxl_state.sender_sync_ready) {
        return false;
    }

    qemu_mutex_lock(&cxl_state.sender_sync_mutex);
    if (cxl_state.sender_shutdown) {
        qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
        return false;
    }
    cxl_state.remap_quiescing = true;
    while (cxl_state.senders_in_flight > 0 && !cxl_state.sender_shutdown) {
        qemu_cond_wait(&cxl_state.sender_sync_cond,
                       &cxl_state.sender_sync_mutex);
    }
    if (cxl_state.sender_shutdown) {
        cxl_state.remap_quiescing = false;
        qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
        return false;
    }
    qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
    return true;
}

static void cxl_quiesce_senders_end(void)
{
    if (!cxl_state.sender_sync_ready) {
        return;
    }

    qemu_mutex_lock(&cxl_state.sender_sync_mutex);
    cxl_state.remap_quiescing = false;
    qemu_cond_broadcast(&cxl_state.sender_sync_cond);
    qemu_mutex_unlock(&cxl_state.sender_sync_mutex);
}

static void cxl_remap_state_init(int fd, uint64_t align, int64_t dev_size)
{
    uint64_t total_ram = ram_bytes_total();

    cxl_state.align = align;
    cxl_state.remap_granule = cxl_choose_remap_granule(align, total_ram);
    cxl_state.total_pages = DIV_ROUND_UP(total_ram, TARGET_PAGE_SIZE);
    cxl_state.total_regions = DIV_ROUND_UP(total_ram, cxl_state.remap_granule);

    if (!cxl_write_redirect_enabled()) {
        warn_report("CXL write-redirect: disabled via %s", CXL_WRITE_REDIRECT_ENV);
        return;
    }

    cxl_state.fd = dup(fd);
    if (cxl_state.fd < 0) {
        warn_report("CXL write-redirect: failed to dup fd, disabled");
        return;
    }
    cxl_state.mmap_size = cxl_mapped_ram_required_bytes(align);
    if (dev_size > 0 && (uint64_t)dev_size < cxl_state.mmap_size) {
        warn_report("CXL write-redirect: device too small for mmap state (%"
                    PRIu64 " < %" PRIu64 ")",
                    (uint64_t)dev_size, (uint64_t)cxl_state.mmap_size);
        close(cxl_state.fd);
        cxl_state.fd = -1;
        return;
    }

    cxl_state.mmap_base = mmap(NULL, cxl_state.mmap_size,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED, cxl_state.fd, 0);
    if (cxl_state.mmap_base == MAP_FAILED) {
        warn_report("CXL write-redirect: mmap state failed (%s), disabled",
                    strerror(errno));
        close(cxl_state.fd);
        cxl_state.fd = -1;
        cxl_state.mmap_base = NULL;
        return;
    }

    cxl_state.migrated_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.pending_remap_bmap = bitmap_new(cxl_state.total_regions);
    cxl_state.remapped_bmap = bitmap_new(cxl_state.total_regions);
    cxl_state.remapped_pages_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.remap_bh = qemu_bh_new(cxl_process_pending_remaps_bh, NULL);
    qemu_mutex_init(&cxl_state.sender_sync_mutex);
    qemu_cond_init(&cxl_state.sender_sync_cond);
    cxl_state.sender_sync_ready = true;
    cxl_state.active = true;
}

static void cxl_remap_state_cleanup(void)
{
    RAMBlock *block;

    if (!cxl_state.active) {
        return;
    }

    cxl_sender_access_shutdown();

    RAMBLOCK_FOREACH_NOT_IGNORED(block) {
        size_t block_first_page;
        size_t block_pages;
        size_t block_last_page;
        size_t page_idx;

        block_first_page = block->offset >> TARGET_PAGE_BITS;
        block_pages = DIV_ROUND_UP(block->used_length, TARGET_PAGE_SIZE);
        block_last_page = MIN(block_first_page + block_pages,
                              cxl_state.total_pages);

        page_idx = find_next_bit(cxl_state.remapped_pages_bmap,
                                 block_last_page, block_first_page);
        while (page_idx < block_last_page) {
            size_t run_end = find_next_zero_bit(cxl_state.remapped_pages_bmap,
                                                block_last_page, page_idx + 1);
            size_t chunk_pages = MAX((size_t)1,
                                     CXL_ROLLBACK_COPY_CHUNK /
                                     TARGET_PAGE_SIZE);
            size_t local_first_page = page_idx - block_first_page;
            size_t npages = MIN(run_end - page_idx, chunk_pages);
            ram_addr_t block_offset = local_first_page << TARGET_PAGE_BITS;
            size_t region_len = npages << TARGET_PAGE_BITS;
            void *host_addr;
            void *tmp;
            void *ret;

            host_addr = block->host + block_offset;
            tmp = g_malloc(region_len);
            memcpy(tmp, host_addr, region_len);

            ret = mmap(host_addr, region_len,
                       PROT_READ | PROT_WRITE,
                       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (ret == MAP_FAILED) {
                warn_report("CXL rollback remap failed at page run %zu-%zu: %s",
                            page_idx, page_idx + npages, strerror(errno));
                g_free(tmp);
                page_idx = find_next_bit(cxl_state.remapped_pages_bmap,
                                         block_last_page, page_idx + npages);
                continue;
            }

            memcpy(host_addr, tmp, region_len);
            g_free(tmp);
            page_idx = find_next_bit(cxl_state.remapped_pages_bmap,
                                     block_last_page, page_idx + npages);
        }
    }

    if (cxl_state.mmap_base) {
        munmap(cxl_state.mmap_base, cxl_state.mmap_size);
    }
    if (cxl_state.fd >= 0) {
        close(cxl_state.fd);
    }
    if (cxl_state.remap_bh) {
        qemu_bh_delete(cxl_state.remap_bh);
    }
    if (cxl_state.sender_sync_ready) {
        qemu_cond_destroy(&cxl_state.sender_sync_cond);
        qemu_mutex_destroy(&cxl_state.sender_sync_mutex);
    }
    g_free(cxl_state.migrated_bmap);
    g_free(cxl_state.pending_remap_bmap);
    g_free(cxl_state.remapped_bmap);
    g_free(cxl_state.remapped_pages_bmap);
    memset(&cxl_state, 0, sizeof(cxl_state));
    cxl_state.fd = -1;
}

void cxl_cleanup_outgoing_migration(void)
{
    cxl_remap_state_cleanup();
    g_free(outgoing_args.devpath);
    outgoing_args.devpath = NULL;
}

bool cxl_send_channel_create(gpointer opaque, Error **errp)
{
    QIOChannelCXL *cioc;
    bool ret = true;

    cioc = qio_channel_cxl_new(outgoing_args.devpath, errp);
    if (!cioc) {
        ret = false;
        goto out;
    }

    multifd_channel_connect(opaque, QIO_CHANNEL(cioc));

out:
    multifd_send_channel_created();
    return ret;
}

static bool cxl_lookup_region(size_t region_idx, RAMBlock **blockp,
                              ram_addr_t *block_offsetp)
{
    RAMBlock *block;
    ram_addr_t global_offset = region_idx * cxl_state.remap_granule;

    RAMBLOCK_FOREACH_NOT_IGNORED(block) {
        if (global_offset < block->offset) {
            continue;
        }
        if (global_offset >= block->offset + block->used_length) {
            continue;
        }

        *blockp = block;
        *block_offsetp = global_offset - block->offset;
        return true;
    }

    return false;
}

static void cxl_try_remap_region(size_t region_idx)
{
    RAMBlock *block;
    size_t first_page;
    size_t npages;
    size_t region_len;
    ram_addr_t block_offset;
    ram_addr_t global_offset = region_idx * cxl_state.remap_granule;
    off_t cxl_offset;
    void *host_addr;
    void *ret;
    uint64_t t_start;

    if (!cxl_state.active) {
        return;
    }

    if (region_idx >= cxl_state.total_regions) {
        qatomic_inc(&cxl_state.skip_out_of_range);
        return;
    }

    if (!cxl_lookup_region(region_idx, &block, &block_offset)) {
        qatomic_inc(&cxl_state.skip_out_of_range);
        return;
    }

    if (!QEMU_IS_ALIGNED(global_offset, cxl_state.remap_granule) ||
        !QEMU_IS_ALIGNED(block_offset, cxl_state.remap_granule)) {
        qatomic_inc(&cxl_state.skip_misaligned);
        return;
    }

    if (block_offset + cxl_state.remap_granule > block->used_length) {
        qatomic_inc(&cxl_state.skip_partial_region);
        return;
    }

    if (test_bit(region_idx, cxl_state.remapped_bmap)) {
        qatomic_inc(&cxl_state.skip_already_remapped);
        return;
    }

    cxl_region_page_span(block, block_offset, &first_page, &npages);
    qatomic_inc(&cxl_state.remap_page_check_calls);
    t_start = cxl_now_ns();
    if (!cxl_region_all_pages_migrated(first_page, npages)) {
        qatomic_add(&cxl_state.remap_page_check_time_ns,
                    cxl_now_ns() - t_start);
        qatomic_inc(&cxl_state.skip_not_fully_migrated);
        return;
    }
    qatomic_add(&cxl_state.remap_page_check_time_ns, cxl_now_ns() - t_start);

    if (test_and_set_bit(region_idx, cxl_state.remapped_bmap)) {
        qatomic_inc(&cxl_state.skip_already_remapped);
        return;
    }

    host_addr = block->host + block_offset;
    cxl_offset = block->pages_offset + block_offset;
    region_len = cxl_state.remap_granule;
    qatomic_inc(&cxl_state.remap_attempts);

    if (!cxl_refresh_region_backing(block, block_offset, region_len)) {
        qatomic_inc(&cxl_state.remap_failures);
        clear_bit_atomic(region_idx, cxl_state.remapped_bmap);
        warn_report("CXL remap refresh failed at region %zu (offset 0x%lx)",
                    region_idx, (unsigned long)cxl_offset);
        return;
    }

    t_start = cxl_now_ns();
    ret = mmap(host_addr, region_len,
               PROT_READ | PROT_WRITE,
               MAP_FIXED | MAP_SHARED,
               cxl_state.fd, cxl_offset);
    qatomic_add(&cxl_state.remap_syscall_time_ns, cxl_now_ns() - t_start);
    if (ret == MAP_FAILED) {
        qatomic_inc(&cxl_state.remap_failures);
        clear_bit_atomic(region_idx, cxl_state.remapped_bmap);
        warn_report("CXL remap failed at region %zu (offset 0x%lx): %s",
                    region_idx, (unsigned long)cxl_offset, strerror(errno));
        return;
    }

    cxl_mark_pages_remapped(first_page, npages);
    qatomic_inc(&cxl_state.remap_successes);
    qatomic_inc(&cxl_state.remapped_regions);
}

static void cxl_process_pending_remaps_bh(void *opaque)
{
    size_t region_idx;
    uint64_t t_start;

    (void)opaque;

    if (!cxl_state.active) {
        return;
    }

    region_idx = find_first_bit(cxl_state.pending_remap_bmap,
                                cxl_state.total_regions);
    if (region_idx >= cxl_state.total_regions) {
        return;
    }

    if (!cxl_quiesce_senders_begin()) {
        return;
    }
    t_start = cxl_now_ns();
    pause_all_vcpus();
    qatomic_inc(&cxl_state.remap_pause_calls);

    while ((region_idx = find_first_bit(cxl_state.pending_remap_bmap,
                                        cxl_state.total_regions)) <
           cxl_state.total_regions) {
        if (!test_and_clear_bit(region_idx, cxl_state.pending_remap_bmap)) {
            continue;
        }
        cxl_try_remap_region(region_idx);
    }

    resume_all_vcpus();
    qatomic_add(&cxl_state.remap_pause_time_ns, cxl_now_ns() - t_start);
    cxl_quiesce_senders_end();

    if (find_first_bit(cxl_state.pending_remap_bmap,
                       cxl_state.total_regions) < cxl_state.total_regions) {
        qemu_bh_schedule(cxl_state.remap_bh);
    }
}

static void cxl_try_remap_range(RAMBlock *block, ram_addr_t block_offset,
                                size_t len)
{
    ram_addr_t range_start;
    ram_addr_t range_end;
    ram_addr_t region_offset;
    bool queued = false;
    uint64_t t_start;

    if (!cxl_state.active || len == 0) {
        return;
    }

    qatomic_inc(&cxl_state.remap_scan_calls);
    t_start = cxl_now_ns();
    range_start = QEMU_ALIGN_DOWN(block_offset, cxl_state.remap_granule);
    range_end = MIN(block_offset + len, block->used_length);
    range_end = ROUND_UP(range_end, cxl_state.remap_granule);

    for (region_offset = range_start;
         region_offset < range_end && region_offset < block->used_length;
         region_offset += cxl_state.remap_granule) {
        size_t region_idx =
            cxl_global_region_index(block->offset + region_offset);

        if (region_idx >= cxl_state.total_regions) {
            qatomic_inc(&cxl_state.skip_out_of_range);
            continue;
        }
        if (test_bit(region_idx, cxl_state.remapped_bmap)) {
            continue;
        }

        set_bit_atomic(region_idx, cxl_state.pending_remap_bmap);
        queued = true;
    }
    qatomic_add(&cxl_state.remap_scan_time_ns, cxl_now_ns() - t_start);

    if (queued && cxl_state.remap_bh) {
        qemu_bh_schedule(cxl_state.remap_bh);
    }
}

int cxl_write_ramblock_iov(QIOChannel *ioc, const struct iovec *iov,
                           int niov, MultiFDPages_t *pages, Error **errp)
{
    ssize_t ret = 0;
    int i, slice_idx, slice_num;
    uintptr_t base, next, offset;
    size_t len;
    RAMBlock *block = pages->block;
    uint64_t t_start;

    slice_idx = 0;
    slice_num = 1;

    for (i = 0; i < niov; i++, slice_num++) {
        base = (uintptr_t)iov[i].iov_base;

        if (i != niov - 1) {
            len = iov[i].iov_len;
            next = (uintptr_t)iov[i + 1].iov_base;
            if (base + len == next) {
                continue;
            }
        }

        offset = (uintptr_t)iov[slice_idx].iov_base - (uintptr_t)block->host;
        if (offset >= block->used_length) {
            error_setg(errp, "offset %" PRIxPTR
                       " outside of ramblock %s range", offset, block->idstr);
            ret = -1;
            break;
        }

        t_start = cxl_now_ns();
        ret = qio_channel_pwritev(ioc, &iov[slice_idx], slice_num,
                                  block->pages_offset + offset, errp);
        qatomic_add(&cxl_state.backing_write_time_ns, cxl_now_ns() - t_start);
        if (ret < 0) {
            break;
        }
        qatomic_inc(&cxl_state.backing_write_calls);
        qatomic_add(&cxl_state.backing_write_bytes, (uint64_t)ret);

        if (cxl_state.active) {
            size_t written_bytes = 0;
            int j;

            t_start = cxl_now_ns();
            for (j = slice_idx; j < slice_idx + slice_num; j++) {
                size_t page_offset = offset + written_bytes;
                size_t first_page = cxl_global_page_index(block, page_offset);
                size_t npages = iov[j].iov_len / TARGET_PAGE_SIZE;
                size_t p;

                for (p = 0; p < npages && (first_page + p) < cxl_state.total_pages; p++) {
                    set_bit_atomic(first_page + p, cxl_state.migrated_bmap);
                }
                written_bytes += iov[j].iov_len;
            }
            qatomic_add(&cxl_state.migrated_bitmap_time_ns, cxl_now_ns() - t_start);
            cxl_try_remap_range(block, offset, written_bytes);
        }

        slice_idx += slice_num;
        slice_num = 0;
    }

    return (ret < 0) ? ret : 0;
}

int multifd_cxl_recv_data(MultiFDRecvParams *p, Error **errp)
{
    MultiFDRecvData *data = p->data;
    size_t ret;

    ret = qio_channel_pread(p->c, (char *)data->opaque,
                            data->size, data->file_offset, errp);
    if (ret != data->size) {
        error_prepend(errp,
                      "multifd recv (%u): read 0x%zx, expected 0x%zx",
                      p->id, ret, data->size);
        return -1;
    }

    return 0;
}

bool cxl_page_is_remapped(ram_addr_t offset)
{
    size_t page_idx;

    if (!cxl_state.active) {
        return false;
    }

    page_idx = offset >> TARGET_PAGE_BITS;
    if (page_idx >= cxl_state.total_pages) {
        return false;
    }

    return test_bit(page_idx, cxl_state.remapped_pages_bmap);
}

uint64_t cxl_clear_remapped_dirty_bits(RAMBlock *block)
{
    size_t block_first_page;
    size_t block_pages;
    size_t block_last_page;
    size_t page_idx;
    uint64_t cleared = 0;
    uint64_t t_start;

    if (!cxl_state.active || !block->bmap) {
        return 0;
    }

    qatomic_inc(&cxl_state.dirty_clear_calls);
    t_start = cxl_now_ns();
    block_pages = DIV_ROUND_UP(block->used_length, TARGET_PAGE_SIZE);
    block_first_page = block->offset >> TARGET_PAGE_BITS;
    block_last_page = MIN(block_first_page + block_pages,
                          cxl_state.total_pages);

    page_idx = find_next_bit(cxl_state.remapped_pages_bmap,
                             block_last_page, block_first_page);
    while (page_idx < block_last_page) {
        size_t run_end = find_next_zero_bit(cxl_state.remapped_pages_bmap,
                                            block_last_page, page_idx + 1);
        size_t local_first_page = page_idx - block_first_page;
        size_t npages = run_end - page_idx;

        if (local_first_page >= block_pages) {
            break;
        }

        if (local_first_page + npages > block_pages) {
            npages = block_pages - local_first_page;
        }

        cleared += bitmap_count_one_with_offset(block->bmap,
                                                local_first_page, npages);
        bitmap_clear(block->bmap, local_first_page, npages);
        page_idx = find_next_bit(cxl_state.remapped_pages_bmap,
                                 block_last_page, run_end);
    }

    qatomic_add(&cxl_state.dirty_cleared_pages, cleared);
    qatomic_add(&cxl_state.dirty_clear_time_ns, cxl_now_ns() - t_start);
    return cleared;
}
