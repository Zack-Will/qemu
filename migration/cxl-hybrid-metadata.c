/*
 * CXL hybrid migration metadata helpers.
 *
 * Copyright (c) 2024 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qapi/error.h"
#include "migration/cxl.h"
#include "system/ramblock.h"
#include "trace.h"

#define CXL_HYBRID_METADATA_VERSION 1
#define CXL_HYBRID_METADATA_ENTRY_HEADER_LEN (1 + 8 + 8 + 4 + 4)
#define CXL_HYBRID_METADATA_HEADER_LEN (4 + 4 + 4)
#define CXL_HYBRID_WARM_PAGE_HEADER_LEN (1 + 8 + 4)
#define CXL_HYBRID_WARM_DESC_HEADER_LEN (1 + 8 + 8 + 4 + 4 + 4)
#define CXL_HYBRID_WARM_DESC_BATCH_HEADER_LEN (4 + 4)
#define CXL_HYBRID_WARM_DESC_BATCH_ENTRY_HEADER_LEN (1 + 8 + 8 + 4 + 4)
#define CXL_HYBRID_STAGING_PAGE_SIZE 4096

typedef struct CXLHybridDstStagingSlot {
    char *ramblock;
    uint64_t guest_offset;
    uint64_t staging_offset;
    size_t length;
    size_t page_size;
    size_t nr_pages;
    unsigned long *present_bitmap;
    uint64_t *external_offsets;
} CXLHybridDstStagingSlot;

typedef struct CXLHybridDstStagingState {
    int fd;
    size_t capacity;
    uint64_t file_limit;
    uint64_t base_offset;
    uint64_t next_offset;
    void *map_base;
    size_t map_len;
    bool shared_map;
    GHashTable *slots;
    QemuMutex lock;
    QemuCond cond;
    uint64_t present_slots;
    uint64_t fault_hits;
    uint64_t fault_misses;
    uint64_t fault_read_bytes;
    uint64_t fault_read_time_ns;
    uint64_t fault_place_successes;
    uint64_t fault_place_failures;
    uint32_t waiters;
    bool sync_ready;
    bool stopping;
} CXLHybridDstStagingState;

static CXLHybridDstStagingState cxl_dst_staging = {
    .fd = -1,
};

uint64_t cxl_hybrid_align_mapping_bytes(uint64_t bytes, uint64_t align)
{
    uint64_t granule = MAX((uint64_t)qemu_real_host_page_size(), align);

    return ROUND_UP(bytes, granule);
}

static void cxl_hybrid_dst_staging_reset_counters(void)
{
    qatomic_set(&cxl_dst_staging.present_slots, 0);
    qatomic_set(&cxl_dst_staging.fault_hits, 0);
    qatomic_set(&cxl_dst_staging.fault_misses, 0);
    qatomic_set(&cxl_dst_staging.fault_read_bytes, 0);
    qatomic_set(&cxl_dst_staging.fault_read_time_ns, 0);
    qatomic_set(&cxl_dst_staging.fault_place_successes, 0);
    qatomic_set(&cxl_dst_staging.fault_place_failures, 0);
}

static char *cxl_hybrid_dst_staging_key(const char *ramblock, uint64_t offset)
{
    return g_strdup_printf("%s:%" PRIx64, ramblock, offset);
}

static void cxl_hybrid_dst_staging_slot_free(gpointer opaque)
{
    CXLHybridDstStagingSlot *slot = opaque;

    if (!slot) {
        return;
    }

    g_free(slot->ramblock);
    g_free(slot->present_bitmap);
    g_free(slot->external_offsets);
    g_free(slot);
}

static bool cxl_hybrid_metadata_entry_valid(const CXLHybridMetadataEntry *entry,
                                            Error **errp)
{
    size_t name_len;

    if (!entry->ramblock || !entry->ramblock[0]) {
        error_setg(errp, "CXL hybrid metadata entry missing ramblock name");
        return false;
    }

    name_len = strlen(entry->ramblock);
    if (name_len > UCHAR_MAX) {
        error_setg(errp,
                   "CXL hybrid metadata ramblock name too long: %zu bytes",
                   name_len);
        return false;
    }

    if (!entry->length) {
        error_setg(errp, "CXL hybrid metadata entry %s has zero length",
                   entry->ramblock);
        return false;
    }

    if (entry->offset > UINT64_MAX - entry->length) {
        error_setg(errp, "CXL hybrid metadata entry %s overflows range",
                   entry->ramblock);
        return false;
    }

    return true;
}

static bool cxl_hybrid_warm_page_valid(const CXLHybridWarmPage *page,
                                       Error **errp)
{
    size_t name_len;

    if (!page || !page->ramblock || !page->ramblock[0]) {
        error_setg(errp, "CXL hybrid warm page missing ramblock name");
        return false;
    }

    name_len = strlen(page->ramblock);
    if (name_len > UCHAR_MAX) {
        error_setg(errp,
                   "CXL hybrid warm page ramblock name too long: %zu bytes",
                   name_len);
        return false;
    }

    if (!page->page_len || !page->data) {
        error_setg(errp, "CXL hybrid warm page missing payload");
        return false;
    }

    if (!QEMU_IS_ALIGNED(page->offset, CXL_HYBRID_STAGING_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(page->page_len, CXL_HYBRID_STAGING_PAGE_SIZE)) {
        error_setg(errp, "CXL hybrid warm page payload must be page aligned");
        return false;
    }

    if (page->offset > UINT64_MAX - page->page_len) {
        error_setg(errp, "CXL hybrid warm page range overflows");
        return false;
    }

    return true;
}

static bool cxl_hybrid_warm_desc_valid(const CXLHybridWarmDescriptor *desc,
                                       Error **errp)
{
    size_t name_len;

    if (!desc || !desc->ramblock || !desc->ramblock[0]) {
        error_setg(errp, "CXL hybrid warm descriptor missing ramblock name");
        return false;
    }

    name_len = strlen(desc->ramblock);
    if (name_len > UCHAR_MAX) {
        error_setg(errp,
                   "CXL hybrid warm descriptor ramblock name too long: %zu bytes",
                   name_len);
        return false;
    }

    if (!desc->page_len) {
        error_setg(errp, "CXL hybrid warm descriptor missing page length");
        return false;
    }

    if (!QEMU_IS_ALIGNED(desc->guest_offset, CXL_HYBRID_STAGING_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(desc->cxl_offset, CXL_HYBRID_STAGING_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(desc->page_len, CXL_HYBRID_STAGING_PAGE_SIZE)) {
        error_setg(errp,
                   "CXL hybrid warm descriptor offsets must be page aligned");
        return false;
    }

    if (desc->guest_offset > UINT64_MAX - desc->page_len ||
        desc->cxl_offset > UINT64_MAX - desc->page_len) {
        error_setg(errp, "CXL hybrid warm descriptor range overflows");
        return false;
    }

    return true;
}

static bool cxl_hybrid_warm_desc_range_valid(
    const CXLHybridWarmDescRange *range, Error **errp)
{
    CXLHybridWarmDescriptor desc = {
        .ramblock = range ? range->ramblock : NULL,
        .guest_offset = range ? range->guest_offset : 0,
        .cxl_offset = range ? range->cxl_offset : 0,
        .page_len = range ? range->page_len : 0,
        .flags = range ? range->flags : 0,
    };

    return cxl_hybrid_warm_desc_valid(&desc, errp);
}

void cxl_hybrid_metadata_cleanup(CXLHybridMetadata *meta)
{
    uint32_t i;

    if (!meta) {
        return;
    }

    for (i = 0; i < meta->nr_entries; i++) {
        g_free(meta->entries[i].ramblock);
    }
    g_free(meta->entries);
    memset(meta, 0, sizeof(*meta));
}

void cxl_hybrid_warm_page_cleanup(CXLHybridWarmPage *page)
{
    if (!page) {
        return;
    }

    g_free(page->ramblock);
    g_free(page->data);
    memset(page, 0, sizeof(*page));
}

void cxl_hybrid_warm_desc_cleanup(CXLHybridWarmDescriptor *desc)
{
    if (!desc) {
        return;
    }

    g_free(desc->ramblock);
    memset(desc, 0, sizeof(*desc));
}

void cxl_hybrid_warm_desc_batch_cleanup(CXLHybridWarmDescBatch *batch)
{
    uint32_t i;

    if (!batch) {
        return;
    }

    for (i = 0; i < batch->nr_entries; i++) {
        g_free(batch->entries[i].ramblock);
    }
    g_free(batch->entries);
    memset(batch, 0, sizeof(*batch));
}

void cxl_hybrid_publish_notify_cleanup(CXLHybridPublishNotify *notify)
{
    if (!notify) {
        return;
    }

    g_free(notify->ramblock);
    memset(notify, 0, sizeof(*notify));
}

bool cxl_hybrid_global_page_offset(const RAMBlock *block,
                                   uint64_t guest_offset,
                                   size_t page_size,
                                   ram_addr_t *global_offsetp)
{
    ram_addr_t global_offset;

    if (!block || !global_offsetp || !page_size) {
        return false;
    }

    if (guest_offset > block->used_length ||
        guest_offset + page_size > block->used_length) {
        return false;
    }

    global_offset = block->offset + guest_offset;
    if (global_offset < block->offset) {
        return false;
    }

    *global_offsetp = global_offset;
    return true;
}

void cxl_hybrid_dst_staging_cleanup(void)
{
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
        cxl_dst_staging.stopping = true;
        if (cxl_dst_staging.map_base) {
            munmap(cxl_dst_staging.map_base, cxl_dst_staging.map_len);
            cxl_dst_staging.map_base = NULL;
            cxl_dst_staging.map_len = 0;
        }
        if (cxl_dst_staging.fd >= 0) {
            close(cxl_dst_staging.fd);
            cxl_dst_staging.fd = -1;
        }
        g_clear_pointer(&cxl_dst_staging.slots, g_hash_table_destroy);
        qemu_cond_broadcast(&cxl_dst_staging.cond);
        while (cxl_dst_staging.waiters) {
            qemu_cond_wait(&cxl_dst_staging.cond, &cxl_dst_staging.lock);
        }
        cxl_dst_staging.capacity = 0;
        cxl_dst_staging.file_limit = 0;
        cxl_dst_staging.base_offset = 0;
        cxl_dst_staging.next_offset = 0;
        cxl_dst_staging.shared_map = false;
        cxl_dst_staging.waiters = 0;
        cxl_dst_staging.stopping = false;
        cxl_hybrid_dst_staging_reset_counters();
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    } else {
        if (cxl_dst_staging.map_base) {
            munmap(cxl_dst_staging.map_base, cxl_dst_staging.map_len);
            cxl_dst_staging.map_base = NULL;
            cxl_dst_staging.map_len = 0;
        }
        if (cxl_dst_staging.fd >= 0) {
            close(cxl_dst_staging.fd);
            cxl_dst_staging.fd = -1;
        }
        g_clear_pointer(&cxl_dst_staging.slots, g_hash_table_destroy);
        cxl_dst_staging.capacity = 0;
        cxl_dst_staging.file_limit = 0;
        cxl_dst_staging.base_offset = 0;
        cxl_dst_staging.next_offset = 0;
        cxl_dst_staging.shared_map = false;
        cxl_dst_staging.waiters = 0;
        cxl_dst_staging.stopping = false;
        cxl_hybrid_dst_staging_reset_counters();
    }
}

int cxl_hybrid_dst_staging_init_fixed_fd(int fd, size_t capacity,
                                         uint64_t base_offset,
                                         uint64_t file_limit,
                                         bool shared_map,
                                         Error **errp)
{
    int owned_fd = -1;
    void *map_base = NULL;

    if (fd < 0) {
        error_setg(errp, "CXL hybrid destination staging missing fd");
        return -EINVAL;
    }

    if (!capacity) {
        error_setg(errp, "CXL hybrid destination staging capacity is zero");
        return -EINVAL;
    }

    if (file_limit < base_offset) {
        error_setg(errp,
                   "CXL hybrid destination staging size overflows: "
                   "base=%" PRIu64 " file_limit=%" PRIu64,
                   base_offset, file_limit);
        return -EOVERFLOW;
    }

    if (file_limit - base_offset < capacity) {
        error_setg(errp,
                   "CXL hybrid destination staging capacity exceeds extent: "
                   "base=%" PRIu64 " capacity=%zu file_limit=%" PRIu64,
                   base_offset, capacity, file_limit);
        return -ENOSPC;
    }

    owned_fd = dup(fd);
    if (owned_fd < 0) {
        error_setg_errno(errp, errno,
                         "Failed to dup CXL hybrid destination staging fd");
        return -errno;
    }

    if (shared_map) {
        map_base = mmap(NULL, file_limit, PROT_READ | PROT_WRITE,
                        MAP_SHARED, owned_fd, 0);
        if (map_base == MAP_FAILED) {
            int saved_errno = errno;

            close(owned_fd);
            error_setg_errno(errp, saved_errno,
                             "Failed to mmap CXL hybrid destination staging");
            return -saved_errno;
        }
    }

    cxl_hybrid_dst_staging_cleanup();
    if (!cxl_dst_staging.sync_ready) {
        qemu_mutex_init(&cxl_dst_staging.lock);
        qemu_cond_init(&cxl_dst_staging.cond);
        cxl_dst_staging.sync_ready = true;
    }

    qemu_mutex_lock(&cxl_dst_staging.lock);
    cxl_dst_staging.fd = owned_fd;
    cxl_dst_staging.capacity = capacity;
    cxl_dst_staging.file_limit = file_limit;
    cxl_dst_staging.base_offset = base_offset;
    cxl_dst_staging.next_offset = base_offset;
    cxl_dst_staging.map_base = map_base;
    cxl_dst_staging.map_len = file_limit;
    cxl_dst_staging.shared_map = shared_map;
    cxl_dst_staging.stopping = false;
    cxl_dst_staging.waiters = 0;
    cxl_hybrid_dst_staging_reset_counters();
    cxl_dst_staging.slots =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                              cxl_hybrid_dst_staging_slot_free);
    qemu_mutex_unlock(&cxl_dst_staging.lock);
    return 0;
}

int cxl_hybrid_dst_staging_init_path_at(const char *path, size_t capacity,
                                        uint64_t base_offset, Error **errp)
{
    int fd;
    uint64_t required_size;
    int ret;

    if (!path || !path[0]) {
        error_setg(errp, "CXL hybrid destination staging missing path");
        return -EINVAL;
    }

    required_size = base_offset + capacity;
    if (required_size < base_offset) {
        error_setg(errp,
                   "CXL hybrid destination staging size overflows: base=%" PRIu64
                   " capacity=%zu",
                   base_offset, capacity);
        return -EOVERFLOW;
    }

    fd = open(path, O_RDWR | O_CLOEXEC | O_CREAT, 0600);
    if (fd < 0) {
        error_setg_errno(errp, errno,
                         "Failed to open CXL hybrid destination staging %s",
                         path);
        return -errno;
    }

    if (ftruncate(fd, required_size) < 0) {
        int saved_errno = errno;

        close(fd);
        error_setg_errno(errp, saved_errno,
                         "Failed to size CXL hybrid destination staging %s",
                         path);
        return -saved_errno;
    }

    ret = cxl_hybrid_dst_staging_init_fixed_fd(fd, capacity, base_offset,
                                               required_size, false, errp);
    close(fd);
    return ret;
}

int cxl_hybrid_dst_staging_init_path(const char *path, size_t capacity,
                                     Error **errp)
{
    return cxl_hybrid_dst_staging_init_path_at(path, capacity, 0, errp);
}

static CXLHybridDstStagingSlot *
cxl_hybrid_dst_staging_lookup(const char *ramblock, uint64_t offset)
{
    GHashTableIter iter;
    gpointer value;

    if (!cxl_dst_staging.slots || !ramblock) {
        return NULL;
    }

    g_hash_table_iter_init(&iter, cxl_dst_staging.slots);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        CXLHybridDstStagingSlot *slot = value;
        uint64_t slot_end;

        if (strcmp(slot->ramblock, ramblock) != 0) {
            continue;
        }

        slot_end = slot->guest_offset + slot->length;
        if (offset >= slot->guest_offset && offset < slot_end) {
            return slot;
        }
    }

    return NULL;
}

static bool cxl_hybrid_dst_staging_is_active_locked(void)
{
    return cxl_dst_staging.fd >= 0 && cxl_dst_staging.slots;
}

static int cxl_hybrid_dst_staging_slot_page_offset(CXLHybridDstStagingSlot *slot,
                                                   uint64_t offset,
                                                   size_t len,
                                                   off_t *file_offset,
                                                   Error **errp)
{
    uint64_t within_slot;
    uint64_t end_offset;

    if (!slot) {
        error_setg(errp, "CXL hybrid destination staging missing slot");
        return -ENOENT;
    }

    if (offset < slot->guest_offset) {
        error_setg(errp,
                   "CXL hybrid destination staging offset 0x%" PRIx64
                   " before slot start 0x%" PRIx64,
                   offset, slot->guest_offset);
        return -EINVAL;
    }

    within_slot = offset - slot->guest_offset;
    if (within_slot > slot->length) {
        error_setg(errp,
                   "CXL hybrid destination staging offset 0x%" PRIx64
                   " beyond slot length %zu",
                   offset, slot->length);
        return -EINVAL;
    }

    end_offset = within_slot + len;
    if (end_offset < within_slot || end_offset > slot->length) {
        error_setg(errp,
                   "CXL hybrid destination staging access overruns slot: "
                   "offset=0x%" PRIx64 " len=%zu slot_len=%zu",
                   offset, len, slot->length);
        return -EINVAL;
    }

    *file_offset = slot->staging_offset + within_slot;
    if (slot->external_offsets && len == slot->page_size &&
        QEMU_IS_ALIGNED(within_slot, slot->page_size)) {
        size_t page = within_slot / slot->page_size;

        if (slot->external_offsets[page] != UINT64_MAX) {
            *file_offset = slot->external_offsets[page];
        }
    }
    return 0;
}

static int cxl_hybrid_dst_staging_slot_page_index(CXLHybridDstStagingSlot *slot,
                                                  uint64_t offset,
                                                  size_t len,
                                                  size_t *first_page,
                                                  size_t *nr_pages,
                                                  Error **errp)
{
    uint64_t within_slot;
    uint64_t end_offset;
    size_t page_size;

    if (!slot) {
        error_setg(errp, "CXL hybrid destination staging missing slot");
        return -ENOENT;
    }

    if (offset < slot->guest_offset) {
        error_setg(errp,
                   "CXL hybrid destination staging offset 0x%" PRIx64
                   " before slot start 0x%" PRIx64,
                   offset, slot->guest_offset);
        return -EINVAL;
    }

    page_size = slot->page_size;
    within_slot = offset - slot->guest_offset;
    end_offset = within_slot + len;
    if (end_offset < within_slot || end_offset > slot->length) {
        error_setg(errp,
                   "CXL hybrid destination staging page range overruns slot: "
                   "offset=0x%" PRIx64 " len=%zu slot_len=%zu",
                   offset, len, slot->length);
        return -EINVAL;
    }

    if (!QEMU_IS_ALIGNED(within_slot, page_size) || !QEMU_IS_ALIGNED(len, page_size)) {
        error_setg(errp,
                   "CXL hybrid destination staging requires page aligned access");
        return -EINVAL;
    }

    *first_page = within_slot / page_size;
    *nr_pages = len / page_size;
    if (*first_page + *nr_pages > slot->nr_pages) {
        error_setg(errp,
                   "CXL hybrid destination staging page index out of range");
        return -EINVAL;
    }

    return 0;
}

static bool cxl_hybrid_dst_staging_range_present_locked(const char *ramblock,
                                                        uint64_t offset,
                                                        size_t len)
{
    CXLHybridDstStagingSlot *slot =
        cxl_hybrid_dst_staging_lookup(ramblock, offset);
    size_t first_page, nr_pages;
    size_t page;

    if (!slot) {
        return false;
    }

    if (cxl_hybrid_dst_staging_slot_page_index(slot, offset, len,
                                               &first_page, &nr_pages,
                                               NULL)) {
        return false;
    }

    for (page = 0; page < nr_pages; page++) {
        if (!test_bit(first_page + page, slot->present_bitmap)) {
            return false;
        }
    }

    return true;
}

int cxl_hybrid_dst_staging_apply_metadata(const CXLHybridMetadata *meta,
                                          Error **errp)
{
    uint32_t i;
    int ret = 0;

    if (!cxl_hybrid_dst_staging_is_active_locked()) {
        error_setg(errp, "CXL hybrid destination staging is not initialized");
        return -EINVAL;
    }

    if (!meta) {
        error_setg(errp, "CXL hybrid destination staging missing metadata");
        return -EINVAL;
    }

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }

    g_hash_table_remove_all(cxl_dst_staging.slots);
    cxl_dst_staging.next_offset = cxl_dst_staging.base_offset;
    qatomic_set(&cxl_dst_staging.present_slots, 0);

    for (i = 0; i < meta->nr_entries; i++) {
        const CXLHybridMetadataEntry *entry = &meta->entries[i];
        uint64_t end_offset;
        CXLHybridDstStagingSlot *slot;
        char *key;

        if (!cxl_hybrid_metadata_entry_valid(entry, errp)) {
            ret = -EINVAL;
            goto out;
        }

        if (entry->length > SIZE_MAX) {
            error_setg(errp,
                       "CXL hybrid destination staging entry too large: %" PRIu64,
                       entry->length);
            ret = -EOVERFLOW;
            goto out;
        }

        end_offset = cxl_dst_staging.next_offset + entry->length;
        if (end_offset < cxl_dst_staging.next_offset ||
            end_offset > cxl_dst_staging.file_limit) {
            error_setg(errp,
                       "CXL hybrid destination staging capacity exhausted");
            ret = -ENOSPC;
            goto out;
        }

        slot = g_new0(CXLHybridDstStagingSlot, 1);
        slot->ramblock = g_strdup(entry->ramblock);
        slot->guest_offset = entry->offset;
        slot->staging_offset = cxl_dst_staging.next_offset;
        slot->length = entry->length;
        slot->page_size = CXL_HYBRID_STAGING_PAGE_SIZE;
        slot->nr_pages = DIV_ROUND_UP(entry->length, slot->page_size);
        slot->present_bitmap = bitmap_new(slot->nr_pages);
        slot->external_offsets = g_new(uint64_t, slot->nr_pages);
        for (size_t page = 0; page < slot->nr_pages; page++) {
            slot->external_offsets[page] = UINT64_MAX;
        }

        key = cxl_hybrid_dst_staging_key(slot->ramblock, slot->guest_offset);
        g_hash_table_insert(cxl_dst_staging.slots, key, slot);
        cxl_dst_staging.next_offset = end_offset;
    }

out:
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }
    return ret;
}

int cxl_hybrid_dst_staging_store_page(const char *ramblock, uint64_t offset,
                                      const void *buf, size_t len,
                                      Error **errp)
{
    CXLHybridDstStagingSlot *slot;
    off_t file_offset;
    size_t first_page, nr_pages;
    size_t page;
    ssize_t ret;
    int rc = 0;

    if (!buf || !len) {
        error_setg(errp, "CXL hybrid destination staging store missing buffer");
        return -EINVAL;
    }

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }

    slot = cxl_hybrid_dst_staging_lookup(ramblock, offset);
    if (!slot) {
        error_setg(errp,
                   "CXL hybrid destination staging slot missing for %s/0x%" PRIx64,
                   ramblock ?: "<null>", offset);
        rc = -ENOENT;
        goto out;
    }

    ret = cxl_hybrid_dst_staging_slot_page_offset(slot, offset, len,
                                                  &file_offset, errp);
    if (ret) {
        rc = ret;
        goto out;
    }
    ret = cxl_hybrid_dst_staging_slot_page_index(slot, offset, len,
                                                 &first_page, &nr_pages, errp);
    if (ret) {
        rc = ret;
        goto out;
    }

    if (cxl_dst_staging.shared_map) {
        memcpy((uint8_t *)cxl_dst_staging.map_base + file_offset, buf, len);
        ret = len;
    } else {
        ret = pwrite(cxl_dst_staging.fd, buf, len, file_offset);
    }
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "Failed to write CXL hybrid destination staging slot");
        rc = -errno;
        goto out;
    }

    if (ret != len) {
        error_setg(errp,
                   "Short write to CXL hybrid destination staging slot: %zd/%zu",
                   ret, len);
        rc = -EIO;
        goto out;
    }

    for (page = 0; page < nr_pages; page++) {
        if (!test_and_set_bit(first_page + page, slot->present_bitmap)) {
            qatomic_inc(&cxl_dst_staging.present_slots);
        }
    }
    if (cxl_dst_staging.sync_ready) {
        qemu_cond_broadcast(&cxl_dst_staging.cond);
    }

out:
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }
    return rc;
}

static int cxl_hybrid_dst_staging_register_external_page_locked(
    const char *ramblock,
    uint64_t guest_offset,
    uint64_t cxl_offset,
    size_t len,
    bool *changedp,
    Error **errp)
{
    CXLHybridDstStagingSlot *slot;
    size_t first_page, nr_pages;
    size_t page;
    int ret;
    int rc = 0;

    if (!cxl_hybrid_dst_staging_is_active_locked()) {
        error_setg(errp, "CXL hybrid destination staging is not initialized");
        return -EINVAL;
    }

    if (!ramblock || !ramblock[0]) {
        error_setg(errp,
                   "CXL hybrid external staging registration missing ramblock");
        return -EINVAL;
    }

    if (!len) {
        error_setg(errp,
                   "CXL hybrid external staging registration length is zero");
        return -EINVAL;
    }

    if (!QEMU_IS_ALIGNED(guest_offset, CXL_HYBRID_STAGING_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(cxl_offset, CXL_HYBRID_STAGING_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(len, CXL_HYBRID_STAGING_PAGE_SIZE)) {
        error_setg(errp,
                   "CXL hybrid external staging registration requires page aligned offsets");
        return -EINVAL;
    }

    if (cxl_offset > UINT64_MAX - len) {
        error_setg(errp,
                   "CXL hybrid external staging registration range overflows");
        return -EOVERFLOW;
    }

    slot = cxl_hybrid_dst_staging_lookup(ramblock, guest_offset);
    if (!slot) {
        char *key;

        slot = g_new0(CXLHybridDstStagingSlot, 1);
        slot->ramblock = g_strdup(ramblock);
        slot->guest_offset = guest_offset;
        slot->staging_offset = cxl_offset;
        slot->length = len;
        slot->page_size = CXL_HYBRID_STAGING_PAGE_SIZE;
        slot->nr_pages = DIV_ROUND_UP(len, slot->page_size);
        slot->present_bitmap = bitmap_new(slot->nr_pages);
        slot->external_offsets = g_new(uint64_t, slot->nr_pages);
        for (page = 0; page < slot->nr_pages; page++) {
            slot->external_offsets[page] = UINT64_MAX;
        }

        key = cxl_hybrid_dst_staging_key(slot->ramblock, slot->guest_offset);
        g_hash_table_insert(cxl_dst_staging.slots, key, slot);
    } else {
        uint64_t slot_end = slot->guest_offset + slot->length;

        if (guest_offset < slot->guest_offset ||
            guest_offset + len > slot_end ||
            guest_offset + len < guest_offset) {
            error_setg(errp,
                       "CXL hybrid external staging registration conflicts with existing slot");
            rc = -EINVAL;
            goto out;
        }
    }

    ret = cxl_hybrid_dst_staging_slot_page_index(slot, guest_offset, len,
                                                 &first_page, &nr_pages, errp);
    if (ret) {
        rc = ret;
        goto out;
    }

    for (page = 0; page < nr_pages; page++) {
        if (slot->external_offsets) {
            slot->external_offsets[first_page + page] =
                cxl_offset + page * slot->page_size;
        }
        if (!test_and_set_bit(first_page + page, slot->present_bitmap)) {
            qatomic_inc(&cxl_dst_staging.present_slots);
            if (changedp) {
                *changedp = true;
            }
        }
    }

out:
    return rc;
}

static CXLHybridDstStagingSlot *cxl_hybrid_dst_staging_find_next_slot_locked(
    const char *ramblock,
    uint64_t guest_offset)
{
    GHashTableIter iter;
    gpointer value;
    CXLHybridDstStagingSlot *best = NULL;

    if (!cxl_dst_staging.slots || !ramblock) {
        return NULL;
    }

    g_hash_table_iter_init(&iter, cxl_dst_staging.slots);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        CXLHybridDstStagingSlot *slot = value;

        if (strcmp(slot->ramblock, ramblock) != 0 ||
            slot->guest_offset <= guest_offset) {
            continue;
        }
        if (!best || slot->guest_offset < best->guest_offset) {
            best = slot;
        }
    }

    return best;
}

static int cxl_hybrid_dst_staging_register_external_range_locked(
    const char *ramblock,
    uint64_t guest_offset,
    uint64_t cxl_offset,
    size_t len,
    bool *changedp,
    Error **errp)
{
    size_t remaining = len;
    uint64_t guest_cursor = guest_offset;
    uint64_t cxl_cursor = cxl_offset;

    while (remaining) {
        CXLHybridDstStagingSlot *slot =
            cxl_hybrid_dst_staging_lookup(ramblock, guest_cursor);
        size_t chunk_len = remaining;
        int ret;

        if (slot) {
            uint64_t slot_end = slot->guest_offset + slot->length;

            chunk_len = MIN(chunk_len, (size_t)(slot_end - guest_cursor));
        } else {
            CXLHybridDstStagingSlot *next_slot =
                cxl_hybrid_dst_staging_find_next_slot_locked(ramblock,
                                                             guest_cursor);

            if (next_slot) {
                chunk_len = MIN(chunk_len,
                                (size_t)(next_slot->guest_offset -
                                         guest_cursor));
            }
        }

        if (!chunk_len ||
            !QEMU_IS_ALIGNED(chunk_len, CXL_HYBRID_STAGING_PAGE_SIZE)) {
            error_setg(errp,
                       "CXL hybrid external staging registration could not split range");
            return -EINVAL;
        }

        ret = cxl_hybrid_dst_staging_register_external_page_locked(
            ramblock, guest_cursor, cxl_cursor, chunk_len, changedp, errp);
        if (ret) {
            return ret;
        }

        remaining -= chunk_len;
        guest_cursor += chunk_len;
        cxl_cursor += chunk_len;
    }

    return 0;
}

int cxl_hybrid_dst_staging_register_external_page(const char *ramblock,
                                                  uint64_t guest_offset,
                                                  uint64_t cxl_offset,
                                                  size_t len,
                                                  Error **errp)
{
    bool changed = false;
    int rc;

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }

    rc = cxl_hybrid_dst_staging_register_external_page_locked(
        ramblock, guest_offset, cxl_offset, len, &changed, errp);
    if (!rc && changed && cxl_dst_staging.sync_ready) {
        qemu_cond_broadcast(&cxl_dst_staging.cond);
    }

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }

    return rc;
}

bool cxl_hybrid_dst_staging_page_present(const char *ramblock, uint64_t offset)
{
    return cxl_hybrid_dst_staging_range_present(ramblock, offset,
                                                CXL_HYBRID_STAGING_PAGE_SIZE);
}

bool cxl_hybrid_dst_staging_range_present(const char *ramblock,
                                          uint64_t offset,
                                          size_t len)
{
    bool present;

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }
    present = cxl_hybrid_dst_staging_range_present_locked(ramblock, offset,
                                                          len);
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }
    return present;
}

int cxl_hybrid_dst_staging_wait_page_present(const char *ramblock,
                                             uint64_t offset,
                                             Error **errp)
{
    return cxl_hybrid_dst_staging_wait_range_present(
        ramblock, offset, CXL_HYBRID_STAGING_PAGE_SIZE, errp);
}

int cxl_hybrid_dst_staging_wait_range_present(const char *ramblock,
                                              uint64_t offset,
                                              size_t len,
                                              Error **errp)
{
    if (!ramblock || !ramblock[0]) {
        error_setg(errp, "CXL hybrid destination wait missing ramblock");
        return -EINVAL;
    }

    if (!len ||
        !QEMU_IS_ALIGNED(offset, CXL_HYBRID_STAGING_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(len, CXL_HYBRID_STAGING_PAGE_SIZE) ||
        offset > UINT64_MAX - len) {
        error_setg(errp, "CXL hybrid destination wait requires page-aligned length");
        return -EINVAL;
    }

    if (!cxl_dst_staging.sync_ready) {
        error_setg(errp, "CXL hybrid destination staging wait is unavailable");
        return -EINVAL;
    }

    qemu_mutex_lock(&cxl_dst_staging.lock);
    if (!cxl_hybrid_dst_staging_is_active_locked()) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
        error_setg(errp, "CXL hybrid destination staging is not initialized");
        return -EINVAL;
    }
    cxl_dst_staging.waiters++;
    while (cxl_hybrid_dst_staging_is_active_locked() &&
           !cxl_hybrid_dst_staging_range_present_locked(
               ramblock, offset, len)) {
        qemu_cond_wait(&cxl_dst_staging.cond, &cxl_dst_staging.lock);
    }
    if (!cxl_hybrid_dst_staging_is_active_locked()) {
        cxl_dst_staging.waiters--;
        qemu_cond_broadcast(&cxl_dst_staging.cond);
        qemu_mutex_unlock(&cxl_dst_staging.lock);
        error_setg(errp, "CXL hybrid destination staging was cleaned up");
        return -ECANCELED;
    }
    cxl_dst_staging.waiters--;
    qemu_cond_broadcast(&cxl_dst_staging.cond);
    qemu_mutex_unlock(&cxl_dst_staging.lock);

    return 0;
}

bool cxl_hybrid_dst_staging_is_active(void)
{
    bool active;

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }
    active = cxl_hybrid_dst_staging_is_active_locked();
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }
    return active;
}

void cxl_hybrid_dst_staging_get_stats(CXLHybridDstStagingStats *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }
    stats->capacity_bytes = cxl_dst_staging.capacity;
    stats->slots = cxl_dst_staging.slots ?
                   g_hash_table_size(cxl_dst_staging.slots) : 0;
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }
    stats->present_slots = qatomic_read(&cxl_dst_staging.present_slots);
    stats->fault_hits = qatomic_read(&cxl_dst_staging.fault_hits);
    stats->fault_misses = qatomic_read(&cxl_dst_staging.fault_misses);
    stats->fault_read_bytes = qatomic_read(&cxl_dst_staging.fault_read_bytes);
    stats->fault_read_time_ns =
        qatomic_read(&cxl_dst_staging.fault_read_time_ns);
    stats->fault_place_successes =
        qatomic_read(&cxl_dst_staging.fault_place_successes);
    stats->fault_place_failures =
        qatomic_read(&cxl_dst_staging.fault_place_failures);
}

void cxl_hybrid_dst_staging_account_fault_miss(void)
{
    qatomic_inc(&cxl_dst_staging.fault_misses);
}

void cxl_hybrid_dst_staging_account_fault_hit(size_t len, uint64_t read_time_ns)
{
    qatomic_inc(&cxl_dst_staging.fault_hits);
    qatomic_add(&cxl_dst_staging.fault_read_bytes, len);
    qatomic_add(&cxl_dst_staging.fault_read_time_ns, read_time_ns);
}

void cxl_hybrid_dst_staging_account_fault_place_result(bool success)
{
    if (success) {
        qatomic_inc(&cxl_dst_staging.fault_place_successes);
    } else {
        qatomic_inc(&cxl_dst_staging.fault_place_failures);
    }
}

int cxl_hybrid_dst_staging_read_page(const char *ramblock, uint64_t offset,
                                     void *buf, size_t len, Error **errp)
{
    CXLHybridDstStagingSlot *slot;
    off_t file_offset;
    size_t first_page, nr_pages;
    size_t page;
    ssize_t ret = 0;

    if (!buf || !len) {
        error_setg(errp, "CXL hybrid destination staging read missing buffer");
        return -EINVAL;
    }

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }
    slot = cxl_hybrid_dst_staging_lookup(ramblock, offset);
    if (!slot || !cxl_hybrid_dst_staging_range_present_locked(ramblock,
                                                              offset, len)) {
        error_setg(errp,
                   "CXL hybrid destination staging page missing for %s/0x%" PRIx64,
                   ramblock ?: "<null>", offset);
        if (cxl_dst_staging.sync_ready) {
            qemu_mutex_unlock(&cxl_dst_staging.lock);
        }
        return -ENOENT;
    }

    ret = cxl_hybrid_dst_staging_slot_page_index(slot, offset, len,
                                                 &first_page, &nr_pages, errp);
    if (ret) {
        if (cxl_dst_staging.sync_ready) {
            qemu_mutex_unlock(&cxl_dst_staging.lock);
        }
        return ret;
    }

    for (page = 0; page < nr_pages; page++) {
        uint64_t page_offset = offset + page * slot->page_size;
        uint8_t *page_buf = (uint8_t *)buf + page * slot->page_size;

        ret = cxl_hybrid_dst_staging_slot_page_offset(slot, page_offset,
                                                      slot->page_size,
                                                      &file_offset, errp);
        if (ret) {
            if (cxl_dst_staging.sync_ready) {
                qemu_mutex_unlock(&cxl_dst_staging.lock);
            }
            return ret;
        }

        if (cxl_dst_staging.shared_map) {
            memcpy(page_buf, (uint8_t *)cxl_dst_staging.map_base + file_offset,
                   slot->page_size);
            ret = slot->page_size;
        } else {
            ret = pread(cxl_dst_staging.fd, page_buf, slot->page_size,
                        file_offset);
        }
        if (ret < 0) {
            int saved_errno = errno;

            if (cxl_dst_staging.sync_ready) {
                qemu_mutex_unlock(&cxl_dst_staging.lock);
            }
            error_setg_errno(errp, saved_errno,
                             "Failed to read CXL hybrid destination staging slot");
            return -saved_errno;
        }

        if (ret != slot->page_size) {
            if (cxl_dst_staging.sync_ready) {
                qemu_mutex_unlock(&cxl_dst_staging.lock);
            }
            error_setg(errp,
                       "Short read from CXL hybrid destination staging slot: %zd/%zu",
                       ret, slot->page_size);
            return -EIO;
        }
    }

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }
    return 0;
}

int cxl_hybrid_metadata_encoded_len(const CXLHybridMetadata *meta,
                                    size_t *len,
                                    Error **errp)
{
    size_t total = CXL_HYBRID_METADATA_HEADER_LEN;
    uint32_t i;

    if (!meta || !len) {
        error_setg(errp, "CXL hybrid metadata length request missing arguments");
        return -EINVAL;
    }

    if (meta->nr_entries && !meta->entries) {
        error_setg(errp, "CXL hybrid metadata has %u entries without storage",
                   meta->nr_entries);
        return -EINVAL;
    }

    for (i = 0; i < meta->nr_entries; i++) {
        const CXLHybridMetadataEntry *entry = &meta->entries[i];
        size_t name_len;

        if (!cxl_hybrid_metadata_entry_valid(entry, errp)) {
            return -EINVAL;
        }

        name_len = strlen(entry->ramblock);
        if (total > SIZE_MAX - CXL_HYBRID_METADATA_ENTRY_HEADER_LEN - name_len) {
            error_setg(errp, "CXL hybrid metadata payload too large");
            return -EOVERFLOW;
        }
        total += CXL_HYBRID_METADATA_ENTRY_HEADER_LEN + name_len;
    }

    *len = total;
    return 0;
}

int cxl_hybrid_warm_page_encoded_len(const CXLHybridWarmPage *page,
                                     size_t *len,
                                     Error **errp)
{
    size_t name_len;

    if (!len) {
        error_setg(errp, "CXL hybrid warm page length request missing output");
        return -EINVAL;
    }

    if (!cxl_hybrid_warm_page_valid(page, errp)) {
        return -EINVAL;
    }

    name_len = strlen(page->ramblock);
    if (page->page_len > SIZE_MAX - CXL_HYBRID_WARM_PAGE_HEADER_LEN - name_len) {
        error_setg(errp, "CXL hybrid warm page payload too large");
        return -EOVERFLOW;
    }

    *len = CXL_HYBRID_WARM_PAGE_HEADER_LEN + name_len + page->page_len;
    return 0;
}

int cxl_hybrid_warm_desc_encoded_len(const CXLHybridWarmDescriptor *desc,
                                     size_t *len,
                                     Error **errp)
{
    size_t name_len;

    if (!len) {
        error_setg(errp, "CXL hybrid warm descriptor length request missing output");
        return -EINVAL;
    }

    if (!cxl_hybrid_warm_desc_valid(desc, errp)) {
        return -EINVAL;
    }

    name_len = strlen(desc->ramblock);
    *len = CXL_HYBRID_WARM_DESC_HEADER_LEN + name_len;
    return 0;
}

int cxl_hybrid_warm_desc_batch_encoded_len(const CXLHybridWarmDescBatch *batch,
                                           size_t *len,
                                           Error **errp)
{
    size_t total = CXL_HYBRID_WARM_DESC_BATCH_HEADER_LEN;
    uint32_t i;

    if (!len) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch length request missing output");
        return -EINVAL;
    }

    if (!batch) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch length request missing batch");
        return -EINVAL;
    }

    if (batch->nr_entries && !batch->entries) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch has %u entries without storage",
                   batch->nr_entries);
        return -EINVAL;
    }

    for (i = 0; i < batch->nr_entries; i++) {
        const CXLHybridWarmDescRange *range = &batch->entries[i];
        size_t name_len;

        if (!cxl_hybrid_warm_desc_range_valid(range, errp)) {
            return -EINVAL;
        }

        name_len = strlen(range->ramblock);
        if (total > SIZE_MAX - CXL_HYBRID_WARM_DESC_BATCH_ENTRY_HEADER_LEN ||
            total + CXL_HYBRID_WARM_DESC_BATCH_ENTRY_HEADER_LEN >
            SIZE_MAX - name_len) {
            error_setg(errp,
                       "CXL hybrid warm descriptor batch payload too large");
            return -EOVERFLOW;
        }

        total += CXL_HYBRID_WARM_DESC_BATCH_ENTRY_HEADER_LEN + name_len;
    }

    *len = total;
    return 0;
}

static void cxl_hybrid_publish_notify_to_desc(
    const CXLHybridPublishNotify *notify,
    CXLHybridWarmDescriptor *desc)
{
    *desc = (CXLHybridWarmDescriptor) {
        .ramblock = notify ? notify->ramblock : NULL,
        .guest_offset = notify ? notify->guest_offset : 0,
        .cxl_offset = notify ? notify->cxl_offset : 0,
        .page_len = notify ? notify->page_len : 0,
        .flags = 0,
        .generation = notify ? notify->generation : 0,
    };
}

int cxl_hybrid_publish_notify_encoded_len(const CXLHybridPublishNotify *notify,
                                          size_t *len,
                                          Error **errp)
{
    CXLHybridWarmDescriptor desc;

    cxl_hybrid_publish_notify_to_desc(notify, &desc);
    return cxl_hybrid_warm_desc_encoded_len(&desc, len, errp);
}

int cxl_hybrid_metadata_encode(const CXLHybridMetadata *meta,
                               uint8_t *buf,
                               size_t len,
                               Error **errp)
{
    uint8_t *p = buf;
    size_t expected_len;
    uint32_t i;
    int ret;

    if (!buf) {
        error_setg(errp, "CXL hybrid metadata encode missing buffer");
        return -EINVAL;
    }

    ret = cxl_hybrid_metadata_encoded_len(meta, &expected_len, errp);
    if (ret) {
        return ret;
    }

    if (len != expected_len) {
        error_setg(errp,
                   "CXL hybrid metadata encode length mismatch: got %zu expected %zu",
                   len, expected_len);
        return -EINVAL;
    }

    stl_be_p(p, meta->version);
    p += 4;
    stl_be_p(p, meta->generation);
    p += 4;
    stl_be_p(p, meta->nr_entries);
    p += 4;

    for (i = 0; i < meta->nr_entries; i++) {
        const CXLHybridMetadataEntry *entry = &meta->entries[i];
        uint8_t name_len = strlen(entry->ramblock);

        *p++ = name_len;
        memcpy(p, entry->ramblock, name_len);
        p += name_len;
        stq_be_p(p, entry->offset);
        p += 8;
        stq_be_p(p, entry->length);
        p += 8;
        stl_be_p(p, entry->flags);
        p += 4;
        stl_be_p(p, entry->heat);
        p += 4;
    }

    return 0;
}

int cxl_hybrid_warm_page_encode(const CXLHybridWarmPage *page,
                                uint8_t *buf,
                                size_t len,
                                Error **errp)
{
    uint8_t *p = buf;
    size_t expected_len;
    uint8_t name_len;
    int ret;

    if (!buf) {
        error_setg(errp, "CXL hybrid warm page encode missing buffer");
        return -EINVAL;
    }

    ret = cxl_hybrid_warm_page_encoded_len(page, &expected_len, errp);
    if (ret) {
        return ret;
    }

    if (len != expected_len) {
        error_setg(errp,
                   "CXL hybrid warm page encode length mismatch: got %zu expected %zu",
                   len, expected_len);
        return -EINVAL;
    }

    name_len = strlen(page->ramblock);
    *p++ = name_len;
    memcpy(p, page->ramblock, name_len);
    p += name_len;
    stq_be_p(p, page->offset);
    p += 8;
    stl_be_p(p, page->page_len);
    p += 4;
    memcpy(p, page->data, page->page_len);
    return 0;
}

int cxl_hybrid_warm_desc_encode(const CXLHybridWarmDescriptor *desc,
                                uint8_t *buf,
                                size_t len,
                                Error **errp)
{
    uint8_t *p = buf;
    size_t expected_len;
    uint8_t name_len;
    int ret;

    if (!buf) {
        error_setg(errp, "CXL hybrid warm descriptor encode missing buffer");
        return -EINVAL;
    }

    ret = cxl_hybrid_warm_desc_encoded_len(desc, &expected_len, errp);
    if (ret) {
        return ret;
    }

    if (len != expected_len) {
        error_setg(errp,
                   "CXL hybrid warm descriptor encode length mismatch: got %zu expected %zu",
                   len, expected_len);
        return -EINVAL;
    }

    name_len = strlen(desc->ramblock);
    *p++ = name_len;
    memcpy(p, desc->ramblock, name_len);
    p += name_len;
    stq_be_p(p, desc->guest_offset);
    p += 8;
    stq_be_p(p, desc->cxl_offset);
    p += 8;
    stl_be_p(p, desc->page_len);
    p += 4;
    stl_be_p(p, desc->flags);
    p += 4;
    stl_be_p(p, desc->generation);
    return 0;
}

int cxl_hybrid_warm_desc_batch_encode(const CXLHybridWarmDescBatch *batch,
                                      uint8_t *buf,
                                      size_t len,
                                      Error **errp)
{
    uint8_t *p = buf;
    size_t expected_len;
    uint32_t i;
    int ret;

    if (!buf) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch encode missing buffer");
        return -EINVAL;
    }

    ret = cxl_hybrid_warm_desc_batch_encoded_len(batch, &expected_len, errp);
    if (ret) {
        return ret;
    }

    if (len != expected_len) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch encode length mismatch: got %zu expected %zu",
                   len, expected_len);
        return -EINVAL;
    }

    stl_be_p(p, batch->generation);
    p += 4;
    stl_be_p(p, batch->nr_entries);
    p += 4;

    for (i = 0; i < batch->nr_entries; i++) {
        const CXLHybridWarmDescRange *range = &batch->entries[i];
        uint8_t name_len = strlen(range->ramblock);

        *p++ = name_len;
        memcpy(p, range->ramblock, name_len);
        p += name_len;
        stq_be_p(p, range->guest_offset);
        p += 8;
        stq_be_p(p, range->cxl_offset);
        p += 8;
        stl_be_p(p, range->page_len);
        p += 4;
        stl_be_p(p, range->flags);
        p += 4;
    }

    return 0;
}

int cxl_hybrid_publish_notify_encode(const CXLHybridPublishNotify *notify,
                                     uint8_t *buf,
                                     size_t len,
                                     Error **errp)
{
    CXLHybridWarmDescriptor desc;

    cxl_hybrid_publish_notify_to_desc(notify, &desc);
    return cxl_hybrid_warm_desc_encode(&desc, buf, len, errp);
}

int cxl_hybrid_metadata_decode(CXLHybridMetadata *meta,
                               const uint8_t *buf,
                               size_t len,
                               Error **errp)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    CXLHybridMetadata decoded = { 0 };
    uint32_t i;

    if (!meta || !buf) {
        error_setg(errp, "CXL hybrid metadata decode missing arguments");
        return -EINVAL;
    }

    if (len < CXL_HYBRID_METADATA_HEADER_LEN) {
        error_setg(errp, "CXL hybrid metadata payload too short: %zu", len);
        return -EINVAL;
    }

    decoded.version = ldl_be_p(p);
    p += 4;
    decoded.generation = ldl_be_p(p);
    p += 4;
    decoded.nr_entries = ldl_be_p(p);
    p += 4;

    if (decoded.version != CXL_HYBRID_METADATA_VERSION) {
        error_setg(errp, "Unsupported CXL hybrid metadata version %u",
                   decoded.version);
        return -EINVAL;
    }

    if (decoded.nr_entries) {
        decoded.entries = g_new0(CXLHybridMetadataEntry, decoded.nr_entries);
    }

    for (i = 0; i < decoded.nr_entries; i++) {
        CXLHybridMetadataEntry *entry = &decoded.entries[i];
        uint8_t name_len;

        if ((size_t)(end - p) < 1) {
            error_setg(errp,
                       "CXL hybrid metadata truncated before entry %u name length",
                       i);
            goto fail;
        }
        name_len = *p++;

        if ((size_t)(end - p) < name_len + CXL_HYBRID_METADATA_ENTRY_HEADER_LEN - 1) {
            error_setg(errp, "CXL hybrid metadata truncated in entry %u", i);
            goto fail;
        }

        entry->ramblock = g_strndup((const char *)p, name_len);
        p += name_len;
        entry->offset = ldq_be_p(p);
        p += 8;
        entry->length = ldq_be_p(p);
        p += 8;
        entry->flags = ldl_be_p(p);
        p += 4;
        entry->heat = ldl_be_p(p);
        p += 4;

        if (!cxl_hybrid_metadata_entry_valid(entry, errp)) {
            goto fail;
        }
    }

    if (p != end) {
        error_setg(errp, "CXL hybrid metadata payload has %zu trailing bytes",
                   (size_t)(end - p));
        goto fail;
    }

    cxl_hybrid_metadata_cleanup(meta);
    *meta = decoded;
    return 0;

fail:
    cxl_hybrid_metadata_cleanup(&decoded);
    return -EINVAL;
}

int cxl_hybrid_warm_page_decode(CXLHybridWarmPage *page,
                                const uint8_t *buf,
                                size_t len,
                                Error **errp)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    CXLHybridWarmPage decoded = { 0 };
    uint8_t name_len;

    if (!page || !buf) {
        error_setg(errp, "CXL hybrid warm page decode missing arguments");
        return -EINVAL;
    }

    if (len < CXL_HYBRID_WARM_PAGE_HEADER_LEN) {
        error_setg(errp, "CXL hybrid warm page payload too short: %zu", len);
        return -EINVAL;
    }

    name_len = *p++;
    if ((size_t)(end - p) < name_len + 8 + 4) {
        error_setg(errp, "CXL hybrid warm page truncated in header");
        return -EINVAL;
    }

    decoded.ramblock = g_strndup((const char *)p, name_len);
    p += name_len;
    decoded.offset = ldq_be_p(p);
    p += 8;
    decoded.page_len = ldl_be_p(p);
    p += 4;

    if ((size_t)(end - p) != decoded.page_len) {
        error_setg(errp,
                   "CXL hybrid warm page length mismatch: payload=%zu expected=%" PRIu32,
                   (size_t)(end - p), decoded.page_len);
        goto fail;
    }

    decoded.data = g_memdup2(p, decoded.page_len);
    if (!cxl_hybrid_warm_page_valid(&decoded, errp)) {
        goto fail;
    }

    cxl_hybrid_warm_page_cleanup(page);
    *page = decoded;
    return 0;

fail:
    cxl_hybrid_warm_page_cleanup(&decoded);
    return -EINVAL;
}

int cxl_hybrid_warm_desc_decode(CXLHybridWarmDescriptor *desc,
                                const uint8_t *buf,
                                size_t len,
                                Error **errp)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    CXLHybridWarmDescriptor decoded = { 0 };
    uint8_t name_len;

    if (!desc || !buf) {
        error_setg(errp, "CXL hybrid warm descriptor decode missing arguments");
        return -EINVAL;
    }

    if (len < CXL_HYBRID_WARM_DESC_HEADER_LEN) {
        error_setg(errp, "CXL hybrid warm descriptor payload too short: %zu",
                   len);
        return -EINVAL;
    }

    name_len = *p++;
    if ((size_t)(end - p) < name_len + CXL_HYBRID_WARM_DESC_HEADER_LEN - 1) {
        error_setg(errp, "CXL hybrid warm descriptor truncated in header");
        return -EINVAL;
    }

    decoded.ramblock = g_strndup((const char *)p, name_len);
    p += name_len;
    decoded.guest_offset = ldq_be_p(p);
    p += 8;
    decoded.cxl_offset = ldq_be_p(p);
    p += 8;
    decoded.page_len = ldl_be_p(p);
    p += 4;
    decoded.flags = ldl_be_p(p);
    p += 4;
    decoded.generation = ldl_be_p(p);
    p += 4;

    if (p != end) {
        error_setg(errp,
                   "CXL hybrid warm descriptor payload has %zu trailing bytes",
                   (size_t)(end - p));
        goto fail;
    }

    if (!cxl_hybrid_warm_desc_valid(&decoded, errp)) {
        goto fail;
    }

    cxl_hybrid_warm_desc_cleanup(desc);
    *desc = decoded;
    return 0;

fail:
    cxl_hybrid_warm_desc_cleanup(&decoded);
    return -EINVAL;
}

int cxl_hybrid_warm_desc_batch_decode(CXLHybridWarmDescBatch *batch,
                                      const uint8_t *buf,
                                      size_t len,
                                      Error **errp)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    CXLHybridWarmDescBatch decoded = { 0 };
    uint32_t i;

    if (!batch || !buf) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch decode missing arguments");
        return -EINVAL;
    }

    if (len < CXL_HYBRID_WARM_DESC_BATCH_HEADER_LEN) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch payload too short: %zu",
                   len);
        return -EINVAL;
    }

    decoded.generation = ldl_be_p(p);
    p += 4;
    decoded.nr_entries = ldl_be_p(p);
    p += 4;

    if (decoded.nr_entries) {
        decoded.entries = g_new0(CXLHybridWarmDescRange, decoded.nr_entries);
    }

    for (i = 0; i < decoded.nr_entries; i++) {
        CXLHybridWarmDescRange *range = &decoded.entries[i];
        uint8_t name_len;

        if ((size_t)(end - p) < 1) {
            error_setg(errp,
                       "CXL hybrid warm descriptor batch truncated before entry %u name length",
                       i);
            goto fail;
        }

        name_len = *p++;
        if ((size_t)(end - p) <
            name_len + CXL_HYBRID_WARM_DESC_BATCH_ENTRY_HEADER_LEN - 1) {
            error_setg(errp,
                       "CXL hybrid warm descriptor batch truncated in entry %u",
                       i);
            goto fail;
        }

        range->ramblock = g_strndup((const char *)p, name_len);
        p += name_len;
        range->guest_offset = ldq_be_p(p);
        p += 8;
        range->cxl_offset = ldq_be_p(p);
        p += 8;
        range->page_len = ldl_be_p(p);
        p += 4;
        range->flags = ldl_be_p(p);
        p += 4;

        if (!cxl_hybrid_warm_desc_range_valid(range, errp)) {
            goto fail;
        }
    }

    if (p != end) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch payload has %zu trailing bytes",
                   (size_t)(end - p));
        goto fail;
    }

    cxl_hybrid_warm_desc_batch_cleanup(batch);
    *batch = decoded;
    return 0;

fail:
    cxl_hybrid_warm_desc_batch_cleanup(&decoded);
    return -EINVAL;
}

int cxl_hybrid_publish_notify_decode(CXLHybridPublishNotify *notify,
                                     const uint8_t *buf,
                                     size_t len,
                                     Error **errp)
{
    CXLHybridWarmDescriptor desc = { 0 };
    int ret;

    if (!notify) {
        error_setg(errp, "CXL hybrid publish notify decode missing output");
        return -EINVAL;
    }

    ret = cxl_hybrid_warm_desc_decode(&desc, buf, len, errp);
    if (ret) {
        return ret;
    }

    cxl_hybrid_publish_notify_cleanup(notify);
    *notify = (CXLHybridPublishNotify) {
        .ramblock = g_steal_pointer(&desc.ramblock),
        .guest_offset = desc.guest_offset,
        .cxl_offset = desc.cxl_offset,
        .page_len = desc.page_len,
        .generation = desc.generation,
    };
    cxl_hybrid_warm_desc_cleanup(&desc);
    return 0;
}

int cxl_hybrid_warm_page_store(const CXLHybridWarmPage *page, Error **errp)
{
    int ret;

    if (!cxl_hybrid_warm_page_valid(page, errp)) {
        return -EINVAL;
    }

    if (!cxl_hybrid_dst_staging_is_active()) {
        error_setg(errp, "CXL hybrid destination staging is not initialized");
        return -EINVAL;
    }

    ret = cxl_hybrid_dst_staging_store_page(page->ramblock, page->offset,
                                            page->data, page->page_len,
                                            errp);
    if (ret) {
        return ret;
    }

    trace_cxl_hybrid_warm_page_recv(page->ramblock, page->offset,
                                    page->page_len);
    return 0;
}

int cxl_hybrid_warm_desc_store(const CXLHybridWarmDescriptor *desc,
                               Error **errp)
{
    int ret;

    if (!cxl_hybrid_warm_desc_valid(desc, errp)) {
        return -EINVAL;
    }

    if (!cxl_hybrid_dst_staging_is_active()) {
        error_setg(errp, "CXL hybrid destination staging is not initialized");
        return -EINVAL;
    }

    ret = cxl_hybrid_dst_staging_register_external_page(desc->ramblock,
                                                        desc->guest_offset,
                                                        desc->cxl_offset,
                                                        desc->page_len,
                                                        errp);
    if (ret) {
        return ret;
    }

    trace_cxl_hybrid_warm_desc_recv(desc->ramblock, desc->guest_offset,
                                    desc->cxl_offset, desc->page_len);
    return 0;
}

int cxl_hybrid_warm_desc_batch_store(const CXLHybridWarmDescBatch *batch,
                                     Error **errp)
{
    uint32_t i;
    uint32_t pages = 0;
    bool changed = false;
    size_t encoded_len;
    int ret = 0;

    if (!batch) {
        error_setg(errp, "CXL hybrid warm descriptor batch missing batch");
        return -EINVAL;
    }

    ret = cxl_hybrid_warm_desc_batch_encoded_len(batch, &encoded_len, errp);
    if (ret) {
        return ret;
    }

    if (!cxl_hybrid_dst_staging_is_active()) {
        error_setg(errp, "CXL hybrid destination staging is not initialized");
        return -EINVAL;
    }

    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_lock(&cxl_dst_staging.lock);
    }

    for (i = 0; i < batch->nr_entries; i++) {
        const CXLHybridWarmDescRange *range = &batch->entries[i];

        ret = cxl_hybrid_dst_staging_register_external_range_locked(
            range->ramblock, range->guest_offset, range->cxl_offset,
            range->page_len, &changed, errp);
        if (ret) {
            goto out;
        }

        pages += range->page_len / CXL_HYBRID_STAGING_PAGE_SIZE;
        trace_cxl_hybrid_warm_desc_recv(range->ramblock, range->guest_offset,
                                        range->cxl_offset, range->page_len);
    }

    if (changed && cxl_dst_staging.sync_ready) {
        qemu_cond_broadcast(&cxl_dst_staging.cond);
    }
    trace_cxl_hybrid_warm_desc_batch_recv(batch->generation, batch->nr_entries,
                                          pages, encoded_len);

out:
    if (cxl_dst_staging.sync_ready) {
        qemu_mutex_unlock(&cxl_dst_staging.lock);
    }
    return ret;
}
