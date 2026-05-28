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
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/madvise.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-migration.h"
#include "channel.h"
#include "cxl.h"
#include "cxl-rdma.h"
#include "migration.h"
#include "postcopy.h"
#include "qemu-file.h"
#include "ram.h"
#include "savevm.h"
#include "io/channel-cxl.h"
#include "options.h"
#include "system/cpus.h"
#include "trace.h"

static struct CXLOutgoingArgs {
    char *devpath;
} outgoing_args;

static CXLMigrationStats *cxl_cleanup_snapshot;

typedef struct CXLIncomingMetadataState {
    CXLHybridMetadata last_meta;
    bool valid;
} CXLIncomingMetadataState;

typedef struct CXLHybridPublishedPageEntry {
    uint64_t cxl_offset;
    uint32_t generation;
    bool valid;
    bool ready;
    bool source_remapped;
} CXLHybridPublishedPageEntry;

typedef struct CXLHybridLastPublishRequestInfo {
    uint64_t count;
    uint64_t guest_offset;
    uint32_t generation;
    char *ramblock;
} CXLHybridLastPublishRequestInfo;

typedef struct CXLHybridLastPublishWaitInfo {
    uint64_t count;
    uint64_t guest_offset;
    uint64_t wait_time_ns;
    int ret;
    uint32_t generation;
    char *ramblock;
    bool has_wait_time_ns;
    bool has_ret;
} CXLHybridLastPublishWaitInfo;

typedef struct CXLHybridPrefaultSpan {
    uint64_t offset;
    uint64_t length;
} CXLHybridPrefaultSpan;

static CXLIncomingMetadataState cxl_incoming_meta_state;
static size_t cxl_global_page_index(RAMBlock *block, ram_addr_t block_offset);
static size_t cxl_source_remap_region_index(ram_addr_t global_offset);
static uint64_t cxl_total_guest_pages(void);
static uint64_t cxl_choose_fault_region_granule(uint64_t align,
                                               uint64_t total_ram);
static uint64_t cxl_choose_source_remap_granule(uint64_t total_ram);
static void cxl_hybrid_prefault_start(void);
static int cxl_destination_setup_init(Error **errp);
static int cxl_destination_backing_open(QIOChannelCXL **ciocp, Error **errp);
static int cxl_destination_control_and_region_init(QIOChannelCXL *cioc,
                                                   Error **errp);
static int cxl_destination_copy_staging_init(QIOChannelCXL *cioc,
                                             Error **errp);
static void cxl_destination_setup_cleanup(void);
static int cxl_destination_region_state_init(int fd, uint64_t align,
                                             uint64_t map_size,
                                             Error **errp);
static void cxl_destination_region_state_cleanup(void);
static bool cxl_hybrid_page_eligible(size_t page_idx);
static uint64_t cxl_hybrid_prefetch_rate_limit(void);
static void cxl_hybrid_warm_sleep(uint32_t pages_sent);
static bool cxl_hybrid_warm_disabled(void);
static void cxl_hybrid_invalidate_published_page(const char *ramblock,
                                                 uint64_t guest_offset);
static bool cxl_hybrid_range_is_remapped(RAMBlock *block,
                                         uint64_t guest_offset,
                                         uint32_t page_len);
static void cxl_hybrid_ensure_publish_mutex(void);
static bool cxl_lookup_source_remap_region(size_t region_idx,
                                           RAMBlock **blockp,
                                           ram_addr_t *block_offsetp);
static void cxl_hybrid_ctrl_publish_pages_visible(size_t first_page,
                                                  size_t npages,
                                                  uint32_t generation);
static bool cxl_hybrid_region_page_span(size_t region_idx,
                                        size_t *first_pagep,
                                        size_t *npagesp);
static bool cxl_hybrid_region_index_from_block_offset(RAMBlock *block,
                                                      ram_addr_t block_offset,
                                                      size_t *region_idxp);
static void cxl_try_remap_region(size_t region_idx);

#define CXL_HYBRID_FAULT_BURST_PAGES 4

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
    uint64_t source_remap_granule;
    CXLHybridPhase phase;
    CXLMigrationSwitchReason switch_reason;
    QemuMutex sender_sync_mutex;
    QemuCond sender_sync_cond;
    unsigned long *migrated_bmap;
    unsigned long *warm_sent_bmap;
    unsigned long *dst_sent_bmap;
    unsigned long *cxl_visible_bmap;
    unsigned long *remaining_bmap;
    unsigned long *warm_dirty_bmap;
    unsigned long *pending_remap_bmap;
    unsigned long *clean_epoch_seen_bmap;
    unsigned long *clean_candidate_bmap;
    unsigned long *clean_inflight_bmap;
    unsigned long *remapped_bmap;
    unsigned long *remapped_pages_bmap;
    size_t total_pages;
    size_t total_regions;
    size_t source_remap_regions;
    uint64_t staged_pages;
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
    uint64_t backing_rate_sleep_count;
    uint64_t backing_rate_sleep_time_ns;
    QemuMutex backing_rate_mutex;
    bool backing_rate_mutex_initialized;
    uint64_t backing_rate_window_start_ns;
    uint64_t backing_rate_window_bytes;
    uint64_t migrated_bitmap_time_ns;
    uint64_t remap_scan_calls;
    uint64_t remap_scan_time_ns;
    uint64_t pending_remap_regions;
    uint64_t clean_pending_remap_regions;
    uint64_t pending_remap_unmigrated_pages;
    uint64_t pending_remap_dirty_pages;
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
    uint64_t clean_remap_scan_calls;
    uint64_t clean_remap_candidate_regions;
    uint64_t clean_remap_copy_bytes;
    uint64_t clean_remap_copy_time_ns;
    uint64_t clean_remap_abandoned_dirty;
    uint64_t clean_remap_budget_exhaustions;
    uint64_t clean_remap_prefault_bytes;
    uint64_t clean_remap_prefault_time_ns;
    uint64_t clean_remap_prefault_errors;
    QemuThread prefault_thread;
    bool prefault_thread_created;
    bool prefault_done;
    bool prefault_failed;
    CXLHybridPrefaultSpan *prefault_spans;
    size_t prefault_span_count;
    uint64_t prefault_bytes;
    uint64_t prefault_time_ns;
    uint64_t prefault_wait_time_ns;
    uint64_t prefault_errors;
    uint64_t phase_transitions;
    /* Fault-control generation stays stable after postcopy run setup. */
    uint32_t source_run_generation;
    uint32_t source_run_generation_valid;
    uint64_t switch_iteration;
    uint64_t source_heat_updates;
    uint64_t source_warm_queue_pages;
    uint64_t source_warm_sent_pages;
    uint64_t source_warm_sent_bytes;
    uint64_t source_warm_skip_received;
    uint64_t source_warm_skip_unstaged;
    uint64_t source_warm_last_miss_offset;
    uint64_t warm_publish_pages;
    uint64_t warm_push_publish_pages;
    uint64_t fault_primary_publish_pages;
    uint64_t fault_burst_publish_pages;
    uint64_t completion_publish_pages;
    uint64_t fault_publish_requests;
    uint64_t fault_publish_waits;
    uint64_t fault_publish_wait_time_ns;
    uint64_t fault_publish_primary_samples;
    uint64_t fault_publish_primary_time_ns;
    uint64_t max_fault_publish_primary_time_ns;
    uint64_t fault_publish_burst_samples;
    uint64_t fault_publish_burst_time_ns;
    uint64_t max_fault_publish_burst_time_ns;
    uint64_t fault_publish_req_recv_samples;
    uint64_t fault_publish_req_recv_time_ns;
    uint64_t max_fault_publish_req_recv_time_ns;
    uint64_t fault_publish_req_handle_samples;
    uint64_t fault_publish_req_handle_time_ns;
    uint64_t max_fault_publish_req_handle_time_ns;
    uint64_t region_publish_requests;
    uint64_t region_publish_pages;
    uint64_t region_publish_time_ns;
    uint64_t publish_memcpy_bytes;
    uint64_t publish_memcpy_time_ns;
    uint64_t stream_publish_ranges;
    uint64_t stream_publish_pages;
    uint64_t stream_publish_time_ns;
    uint64_t stream_publish_local_bitmap_time_ns;
    uint64_t stream_publish_shared_visible_time_ns;
    uint64_t stream_write_calls;
    uint64_t stream_write_bytes;
    uint64_t stream_write_max_bytes;
    uint64_t stream_write_time_ns;
    uint64_t stream_fault_pause_calls;
    uint64_t stream_fault_pause_time_ns;
    uint64_t stream_fault_pause_max_time_ns;
    uint64_t dst_region_map_attempts;
    uint64_t dst_region_map_successes;
    uint64_t dst_region_map_failures;
    uint64_t dst_region_map_time_ns;
    uint64_t dst_region_wake_failures;
    uint64_t dst_region_wait_samples;
    uint64_t dst_region_wait_time_ns;
    uint64_t max_dst_region_wait_time_ns;
    uint64_t dst_region_fallback_copies;
    CXLHybridLastPublishRequestInfo last_publish_request;
    CXLHybridLastPublishWaitInfo last_publish_wait_begin;
    CXLHybridLastPublishWaitInfo last_publish_wait_complete;
    uint64_t publish_copied_pages;
    uint64_t publish_copied_bytes;
    uint64_t publish_skip_ready;
    uint64_t publish_failures;
    struct {
        uint64_t ram_pages;
        uint64_t staged_pages;
        uint64_t warm_push_publish_pages;
        uint64_t fault_primary_publish_pages;
        uint64_t fault_burst_publish_pages;
        uint64_t completion_publish_pages;
        CXLHybridPhase phase;
    } iter_begin;
    uint64_t last_iterate_ram_pages;
    uint64_t last_iterate_staged_pages_delta;
    uint64_t last_iterate_warm_push_pages;
    uint64_t last_iterate_fault_primary_pages;
    uint64_t last_iterate_fault_burst_pages;
    CXLHybridPhase last_iterate_phase;
    size_t warm_scan_cursor;
    ram_addr_t warm_last_miss_offset;
    char *warm_last_miss_ramblock;
    CXLHybridDstRegionState dst_region_state;
    CXLHybridPublishedPageEntry *published_page_state;
    QemuMutex publish_mutex;
    bool publish_mutex_ready;
    bool sender_sync_ready;
    bool sender_shutdown;
    bool remap_quiescing;
    bool hybrid_enabled;
    bool active;
} cxl_state = {
    .fd = -1,
    .phase = CXL_HYBRID_PHASE_DISABLED,
    .switch_reason = CXL_MIGRATION_SWITCH_REASON_NONE,
};

static void cxl_remap_state_init(int fd, uint64_t align, int64_t dev_size);
static void cxl_process_pending_remaps(void);

static void cxl_clean_remap_init_remaining_bmap(void)
{
    RAMBlock *block;
    uint64_t region_len = cxl_state.source_remap_granule;
    size_t npages;

    if (!migrate_cxl_clean_remap_enable() || !cxl_state.remaining_bmap ||
        !region_len || !QEMU_IS_ALIGNED(region_len, TARGET_PAGE_SIZE)) {
        return;
    }

    npages = region_len >> TARGET_PAGE_BITS;
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_NOT_IGNORED(block) {
        ram_addr_t offset;

        for (offset = 0;
             offset + region_len <= block->used_length;
             offset += region_len) {
            size_t first_page = cxl_global_page_index(block, offset);

            if (first_page >= cxl_state.total_pages) {
                continue;
            }
            bitmap_set(cxl_state.remaining_bmap, first_page,
                       MIN(npages, cxl_state.total_pages - first_page));
        }
    }
}

#define CXL_BACKING_ALIGN_FALLBACK (2 * 1024 * 1024)
#define CXL_ROLLBACK_COPY_CHUNK (2 * 1024 * 1024)
#define CXL_WRITE_REDIRECT_ENV "QEMU_CXL_WRITE_REDIRECT"
#define CXL_HYBRID_WARM_DISABLE_ENV "QEMU_CXL_HYBRID_WARM_DISABLE"
#define CXL_HYBRID_BRAKE_FIRST_ENV "QEMU_CXL_HYBRID_BRAKE_FIRST"
/*
 * migration/ram.c currently aligns mapped-ram page offsets to 1MiB (or higher)
 * and places a per-RAMBlock bitmap ahead of the page data. If we mmap the CXL
 * backing for write-redirect, we must cover those bitmap+alignment gaps too.
 */
#define CXL_MAPPED_RAM_FILE_OFFSET_ALIGNMENT (0x100000ULL)
#define CXL_HYBRID_METADATA_FLAG_SOURCE_REMAPPED BIT(0)

static inline uint64_t cxl_now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static void cxl_hybrid_record_timing(uint64_t *samplesp,
                                     uint64_t *timep,
                                     uint64_t *maxp,
                                     uint64_t elapsed_ns)
{
    uint64_t observed_max;

    qatomic_inc(samplesp);
    qatomic_add(timep, elapsed_ns);

    observed_max = qatomic_read(maxp);
    while (elapsed_ns > observed_max) {
        uint64_t actual_max;

        actual_max = qatomic_cmpxchg(maxp, observed_max, elapsed_ns);
        if (actual_max == observed_max) {
            break;
        }
        observed_max = actual_max;
    }
}

static void cxl_hybrid_ensure_publish_mutex(void)
{
    if (cxl_state.publish_mutex_ready) {
        return;
    }

    qemu_mutex_init(&cxl_state.publish_mutex);
    cxl_state.publish_mutex_ready = true;
}

static void cxl_backing_rate_limit(size_t bytes)
{
    uint64_t rate = migrate_cxl_backing_rate();
    uint64_t now_ns;
    uint64_t elapsed_ns;
    uint64_t allowed_bytes;
    uint64_t sleep_ns;
    uint64_t sleep_start_ns;

    if (!rate || !bytes) {
        return;
    }

    if (!cxl_state.backing_rate_mutex_initialized) {
        qemu_mutex_init(&cxl_state.backing_rate_mutex);
        cxl_state.backing_rate_mutex_initialized = true;
    }

    qemu_mutex_lock(&cxl_state.backing_rate_mutex);
    now_ns = cxl_now_ns();
    if (!cxl_state.backing_rate_window_start_ns) {
        cxl_state.backing_rate_window_start_ns = now_ns;
        cxl_state.backing_rate_window_bytes = 0;
    }

    elapsed_ns = now_ns - cxl_state.backing_rate_window_start_ns;
    if (elapsed_ns >= NANOSECONDS_PER_SECOND) {
        cxl_state.backing_rate_window_start_ns = now_ns;
        cxl_state.backing_rate_window_bytes = 0;
        elapsed_ns = 0;
    }
    allowed_bytes = (rate * elapsed_ns) / NANOSECONDS_PER_SECOND;
    if (cxl_state.backing_rate_window_bytes + bytes > allowed_bytes) {
        uint64_t target_bytes = cxl_state.backing_rate_window_bytes + bytes;
        uint64_t target_elapsed_ns =
            DIV_ROUND_UP(target_bytes * NANOSECONDS_PER_SECOND, rate);

        sleep_ns = target_elapsed_ns - elapsed_ns;
        qatomic_inc(&cxl_state.backing_rate_sleep_count);
        qemu_mutex_unlock(&cxl_state.backing_rate_mutex);

        sleep_start_ns = cxl_now_ns();
        g_usleep(DIV_ROUND_UP(sleep_ns, 1000));
        qatomic_add(&cxl_state.backing_rate_sleep_time_ns,
                    cxl_now_ns() - sleep_start_ns);

        qemu_mutex_lock(&cxl_state.backing_rate_mutex);
    }

    cxl_state.backing_rate_window_bytes += bytes;
    qemu_mutex_unlock(&cxl_state.backing_rate_mutex);
}

static void cxl_hybrid_publish_transition_lock(void)
{
    cxl_hybrid_ensure_publish_mutex();
    qemu_mutex_lock(&cxl_state.publish_mutex);
}

static void cxl_hybrid_publish_transition_unlock(void)
{
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
}

static void cxl_hybrid_set_last_publish_request(
    CXLHybridLastPublishRequestInfo *info,
    const char *ramblock,
    uint64_t guest_offset,
    uint32_t generation,
    uint64_t count)
{
    if (!info) {
        return;
    }

    g_free(info->ramblock);
    info->ramblock = g_strdup(ramblock);
    info->guest_offset = guest_offset;
    info->generation = generation;
    info->count = count;
}

static void cxl_hybrid_set_last_publish_wait(
    CXLHybridLastPublishWaitInfo *info,
    const char *ramblock,
    uint64_t guest_offset,
    uint32_t generation,
    uint64_t count,
    bool has_wait_time_ns,
    uint64_t wait_time_ns,
    bool has_ret,
    int ret)
{
    if (!info) {
        return;
    }

    g_free(info->ramblock);
    info->ramblock = g_strdup(ramblock);
    info->guest_offset = guest_offset;
    info->generation = generation;
    info->count = count;
    info->has_wait_time_ns = has_wait_time_ns;
    info->wait_time_ns = wait_time_ns;
    info->has_ret = has_ret;
    info->ret = ret;
}

static uint64_t cxl_hybrid_current_staged_pages(void)
{
    return qatomic_read(&cxl_state.staged_pages);
}

static bool cxl_hybrid_test_and_set_bit_atomic(size_t nr, unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = addr + BIT_WORD(nr);

    return (qatomic_fetch_or(p, mask) & mask) != 0;
}

static void cxl_hybrid_ctrl_publish_pages_visible(size_t first_page,
                                                  size_t npages,
                                                  uint32_t generation)
{
    size_t pages_per_region;
    size_t end_page;
    size_t page;

    cxl_hybrid_ctrl_set_pages_visible(first_page, npages, generation);

    if (!npages || !cxl_state.remap_granule ||
        !QEMU_IS_ALIGNED(cxl_state.remap_granule, TARGET_PAGE_SIZE)) {
        return;
    }

    pages_per_region = cxl_state.remap_granule >> TARGET_PAGE_BITS;
    if (!pages_per_region) {
        return;
    }

    end_page = MIN(first_page + npages, cxl_state.total_pages);
    for (page = first_page; page < end_page; ) {
        size_t region_first = QEMU_ALIGN_DOWN(page, pages_per_region);
        size_t region_pages = MIN(pages_per_region,
                                  cxl_state.total_pages - region_first);

        cxl_hybrid_ctrl_synthesize_region_visible(region_first, region_pages,
                                                  generation);
        page = region_first + region_pages;
    }
}

static bool cxl_hybrid_region_page_span(size_t region_idx,
                                        size_t *first_pagep,
                                        size_t *npagesp)
{
    uint64_t first_page;
    uint64_t pages_per_region;

    if (!first_pagep || !npagesp ||
        !cxl_state.remap_granule ||
        !QEMU_IS_ALIGNED(cxl_state.remap_granule, TARGET_PAGE_SIZE)) {
        return false;
    }

    pages_per_region = cxl_state.remap_granule >> TARGET_PAGE_BITS;
    first_page = (uint64_t)region_idx * pages_per_region;
    if (!pages_per_region || first_page >= cxl_state.total_pages) {
        return false;
    }

    *first_pagep = first_page;
    *npagesp = MIN(pages_per_region, cxl_state.total_pages - first_page);
    return *npagesp != 0;
}

static bool cxl_hybrid_region_index_from_block_offset(RAMBlock *block,
                                                      ram_addr_t block_offset,
                                                      size_t *region_idxp)
{
    ram_addr_t global_offset;

    if (!region_idxp || !cxl_state.remap_granule ||
        !QEMU_IS_ALIGNED(cxl_state.remap_granule, TARGET_PAGE_SIZE) ||
        !cxl_hybrid_global_page_offset(block, block_offset, TARGET_PAGE_SIZE,
                                       &global_offset)) {
        return false;
    }

    *region_idxp = global_offset / cxl_state.remap_granule;
    return *region_idxp < cxl_state.total_regions;
}

bool cxl_hybrid_commit_rdma_ready_region(uint64_t region_index,
                                         uint32_t generation)
{
    size_t first_page;
    size_t npages;
    size_t page;

    if (!migrate_cxl_rdma_sidecar()) {
        return false;
    }

    if (!cxl_hybrid_region_page_span(region_index, &first_page, &npages)) {
        return false;
    }
    if (!cxl_hybrid_region_commit_rdma_ready(region_index)) {
        return false;
    }

    for (page = 0; page < npages; page++) {
        size_t page_idx = first_page + page;

        if (cxl_state.cxl_visible_bmap) {
            set_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);
        }
        if (cxl_state.remaining_bmap) {
            clear_bit_atomic(page_idx, cxl_state.remaining_bmap);
        }
    }
    cxl_hybrid_ctrl_publish_pages_visible(first_page, npages, generation);
    return true;
}

void cxl_hybrid_commit_rdma_ready_regions_for_postcopy(void)
{
    uint32_t generation = cxl_hybrid_fault_publish_generation();
    uint64_t region_index;

    if (!migrate_cxl_rdma_sidecar()) {
        return;
    }

    for (region_index = 0; region_index < cxl_state.total_regions;
         region_index++) {
        cxl_hybrid_commit_rdma_ready_region(region_index, generation);
    }
}

static void cxl_hybrid_mark_page_staged(size_t page_idx)
{
    if (!cxl_state.migrated_bmap ||
        page_idx >= cxl_state.total_pages ||
        cxl_hybrid_test_and_set_bit_atomic(page_idx, cxl_state.migrated_bmap)) {
        return;
    }

    qatomic_inc(&cxl_state.staged_pages);
}

static void cxl_hybrid_record_publish_source(CXLHybridPublishSource source,
                                             uint64_t npages)
{
    switch (source) {
    case CXL_HYBRID_PUBLISH_SOURCE_WARM_PUSH:
        qatomic_add(&cxl_state.warm_push_publish_pages, npages);
        break;
    case CXL_HYBRID_PUBLISH_SOURCE_FAULT_PRIMARY:
        qatomic_add(&cxl_state.fault_primary_publish_pages, npages);
        break;
    case CXL_HYBRID_PUBLISH_SOURCE_FAULT_BURST:
        qatomic_add(&cxl_state.fault_burst_publish_pages, npages);
        break;
    case CXL_HYBRID_PUBLISH_SOURCE_COMPLETION:
        qatomic_add(&cxl_state.completion_publish_pages, npages);
        break;
    case CXL_HYBRID_PUBLISH_SOURCE_UNSPECIFIED:
    default:
        break;
    }
}

void cxl_hybrid_iteration_snapshot_begin(uint64_t ram_pages)
{
    cxl_state.iter_begin.ram_pages = ram_pages;
    cxl_state.iter_begin.staged_pages = cxl_hybrid_current_staged_pages();
    cxl_state.iter_begin.warm_push_publish_pages =
        qatomic_read(&cxl_state.warm_push_publish_pages);
    cxl_state.iter_begin.fault_primary_publish_pages =
        qatomic_read(&cxl_state.fault_primary_publish_pages);
    cxl_state.iter_begin.fault_burst_publish_pages =
        qatomic_read(&cxl_state.fault_burst_publish_pages);
    cxl_state.iter_begin.completion_publish_pages =
        qatomic_read(&cxl_state.completion_publish_pages);
    cxl_state.iter_begin.phase = cxl_state.phase;
}

void cxl_hybrid_iteration_snapshot_end(uint64_t ram_pages)
{
    uint64_t staged_pages = cxl_hybrid_current_staged_pages();
    uint64_t warm_push = qatomic_read(&cxl_state.warm_push_publish_pages);
    uint64_t fault_primary = qatomic_read(&cxl_state.fault_primary_publish_pages);
    uint64_t fault_burst = qatomic_read(&cxl_state.fault_burst_publish_pages);
    uint64_t completion = qatomic_read(&cxl_state.completion_publish_pages);

    qatomic_set(&cxl_state.last_iterate_ram_pages,
                ram_pages - cxl_state.iter_begin.ram_pages);
    qatomic_set(&cxl_state.last_iterate_staged_pages_delta,
                staged_pages - cxl_state.iter_begin.staged_pages);
    qatomic_set(&cxl_state.last_iterate_warm_push_pages,
                warm_push - cxl_state.iter_begin.warm_push_publish_pages);
    qatomic_set(&cxl_state.last_iterate_fault_primary_pages,
                fault_primary - cxl_state.iter_begin.fault_primary_publish_pages);
    qatomic_set(&cxl_state.last_iterate_fault_burst_pages,
                fault_burst - cxl_state.iter_begin.fault_burst_publish_pages);
    qatomic_set(&cxl_state.last_iterate_phase, cxl_state.iter_begin.phase);

    trace_cxl_hybrid_iteration_profile(
        qatomic_read(&cxl_state.last_iterate_phase),
        qatomic_read(&cxl_state.last_iterate_ram_pages),
        qatomic_read(&cxl_state.last_iterate_staged_pages_delta),
        staged_pages,
        cxl_state.total_pages,
        qatomic_read(&cxl_state.last_iterate_warm_push_pages),
        qatomic_read(&cxl_state.last_iterate_fault_primary_pages),
        qatomic_read(&cxl_state.last_iterate_fault_burst_pages));
    if (trace_event_get_state(TRACE_CXL_HYBRID_ITERATION_PROFILE_TS)) {
        trace_cxl_hybrid_iteration_profile_ts(
            cxl_now_ns(),
            qatomic_read(&cxl_state.last_iterate_phase),
            qatomic_read(&cxl_state.last_iterate_ram_pages),
            qatomic_read(&cxl_state.last_iterate_staged_pages_delta),
            staged_pages,
            cxl_state.total_pages,
            qatomic_read(&cxl_state.last_iterate_warm_push_pages),
            qatomic_read(&cxl_state.last_iterate_fault_primary_pages),
            qatomic_read(&cxl_state.last_iterate_fault_burst_pages));
    }

    /*
     * Keep completion-originated republishes in their own cumulative counter.
     * They are not reported as a distinct per-iteration field yet because the
     * user-facing question is about warm push and fault-triggered traffic.
     */
    (void)(completion - cxl_state.iter_begin.completion_publish_pages);
}

static CXLMigrationPublishRequestInfo *cxl_hybrid_export_last_publish_request(
    const CXLHybridLastPublishRequestInfo *src)
{
    CXLMigrationPublishRequestInfo *dst;

    if (!src || !src->ramblock || !src->count) {
        return NULL;
    }

    dst = g_new0(CXLMigrationPublishRequestInfo, 1);
    dst->count = src->count;
    dst->ramblock = g_strdup(src->ramblock);
    dst->guest_offset = src->guest_offset;
    dst->generation = src->generation;
    return dst;
}

static CXLMigrationPublishWaitInfo *cxl_hybrid_export_last_publish_wait(
    const CXLHybridLastPublishWaitInfo *src)
{
    CXLMigrationPublishWaitInfo *dst;

    if (!src || !src->ramblock || !src->count) {
        return NULL;
    }

    dst = g_new0(CXLMigrationPublishWaitInfo, 1);
    dst->count = src->count;
    dst->ramblock = g_strdup(src->ramblock);
    dst->guest_offset = src->guest_offset;
    dst->generation = src->generation;
    dst->has_wait_time_ns = src->has_wait_time_ns;
    dst->wait_time_ns = src->wait_time_ns;
    dst->has_ret = src->has_ret;
    dst->ret = src->ret;
    return dst;
}

static void cxl_hybrid_clear_cleanup_snapshot(void)
{
    qapi_free_CXLMigrationStats(cxl_cleanup_snapshot);
    cxl_cleanup_snapshot = NULL;
}

static void cxl_hybrid_latch_cleanup_snapshot(void)
{
    g_autoptr(MigrationInfo) info = g_new0(MigrationInfo, 1);

    cxl_hybrid_clear_cleanup_snapshot();
    cxl_populate_migration_info(info);
    cxl_cleanup_snapshot = info->x_cxl;
    info->x_cxl = NULL;
}

static void cxl_hybrid_metadata_append_entry(CXLHybridMetadata *meta,
                                             RAMBlock *block,
                                             uint64_t offset,
                                             uint64_t length)
{
    CXLHybridMetadataEntry *entry;

    meta->entries = g_renew(CXLHybridMetadataEntry, meta->entries,
                            meta->nr_entries + 1);
    entry = &meta->entries[meta->nr_entries++];
    entry->ramblock = g_strdup(block->idstr);
    entry->offset = offset;
    entry->length = length;
    entry->flags = CXL_HYBRID_METADATA_FLAG_SOURCE_REMAPPED;
    entry->heat = 0;
}

static void cxl_hybrid_metadata_snapshot_page_bitmap(CXLHybridMetadata *meta,
                                                     RAMBlock *block,
                                                     const unsigned long *bitmap)
{
    size_t block_first_page;
    size_t block_pages;
    size_t block_last_page;
    size_t page_idx;

    if (!meta || !block || !bitmap) {
        return;
    }

    block_pages = DIV_ROUND_UP(block->used_length, TARGET_PAGE_SIZE);
    if (block_pages == 0) {
        return;
    }

    block_first_page = block->offset >> TARGET_PAGE_BITS;
    block_last_page = MIN(block_first_page + block_pages, cxl_state.total_pages);
    page_idx = find_next_bit(bitmap, block_last_page, block_first_page);

    while (page_idx < block_last_page) {
        size_t run_end = find_next_zero_bit(bitmap, block_last_page, page_idx + 1);
        size_t local_first_page = page_idx - block_first_page;
        uint64_t run_offset = (uint64_t)local_first_page << TARGET_PAGE_BITS;
        uint64_t run_length = (uint64_t)(run_end - page_idx) << TARGET_PAGE_BITS;

        cxl_hybrid_metadata_append_entry(meta, block, run_offset, run_length);
        page_idx = find_next_bit(bitmap, block_last_page, run_end);
    }
}

int cxl_hybrid_metadata_snapshot_source(CXLHybridMetadata *meta, Error **errp)
{
    CXLHybridMetadata snapshot = {
        .version = CXL_HYBRID_METADATA_VERSION,
        .generation = cxl_hybrid_fault_publish_generation(),
    };
    RAMBlock *block;

    if (!meta) {
        error_setg(errp, "CXL hybrid metadata snapshot missing output");
        return -EINVAL;
    }

    if (!cxl_state.hybrid_enabled) {
        cxl_hybrid_metadata_cleanup(meta);
        *meta = snapshot;
        return 0;
    }

    if (cxl_state.remap_granule == 0) {
        error_setg(errp, "CXL hybrid metadata snapshot missing remap granule");
        return -EINVAL;
    }

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_NOT_IGNORED(block) {
        cxl_hybrid_metadata_snapshot_page_bitmap(&snapshot, block,
                                                 cxl_state.migrated_bmap);
    }

    cxl_hybrid_metadata_cleanup(meta);
    *meta = snapshot;
    return 0;
}

int cxl_hybrid_metadata_send(QEMUFile *f, Error **errp)
{
    CXLHybridMetadata meta = { 0 };
    g_autofree uint8_t *buf = NULL;
    size_t len;
    int ret;

    ret = cxl_hybrid_metadata_snapshot_source(&meta, errp);
    if (ret) {
        return ret;
    }

    ret = cxl_hybrid_metadata_encoded_len(&meta, &len, errp);
    if (ret) {
        goto out;
    }

    if (len > UINT16_MAX) {
        error_setg(errp, "CXL hybrid metadata command too large: %zu", len);
        ret = -E2BIG;
        goto out;
    }

    buf = g_malloc(len);
    ret = cxl_hybrid_metadata_encode(&meta, buf, len, errp);
    if (ret) {
        goto out;
    }

    trace_cxl_hybrid_metadata_send(meta.generation, meta.nr_entries, len);
    qemu_savevm_send_cxl_hybrid_metadata(f, meta.generation,
                                         meta.nr_entries, buf, len);
    if (qemu_file_get_error(f)) {
        error_setg(errp, "Failed to send CXL hybrid metadata command");
        ret = -EIO;
        goto out;
    }

    ret = 0;
out:
    cxl_hybrid_metadata_cleanup(&meta);
    return ret;
}

int cxl_hybrid_metadata_recv(const uint8_t *buf, size_t len, Error **errp)
{
    CXLHybridMetadata meta = { 0 };
    int ret;

    ret = cxl_hybrid_metadata_decode(&meta, buf, len, errp);
    if (ret) {
        return ret;
    }

    cxl_hybrid_metadata_cleanup(&cxl_incoming_meta_state.last_meta);
    cxl_incoming_meta_state.last_meta = meta;
    cxl_incoming_meta_state.valid = true;
    if (cxl_hybrid_dst_staging_is_active()) {
        ret = cxl_hybrid_dst_staging_apply_metadata(&meta, errp);
        if (ret) {
            cxl_hybrid_metadata_cleanup(&cxl_incoming_meta_state.last_meta);
            cxl_incoming_meta_state.valid = false;
            return ret;
        }
    }
    ret = cxl_hybrid_control_activate_destination(errp);
    if (ret) {
        cxl_hybrid_metadata_cleanup(&cxl_incoming_meta_state.last_meta);
        cxl_incoming_meta_state.valid = false;
        return ret;
    }
    trace_cxl_hybrid_metadata_recv(meta.generation, meta.nr_entries, len);
    return 0;
}

void cxl_hybrid_metadata_cleanup_incoming(void)
{
    cxl_hybrid_metadata_cleanup(&cxl_incoming_meta_state.last_meta);
    cxl_incoming_meta_state.valid = false;
    cxl_destination_setup_cleanup();
}

int cxl_hybrid_try_resolve_fault(MigrationIncomingState *mis, RAMBlock *rb,
                                 ram_addr_t offset,
                                 int (*place_page)(MigrationIncomingState *mis,
                                                   void *host, void *from,
                                                   RAMBlock *rb),
                                 Error **errp)
{
    g_autofree uint8_t *page = NULL;
    const char *ramblock;
    void *host;
    size_t pagesize;
    uint64_t read_start_ns;
    uint64_t read_time_ns;
    uint64_t place_start_ns;
    uint64_t place_time_ns;
    int ret;

    if (!rb || !place_page) {
        error_setg(errp, "CXL hybrid fault resolve missing arguments");
        return -EINVAL;
    }

    ramblock = qemu_ram_get_idstr(rb);
    if (!cxl_hybrid_dst_staging_is_active()) {
        return 0;
    }

    pagesize = qemu_ram_pagesize(rb);
    if (!offset_in_ramblock(rb, offset) || pagesize > rb->used_length - offset) {
        error_setg(errp,
                   "CXL hybrid fault offset 0x%lx outside ramblock %s",
                   (unsigned long)offset, ramblock);
        return -EINVAL;
    }

    if (!cxl_hybrid_dst_staging_range_present(ramblock, offset, pagesize)) {
        cxl_hybrid_dst_staging_account_fault_miss();
        trace_cxl_hybrid_fault_miss(ramblock, offset);
        return 0;
    }

    page = g_malloc(pagesize);
    read_start_ns = cxl_now_ns();
    ret = cxl_hybrid_dst_staging_read_page(ramblock, offset, page, pagesize,
                                           errp);
    if (ret) {
        return ret;
    }
    read_time_ns = cxl_now_ns() - read_start_ns;
    cxl_hybrid_dst_staging_account_fault_hit(pagesize, read_time_ns);
    trace_cxl_hybrid_fault_hit(ramblock, offset, pagesize, read_time_ns);

    host = ramblock_ptr(rb, offset);
    place_start_ns = cxl_now_ns();
    ret = place_page(mis, host, page, rb);
    place_time_ns = cxl_now_ns() - place_start_ns;
    trace_cxl_hybrid_fault_place(ramblock, offset, place_time_ns, ret);
    if (ret) {
        cxl_hybrid_dst_staging_account_fault_place_result(false);
        error_setg(errp, "CXL hybrid fault placement failed: %d", ret);
        return ret;
    }

    cxl_hybrid_dst_staging_account_fault_place_result(true);
    return 1;
}

uint32_t cxl_hybrid_fault_publish_generation(void)
{
    return cxl_hybrid_select_fault_publish_generation(
        cxl_incoming_meta_state.valid,
        cxl_incoming_meta_state.last_meta.generation,
        qatomic_read(&cxl_state.source_run_generation_valid) != 0,
        qatomic_read(&cxl_state.source_run_generation),
        qatomic_read(&cxl_state.phase_transitions));
}

uint32_t cxl_hybrid_fault_publish_generation_begin_source_run(void)
{
    uint32_t generation = (uint32_t)qatomic_read(&cxl_state.phase_transitions);

    qatomic_set(&cxl_state.source_run_generation, generation);
    qatomic_set(&cxl_state.source_run_generation_valid, 1);
    return generation;
}

void cxl_hybrid_fault_publish_generation_end_source_run(void)
{
    qatomic_set(&cxl_state.source_run_generation_valid, 0);
    qatomic_set(&cxl_state.source_run_generation, 0);
}

static bool cxl_hybrid_mark_region_fault_requested(MigrationIncomingState *mis,
                                                   RAMBlock *rb,
                                                   ram_addr_t offset,
                                                   void *fault_host,
                                                   uint64_t haddr,
                                                   uint32_t tid)
{
    WITH_QEMU_LOCK_GUARD(&mis->page_request_mutex) {
        if (ramblock_recv_bitmap_test_byte_offset(rb, offset)) {
            return true;
        }
        if (!g_tree_lookup(mis->page_requested, fault_host)) {
            g_tree_insert(mis->page_requested, fault_host, (gpointer)1);
            qatomic_inc(&mis->page_requested_count);
            trace_postcopy_page_req_add(fault_host,
                                        mis->page_requested_count);
            if (trace_event_get_state(TRACE_POSTCOPY_PAGE_REQ_ADD_TS)) {
                trace_postcopy_page_req_add_ts(cxl_now_ns(), fault_host,
                                               mis->page_requested_count);
            }
        }
        mark_postcopy_blocktime_begin(haddr, tid, rb);
    }

    return false;
}

static int cxl_hybrid_dst_remap_region(MigrationIncomingState *mis,
                                       RAMBlock *rb,
                                       const CXLHybridFaultRegionGeometry *g,
                                       void *fault_host_addr,
                                       Error **errp)
{
    void *host_addr;
    void *ret;
    uint64_t start_ns;
    uint64_t mmap_ns;
    uint64_t wake_start_ns;
    uint64_t wake_ns;
    uint32_t generation;
    bool received = false;
    int rc;

    if (!mis || !rb || !g) {
        error_setg(errp, "CXL hybrid destination remap missing arguments");
        return -EINVAL;
    }
    if (cxl_state.fd < 0) {
        error_setg(errp, "CXL hybrid destination remap missing CXL fd");
        return -ENODEV;
    }
    while (!cxl_hybrid_dst_region_try_begin_remap(&cxl_state.dst_region_state,
                                                  g->region_index)) {
        if (cxl_hybrid_dst_region_remapped(&cxl_state.dst_region_state,
                                           g->region_index)) {
            return 1;
        }
        if (cxl_hybrid_dst_region_copy_owned(&cxl_state.dst_region_state,
                                             g->region_index)) {
            if (migrate_cxl_fault_resolve_region_remap()) {
                error_setg(errp,
                           "CXL hybrid strict region remap hit copy-owned region");
                return -EACCES;
            }
            return 0;
        }
        cxl_hybrid_dst_region_wait_not_remapping(&cxl_state.dst_region_state,
                                                 g->region_index);
    }

    generation = cxl_hybrid_fault_publish_generation();
    cxl_hybrid_ctrl_mark_region_owned(g->region_index, generation);
    if (!cxl_hybrid_ctrl_region_owned(g->region_index, generation)) {
        error_setg(errp,
                   "CXL hybrid destination region ownership was not published");
        cxl_hybrid_dst_region_finish_remap(&cxl_state.dst_region_state,
                                           g->region_index, false);
        qatomic_inc(&cxl_state.dst_region_map_failures);
        return -EINVAL;
    }

    host_addr = rb->host + g->block_offset;
    start_ns = cxl_now_ns();
    ret = mmap(host_addr, g->region_len, PROT_READ | PROT_WRITE,
               MAP_FIXED | MAP_SHARED, cxl_state.fd, g->cxl_offset);
    mmap_ns = cxl_now_ns() - start_ns;
    qatomic_add(&cxl_state.dst_region_map_time_ns, mmap_ns);
    qatomic_inc(&cxl_state.dst_region_map_attempts);
    if (ret == MAP_FAILED) {
        rc = -errno;
        qatomic_inc(&cxl_state.dst_region_map_failures);
        error_setg_errno(errp, errno,
                         "CXL hybrid destination region remap failed");
        cxl_hybrid_dst_region_finish_remap(&cxl_state.dst_region_state,
                                           g->region_index, false);
        return rc;
    }

    wake_start_ns = cxl_now_ns();
    rc = postcopy_mark_range_received_and_wake(mis, rb, host_addr,
                                               g->region_len,
                                               fault_host_addr,
                                               &received);
    wake_ns = cxl_now_ns() - wake_start_ns;
    if (rc) {
        cxl_hybrid_dst_region_finish_remap(&cxl_state.dst_region_state,
                                           g->region_index, received);
        qatomic_inc(&cxl_state.dst_region_wake_failures);
        return rc;
    }

    cxl_hybrid_dst_region_finish_remap(&cxl_state.dst_region_state,
                                       g->region_index, true);
    qatomic_inc(&cxl_state.dst_region_map_successes);
    trace_cxl_hybrid_dst_region_remap(qemu_ram_get_idstr(rb),
                                      g->block_offset, g->region_len,
                                      g->cxl_offset, g->region_index,
                                      mmap_ns, wake_ns, received);
    if (trace_event_get_state(TRACE_CXL_HYBRID_DST_REGION_REMAP_TS)) {
        trace_cxl_hybrid_dst_region_remap_ts(
            cxl_now_ns(), qemu_ram_get_idstr(rb), g->block_offset,
            g->region_len, g->cxl_offset, g->region_index, mmap_ns,
            wake_ns, received);
    }
    return 1;
}

static int cxl_hybrid_wait_and_resolve_region_fault(MigrationIncomingState *mis,
                                                    RAMBlock *rb,
                                                    ram_addr_t offset,
                                                    uint64_t haddr,
                                                    uint32_t tid,
                                                    Error **errp)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    void *fault_host;
    uint32_t generation;
    uint64_t wait_start_ns;
    int ret;

    ret = cxl_hybrid_fault_region_compute(rb->offset, rb->used_length,
                                          rb->pages_offset, offset,
                                          cxl_state.remap_granule,
                                          TARGET_PAGE_SIZE, &g, errp);
    if (ret) {
        return ret;
    }

    fault_host = ramblock_ptr(rb, offset);
    if (cxl_hybrid_mark_region_fault_requested(mis, rb, offset, fault_host,
                                               haddr, tid)) {
        return 0;
    }

    generation = cxl_hybrid_fault_publish_generation();
    wait_start_ns = cxl_now_ns();
    if (!cxl_hybrid_ctrl_region_bit_visible(g.first_page_index, g.nr_pages,
                                            generation)) {
        ret = cxl_hybrid_ctrl_enqueue_fault_region_request(g.first_page_index,
                                                           g.nr_pages,
                                                           generation,
                                                           wait_start_ns,
                                                           NULL,
                                                           errp);
        if (ret) {
            return ret;
        }
    }

    ret = cxl_hybrid_ctrl_wait_region_visible(g.first_page_index, g.nr_pages,
                                              generation, errp);
    cxl_hybrid_record_timing(&cxl_state.dst_region_wait_samples,
                             &cxl_state.dst_region_wait_time_ns,
                             &cxl_state.max_dst_region_wait_time_ns,
                             cxl_now_ns() - wait_start_ns);
    if (ret) {
        return ret;
    }

    ret = cxl_hybrid_dst_remap_region(mis, rb, &g, fault_host, errp);
    return ret < 0 ? ret : 0;
}

static int cxl_hybrid_try_resolve_region_fault_fast(
    MigrationIncomingState *mis,
    RAMBlock *rb,
    ram_addr_t offset,
    uint64_t haddr,
    uint32_t tid,
    uint64_t *region_indexp,
    bool *poison_on_fallback,
    Error **errp)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    void *fault_host;
    uint32_t generation;
    int ret;

    if (region_indexp) {
        *region_indexp = UINT64_MAX;
    }
    if (poison_on_fallback) {
        *poison_on_fallback = false;
    }

retry:
    ret = cxl_hybrid_fault_region_compute(rb->offset, rb->used_length,
                                          rb->pages_offset, offset,
                                          cxl_state.remap_granule,
                                          TARGET_PAGE_SIZE, &g, errp);
    if (ret) {
        return ret;
    }
    if (region_indexp) {
        *region_indexp = g.region_index;
    }
    if (!cxl_hybrid_dst_region_can_remap(&cxl_state.dst_region_state,
                                         g.region_index)) {
        if (cxl_hybrid_dst_region_copy_owned(&cxl_state.dst_region_state,
                                             g.region_index)) {
            return 0;
        }
        if (cxl_hybrid_dst_region_remapped(&cxl_state.dst_region_state,
                                           g.region_index)) {
            return 1;
        }
        cxl_hybrid_dst_region_wait_not_remapping(&cxl_state.dst_region_state,
                                                 g.region_index);
        goto retry;
    }

    if (cxl_hybrid_dst_region_remapped(&cxl_state.dst_region_state,
                                       g.region_index)) {
        return 1;
    }

    generation = cxl_hybrid_fault_publish_generation();
    if (!cxl_hybrid_ctrl_region_bit_visible(g.first_page_index, g.nr_pages,
                                            generation)) {
        if (cxl_hybrid_dst_region_try_mark_copy_owned(
                &cxl_state.dst_region_state, g.region_index)) {
            if (poison_on_fallback) {
                *poison_on_fallback = true;
            }
            return 0;
        }
        if (cxl_hybrid_dst_region_copy_owned(&cxl_state.dst_region_state,
                                             g.region_index)) {
            return 0;
        }
        if (cxl_hybrid_dst_region_remapped(&cxl_state.dst_region_state,
                                           g.region_index)) {
            return 1;
        }
        cxl_hybrid_dst_region_wait_not_remapping(&cxl_state.dst_region_state,
                                                 g.region_index);
        goto retry;
    }

    fault_host = ramblock_ptr(rb, offset);
    if (cxl_hybrid_mark_region_fault_requested(mis, rb, offset, fault_host,
                                               haddr, tid)) {
        return 1;
    }

    return cxl_hybrid_dst_remap_region(mis, rb, &g, fault_host, errp);
}

static int cxl_destination_region_state_init(int fd, uint64_t align,
                                             uint64_t map_size,
                                             Error **errp)
{
    uint64_t total_ram;
    uint64_t pages_per_region;
    uint64_t required;
    int owned_fd;

    if (!migrate_cxl_fault_resolve_uses_region()) {
        return 0;
    }
    if (fd < 0) {
        error_setg(errp, "CXL hybrid destination region remap missing fd");
        return -EINVAL;
    }

    total_ram = ram_bytes_total();
    cxl_state.align = align;
    cxl_state.remap_granule = cxl_choose_fault_region_granule(align,
                                                              total_ram);
    if (!cxl_state.remap_granule ||
        cxl_state.remap_granule < TARGET_PAGE_SIZE ||
        cxl_state.remap_granule % TARGET_PAGE_SIZE ||
        !QEMU_IS_ALIGNED(cxl_state.remap_granule, align)) {
        error_setg(errp,
                   "CXL hybrid destination remap granule is invalid: %" PRIu64,
                   cxl_state.remap_granule);
        return -EINVAL;
    }

    required = cxl_hybrid_mapped_ram_required_bytes(align);
    if (map_size < required) {
        error_setg(errp,
                   "CXL hybrid destination remap backing too small: "
                   "%" PRIu64 " < %" PRIu64,
                   map_size, required);
        return -ENOSPC;
    }

    pages_per_region = cxl_state.remap_granule / TARGET_PAGE_SIZE;
    cxl_state.total_pages = cxl_total_guest_pages();
    cxl_state.total_regions = DIV_ROUND_UP(cxl_state.total_pages,
                                           pages_per_region);

    owned_fd = dup(fd);
    if (owned_fd < 0) {
        error_setg_errno(errp, errno,
                         "Failed to dup CXL hybrid destination remap fd");
        return -errno;
    }

    cxl_destination_region_state_cleanup();
    cxl_state.fd = owned_fd;
    if (migrate_cxl_rdma_sidecar()) {
        cxl_state.mmap_size = required;
        cxl_state.mmap_base = mmap(NULL, cxl_state.mmap_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED, cxl_state.fd, 0);
        if (cxl_state.mmap_base == MAP_FAILED) {
            int saved_errno = errno;

            error_setg_errno(errp, saved_errno,
                             "Failed to mmap CXL hybrid destination backing");
            cxl_state.mmap_base = NULL;
            close(cxl_state.fd);
            cxl_state.fd = -1;
            cxl_state.mmap_size = 0;
            return -saved_errno;
        }
    }
    cxl_hybrid_dst_region_state_init_for_test(&cxl_state.dst_region_state,
                                              cxl_state.total_regions);
    if (!cxl_hybrid_start_rdma_sidecar(true, false, errp)) {
        cxl_destination_region_state_cleanup();
        return -EINVAL;
    }
    return 0;
}

static void cxl_destination_region_state_cleanup(void)
{
    cxl_rdma_sidecar_stop();
    cxl_hybrid_dst_region_state_destroy_for_test(&cxl_state.dst_region_state);
    if (!cxl_state.active && cxl_state.fd >= 0) {
        if (cxl_state.mmap_base) {
            munmap(cxl_state.mmap_base, cxl_state.mmap_size);
            cxl_state.mmap_base = NULL;
            cxl_state.mmap_size = 0;
        }
        close(cxl_state.fd);
        cxl_state.fd = -1;
    }
}

int cxl_hybrid_wait_and_resolve_fault(MigrationIncomingState *mis,
                                      RAMBlock *rb,
                                      ram_addr_t offset,
                                      uint64_t haddr,
                                      uint32_t tid,
                                      int (*place_page)(MigrationIncomingState *mis,
                                                        void *host,
                                                        void *from,
                                                        RAMBlock *rb),
                                      Error **errp)
{
    const char *ramblock;
    void *aligned;
    bool publish_req_sent = false;
    size_t page_index;
    size_t pagesize;
    uint32_t generation;
    uint64_t wait_start_ns;
    uint64_t wait_time_ns;
    uint64_t cxl_offset;
    int ret;

    if (!mis || !rb || !place_page) {
        error_setg(errp, "CXL hybrid fault wait missing arguments");
        return -EINVAL;
    }

    if (migrate_cxl_fault_resolve_region_remap()) {
        return cxl_hybrid_wait_and_resolve_region_fault(mis, rb, offset,
                                                        haddr, tid, errp);
    }

    if (migrate_cxl_fault_resolve_region_remap_fallback_copy()) {
        uint64_t region_index = UINT64_MAX;
        bool poison_on_fallback = false;

        ret = cxl_hybrid_try_resolve_region_fault_fast(
            mis, rb, offset, haddr, tid, &region_index, &poison_on_fallback,
            errp);
        if (ret < 0) {
            return ret;
        }
        if (ret > 0) {
            return 0;
        }
        if (poison_on_fallback && region_index != UINT64_MAX) {
            cxl_hybrid_dst_region_mark_copy_owned(
                &cxl_state.dst_region_state, region_index);
            qatomic_inc(&cxl_state.dst_region_fallback_copies);
        }
    }

    ret = cxl_hybrid_try_resolve_fault(mis, rb, offset, place_page, errp);
    if (ret < 0) {
        return ret;
    }
    if (ret > 0) {
        return 0;
    }

    ramblock = qemu_ram_get_idstr(rb);
    pagesize = qemu_ram_pagesize(rb);
    aligned = (void *)(uintptr_t)ROUND_DOWN(haddr, pagesize);

    /*
     * Hybrid postcopy must resolve misses through CXL.  Mark the fault as
     * outstanding for blocktime accounting, but do not enqueue a legacy RAM
     * payload request.
     */
    WITH_QEMU_LOCK_GUARD(&mis->page_request_mutex) {
        if (ramblock_recv_bitmap_test_byte_offset(rb, offset)) {
            return 0;
        }
        if (!g_tree_lookup(mis->page_requested, aligned)) {
            g_tree_insert(mis->page_requested, aligned, (gpointer)1);
            qatomic_inc(&mis->page_requested_count);
            trace_postcopy_page_req_add(aligned, mis->page_requested_count);
            if (trace_event_get_state(TRACE_POSTCOPY_PAGE_REQ_ADD_TS)) {
                trace_postcopy_page_req_add_ts(cxl_now_ns(), aligned,
                                               mis->page_requested_count);
            }
        }
        mark_postcopy_blocktime_begin(haddr, tid, rb);
    }

    generation = cxl_hybrid_fault_publish_generation();
    wait_start_ns = cxl_now_ns();
    page_index = cxl_global_page_index(rb, offset);
    if (!cxl_hybrid_ctrl_page_visible(page_index, generation)) {
        ret = cxl_hybrid_ctrl_enqueue_fault_request(page_index, generation,
                                                    wait_start_ns,
                                                    &publish_req_sent, errp);
        if (ret) {
            return ret;
        }
    }
    if (publish_req_sent) {
        qatomic_inc(&cxl_state.fault_publish_requests);
        trace_cxl_hybrid_publish_request_send(ramblock, offset, generation);
    }

    qatomic_inc(&cxl_state.fault_publish_waits);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
        cxl_hybrid_set_last_publish_wait(
            &cxl_state.last_publish_wait_begin,
            ramblock,
            offset,
            generation,
            cxl_state.last_publish_wait_begin.count + 1,
            false, 0,
            false, 0);
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    trace_cxl_hybrid_publish_wait_begin(ramblock, offset, generation);
    ret = cxl_hybrid_ctrl_wait_page_visible(page_index, generation, errp);
    if (!ret &&
        !cxl_hybrid_source_page_cxl_offset(ramblock, offset, &cxl_offset)) {
        error_setg(errp,
                   "CXL hybrid fault page %s/0x%" PRIx64
                   " has no stable CXL offset",
                   ramblock, (uint64_t)offset);
        ret = -EINVAL;
    }
    if (!ret) {
        if (!cxl_hybrid_dst_staging_is_active()) {
            error_setg(errp,
                       "CXL hybrid copy fault resolution requires destination staging");
            ret = -EINVAL;
        } else {
            ret = cxl_hybrid_dst_staging_register_external_page(
                ramblock, offset, cxl_offset, pagesize, errp);
        }
    }
    wait_time_ns = cxl_now_ns() - wait_start_ns;
    qatomic_add(&cxl_state.fault_publish_wait_time_ns, wait_time_ns);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
        cxl_hybrid_set_last_publish_wait(
            &cxl_state.last_publish_wait_complete,
            ramblock,
            offset,
            generation,
            cxl_state.last_publish_wait_complete.count + 1,
            true, wait_time_ns,
            true, ret);
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    trace_cxl_hybrid_publish_wait_complete(ramblock, offset, generation,
                                           wait_time_ns, ret);
    if (ret) {
        return ret;
    }

    ret = cxl_hybrid_try_resolve_fault(mis, rb, offset, place_page, errp);
    if (ret < 0) {
        return ret;
    }
    if (ret == 0) {
        error_setg(errp,
                   "CXL hybrid visible page did not make %s/0x%" PRIx64
                   " available",
                   ramblock, (uint64_t)offset);
        return -ENOENT;
    }

    return 0;
}

static bool cxl_destination_copy_staging_needed(void)
{
    return migrate_cxl_fault_resolve_copy() ||
           migrate_cxl_fault_resolve_region_remap_fallback_copy();
}

static int cxl_destination_backing_open(QIOChannelCXL **ciocp, Error **errp)
{
    QIOChannelCXL *cioc;
    const char *path = migrate_cxl_path();

    if (!path) {
        error_setg(errp, "CXL hybrid destination setup requires cxl-path");
        return -EINVAL;
    }

    cioc = qio_channel_cxl_new_path(path, O_RDWR | O_CLOEXEC, errp);
    if (!cioc) {
        return -EINVAL;
    }

    if (cioc->map_size == 0) {
        error_setg(errp, "CXL hybrid destination backing size is unknown");
        object_unref(OBJECT(cioc));
        return -EINVAL;
    }

    *ciocp = cioc;
    return 0;
}

static int cxl_destination_control_and_region_init(QIOChannelCXL *cioc,
                                                   Error **errp)
{
    int ret;

    ret = cxl_hybrid_control_init_destination(errp);
    if (ret) {
        return ret;
    }

    ret = cxl_destination_region_state_init(cioc->fd, cioc->align,
                                            cioc->map_size, errp);
    if (ret) {
        cxl_hybrid_control_cleanup_destination();
        return ret;
    }

    return 0;
}

static int cxl_destination_copy_staging_init(QIOChannelCXL *cioc, Error **errp)
{
    int ret;
    uint64_t base_offset;
    uint64_t total_capacity;

    total_capacity = cioc->map_size;
    base_offset = cxl_hybrid_reserved_region_bytes(cioc->align, true);
    if (base_offset >= total_capacity) {
        error_setg(errp,
                   "CXL hybrid destination staging has no free space after mapped-ram backing: "
                   "base=%" PRIu64 " capacity=%" PRIu64,
                   base_offset, total_capacity);
        return -ENOSPC;
    }

    ret = cxl_hybrid_dst_staging_init_fixed_fd(cioc->fd,
                                               total_capacity - base_offset,
                                               base_offset,
                                               total_capacity,
                                               cioc->dev_size > 0,
                                               errp);
    if (ret) {
        return ret;
    }

    if (cxl_incoming_meta_state.valid) {
        ret = cxl_hybrid_dst_staging_apply_metadata(
            &cxl_incoming_meta_state.last_meta, errp);
        if (ret) {
            cxl_hybrid_dst_staging_cleanup();
            return ret;
        }
    }

    return 0;
}

static int cxl_destination_setup_init(Error **errp)
{
    QIOChannelCXL *cioc = NULL;
    int ret;

    if (!migrate_cxl_hybrid()) {
        return 0;
    }

    ret = cxl_destination_backing_open(&cioc, errp);
    if (ret) {
        return ret;
    }

    ret = cxl_destination_control_and_region_init(cioc, errp);
    if (ret) {
        cxl_destination_setup_cleanup();
        object_unref(OBJECT(cioc));
        return ret;
    }

    if (cxl_destination_copy_staging_needed()) {
        ret = cxl_destination_copy_staging_init(cioc, errp);
        if (ret) {
            cxl_destination_setup_cleanup();
            object_unref(OBJECT(cioc));
            return ret;
        }
    }

    object_unref(OBJECT(cioc));
    return 0;
}

static void cxl_destination_setup_cleanup(void)
{
    cxl_hybrid_control_cleanup_destination();
    cxl_destination_region_state_cleanup();
    cxl_hybrid_dst_staging_cleanup();
}

bool cxl_hybrid_init_destination(Error **errp)
{
    int ret;

    cxl_hybrid_ensure_publish_mutex();
    cxl_state.hybrid_enabled = migrate_cxl_hybrid();
    if (cxl_state.hybrid_enabled &&
        cxl_state.phase == CXL_HYBRID_PHASE_DISABLED) {
        cxl_state.phase = CXL_HYBRID_PHASE_PRECOPY_BULK;
        cxl_state.phase_transitions = MAX(cxl_state.phase_transitions, 1);
    }
    if (cxl_state.hybrid_enabled && !migrate_cxl_shared_backing()) {
        error_setg(errp,
                   "CXL hybrid postcopy requires x-cxl-shared-backing=true");
        return false;
    }
    ret = cxl_destination_setup_init(errp);
    if (ret) {
        return false;
    }
    return true;
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

static bool cxl_hybrid_brake_first_enabled(void)
{
    const char *value = g_getenv(CXL_HYBRID_BRAKE_FIRST_ENV);

    if (!value) {
        return false;
    }

    return strcmp(value, "0") != 0 &&
           g_ascii_strcasecmp(value, "off") != 0 &&
           g_ascii_strcasecmp(value, "false") != 0 &&
           g_ascii_strcasecmp(value, "no") != 0;
}

static bool cxl_remap_active(void)
{
    if (!cxl_state.active) {
        return false;
    }

    if (!cxl_state.hybrid_enabled) {
        return true;
    }

    return cxl_state.phase == CXL_HYBRID_PHASE_PRECOPY_BRAKE ||
           cxl_state.phase == CXL_HYBRID_PHASE_SWITCHING ||
           cxl_state.phase == CXL_HYBRID_PHASE_POSTCOPY_WARM;
}

static uint64_t cxl_parse_remap_granule_override(uint64_t fallback)
{
    const char *value = g_getenv(CXL_REMAP_GRANULE_ENV);
    uint64_t configured = migrate_cxl_brake_remap_granule();

    if (configured) {
        fallback = configured;
    }

    if (value && *value) {
        char *endptr = NULL;
        uint64_t requested = g_ascii_strtoull(value, &endptr, 0);

        if (endptr && *endptr == '\0' && requested > 0) {
            fallback = requested;
        } else {
            warn_report("CXL write-redirect: ignoring invalid %s=%s",
                        CXL_REMAP_GRANULE_ENV, value);
        }
    }

    return fallback;
}

static uint64_t cxl_choose_fault_region_granule(uint64_t align,
                                                uint64_t total_ram)
{
    return cxl_hybrid_choose_fault_region_granule(
        align, CXL_FAULT_REGION_GRANULE_DEFAULT, total_ram);
}

static uint64_t cxl_choose_source_remap_granule(uint64_t total_ram)
{
    uint64_t configured =
        cxl_parse_remap_granule_override(CXL_REMAP_GRANULE_DEFAULT);

    return cxl_hybrid_choose_source_remap_granule(TARGET_PAGE_SIZE,
                                                  configured, total_ram);
}

static uint64_t cxl_total_guest_pages(void)
{
    RAMBlock *block;
    uint64_t total_pages = 0;

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_MIGRATABLE(block) {
        total_pages = MAX(total_pages,
                          DIV_ROUND_UP(block->offset + block->used_length,
                                       TARGET_PAGE_SIZE));
    }

    return total_pages;
}

bool cxl_use_mapped_ram_backing(void)
{
    return migrate_mapped_ram() && migrate_cxl_path_enabled();
}

static uint64_t cxl_mapped_ram_pages_offset_alignment(uint64_t align)
{
    bool use_region_remap = migrate_cxl_fault_resolve_uses_region();
    uint64_t remap_granule = use_region_remap ?
        cxl_choose_fault_region_granule(align, ram_bytes_total()) : 0;

    return cxl_hybrid_mapped_ram_pages_offset_alignment(
        CXL_MAPPED_RAM_FILE_OFFSET_ALIGNMENT, align, remap_granule,
        use_region_remap);
}

uint64_t cxl_hybrid_mapped_ram_required_bytes(uint64_t align)
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
        uint64_t pages_offset;
        uint64_t pages_len;

        if (migrate_ram_is_ignored(block)) {
            continue;
        }

        if (!cxl_hybrid_mapped_ram_layout_next(&offset, block->used_length,
                                               pages_align, TARGET_PAGE_SIZE,
                                               &pages_offset, &pages_len)) {
            return 0;
        }
    }

    /*
     * Real devdax mappings require offset/length alignment at the device
     * granule, not just host page size.
     */
    return ROUND_UP(offset,
                    MAX((uint64_t)qemu_real_host_page_size(), align));
}

uint64_t cxl_mapped_ram_alignment(void)
{
    return cxl_state.align ? cxl_state.align : CXL_BACKING_ALIGN_FALLBACK;
}

uint64_t cxl_mapped_ram_pages_alignment(void)
{
    return cxl_mapped_ram_pages_offset_alignment(cxl_mapped_ram_alignment());
}

uint64_t cxl_hybrid_fault_region_granule(void)
{
    uint64_t align = cxl_mapped_ram_alignment();
    uint64_t total_ram = ram_bytes_total();

    return cxl_state.remap_granule ?
        cxl_state.remap_granule :
        cxl_choose_fault_region_granule(align, total_ram);
}

static void cxl_hybrid_prefault_reset(void)
{
    g_free(cxl_state.prefault_spans);
    cxl_state.prefault_spans = NULL;
    cxl_state.prefault_span_count = 0;
    cxl_state.prefault_thread_created = false;
    cxl_state.prefault_done = false;
    cxl_state.prefault_failed = false;
}

static bool cxl_hybrid_prefault_build_spans(uint64_t pages_align)
{
    RAMBlock *block;
    uint64_t layout_offset = 0;

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_MIGRATABLE(block) {
        uint64_t pages_offset;
        uint64_t pages_len;

        if (migrate_ram_is_ignored(block)) {
            continue;
        }

        if (!cxl_hybrid_mapped_ram_layout_next(&layout_offset,
                                               block->used_length,
                                               pages_align,
                                               TARGET_PAGE_SIZE,
                                               &pages_offset, &pages_len)) {
            qatomic_inc(&cxl_state.prefault_errors);
            return false;
        }
        if (!pages_len) {
            continue;
        }
        cxl_state.prefault_spans = g_renew(CXLHybridPrefaultSpan,
                                           cxl_state.prefault_spans,
                                           cxl_state.prefault_span_count + 1);
        cxl_state.prefault_spans[cxl_state.prefault_span_count++] =
            (CXLHybridPrefaultSpan) {
                .offset = pages_offset,
                .length = pages_len,
            };
    }

    return true;
}

static void *cxl_hybrid_prefault_worker(void *opaque)
{
    uint64_t start_ns = cxl_now_ns();
    size_t i;

    (void)opaque;

    for (i = 0; i < cxl_state.prefault_span_count; i++) {
        CXLHybridPrefaultSpan *span = &cxl_state.prefault_spans[i];
        uint8_t *addr = (uint8_t *)cxl_state.mmap_base + span->offset;

        if (qemu_madvise(addr, span->length, QEMU_MADV_POPULATE_WRITE)) {
            qatomic_inc(&cxl_state.prefault_errors);
            cxl_state.prefault_failed = true;
            break;
        }
        qatomic_add(&cxl_state.prefault_bytes, span->length);
    }

    qatomic_add(&cxl_state.prefault_time_ns, cxl_now_ns() - start_ns);
    qatomic_set(&cxl_state.prefault_done, true);
    return NULL;
}

static void cxl_hybrid_prefault_start(void)
{
    uint64_t pages_align;

    if (!cxl_state.active || !cxl_state.hybrid_enabled ||
        !cxl_state.mmap_base || cxl_state.prefault_thread_created) {
        return;
    }

    pages_align = cxl_mapped_ram_pages_offset_alignment(cxl_state.align);
    if (!cxl_hybrid_prefault_build_spans(pages_align) ||
        !cxl_state.prefault_span_count) {
        cxl_state.prefault_done = true;
        return;
    }

    qemu_thread_create(&cxl_state.prefault_thread,
                       "cxl-prefault",
                       cxl_hybrid_prefault_worker,
                       NULL,
                       QEMU_THREAD_JOINABLE);
    cxl_state.prefault_thread_created = true;
}

void cxl_hybrid_prefault_wait_before_postcopy(void)
{
    uint64_t start_ns;

    if (!cxl_state.prefault_thread_created) {
        return;
    }

    start_ns = cxl_now_ns();
    qemu_thread_join(&cxl_state.prefault_thread);
    qatomic_add(&cxl_state.prefault_wait_time_ns, cxl_now_ns() - start_ns);
    cxl_state.prefault_thread_created = false;
}

uint64_t cxl_hybrid_reserved_region_bytes(uint64_t align,
                                          bool use_fault_control)
{
    uint64_t bytes = cxl_hybrid_mapped_ram_required_bytes(align);

    if (use_fault_control) {
        bytes += cxl_hybrid_fault_control_region_allocation_bytes(align);
    }
    return bytes;
}

void cxl_populate_migration_info(MigrationInfo *info)
{
    CXLHybridDstStagingStats dst_stats = { 0 };
    CXLHybridRDMASidecarBulkStats rdma_bulk_stats = { 0 };
    CXLHybridRDMASidecarStats rdma_stats = { 0 };

    cxl_hybrid_dst_staging_get_stats(&dst_stats);
    cxl_rdma_sidecar_get_stats(&rdma_bulk_stats);
    cxl_hybrid_get_rdma_sidecar_stats(&rdma_stats);
    if (!cxl_state.active && cxl_cleanup_snapshot) {
        info->x_cxl = QAPI_CLONE(CXLMigrationStats, cxl_cleanup_snapshot);
        info->x_cxl->active = false;
        info->x_cxl->phase = CXL_HYBRID_PHASE_DISABLED;
        return;
    }

    info->x_cxl = g_malloc0(sizeof(*info->x_cxl));
    info->x_cxl->active = cxl_state.active;
    info->x_cxl->align = cxl_mapped_ram_alignment();
    info->x_cxl->remap_granule = cxl_state.source_remap_granule;
    info->x_cxl->total_regions = cxl_state.source_remap_regions;
    info->x_cxl->remapped_regions = qatomic_read(&cxl_state.remapped_regions);
    info->x_cxl->remap_coverage = cxl_hybrid_source_remap_coverage();
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
    info->x_cxl->backing_rate_sleep_count =
        qatomic_read(&cxl_state.backing_rate_sleep_count);
    info->x_cxl->backing_rate_sleep_time_ns =
        qatomic_read(&cxl_state.backing_rate_sleep_time_ns);
    info->x_cxl->migrated_bitmap_time_ns =
        qatomic_read(&cxl_state.migrated_bitmap_time_ns);
    info->x_cxl->remap_scan_calls =
        qatomic_read(&cxl_state.remap_scan_calls);
    info->x_cxl->remap_scan_time_ns =
        qatomic_read(&cxl_state.remap_scan_time_ns);
    info->x_cxl->pending_remap_regions =
        qatomic_read(&cxl_state.pending_remap_regions);
    info->x_cxl->clean_pending_remap_regions =
        qatomic_read(&cxl_state.clean_pending_remap_regions);
    info->x_cxl->pending_remap_unmigrated_pages =
        qatomic_read(&cxl_state.pending_remap_unmigrated_pages);
    info->x_cxl->pending_remap_dirty_pages =
        qatomic_read(&cxl_state.pending_remap_dirty_pages);
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
    info->x_cxl->clean_remap_scan_calls =
        qatomic_read(&cxl_state.clean_remap_scan_calls);
    info->x_cxl->clean_remap_candidate_regions =
        qatomic_read(&cxl_state.clean_remap_candidate_regions);
    info->x_cxl->clean_remap_copy_bytes =
        qatomic_read(&cxl_state.clean_remap_copy_bytes);
    info->x_cxl->clean_remap_copy_time_ns =
        qatomic_read(&cxl_state.clean_remap_copy_time_ns);
    info->x_cxl->clean_remap_abandoned_dirty =
        qatomic_read(&cxl_state.clean_remap_abandoned_dirty);
    info->x_cxl->clean_remap_budget_exhaustions =
        qatomic_read(&cxl_state.clean_remap_budget_exhaustions);
    info->x_cxl->clean_remap_pending_bytes =
        cxl_hybrid_clean_remap_pending_bytes();
    info->x_cxl->clean_remap_coverage =
        cxl_hybrid_clean_remap_coverage();
    info->x_cxl->clean_remap_prefault_bytes =
        qatomic_read(&cxl_state.clean_remap_prefault_bytes);
    info->x_cxl->clean_remap_prefault_time_ns =
        qatomic_read(&cxl_state.clean_remap_prefault_time_ns);
    info->x_cxl->clean_remap_prefault_errors =
        qatomic_read(&cxl_state.clean_remap_prefault_errors);
    info->x_cxl->prefault_bytes =
        qatomic_read(&cxl_state.prefault_bytes);
    info->x_cxl->prefault_time_ns =
        qatomic_read(&cxl_state.prefault_time_ns);
    info->x_cxl->prefault_wait_time_ns =
        qatomic_read(&cxl_state.prefault_wait_time_ns);
    info->x_cxl->prefault_errors =
        qatomic_read(&cxl_state.prefault_errors);
    info->x_cxl->phase = cxl_state.active ? cxl_state.phase :
                         CXL_HYBRID_PHASE_DISABLED;
    info->x_cxl->phase_transitions =
        qatomic_read(&cxl_state.phase_transitions);
    info->x_cxl->switch_reason = cxl_state.switch_reason;
    info->x_cxl->switch_iteration = cxl_state.switch_iteration;
    info->x_cxl->dst_stage_capacity_bytes = dst_stats.capacity_bytes;
    info->x_cxl->dst_stage_slots = dst_stats.slots;
    info->x_cxl->dst_stage_present_slots = dst_stats.present_slots;
    info->x_cxl->dst_fault_hits = dst_stats.fault_hits;
    info->x_cxl->dst_fault_misses = dst_stats.fault_misses;
    info->x_cxl->dst_fault_read_bytes = dst_stats.fault_read_bytes;
    info->x_cxl->dst_fault_read_time_ns = dst_stats.fault_read_time_ns;
    info->x_cxl->dst_fault_place_successes = dst_stats.fault_place_successes;
    info->x_cxl->dst_fault_place_failures = dst_stats.fault_place_failures;
    info->x_cxl->dst_region_map_attempts =
        qatomic_read(&cxl_state.dst_region_map_attempts);
    info->x_cxl->dst_region_map_successes =
        qatomic_read(&cxl_state.dst_region_map_successes);
    info->x_cxl->dst_region_map_failures =
        qatomic_read(&cxl_state.dst_region_map_failures);
    info->x_cxl->dst_region_map_time_ns =
        qatomic_read(&cxl_state.dst_region_map_time_ns);
    info->x_cxl->dst_region_wake_failures =
        qatomic_read(&cxl_state.dst_region_wake_failures);
    info->x_cxl->dst_region_wait_samples =
        qatomic_read(&cxl_state.dst_region_wait_samples);
    info->x_cxl->dst_region_wait_time_ns =
        qatomic_read(&cxl_state.dst_region_wait_time_ns);
    info->x_cxl->max_dst_region_wait_time_ns =
        qatomic_read(&cxl_state.max_dst_region_wait_time_ns);
    info->x_cxl->dst_region_fallback_copies =
        qatomic_read(&cxl_state.dst_region_fallback_copies);
    info->x_cxl->staged_pages = cxl_hybrid_current_staged_pages();
    if (cxl_state.total_pages) {
        info->x_cxl->staged_pages_percent =
            ((double)info->x_cxl->staged_pages * 100.0) /
            (double)cxl_state.total_pages;
    }
    info->x_cxl->warm_publish_pages =
        qatomic_read(&cxl_state.warm_publish_pages);
    info->x_cxl->completion_publish_pages =
        qatomic_read(&cxl_state.completion_publish_pages);
    info->x_cxl->last_iterate_ram_pages =
        qatomic_read(&cxl_state.last_iterate_ram_pages);
    info->x_cxl->last_iterate_staged_pages_delta =
        qatomic_read(&cxl_state.last_iterate_staged_pages_delta);
    info->x_cxl->last_iterate_warm_push_pages =
        qatomic_read(&cxl_state.last_iterate_warm_push_pages);
    info->x_cxl->last_iterate_fault_primary_pages =
        qatomic_read(&cxl_state.last_iterate_fault_primary_pages);
    info->x_cxl->last_iterate_fault_burst_pages =
        qatomic_read(&cxl_state.last_iterate_fault_burst_pages);
    info->x_cxl->last_iterate_phase =
        qatomic_read(&cxl_state.last_iterate_phase);
    info->x_cxl->fault_publish_requests =
        qatomic_read(&cxl_state.fault_publish_requests);
    info->x_cxl->fault_publish_waits =
        qatomic_read(&cxl_state.fault_publish_waits);
    info->x_cxl->fault_publish_wait_time_ns =
        qatomic_read(&cxl_state.fault_publish_wait_time_ns);
    info->x_cxl->region_publish_requests =
        qatomic_read(&cxl_state.region_publish_requests);
    info->x_cxl->region_publish_pages =
        qatomic_read(&cxl_state.region_publish_pages);
    info->x_cxl->region_publish_time_ns =
        qatomic_read(&cxl_state.region_publish_time_ns);
    info->x_cxl->rdma_bulk_regions =
        rdma_bulk_stats.rdma_bulk_regions;
    info->x_cxl->rdma_bulk_bytes =
        rdma_bulk_stats.rdma_bulk_bytes;
    info->x_cxl->rdma_sidecar_connect_time_ns =
        rdma_stats.rdma_sidecar_connect_time_ns;
    info->x_cxl->rdma_sidecar_registered_bytes =
        rdma_stats.rdma_sidecar_registered_bytes;
    info->x_cxl->rdma_sidecar_posted_regions =
        rdma_stats.rdma_sidecar_posted_regions;
    info->x_cxl->rdma_sidecar_posted_bytes =
        rdma_stats.rdma_sidecar_posted_bytes;
    info->x_cxl->rdma_sidecar_completed_regions =
        rdma_stats.rdma_sidecar_completed_regions;
    info->x_cxl->rdma_sidecar_completed_bytes =
        rdma_stats.rdma_sidecar_completed_bytes;
    info->x_cxl->rdma_sidecar_stale_regions =
        rdma_stats.rdma_sidecar_stale_regions;
    info->x_cxl->rdma_sidecar_cxl_race_lost_regions =
        rdma_stats.rdma_sidecar_cxl_race_lost_regions;
    info->x_cxl->rdma_sidecar_failed_regions =
        rdma_stats.rdma_sidecar_failed_regions;
    info->x_cxl->rdma_sidecar_no_candidate_events =
        rdma_stats.rdma_sidecar_no_candidate_events;
    info->x_cxl->rdma_sidecar_budget_skip_events =
        rdma_stats.rdma_sidecar_budget_skip_events;
    info->x_cxl->rdma_sidecar_max_inflight_regions =
        rdma_stats.rdma_sidecar_max_inflight_regions;
    info->x_cxl->rdma_sidecar_max_cover_percent =
        rdma_stats.rdma_sidecar_max_cover_percent;
    info->x_cxl->rdma_sidecar_failed =
        rdma_stats.rdma_sidecar_failed;
    info->x_cxl->rdma_ready_regions =
        rdma_stats.rdma_ready_regions;
    info->x_cxl->rdma_ready_pages =
        rdma_stats.rdma_ready_pages;
    info->x_cxl->rdma_invalidated_regions =
        rdma_stats.rdma_invalidated_regions;
    info->x_cxl->rdma_ready_pages_lost =
        rdma_stats.rdma_ready_pages_lost;
    info->x_cxl->cxl_republish_regions_due_to_rdma_invalidate =
        rdma_stats.cxl_republish_regions_due_to_rdma_invalidate;
    info->x_cxl->cxl_republish_pages_due_to_rdma_invalidate =
        rdma_stats.cxl_republish_pages_due_to_rdma_invalidate;
    info->x_cxl->rdma_invalidate_publish_amplification =
        (double)info->x_cxl->cxl_republish_pages_due_to_rdma_invalidate /
        (double)MAX(info->x_cxl->rdma_ready_pages_lost, 1);
    info->x_cxl->publish_memcpy_bytes =
        qatomic_read(&cxl_state.publish_memcpy_bytes);
    info->x_cxl->publish_memcpy_time_ns =
        qatomic_read(&cxl_state.publish_memcpy_time_ns);
    info->x_cxl->stream_publish_ranges =
        qatomic_read(&cxl_state.stream_publish_ranges);
    info->x_cxl->stream_publish_pages =
        qatomic_read(&cxl_state.stream_publish_pages);
    info->x_cxl->stream_publish_time_ns =
        qatomic_read(&cxl_state.stream_publish_time_ns);
    info->x_cxl->stream_publish_local_bitmap_time_ns =
        qatomic_read(&cxl_state.stream_publish_local_bitmap_time_ns);
    info->x_cxl->stream_publish_shared_visible_time_ns =
        qatomic_read(&cxl_state.stream_publish_shared_visible_time_ns);
    info->x_cxl->stream_write_calls =
        qatomic_read(&cxl_state.stream_write_calls);
    info->x_cxl->stream_write_bytes =
        qatomic_read(&cxl_state.stream_write_bytes);
    info->x_cxl->stream_write_max_bytes =
        qatomic_read(&cxl_state.stream_write_max_bytes);
    info->x_cxl->stream_write_time_ns =
        qatomic_read(&cxl_state.stream_write_time_ns);
    info->x_cxl->stream_fault_pause_calls =
        qatomic_read(&cxl_state.stream_fault_pause_calls);
    info->x_cxl->stream_fault_pause_time_ns =
        qatomic_read(&cxl_state.stream_fault_pause_time_ns);
    info->x_cxl->stream_fault_pause_max_time_ns =
        qatomic_read(&cxl_state.stream_fault_pause_max_time_ns);
    info->x_cxl->fault_publish_primary_samples =
        qatomic_read(&cxl_state.fault_publish_primary_samples);
    info->x_cxl->fault_publish_primary_time_ns =
        qatomic_read(&cxl_state.fault_publish_primary_time_ns);
    info->x_cxl->max_fault_publish_primary_time_ns =
        qatomic_read(&cxl_state.max_fault_publish_primary_time_ns);
    info->x_cxl->fault_publish_burst_samples =
        qatomic_read(&cxl_state.fault_publish_burst_samples);
    info->x_cxl->fault_publish_burst_time_ns =
        qatomic_read(&cxl_state.fault_publish_burst_time_ns);
    info->x_cxl->max_fault_publish_burst_time_ns =
        qatomic_read(&cxl_state.max_fault_publish_burst_time_ns);
    info->x_cxl->fault_publish_req_recv_samples =
        qatomic_read(&cxl_state.fault_publish_req_recv_samples);
    info->x_cxl->fault_publish_req_recv_time_ns =
        qatomic_read(&cxl_state.fault_publish_req_recv_time_ns);
    info->x_cxl->max_fault_publish_req_recv_time_ns =
        qatomic_read(&cxl_state.max_fault_publish_req_recv_time_ns);
    info->x_cxl->fault_publish_req_handle_samples =
        qatomic_read(&cxl_state.fault_publish_req_handle_samples);
    info->x_cxl->fault_publish_req_handle_time_ns =
        qatomic_read(&cxl_state.fault_publish_req_handle_time_ns);
    info->x_cxl->max_fault_publish_req_handle_time_ns =
        qatomic_read(&cxl_state.max_fault_publish_req_handle_time_ns);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    info->x_cxl->last_publish_request =
        cxl_hybrid_export_last_publish_request(
            &cxl_state.last_publish_request);
    info->x_cxl->last_publish_wait_begin =
        cxl_hybrid_export_last_publish_wait(
            &cxl_state.last_publish_wait_begin);
    info->x_cxl->last_publish_wait_complete =
        cxl_hybrid_export_last_publish_wait(
            &cxl_state.last_publish_wait_complete);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
}

bool cxl_hybrid_lookup_global_page(size_t page_idx, RAMBlock **blockp,
                                   ram_addr_t *block_offsetp)
{
    RAMBlock *block;
    ram_addr_t global_offset = (ram_addr_t)page_idx << TARGET_PAGE_BITS;

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

static bool cxl_hybrid_page_eligible(size_t page_idx)
{
    return cxl_hybrid_warm_page_eligible_for_push(
        cxl_state.migrated_bmap, cxl_state.warm_sent_bmap,
        cxl_state.dst_sent_bmap, cxl_state.cxl_visible_bmap, page_idx);
}

static void cxl_hybrid_mark_page_range(unsigned long *bitmap,
                                       const char *rbname,
                                       ram_addr_t offset,
                                       ram_addr_t len)
{
    RAMBlock *block = qemu_ram_block_by_name(rbname);
    size_t first_page;
    size_t npages;
    size_t page;

    if (!bitmap || !block || !len || !QEMU_IS_ALIGNED(offset, TARGET_PAGE_SIZE)) {
        return;
    }
    if (!offset_in_ramblock(block, offset) ||
        offset + len > block->used_length ||
        !QEMU_IS_ALIGNED(len, TARGET_PAGE_SIZE)) {
        return;
    }

    first_page = cxl_global_page_index(block, offset);
    npages = len >> TARGET_PAGE_BITS;
    for (page = 0; page < npages && first_page + page < cxl_state.total_pages;
         page++) {
        set_bit_atomic(first_page + page, bitmap);
    }
}

static int cxl_hybrid_send_selected_page(MigrationState *s, size_t page_idx,
                                         Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    uint32_t generation;
    uint64_t cxl_offset;
    int ret;

    (void)s;

    if (!cxl_hybrid_lookup_global_page(page_idx, &block, &block_offset)) {
        error_setg(errp, "CXL hybrid warm push page %zu no longer resolves",
                   page_idx);
        return -ENOENT;
    }

    if (!cxl_hybrid_page_eligible(page_idx)) {
        trace_cxl_hybrid_warm_page_skip_unstaged(block->idstr, block_offset);
        qatomic_inc(&cxl_state.source_warm_skip_unstaged);
        return 0;
    }

    generation = cxl_hybrid_fault_publish_generation();
    ret = cxl_hybrid_publish_page_to_cxl(block->idstr, block_offset,
                                         TARGET_PAGE_SIZE, generation,
                                         CXL_HYBRID_PUBLISH_SOURCE_WARM_PUSH,
                                         &cxl_offset, errp);
    if (ret) {
        return ret;
    }

    set_bit_atomic(page_idx, cxl_state.warm_sent_bmap);
    clear_bit_atomic(page_idx, cxl_state.warm_dirty_bmap);
    qatomic_inc(&cxl_state.source_warm_queue_pages);
    qatomic_inc(&cxl_state.source_warm_sent_pages);
    qatomic_add(&cxl_state.source_warm_sent_bytes, TARGET_PAGE_SIZE);
    trace_cxl_hybrid_warm_page_queued(block->idstr, block_offset);
    return 1;
}
static int cxl_hybrid_completion_publish_remaining_page(
    MigrationState *s,
    size_t page_idx,
    Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    uint32_t generation;
    int ret;

    if (!s) {
        error_setg(errp, "CXL hybrid completion missing migration state");
        return -EINVAL;
    }

    if (page_idx >= cxl_state.total_pages || !cxl_state.remaining_bmap ||
        !test_bit(page_idx, cxl_state.remaining_bmap)) {
        return 0;
    }

    if (!cxl_hybrid_lookup_global_page(page_idx, &block, &block_offset)) {
        error_setg(errp,
                   "CXL hybrid completion page %zu no longer resolves",
                   page_idx);
        return -ENOENT;
    }

    if (!cxl_state.migrated_bmap ||
        !test_bit(page_idx, cxl_state.migrated_bmap)) {
        clear_bit_atomic(page_idx, cxl_state.remaining_bmap);
        return 0;
    }

    if (cxl_state.cxl_visible_bmap &&
        test_bit(page_idx, cxl_state.cxl_visible_bmap)) {
        clear_bit_atomic(page_idx, cxl_state.remaining_bmap);
        return 0;
    }

    generation = cxl_hybrid_fault_publish_generation();
    ret = cxl_hybrid_publish_page_to_cxl(block->idstr, block_offset,
                                         TARGET_PAGE_SIZE, generation,
                                         CXL_HYBRID_PUBLISH_SOURCE_COMPLETION,
                                         &(uint64_t){ 0 }, errp);
    if (ret) {
        return ret;
    }

    qatomic_inc(&cxl_state.source_warm_sent_pages);
    qatomic_add(&cxl_state.source_warm_sent_bytes, TARGET_PAGE_SIZE);
    return 1;
}

static size_t cxl_hybrid_pick_recent_miss_page(void)
{
    RAMBlock *block;
    size_t page_idx;
    ram_addr_t offset;

    if (!cxl_state.warm_last_miss_ramblock) {
        return SIZE_MAX;
    }

    block = qemu_ram_block_by_name(cxl_state.warm_last_miss_ramblock);
    if (!block || !offset_in_ramblock(block, cxl_state.warm_last_miss_offset)) {
        return SIZE_MAX;
    }

    offset = cxl_state.warm_last_miss_offset;
    if (offset + TARGET_PAGE_SIZE <= block->used_length) {
        page_idx = cxl_global_page_index(block, offset);
        if (cxl_hybrid_page_eligible(page_idx) &&
            (!cxl_state.dst_sent_bmap ||
             !test_bit(page_idx, cxl_state.dst_sent_bmap))) {
            return page_idx;
        }
    }

    if (offset >= TARGET_PAGE_SIZE) {
        page_idx = cxl_global_page_index(block, offset - TARGET_PAGE_SIZE);
        if (cxl_hybrid_page_eligible(page_idx) &&
            (!cxl_state.dst_sent_bmap ||
             !test_bit(page_idx, cxl_state.dst_sent_bmap))) {
            return page_idx;
        }
    }

    if (offset + (2 * TARGET_PAGE_SIZE) <= block->used_length) {
        page_idx = cxl_global_page_index(block, offset + TARGET_PAGE_SIZE);
        if (cxl_hybrid_page_eligible(page_idx) &&
            (!cxl_state.dst_sent_bmap ||
             !test_bit(page_idx, cxl_state.dst_sent_bmap))) {
            return page_idx;
        }
    }

    return SIZE_MAX;
}

static uint64_t cxl_hybrid_prefetch_rate_limit(void)
{
    uint64_t cap = migrate_cxl_prefetch_rate();
    uint64_t postcopy_cap = migrate_max_postcopy_bandwidth();

    if (!cap) {
        return postcopy_cap;
    }
    if (!postcopy_cap) {
        return cap;
    }

    return MIN(cap, postcopy_cap);
}

static void cxl_hybrid_warm_sleep(uint32_t pages_sent)
{
    uint64_t rate = cxl_hybrid_prefetch_rate_limit();
    uint64_t bytes;
    uint64_t delay_us;

    if (!rate || !pages_sent) {
        return;
    }

    bytes = (uint64_t)pages_sent * TARGET_PAGE_SIZE;
    delay_us = DIV_ROUND_UP(bytes * G_USEC_PER_SEC, rate);
    if (delay_us > 0) {
        g_usleep(delay_us);
    }
}

static bool cxl_hybrid_warm_disabled(void)
{
    const char *value = g_getenv(CXL_HYBRID_WARM_DISABLE_ENV);

    if (!value || !*value) {
        return false;
    }

    return strcmp(value, "0") != 0 &&
           g_ascii_strcasecmp(value, "off") != 0 &&
           g_ascii_strcasecmp(value, "false") != 0 &&
           g_ascii_strcasecmp(value, "no") != 0;
}

static bool cxl_hybrid_rdma_bulk_region_complete(RAMBlock *block,
                                                 ram_addr_t block_offset)
{
    ram_addr_t global_offset;

    return block &&
           cxl_state.mmap_base &&
           cxl_state.remap_granule &&
           QEMU_IS_ALIGNED(cxl_state.remap_granule, TARGET_PAGE_SIZE) &&
           cxl_hybrid_global_page_offset(block, block_offset,
                                         cxl_state.remap_granule,
                                         &global_offset) &&
           QEMU_IS_ALIGNED(global_offset, cxl_state.remap_granule) &&
           QEMU_IS_ALIGNED(block_offset, cxl_state.remap_granule) &&
           block_offset + cxl_state.remap_granule <= block->used_length &&
           block->pages_offset <= UINT64_MAX - block_offset &&
           block->pages_offset + block_offset <=
               cxl_state.mmap_size - cxl_state.remap_granule;
}

bool cxl_hybrid_region_can_use_rdma_bulk(RAMBlock *block,
                                         ram_addr_t block_offset)
{
    return migrate_cxl_rdma_sidecar() &&
           cxl_hybrid_rdma_bulk_region_complete(block, block_offset);
}

bool cxl_hybrid_rdma_bulk_claim_init(CXLHybridRDMABulkClaim *claim,
                                     RAMBlock *block,
                                     ram_addr_t block_offset)
{
    size_t region_idx;
    uint64_t cxl_offset;

    if (!claim ||
        !cxl_hybrid_region_can_use_rdma_bulk(block, block_offset) ||
        !cxl_hybrid_region_index_from_block_offset(block, block_offset,
                                                  &region_idx)) {
        return false;
    }

    cxl_offset = block->pages_offset + block_offset;
    *claim = (CXLHybridRDMABulkClaim) {
        .block = block,
        .block_offset = block_offset,
        .global_offset = (uint64_t)region_idx * cxl_state.remap_granule,
        .cxl_offset = cxl_offset,
        .region_index = region_idx,
        .bytes = cxl_state.remap_granule,
        .pages = cxl_state.remap_granule >> TARGET_PAGE_BITS,
        .src = block->host + block_offset,
        .dst = (uint8_t *)cxl_state.mmap_base + cxl_offset,
    };
    return true;
}

bool cxl_hybrid_rdma_sidecar_get_backing(void **basep, size_t *sizep)
{
    if (!basep || !sizep || !cxl_state.mmap_base || !cxl_state.mmap_size) {
        return false;
    }

    *basep = cxl_state.mmap_base;
    *sizep = cxl_state.mmap_size;
    return true;
}

static bool cxl_hybrid_rdma_sidecar_migration_running(void *opaque)
{
    return migration_is_running();
}

static bool cxl_hybrid_rdma_sidecar_migration_postcopy(void *opaque)
{
    return migration_in_postcopy();
}

static bool cxl_hybrid_rdma_sidecar_migration_failed(void *opaque)
{
    return migrate_has_error(migrate_get_current());
}

static bool cxl_hybrid_rdma_sidecar_claim_bulk_region(
    CXLHybridRDMABulkClaim *claim,
    void *opaque)
{
    return cxl_hybrid_rdma_try_claim_bulk_region(claim);
}

static void cxl_hybrid_rdma_sidecar_drop_bulk_claim(
    const CXLHybridRDMABulkClaim *claim,
    void *opaque)
{
    cxl_hybrid_rdma_drop_bulk_claim(claim);
}

static void cxl_hybrid_rdma_sidecar_propagate_error(Error *err, void *opaque)
{
    migrate_error_propagate(migrate_get_current(), err);
}

static int cxl_hybrid_rdma_sidecar_foreach_ramblock(RAMBlockIterFunc func,
                                                    void *iter_opaque,
                                                    void *opaque)
{
    return foreach_not_ignored_block(func, iter_opaque);
}

static void cxl_hybrid_note_cxl_publish_regions(RAMBlock *block,
                                                ram_addr_t block_offset,
                                                uint64_t len)
{
    ram_addr_t first_global_offset;
    ram_addr_t last_global_offset;
    uint64_t region_start;
    uint64_t region_end;
    uint64_t region_idx;

    if (!block || !len || !cxl_state.remap_granule ||
        !cxl_hybrid_global_page_offset(block, block_offset, TARGET_PAGE_SIZE,
                                       &first_global_offset) ||
        !cxl_hybrid_global_page_offset(
            block, block_offset + len - TARGET_PAGE_SIZE,
            TARGET_PAGE_SIZE, &last_global_offset)) {
        return;
    }

    region_start = first_global_offset / cxl_state.remap_granule;
    region_end = last_global_offset / cxl_state.remap_granule;
    for (region_idx = region_start; region_idx <= region_end; region_idx++) {
        cxl_hybrid_region_note_cxl_republish(region_idx);
    }
}

bool cxl_hybrid_start_rdma_sidecar(bool incoming, bool wait_for_setup,
                                   Error **errp)
{
    static const CXLHybridRDMASidecarOps ops = {
        .migration_running = cxl_hybrid_rdma_sidecar_migration_running,
        .migration_postcopy = cxl_hybrid_rdma_sidecar_migration_postcopy,
        .migration_failed = cxl_hybrid_rdma_sidecar_migration_failed,
        .claim_bulk_region = cxl_hybrid_rdma_sidecar_claim_bulk_region,
        .drop_bulk_claim = cxl_hybrid_rdma_sidecar_drop_bulk_claim,
        .propagate_error = cxl_hybrid_rdma_sidecar_propagate_error,
        .foreach_ramblock = cxl_hybrid_rdma_sidecar_foreach_ramblock,
    };
    CXLHybridRDMASidecarConfig cfg;

    if (!migrate_cxl_rdma_sidecar()) {
        return true;
    }
    if (!cxl_state.total_regions || !cxl_state.remap_granule) {
        error_setg(errp, "CXL RDMA sidecar region geometry is not initialized");
        return false;
    }

    cfg = (CXLHybridRDMASidecarConfig) {
        .addr = migrate_cxl_rdma_sidecar_address(),
        .total_regions = cxl_state.total_regions,
        .bytes_per_region = cxl_state.remap_granule,
        .pages_per_region = DIV_ROUND_UP(cxl_state.remap_granule,
                                         TARGET_PAGE_SIZE),
        .max_inflight_regions =
            migrate_cxl_rdma_sidecar_max_inflight_regions(),
        .max_cover_percent =
            migrate_cxl_rdma_sidecar_max_cover_percent(),
        .pin_all = migrate_rdma_pin_all(),
        .incoming = incoming,
        .ops = &ops,
    };

    if (wait_for_setup) {
        return cxl_rdma_sidecar_start(&cfg, errp) == 0;
    }
    return cxl_rdma_sidecar_start_async(&cfg, errp);
}

void cxl_hybrid_invalidate_rdma_ready_region_for_page(RAMBlock *block,
                                                      ram_addr_t block_offset)
{
    size_t region_idx;

    if (!migrate_cxl_rdma_sidecar() ||
        !cxl_hybrid_region_index_from_block_offset(block, block_offset,
                                                  &region_idx)) {
        return;
    }

    cxl_hybrid_invalidate_region_rdma_ready(region_idx);
}

void cxl_hybrid_record_warm_miss(const char *rbname, ram_addr_t start)
{
    if (!cxl_state.hybrid_enabled || !rbname) {
        return;
    }

    g_free(cxl_state.warm_last_miss_ramblock);
    cxl_state.warm_last_miss_ramblock = g_strdup(rbname);
    cxl_state.warm_last_miss_offset = start;
    qatomic_set(&cxl_state.source_warm_last_miss_offset, start);
}

void cxl_hybrid_account_warm_dirty(const char *rbname, ram_addr_t offset,
                                   ram_addr_t len)
{
    RAMBlock *block;
    size_t page_idx;
    ram_addr_t page_offset;
    ram_addr_t end;
    size_t region_idx;

    if (!cxl_state.hybrid_enabled || !rbname) {
        return;
    }

    cxl_hybrid_mark_page_range(cxl_state.warm_dirty_bmap, rbname, offset, len);
    block = qemu_ram_block_by_name(rbname);
    if (block &&
        cxl_hybrid_region_index_from_block_offset(block, offset,
                                                  &region_idx)) {
        cxl_hybrid_invalidate_region_rdma_ready(region_idx);
    }
    if (block && cxl_state.published_page_state &&
        QEMU_IS_ALIGNED(offset, TARGET_PAGE_SIZE) &&
        QEMU_IS_ALIGNED(len, TARGET_PAGE_SIZE) &&
        offset_in_ramblock(block, offset) &&
        offset + len <= block->used_length) {
        end = offset + len;
        for (page_offset = offset; page_offset < end;
             page_offset += TARGET_PAGE_SIZE) {
            if (!cxl_page_is_remapped(block->offset + page_offset)) {
                page_idx = cxl_global_page_index(block, page_offset);
                if (cxl_state.cxl_visible_bmap) {
                    clear_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);
                }
                if (cxl_state.migrated_bmap &&
                    test_bit(page_idx, cxl_state.migrated_bmap) &&
                    cxl_state.remaining_bmap) {
                    set_bit_atomic(page_idx, cxl_state.remaining_bmap);
                }
                cxl_hybrid_invalidate_published_page(rbname, page_offset);
            }
        }
    }
    qatomic_inc(&cxl_state.source_heat_updates);
}

void cxl_hybrid_account_dst_page_sent(const char *rbname, ram_addr_t offset,
                                      ram_addr_t len)
{
    RAMBlock *block;

    if (!cxl_state.hybrid_enabled || !rbname) {
        return;
    }

    block = qemu_ram_block_by_name(rbname);
    if (!block) {
        return;
    }

    cxl_hybrid_account_dst_pages_sent(
        block, offset, len, cxl_hybrid_fault_publish_generation());
}

void cxl_hybrid_account_dst_pages_sent(RAMBlock *block, ram_addr_t offset,
                                       ram_addr_t len, uint32_t generation)
{
    uint64_t start_ns;
    uint64_t local_start_ns;
    uint64_t shared_start_ns;
    size_t first_page;
    size_t npages;
    size_t page;

    if (!cxl_state.hybrid_enabled || !block) {
        return;
    }

    if (!len || !QEMU_IS_ALIGNED(offset, TARGET_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(len, TARGET_PAGE_SIZE) ||
        !offset_in_ramblock(block, offset) ||
        offset + len > block->used_length) {
        return;
    }

    first_page = cxl_global_page_index(block, offset);
    npages = len >> TARGET_PAGE_BITS;

    start_ns = cxl_now_ns();
    local_start_ns = start_ns;
    for (page = 0; page < npages && first_page + page < cxl_state.total_pages;
         page++) {
        size_t page_idx = first_page + page;

        if (cxl_state.dst_sent_bmap) {
            set_bit_atomic(page_idx, cxl_state.dst_sent_bmap);
        }
        if (cxl_state.cxl_visible_bmap) {
            set_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);
        }
        if (cxl_state.remaining_bmap) {
            clear_bit_atomic(page_idx, cxl_state.remaining_bmap);
        }
    }
    qatomic_add(&cxl_state.stream_publish_local_bitmap_time_ns,
                cxl_now_ns() - local_start_ns);

    shared_start_ns = cxl_now_ns();
    cxl_hybrid_ctrl_publish_pages_visible(first_page, npages, generation);
    qatomic_add(&cxl_state.stream_publish_shared_visible_time_ns,
                cxl_now_ns() - shared_start_ns);
    qatomic_inc(&cxl_state.stream_publish_ranges);
    qatomic_add(&cxl_state.stream_publish_pages, npages);
    qatomic_add(&cxl_state.stream_publish_time_ns, cxl_now_ns() - start_ns);
}

void cxl_hybrid_account_stream_write(uint64_t bytes, uint64_t elapsed_ns)
{
    uint64_t old_max;

    if (!cxl_state.hybrid_enabled || !bytes) {
        return;
    }

    qatomic_inc(&cxl_state.stream_write_calls);
    qatomic_add(&cxl_state.stream_write_bytes, bytes);
    old_max = qatomic_read(&cxl_state.stream_write_max_bytes);
    while (bytes > old_max &&
           qatomic_cmpxchg(&cxl_state.stream_write_max_bytes,
                           old_max, bytes) != old_max) {
        old_max = qatomic_read(&cxl_state.stream_write_max_bytes);
    }
    qatomic_add(&cxl_state.stream_write_time_ns, elapsed_ns);
}

bool cxl_hybrid_fault_pressure_active(void)
{
    if (!cxl_state.hybrid_enabled) {
        return false;
    }

    return cxl_hybrid_ctrl_fault_pressure(cxl_hybrid_fault_publish_generation());
}

uint64_t cxl_hybrid_wait_fault_pressure_clear(void)
{
    uint32_t generation;
    bool fault_pressure;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint64_t old_max;

    if (!cxl_state.hybrid_enabled) {
        return 0;
    }

    generation = cxl_hybrid_fault_publish_generation();
    fault_pressure = cxl_hybrid_ctrl_fault_pressure(generation);
    if (!fault_pressure) {
        return 0;
    }

    start_ns = cxl_now_ns();
    qatomic_inc(&cxl_state.stream_fault_pause_calls);
    while (fault_pressure) {
        if (cxl_hybrid_control_source_run_completed(generation) ||
            !migration_is_running()) {
            break;
        }
        g_usleep(50);
        fault_pressure = cxl_hybrid_ctrl_fault_pressure(generation);
    }

    elapsed_ns = cxl_now_ns() - start_ns;
    qatomic_add(&cxl_state.stream_fault_pause_time_ns, elapsed_ns);
    old_max = qatomic_read(&cxl_state.stream_fault_pause_max_time_ns);
    while (elapsed_ns > old_max &&
           qatomic_cmpxchg(&cxl_state.stream_fault_pause_max_time_ns,
                           old_max, elapsed_ns) != old_max) {
        old_max = qatomic_read(&cxl_state.stream_fault_pause_max_time_ns);
    }

    return elapsed_ns;
}

bool cxl_hybrid_source_page_visible(RAMBlock *block, ram_addr_t offset,
                                    uint32_t generation)
{
    ram_addr_t global_offset;

    if (!cxl_hybrid_global_page_offset(block, offset, TARGET_PAGE_SIZE,
                                       &global_offset)) {
        return false;
    }

    return cxl_hybrid_ctrl_page_visible(global_offset >> TARGET_PAGE_BITS,
                                        generation);
}

void cxl_hybrid_warm_stats(CXLHybridWarmStats *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->source_heat_updates = qatomic_read(&cxl_state.source_heat_updates);
    stats->source_warm_queue_pages =
        qatomic_read(&cxl_state.source_warm_queue_pages);
    stats->source_warm_sent_pages =
        qatomic_read(&cxl_state.source_warm_sent_pages);
    stats->source_warm_sent_bytes =
        qatomic_read(&cxl_state.source_warm_sent_bytes);
    stats->source_warm_skip_received =
        qatomic_read(&cxl_state.source_warm_skip_received);
    stats->source_warm_skip_unstaged =
        qatomic_read(&cxl_state.source_warm_skip_unstaged);
    stats->source_warm_last_miss_offset =
        qatomic_read(&cxl_state.source_warm_last_miss_offset);
}

bool cxl_hybrid_start_warm_push(MigrationState *s)
{
    if (!cxl_state.hybrid_enabled || !s) {
        return false;
    }

    cxl_state.warm_scan_cursor = 0;
    return true;
}

void cxl_hybrid_stop_warm_push(void)
{
    cxl_state.warm_scan_cursor = 0;
}

bool cxl_hybrid_source_page_cxl_offset(const char *ramblock,
                                       uint64_t guest_offset,
                                       uint64_t *cxl_offsetp)
{
    RAMBlock *block;

    if (!ramblock || !cxl_offsetp) {
        return false;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block) {
        return false;
    }
    if (guest_offset > block->used_length ||
        guest_offset + TARGET_PAGE_SIZE > block->used_length) {
        return false;
    }

    *cxl_offsetp = block->pages_offset + guest_offset;
    return true;
}

static CXLHybridPublishedPageEntry *cxl_hybrid_lookup_published_page_by_index(
    size_t page_idx)
{
    if (!cxl_state.published_page_state || page_idx >= cxl_state.total_pages) {
        return NULL;
    }

    return &cxl_state.published_page_state[page_idx];
}

static CXLHybridPublishedPageEntry *cxl_hybrid_lookup_published_page(
    const char *ramblock, uint64_t guest_offset, size_t *page_idxp)
{
    RAMBlock *block;
    size_t page_idx;

    if (!ramblock) {
        return NULL;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block || !offset_in_ramblock(block, guest_offset) ||
        guest_offset + TARGET_PAGE_SIZE > block->used_length) {
        return NULL;
    }

    page_idx = cxl_global_page_index(block, guest_offset);
    if (page_idxp) {
        *page_idxp = page_idx;
    }

    return cxl_hybrid_lookup_published_page_by_index(page_idx);
}

static void cxl_hybrid_mark_page_cxl_visible(size_t page_idx)
{
    if (page_idx >= cxl_state.total_pages) {
        return;
    }

    if (cxl_state.cxl_visible_bmap) {
        set_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);
    }
    if (cxl_state.remaining_bmap) {
        clear_bit_atomic(page_idx, cxl_state.remaining_bmap);
    }
}

static void cxl_hybrid_mark_page_remaining(size_t page_idx)
{
    if (page_idx >= cxl_state.total_pages) {
        return;
    }

    if (cxl_state.cxl_visible_bmap &&
        test_bit(page_idx, cxl_state.cxl_visible_bmap)) {
        return;
    }

    if (cxl_state.remaining_bmap) {
        set_bit_atomic(page_idx, cxl_state.remaining_bmap);
    }
}

bool cxl_hybrid_source_region_owned_by_destination_generation(
    RAMBlock *block,
    ram_addr_t offset,
    uint32_t generation)
{
    CXLHybridFaultRegionGeometry g = { 0 };

    if (!migrate_cxl_fault_resolve_uses_region() || !block) {
        return false;
    }

    if (cxl_hybrid_fault_region_compute(block->offset, block->used_length,
                                         block->pages_offset, offset,
                                         cxl_state.remap_granule,
                                         TARGET_PAGE_SIZE, &g, NULL)) {
        return false;
    }

    return cxl_hybrid_ctrl_region_owned(g.region_index, generation);
}

bool cxl_hybrid_source_region_owned_by_destination(RAMBlock *block,
                                                   ram_addr_t offset)
{
    return cxl_hybrid_source_region_owned_by_destination_generation(
        block, offset, cxl_hybrid_fault_publish_generation());
}

static bool cxl_hybrid_published_page_ready(const char *ramblock,
                                            uint64_t guest_offset,
                                            uint32_t generation,
                                            uint64_t cxl_offset,
                                            bool source_remapped)
{
    CXLHybridPublishedPageEntry *entry;
    size_t page_idx;

    if (source_remapped) {
        return true;
    }

    entry = cxl_hybrid_lookup_published_page(ramblock, guest_offset, &page_idx);
    return entry && entry->valid && entry->ready &&
           cxl_state.cxl_visible_bmap &&
           test_bit(page_idx, cxl_state.cxl_visible_bmap) &&
           entry->generation == generation &&
           entry->cxl_offset == cxl_offset;
}

static bool cxl_hybrid_range_is_remapped(RAMBlock *block,
                                         uint64_t guest_offset,
                                         uint32_t page_len)
{
    uint64_t offset;
    uint64_t end;

    if (!block || !page_len ||
        guest_offset > UINT64_MAX - page_len ||
        !QEMU_IS_ALIGNED(guest_offset, TARGET_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(page_len, TARGET_PAGE_SIZE)) {
        return false;
    }

    end = guest_offset + page_len;
    for (offset = guest_offset; offset < end; offset += TARGET_PAGE_SIZE) {
        if (!cxl_page_is_remapped(block->offset + offset)) {
            return false;
        }
    }

    return true;
}

static void cxl_hybrid_invalidate_published_page(const char *ramblock,
                                                 uint64_t guest_offset)
{
    CXLHybridPublishedPageEntry *entry;
    size_t page_idx;

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    entry = cxl_hybrid_lookup_published_page(ramblock, guest_offset, &page_idx);
    if (entry) {
        entry->ready = false;
        if (cxl_state.cxl_visible_bmap) {
            clear_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);
        }
        cxl_hybrid_ctrl_clear_page_visible(page_idx);
        if (cxl_state.migrated_bmap &&
            test_bit(page_idx, cxl_state.migrated_bmap) &&
            cxl_state.remaining_bmap) {
            set_bit_atomic(page_idx, cxl_state.remaining_bmap);
        }
    }
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
}

static int cxl_hybrid_copy_page_to_stable_cxl(RAMBlock *block,
                                              uint64_t guest_offset,
                                              uint64_t cxl_offset,
                                              uint32_t page_len,
                                              Error **errp)
{
    uint8_t *src;
    uint8_t *dst;
    uint64_t start_ns;

    if (!page_len || !QEMU_IS_ALIGNED(page_len, TARGET_PAGE_SIZE)) {
        error_setg(errp,
                   "CXL hybrid publish page %s/0x%" PRIx64
                   " has invalid length %u",
                   block->idstr, guest_offset, page_len);
        return -EINVAL;
    }

    if (!cxl_state.mmap_base || cxl_offset + page_len > cxl_state.mmap_size) {
        error_setg(errp,
                   "CXL hybrid publish page %s/0x%" PRIx64
                   " missing writable CXL backing",
                   block->idstr, guest_offset);
        return -ENODEV;
    }

    src = block->host + guest_offset;
    dst = (uint8_t *)cxl_state.mmap_base + cxl_offset;
    start_ns = cxl_now_ns();
    memcpy(dst, src, page_len);
    qatomic_add(&cxl_state.publish_memcpy_time_ns, cxl_now_ns() - start_ns);
    qatomic_add(&cxl_state.publish_memcpy_bytes, page_len);
    return 0;
}

static int cxl_hybrid_source_backing_write_begin(RAMBlock *block,
                                                 ram_addr_t offset,
                                                 size_t page_idx,
                                                 uint32_t generation,
                                                 bool *started,
                                                 bool *skip_visible_owned,
                                                 Error **errp)
{
    *started = false;
    *skip_visible_owned = false;

    if (!migrate_cxl_fault_resolve_uses_region()) {
        return 0;
    }

    if (cxl_hybrid_source_region_owned_by_destination_generation(
            block, offset, generation)) {
        if (cxl_hybrid_ctrl_page_visible(page_idx, generation)) {
            *skip_visible_owned = true;
            return 0;
        }
        error_setg(errp,
                   "CXL hybrid source write refused for destination-owned "
                   "invisible region page %s/0x%" PRIx64,
                   block->idstr, offset);
        return -EACCES;
    }

    cxl_hybrid_ctrl_source_write_begin();
    *started = true;

    if (cxl_hybrid_source_region_owned_by_destination_generation(
            block, offset, generation)) {
        cxl_hybrid_ctrl_source_write_end();
        *started = false;
        if (cxl_hybrid_ctrl_page_visible(page_idx, generation)) {
            *skip_visible_owned = true;
            return 0;
        }
        error_setg(errp,
                   "CXL hybrid source write raced with destination-owned "
                   "invisible region page %s/0x%" PRIx64,
                   block->idstr, offset);
        return -EACCES;
    }

    return 0;
}

static void cxl_hybrid_source_backing_write_end(bool started)
{
    if (started) {
        cxl_hybrid_ctrl_source_write_end();
    }
}

void cxl_hybrid_get_publish_stats(CXLHybridPublishStats *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->requests = qatomic_read(&cxl_state.warm_publish_pages);
    stats->copied_pages = qatomic_read(&cxl_state.publish_copied_pages);
    stats->copied_bytes = qatomic_read(&cxl_state.publish_copied_bytes);
    stats->skip_ready = qatomic_read(&cxl_state.publish_skip_ready);
    stats->failures = qatomic_read(&cxl_state.publish_failures);
}

bool cxl_hybrid_get_published_page_state(const char *ramblock,
                                         uint64_t guest_offset,
                                         CXLHybridPublishedPageState *state)
{
    CXLHybridPublishedPageEntry *entry;
    size_t page_idx;
    bool found = false;

    if (!state) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    entry = cxl_hybrid_lookup_published_page(ramblock, guest_offset, &page_idx);
    if (!entry || !entry->valid) {
        goto out;
    }

    state->valid = true;
    state->ready = entry->ready;
    state->copied = !entry->source_remapped;
    state->generation = entry->generation;
    state->cxl_offset = entry->cxl_offset;
    found = true;

out:
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    return found;
}

void cxl_hybrid_note_publish_request_received(const char *ramblock,
                                              uint64_t guest_offset,
                                              uint32_t generation,
                                              uint64_t req_recv_ns)
{
    trace_cxl_hybrid_publish_request_recv(ramblock, guest_offset, generation);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
        cxl_hybrid_set_last_publish_request(
            &cxl_state.last_publish_request,
            ramblock,
            guest_offset,
            generation,
            cxl_state.last_publish_request.count + 1);
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    if (req_recv_ns) {
        cxl_hybrid_record_timing(&cxl_state.fault_publish_req_recv_samples,
                                 &cxl_state.fault_publish_req_recv_time_ns,
                                 &cxl_state.max_fault_publish_req_recv_time_ns,
                                 cxl_now_ns() - req_recv_ns);
    }
}

int cxl_hybrid_publish_page_to_cxl(const char *ramblock,
                                   uint64_t guest_offset,
                                   uint32_t page_len,
                                   uint32_t generation,
                                   CXLHybridPublishSource source,
                                   uint64_t *cxl_offsetp,
                                   Error **errp)
{
    RAMBlock *block;
    ram_addr_t global_offset;
    CXLHybridPublishedPageEntry *entry;
    size_t page_idx;
    bool source_remapped;
    bool already_ready;
    bool skip_ready;
    bool source_write_started = false;
    int ret;

    qatomic_inc(&cxl_state.warm_publish_pages);

    if (!ramblock || !cxl_offsetp) {
        qatomic_inc(&cxl_state.publish_failures);
        error_setg(errp, "CXL hybrid publish page missing arguments");
        return -EINVAL;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block) {
        qatomic_inc(&cxl_state.publish_failures);
        error_setg(errp, "CXL hybrid publish page %s has no RAMBlock", ramblock);
        return -ENOENT;
    }

    if (!page_len || !QEMU_IS_ALIGNED(page_len, TARGET_PAGE_SIZE)) {
        qatomic_inc(&cxl_state.publish_failures);
        error_setg(errp,
                   "CXL hybrid publish page %s/0x%" PRIx64
                   " has invalid length %u",
                   ramblock, guest_offset, page_len);
        return -EINVAL;
    }

    if (!cxl_hybrid_source_page_cxl_offset(ramblock, guest_offset, cxl_offsetp)) {
        qatomic_inc(&cxl_state.publish_failures);
        error_setg(errp,
                   "CXL hybrid publish page %s/0x%" PRIx64 " has no stable CXL offset",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    if (!cxl_hybrid_global_page_offset(block, guest_offset, page_len,
                                       &global_offset)) {
        qatomic_inc(&cxl_state.publish_failures);
        error_setg(errp,
                   "CXL hybrid publish page %s/0x%" PRIx64 " has invalid global offset",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    page_idx = cxl_global_page_index(block, guest_offset);
    if (!cxl_state.published_page_state || page_idx >= cxl_state.total_pages) {
        qatomic_inc(&cxl_state.publish_failures);
        error_setg(errp,
                   "CXL hybrid publish page %s/0x%" PRIx64
                   " has no publish state slot",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    source_remapped = cxl_hybrid_range_is_remapped(block, guest_offset,
                                                   page_len);

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    already_ready = cxl_hybrid_published_page_ready(
        ramblock, guest_offset, generation, *cxl_offsetp, source_remapped);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    trace_cxl_hybrid_publish_begin(ramblock, guest_offset, *cxl_offsetp,
                                   generation, source_remapped);

    skip_ready = source_remapped || already_ready;
    if (!skip_ready) {
        ret = cxl_hybrid_source_backing_write_begin(
            block, guest_offset, page_idx, generation, &source_write_started,
            &skip_ready, errp);
        if (ret) {
            qatomic_inc(&cxl_state.publish_failures);
            return ret;
        }
    }

    if (!skip_ready) {
        ret = cxl_hybrid_copy_page_to_stable_cxl(block, guest_offset,
                                                 *cxl_offsetp, page_len, errp);
        cxl_hybrid_source_backing_write_end(source_write_started);
        if (ret) {
            qatomic_inc(&cxl_state.publish_failures);
            return ret;
        }
        qatomic_add(&cxl_state.publish_copied_pages,
                    page_len >> TARGET_PAGE_BITS);
        qatomic_add(&cxl_state.publish_copied_bytes, page_len);
    } else {
        cxl_hybrid_source_backing_write_end(source_write_started);
        qatomic_inc(&cxl_state.publish_skip_ready);
        trace_cxl_hybrid_publish_skip_ready(ramblock, guest_offset, *cxl_offsetp,
                                            generation);
    }

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    entry = cxl_hybrid_lookup_published_page_by_index(page_idx);
    entry->cxl_offset = *cxl_offsetp;
    entry->generation = generation;
    entry->valid = true;
    entry->ready = true;
    entry->source_remapped = source_remapped;
    cxl_hybrid_mark_page_cxl_visible(page_idx);
    cxl_hybrid_note_cxl_publish_regions(block, guest_offset, page_len);
    cxl_hybrid_ctrl_publish_pages_visible(page_idx, page_len >> TARGET_PAGE_BITS,
                                          generation);
    cxl_hybrid_record_publish_source(source, page_len >> TARGET_PAGE_BITS);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }

    trace_cxl_hybrid_publish_complete(ramblock, guest_offset, *cxl_offsetp,
                                      generation, !source_remapped);
    return 0;
}

static void cxl_hybrid_publish_fault_burst(const char *ramblock,
                                           uint64_t guest_offset,
                                           uint32_t page_len,
                                           uint32_t generation,
                                           Error **errp)
{
    RAMBlock *block;
    uint32_t page;

    (void)errp;

    if (!ramblock || page_len != TARGET_PAGE_SIZE) {
        return;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block || guest_offset > block->used_length ||
        page_len > block->used_length - guest_offset) {
        return;
    }

    for (page = 1; page < CXL_HYBRID_FAULT_BURST_PAGES; page++) {
        uint64_t neighbor_offset;
        uint64_t cxl_offset;
        Error *local_err = NULL;
        int ret;

        if (guest_offset > UINT64_MAX - page_len -
            (uint64_t)(page - 1) * TARGET_PAGE_SIZE) {
            break;
        }
        neighbor_offset = guest_offset + page_len +
                          (uint64_t)(page - 1) * TARGET_PAGE_SIZE;
        if (neighbor_offset > block->used_length ||
            TARGET_PAGE_SIZE > block->used_length - neighbor_offset) {
            break;
        }

        ret = cxl_hybrid_publish_page_to_cxl(ramblock, neighbor_offset,
                                             TARGET_PAGE_SIZE, generation,
                                             CXL_HYBRID_PUBLISH_SOURCE_FAULT_BURST,
                                             &cxl_offset, &local_err);
        if (ret) {
            warn_report_err(local_err);
            continue;
        }
    }
}

static int cxl_hybrid_publish_region_span_to_cxl(RAMBlock *block,
                                                 ram_addr_t block_offset,
                                                 uint64_t first_page,
                                                 uint32_t nr_pages,
                                                 uint32_t generation,
                                                 int *publishedp,
                                                 Error **errp)
{
    CXLHybridPublishedPageEntry *entry;
    uint64_t cxl_offset;
    uint64_t len64;
    uint32_t len;
    uint32_t page;
    bool source_remapped;
    bool source_write_started = false;
    bool skip_visible_owned = false;
    int ret;

    if (!block || !nr_pages || !publishedp) {
        error_setg(errp, "CXL hybrid region span publish missing arguments");
        return -EINVAL;
    }
    if (nr_pages > UINT32_MAX / TARGET_PAGE_SIZE) {
        error_setg(errp,
                   "CXL hybrid region span publish is too large: %u pages",
                   nr_pages);
        return -EOVERFLOW;
    }

    len64 = (uint64_t)nr_pages * TARGET_PAGE_SIZE;
    len = (uint32_t)len64;
    if (block_offset > block->used_length ||
        len64 > block->used_length - block_offset) {
        error_setg(errp,
                   "CXL hybrid region span publish %s/0x" RAM_ADDR_FMT
                   " length=%u is outside RAMBlock",
                   block->idstr, block_offset, len);
        return -ERANGE;
    }
    if (block->pages_offset > UINT64_MAX - block_offset) {
        error_setg(errp,
                   "CXL hybrid region span publish CXL offset overflows");
        return -EOVERFLOW;
    }

    cxl_offset = block->pages_offset + block_offset;
    if (!cxl_state.mmap_base || cxl_offset > UINT64_MAX - len64 ||
        cxl_offset + len64 > cxl_state.mmap_size) {
        error_setg(errp,
                   "CXL hybrid region span publish %s/0x" RAM_ADDR_FMT
                   " missing writable CXL backing",
                   block->idstr, block_offset);
        return -ENODEV;
    }

    qatomic_add(&cxl_state.warm_publish_pages, nr_pages);
    source_remapped = cxl_hybrid_range_is_remapped(block, block_offset, len);

    if (!source_remapped) {
        ret = cxl_hybrid_source_backing_write_begin(
            block, block_offset, first_page, generation, &source_write_started,
            &skip_visible_owned, errp);
        if (ret) {
            qatomic_add(&cxl_state.publish_failures, nr_pages);
            return ret;
        }
    }

    if (skip_visible_owned) {
        for (page = 0; page < nr_pages; page++) {
            if (!cxl_hybrid_ctrl_page_visible(first_page + page,
                                              generation)) {
                error_setg(errp,
                           "CXL hybrid region span publish raced with "
                           "destination-owned invisible page %" PRIu64,
                           first_page + page);
                cxl_hybrid_source_backing_write_end(source_write_started);
                qatomic_add(&cxl_state.publish_failures, nr_pages - page);
                return -EACCES;
            }
        }
        cxl_hybrid_source_backing_write_end(source_write_started);
        qatomic_add(&cxl_state.publish_skip_ready, nr_pages);
        return 0;
    }

    if (!source_remapped) {
        ret = cxl_hybrid_copy_page_to_stable_cxl(block, block_offset,
                                                 cxl_offset, len, errp);
        cxl_hybrid_source_backing_write_end(source_write_started);
        if (ret) {
            qatomic_add(&cxl_state.publish_failures, nr_pages);
            return ret;
        }
        qatomic_add(&cxl_state.publish_copied_pages, nr_pages);
        qatomic_add(&cxl_state.publish_copied_bytes, len64);
    } else {
        cxl_hybrid_source_backing_write_end(source_write_started);
        qatomic_add(&cxl_state.publish_skip_ready, nr_pages);
    }

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    for (page = 0; page < nr_pages; page++) {
        uint64_t page_cxl_offset =
            cxl_offset + (uint64_t)page * TARGET_PAGE_SIZE;
        size_t page_idx = first_page + page;

        entry = cxl_hybrid_lookup_published_page_by_index(page_idx);
        if (!entry) {
            if (cxl_state.publish_mutex_ready) {
                qemu_mutex_unlock(&cxl_state.publish_mutex);
            }
            qatomic_add(&cxl_state.publish_failures, nr_pages - page);
            error_setg(errp,
                       "CXL hybrid region span publish page %" PRIu64
                       " has no publish state slot",
                       (uint64_t)page_idx);
            return -EINVAL;
        }

        entry->cxl_offset = page_cxl_offset;
        entry->generation = generation;
        entry->valid = true;
        entry->ready = true;
        entry->source_remapped = source_remapped;
        cxl_hybrid_mark_page_cxl_visible(page_idx);
    }
    cxl_hybrid_note_cxl_publish_regions(block, block_offset, len64);
    cxl_hybrid_ctrl_publish_pages_visible(first_page, nr_pages, generation);
    cxl_hybrid_record_publish_source(CXL_HYBRID_PUBLISH_SOURCE_FAULT_PRIMARY,
                                     nr_pages);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }

    *publishedp += nr_pages;
    return 0;
}

int cxl_hybrid_publish_fault_region_request_core(uint64_t first_page,
                                                 uint32_t nr_pages,
                                                 uint32_t generation,
                                                 uint64_t req_recv_ns,
                                                 Error **errp)
{
    uint64_t start_ns = cxl_now_ns();
    int published = 0;
    int ret;
    uint32_t page;

    (void)req_recv_ns;

    if (!nr_pages || first_page >= cxl_state.total_pages ||
        nr_pages > cxl_state.total_pages - first_page) {
        error_setg(errp,
                   "CXL hybrid fault region request out of range "
                   "(first-page=%" PRIu64 " pages=%u total-pages=%zu)",
                   first_page, nr_pages, cxl_state.total_pages);
        return -EINVAL;
    }

    for (page = 0; page < nr_pages; ) {
        RAMBlock *span_block = NULL;
        ram_addr_t span_offset = 0;
        uint64_t span_first_page = first_page + page;
        uint32_t span_pages = 0;

        while (page < nr_pages) {
            uint64_t page_index = first_page + page;
            RAMBlock *block;
            ram_addr_t block_offset;

            if (cxl_hybrid_ctrl_page_visible(page_index, generation)) {
                break;
            }

            if (!cxl_hybrid_lookup_global_page(page_index, &block,
                                               &block_offset)) {
                error_setg(errp,
                           "CXL hybrid fault region page is unresolved "
                           "(page=%" PRIu64 ")",
                           page_index);
                return -ENOENT;
            }

            if (cxl_hybrid_source_region_owned_by_destination_generation(
                    block, block_offset, generation)) {
                if (!cxl_hybrid_ctrl_page_visible(page_index, generation)) {
                    error_setg(errp,
                               "CXL hybrid region request page %" PRIu64
                               " is destination-owned before becoming visible",
                               page_index);
                    return -EACCES;
                }
                break;
            }

            if (span_pages &&
                (block != span_block ||
                 block_offset != span_offset +
                                 (ram_addr_t)span_pages * TARGET_PAGE_SIZE)) {
                break;
            }

            if (!span_pages) {
                span_block = block;
                span_offset = block_offset;
                span_first_page = page_index;
            }
            span_pages++;
            page++;
        }

        if (!span_pages) {
            page++;
            continue;
        }

        ret = cxl_hybrid_publish_region_span_to_cxl(
            span_block, span_offset, span_first_page, span_pages, generation,
            &published, errp);
        if (ret) {
            return ret;
        }
    }

    cxl_hybrid_ctrl_set_region_visible(first_page, nr_pages, generation);
    qatomic_inc(&cxl_state.region_publish_requests);
    qatomic_add(&cxl_state.region_publish_pages, published);
    start_ns = cxl_now_ns() - start_ns;
    qatomic_add(&cxl_state.region_publish_time_ns, start_ns);
    trace_cxl_hybrid_region_publish_complete(first_page, nr_pages, published,
                                             generation, start_ns);
    return 0;
}

int cxl_hybrid_publish_fault_request_core(const char *ramblock,
                                          uint64_t guest_offset,
                                          uint32_t page_len,
                                          uint32_t generation,
                                          bool emit_burst,
                                          Error **errp)
{
    RAMBlock *block;
    uint64_t cxl_offset;
    uint64_t handle_start_ns;
    uint64_t primary_start_ns;
    uint64_t burst_start_ns;
    uint64_t handle_time_ns;
    uint64_t primary_time_ns;
    uint64_t burst_time_ns;
    int ret;

    if (!ramblock) {
        error_setg(errp, "CXL hybrid publish fault request missing arguments");
        return -EINVAL;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block || !offset_in_ramblock(block, guest_offset) ||
        guest_offset + page_len > block->used_length) {
        error_setg(errp, "CXL hybrid publish fault request page is invalid");
        return -EINVAL;
    }

    handle_start_ns = cxl_now_ns();
    primary_start_ns = handle_start_ns;
    ret = cxl_hybrid_publish_page_to_cxl(ramblock, guest_offset, page_len,
                                         generation,
                                         CXL_HYBRID_PUBLISH_SOURCE_FAULT_PRIMARY,
                                         &cxl_offset, errp);
    if (ret) {
        return ret;
    }

    primary_time_ns = cxl_now_ns() - primary_start_ns;
    cxl_hybrid_record_timing(&cxl_state.fault_publish_primary_samples,
                             &cxl_state.fault_publish_primary_time_ns,
                             &cxl_state.max_fault_publish_primary_time_ns,
                             primary_time_ns);

    if (emit_burst &&
        cxl_hybrid_fault_resolve_mode_emits_burst(
            migrate_cxl_fault_resolve_mode())) {
        burst_start_ns = cxl_now_ns();
        cxl_hybrid_publish_fault_burst(ramblock, guest_offset, page_len,
                                       generation, errp);
        burst_time_ns = cxl_now_ns() - burst_start_ns;
        cxl_hybrid_record_timing(&cxl_state.fault_publish_burst_samples,
                                 &cxl_state.fault_publish_burst_time_ns,
                                 &cxl_state.max_fault_publish_burst_time_ns,
                                 burst_time_ns);
    }

    handle_time_ns = cxl_now_ns() - handle_start_ns;
    cxl_hybrid_record_timing(&cxl_state.fault_publish_req_handle_samples,
                             &cxl_state.fault_publish_req_handle_time_ns,
                             &cxl_state.max_fault_publish_req_handle_time_ns,
                             handle_time_ns);
    return 0;
}

int cxl_hybrid_warm_push_iteration(MigrationState *s, Error **errp)
{
    uint32_t batch_pages = migrate_cxl_prefetch_batch_pages();
    uint32_t sent = 0;
    size_t page_idx;
    int ret;

    if (!cxl_state.hybrid_enabled ||
        cxl_state.phase != CXL_HYBRID_PHASE_POSTCOPY_WARM ||
        !s || !s->to_dst_file ||
        cxl_hybrid_warm_disabled()) {
        return 0;
    }

    if (batch_pages == 0) {
        batch_pages = 16;
    }

    page_idx = cxl_hybrid_pick_recent_miss_page();
    if (page_idx < cxl_state.total_pages) {
        ret = cxl_hybrid_send_selected_page(s, page_idx, errp);
        if (ret < 0) {
            return ret;
        }
        sent += ret > 0;
    }

    while (sent < batch_pages && cxl_state.total_pages) {
        size_t scanned = 0;
        bool advanced = false;

        while (scanned < cxl_state.total_pages) {
            page_idx = cxl_state.warm_scan_cursor++ % cxl_state.total_pages;
            scanned++;
            if (!cxl_hybrid_page_eligible(page_idx)) {
                continue;
            }

            ret = cxl_hybrid_send_selected_page(s, page_idx, errp);
            if (ret < 0) {
                return ret;
            }
            if (ret > 0) {
                sent++;
                advanced = true;
                break;
            }
        }

        if (!advanced) {
            break;
        }
    }

    cxl_hybrid_warm_sleep(sent);
    return sent;
}

int cxl_hybrid_completion_publish_remaining_pages(MigrationState *s,
                                                  Error **errp)
{
    size_t page_idx;
    int sent = 0;
    int ret;

    if (!cxl_state.hybrid_enabled ||
        cxl_state.phase != CXL_HYBRID_PHASE_POSTCOPY_WARM ||
        !s || !s->to_dst_file || !cxl_state.remaining_bmap) {
        return 0;
    }

    page_idx = find_next_bit(cxl_state.remaining_bmap, cxl_state.total_pages, 0);
    while (page_idx < cxl_state.total_pages) {
        ret = cxl_hybrid_completion_publish_remaining_page(s, page_idx, errp);
        if (ret < 0) {
            return ret;
        }
        sent += ret;
        page_idx = find_next_bit(cxl_state.remaining_bmap,
                                 cxl_state.total_pages,
                                 page_idx + 1);
    }
    return sent;
}

int cxl_hybrid_completion_publish_remaining_regions(Error **errp)
{
    RAMBlock *block;
    uint64_t pages_per_region;
    uint32_t generation;
    int sent = 0;
    int ret;

    if (!cxl_state.hybrid_enabled ||
        cxl_state.phase != CXL_HYBRID_PHASE_POSTCOPY_WARM ||
        !cxl_state.total_pages || !cxl_state.remap_granule) {
        return 0;
    }

    pages_per_region = DIV_ROUND_UP(cxl_state.remap_granule,
                                    TARGET_PAGE_SIZE);
    if (!pages_per_region || pages_per_region > UINT32_MAX) {
        error_setg(errp,
                   "CXL hybrid completion region granule is invalid: %" PRIu64,
                   cxl_state.remap_granule);
        return -EINVAL;
    }

    generation = cxl_hybrid_fault_publish_generation();
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_NOT_IGNORED(block) {
        size_t block_first_page = block->offset >> TARGET_PAGE_BITS;
        size_t block_pages = DIV_ROUND_UP(block->used_length,
                                          TARGET_PAGE_SIZE);
        size_t block_last_page = MIN(block_first_page + block_pages,
                                     cxl_state.total_pages);
        size_t first_page;

        first_page = QEMU_ALIGN_UP(block_first_page, pages_per_region);
        while (first_page < block_last_page) {
            uint32_t nr_pages = MIN(pages_per_region,
                                    cxl_state.total_pages - first_page);

            if (first_page + nr_pages > block_last_page) {
                break;
            }
            if (!cxl_hybrid_region_cxl_bulk_allowed(
                    first_page / pages_per_region)) {
                first_page += pages_per_region;
                continue;
            }
            if (!cxl_hybrid_ctrl_region_bit_visible(first_page, nr_pages,
                                                    generation)) {
                ret = cxl_hybrid_publish_fault_region_request_core(
                    first_page, nr_pages, generation, 0, errp);
                if (ret) {
                    return ret;
                }
                sent += nr_pages;
            }
            first_page += pages_per_region;
        }
    }

    return sent;
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
        uint64_t required = cxl_hybrid_reserved_region_bytes(
            cioc->align, migrate_cxl_hybrid());

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

static size_t cxl_source_remap_region_index(ram_addr_t global_offset)
{
    return global_offset / cxl_state.source_remap_granule;
}

static void cxl_source_remap_region_page_span(RAMBlock *block,
                                              ram_addr_t block_offset,
                                              size_t *first_page,
                                              size_t *npages)
{
    size_t pages_per_region =
        DIV_ROUND_UP(cxl_state.source_remap_granule, TARGET_PAGE_SIZE);

    *first_page = cxl_global_page_index(block, block_offset);
    *npages = pages_per_region;
    if (*first_page + *npages > cxl_state.total_pages) {
        *npages = cxl_state.total_pages - *first_page;
    }
}

static void cxl_mark_pages_remapped(size_t region_idx, size_t first_page,
                                    size_t npages, uint64_t cxl_offset)
{
    size_t page;
    uint32_t generation;

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    generation = cxl_hybrid_fault_publish_generation();
    for (page = 0; page < npages; page++) {
        size_t page_idx = first_page + page;
        CXLHybridPublishedPageEntry *entry =
            cxl_hybrid_lookup_published_page_by_index(page_idx);

        set_bit_atomic(page_idx, cxl_state.remapped_pages_bmap);
        if (cxl_state.cxl_visible_bmap) {
            set_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);
        }
        if (cxl_state.remaining_bmap) {
            clear_bit_atomic(page_idx, cxl_state.remaining_bmap);
        }
        if (entry) {
            entry->cxl_offset = cxl_offset +
                                (uint64_t)page * TARGET_PAGE_SIZE;
            entry->generation = generation;
            entry->valid = true;
            entry->ready = true;
            entry->source_remapped = true;
        }
    }
    cxl_hybrid_ctrl_publish_pages_visible(first_page, npages, generation);
    cxl_hybrid_test_and_set_bit_atomic(region_idx, cxl_state.remapped_bmap);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
}

static bool cxl_remapped_region_all_pages_ready(size_t first_page,
                                                size_t npages,
                                                size_t *missing_pagep)
{
    size_t page;

    if (!cxl_state.remapped_pages_bmap) {
        if (missing_pagep) {
            *missing_pagep = first_page;
        }
        return false;
    }

    for (page = 0; page < npages; page++) {
        size_t page_idx = first_page + page;

        if (!test_bit(page_idx, cxl_state.remapped_pages_bmap)) {
            if (missing_pagep) {
                *missing_pagep = page_idx;
            }
            return false;
        }
    }

    return true;
}

static int cxl_publish_remapped_region_for_postcopy(size_t region_idx,
                                                    uint32_t generation,
                                                    size_t *published_pagesp,
                                                    Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    size_t first_page;
    size_t npages;
    size_t missing_page = 0;
    size_t page;

    if (!cxl_lookup_source_remap_region(region_idx, &block, &block_offset)) {
        error_setg(errp,
                   "CXL hybrid postcopy publish remapped region %zu has no RAMBlock",
                   region_idx);
        return -ENOENT;
    }

    if (!QEMU_IS_ALIGNED(region_idx * cxl_state.source_remap_granule,
                         cxl_state.source_remap_granule) ||
        !QEMU_IS_ALIGNED(block_offset, cxl_state.source_remap_granule) ||
        block_offset + cxl_state.source_remap_granule > block->used_length) {
        error_setg(errp,
                   "CXL hybrid postcopy publish remapped region %zu is not a complete span",
                   region_idx);
        return -EINVAL;
    }

    cxl_source_remap_region_page_span(block, block_offset, &first_page,
                                      &npages);
    if (!npages ||
        !cxl_remapped_region_all_pages_ready(first_page, npages,
                                             &missing_page)) {
        error_setg(errp,
                   "CXL hybrid postcopy publish remapped region %zu is missing remapped page %zu",
                   region_idx, missing_page);
        return -EINVAL;
    }

    for (page = 0; page < npages; page++) {
        size_t page_idx = first_page + page;
        CXLHybridPublishedPageEntry *entry =
            cxl_hybrid_lookup_published_page_by_index(page_idx);

        if (!entry) {
            error_setg(errp,
                       "CXL hybrid postcopy publish remapped page %zu has no publish state slot",
                       page_idx);
            return -EINVAL;
        }

        entry->cxl_offset = block->pages_offset + block_offset +
                            (uint64_t)page * TARGET_PAGE_SIZE;
        entry->generation = generation;
        entry->valid = true;
        entry->ready = true;
        entry->source_remapped = true;
        cxl_hybrid_mark_page_cxl_visible(page_idx);
    }
    cxl_hybrid_ctrl_publish_pages_visible(first_page, npages, generation);

    qatomic_inc(&cxl_state.region_publish_requests);
    qatomic_add(&cxl_state.region_publish_pages, npages);
    qatomic_add(&cxl_state.warm_publish_pages, npages);
    qatomic_add(&cxl_state.publish_skip_ready, npages);
    cxl_hybrid_record_publish_source(CXL_HYBRID_PUBLISH_SOURCE_COMPLETION,
                                     npages);
    *published_pagesp += npages;
    return 0;
}

static int cxl_hybrid_publish_precopy_remapped_regions_locked(Error **errp)
{
    uint64_t start_ns;
    uint32_t generation;
    size_t region_idx;
    size_t published_pages = 0;
    int ret;

    if (!cxl_remap_active()) {
        return 0;
    }
    if (!cxl_state.remapped_bmap || !cxl_state.remapped_pages_bmap ||
        !cxl_state.published_page_state) {
        error_setg(errp,
                   "CXL hybrid postcopy remapped region publish state is not initialized");
        return -EINVAL;
    }

    start_ns = cxl_now_ns();
    generation = cxl_hybrid_fault_publish_generation();
    region_idx = find_next_bit(cxl_state.remapped_bmap,
                               cxl_state.source_remap_regions, 0);
    while (region_idx < cxl_state.source_remap_regions) {
        ret = cxl_publish_remapped_region_for_postcopy(
            region_idx, generation, &published_pages, errp);
        if (ret) {
            return ret;
        }
        region_idx = find_next_bit(cxl_state.remapped_bmap,
                                   cxl_state.source_remap_regions,
                                   region_idx + 1);
    }

    qatomic_add(&cxl_state.region_publish_time_ns, cxl_now_ns() - start_ns);
    return 0;
}

static int cxl_hybrid_publish_staged_run_for_postcopy(RAMBlock *block,
                                                      size_t first_page,
                                                      size_t npages,
                                                      uint32_t generation,
                                                      size_t *published_pagesp,
                                                      Error **errp)
{
    size_t block_first_page;
    size_t page;

    if (!block || !published_pagesp || !npages) {
        error_setg(errp,
                   "CXL hybrid postcopy publish staged run missing arguments");
        return -EINVAL;
    }
    if (!cxl_state.published_page_state || first_page >= cxl_state.total_pages) {
        error_setg(errp,
                   "CXL hybrid postcopy publish staged run missing page state");
        return -EINVAL;
    }

    block_first_page = block->offset >> TARGET_PAGE_BITS;
    for (page = 0; page < npages; page++) {
        size_t page_idx = first_page + page;
        size_t local_page = page_idx - block_first_page;
        CXLHybridPublishedPageEntry *entry;
        uint64_t cxl_offset;

        if (page_idx >= cxl_state.total_pages) {
            break;
        }
        if (block->bmap && test_bit(local_page, block->bmap)) {
            break;
        }
        if (!test_bit(page_idx, cxl_state.migrated_bmap)) {
            break;
        }
        entry = cxl_hybrid_lookup_published_page_by_index(page_idx);
        if (!entry) {
            error_setg(errp,
                       "CXL hybrid postcopy publish staged page %zu has no publish state slot",
                       page_idx);
            return -EINVAL;
        }
        cxl_offset = block->pages_offset + ((uint64_t)local_page << TARGET_PAGE_BITS);
        entry->cxl_offset = cxl_offset;
        entry->generation = generation;
        entry->valid = true;
        entry->ready = true;
        entry->source_remapped = false;
        cxl_hybrid_mark_page_cxl_visible(page_idx);
    }

    if (!page) {
        return 0;
    }

    cxl_hybrid_note_cxl_publish_regions(
        block,
        (first_page - block_first_page) << TARGET_PAGE_BITS,
        (uint64_t)page << TARGET_PAGE_BITS);
    cxl_hybrid_ctrl_publish_pages_visible(first_page, page, generation);
    qatomic_add(&cxl_state.region_publish_pages, page);
    qatomic_add(&cxl_state.warm_publish_pages, page);
    qatomic_add(&cxl_state.publish_skip_ready, page);
    cxl_hybrid_record_publish_source(CXL_HYBRID_PUBLISH_SOURCE_COMPLETION,
                                     page);
    *published_pagesp += page;
    return 0;
}

int cxl_hybrid_publish_staged_pages_for_postcopy(Error **errp)
{
    RAMBlock *block;
    uint64_t start_ns;
    uint32_t generation;
    size_t published_pages = 0;
    int ret = 0;

    if (!cxl_state.hybrid_enabled || !cxl_state.active) {
        return 0;
    }
    if (!cxl_state.migrated_bmap || !cxl_state.published_page_state ||
        !cxl_state.remapped_pages_bmap) {
        error_setg(errp,
                   "CXL hybrid postcopy staged publish state is not initialized");
        return -EINVAL;
    }

    start_ns = cxl_now_ns();
    generation = cxl_hybrid_fault_publish_generation();

    cxl_hybrid_publish_transition_lock();
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_NOT_IGNORED(block) {
        size_t block_first_page = block->offset >> TARGET_PAGE_BITS;
        size_t block_pages = DIV_ROUND_UP(block->used_length, TARGET_PAGE_SIZE);
        size_t block_last_page = MIN(block_first_page + block_pages,
                                     cxl_state.total_pages);
        size_t page_idx;

        page_idx = find_next_bit(cxl_state.migrated_bmap, block_last_page,
                                 block_first_page);
        while (page_idx < block_last_page) {
            size_t local_page = page_idx - block_first_page;
            size_t scan_limit;
            size_t remapped_page;
            size_t dirty_page = block->bmap ?
                find_next_bit(block->bmap, block_pages, local_page) :
                block_pages;
            size_t run_end;
            size_t region_idx;

            if (test_bit(page_idx, cxl_state.remapped_pages_bmap)) {
                page_idx = find_next_zero_bit(cxl_state.remapped_pages_bmap,
                                              block_last_page, page_idx + 1);
                page_idx = find_next_bit(cxl_state.migrated_bmap,
                                         block_last_page, page_idx);
                continue;
            }
            if (dirty_page == local_page) {
                page_idx = find_next_bit(cxl_state.migrated_bmap,
                                         block_last_page, page_idx + 1);
                continue;
            }
            if (cxl_hybrid_region_index_from_block_offset(
                    block, local_page << TARGET_PAGE_BITS, &region_idx) &&
                !cxl_hybrid_region_cxl_bulk_allowed(region_idx)) {
                page_idx = find_next_bit(cxl_state.migrated_bmap,
                                         block_last_page, page_idx + 1);
                continue;
            }

            scan_limit = MIN(block_last_page, block_first_page + dirty_page);
            remapped_page = find_next_bit(cxl_state.remapped_pages_bmap,
                                          scan_limit, page_idx);
            scan_limit = MIN(scan_limit, remapped_page);
            run_end = find_next_zero_bit(cxl_state.migrated_bmap,
                                         scan_limit,
                                         page_idx + 1);
            ret = cxl_hybrid_publish_staged_run_for_postcopy(
                block, page_idx, run_end - page_idx, generation,
                &published_pages, errp);
            if (ret) {
                goto out;
            }
            page_idx = run_end < block_last_page ?
                find_next_bit(cxl_state.migrated_bmap, block_last_page,
                              MAX(run_end, page_idx + 1)) :
                block_last_page;
        }
    }

out:
    cxl_hybrid_publish_transition_unlock();
    qatomic_add(&cxl_state.region_publish_time_ns, cxl_now_ns() - start_ns);
    return ret;
}

int cxl_hybrid_begin_source_run_with_precopy_remaps(Error **errp)
{
    int ret;

    cxl_hybrid_publish_transition_lock();
    ret = cxl_hybrid_control_begin_source_run(errp);
    if (!ret) {
        ret = cxl_hybrid_publish_precopy_remapped_regions_locked(errp);
    }
    cxl_hybrid_publish_transition_unlock();
    return ret;
}

bool cxl_sender_access_begin(void)
{
    if (!cxl_remap_active() || !cxl_state.sender_sync_ready) {
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
    if (!cxl_remap_active() || !cxl_state.sender_sync_ready) {
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

    cxl_hybrid_clear_cleanup_snapshot();
    cxl_hybrid_reset_rdma_sidecar_stats();
    cxl_rdma_sidecar_stop();
    cxl_hybrid_rdma_sidecar_global_destroy();
    cxl_state.align = align;
    cxl_state.remap_granule = cxl_choose_fault_region_granule(align,
                                                              total_ram);
    cxl_state.source_remap_granule =
        cxl_choose_source_remap_granule(total_ram);
    cxl_state.total_pages = DIV_ROUND_UP(total_ram, TARGET_PAGE_SIZE);
    cxl_state.total_regions = DIV_ROUND_UP(total_ram, cxl_state.remap_granule);
    cxl_state.source_remap_regions =
        DIV_ROUND_UP(total_ram, cxl_state.source_remap_granule);
    cxl_state.hybrid_enabled = migrate_cxl_hybrid();
    if (migrate_cxl_rdma_sidecar()) {
        cxl_hybrid_rdma_sidecar_global_init(
            cxl_state.total_regions,
            DIV_ROUND_UP(cxl_state.remap_granule, TARGET_PAGE_SIZE));
        cxl_hybrid_set_rdma_sidecar_budget_stats(
            migrate_cxl_rdma_sidecar_max_inflight_regions(),
            migrate_cxl_rdma_sidecar_max_cover_percent());
    }
    cxl_state.phase = CXL_HYBRID_PHASE_DISABLED;
    if (cxl_state.hybrid_enabled) {
        cxl_state.phase = (migrate_cxl_clean_remap_enable() ||
                           cxl_hybrid_brake_first_enabled()) ?
                          CXL_HYBRID_PHASE_PRECOPY_BRAKE :
                          CXL_HYBRID_PHASE_PRECOPY_BULK;
    }
    cxl_state.switch_reason = CXL_MIGRATION_SWITCH_REASON_NONE;
    cxl_state.switch_iteration = 0;
    cxl_state.phase_transitions = cxl_state.hybrid_enabled ? 1 : 0;
    cxl_hybrid_fault_publish_generation_end_source_run();
    cxl_hybrid_ensure_publish_mutex();

    if (!cxl_write_redirect_enabled()) {
        warn_report("CXL write-redirect: disabled via %s", CXL_WRITE_REDIRECT_ENV);
        return;
    }

    cxl_state.fd = dup(fd);
    if (cxl_state.fd < 0) {
        warn_report("CXL write-redirect: failed to dup fd, disabled");
        return;
    }
    cxl_state.mmap_size = cxl_hybrid_mapped_ram_required_bytes(align);
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
    cxl_state.warm_sent_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.dst_sent_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.cxl_visible_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.remaining_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.warm_dirty_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.pending_remap_bmap = bitmap_new(cxl_state.source_remap_regions);
    cxl_state.clean_epoch_seen_bmap =
        bitmap_new(cxl_state.source_remap_regions);
    cxl_state.clean_candidate_bmap =
        bitmap_new(cxl_state.source_remap_regions);
    cxl_state.clean_inflight_bmap =
        bitmap_new(cxl_state.source_remap_regions);
    cxl_state.remapped_bmap = bitmap_new(cxl_state.source_remap_regions);
    cxl_state.remapped_pages_bmap = bitmap_new(cxl_state.total_pages);
    cxl_clean_remap_init_remaining_bmap();
    cxl_state.published_page_state = g_new0(CXLHybridPublishedPageEntry,
                                            cxl_state.total_pages);
    qemu_mutex_init(&cxl_state.sender_sync_mutex);
    qemu_cond_init(&cxl_state.sender_sync_cond);
    cxl_state.sender_sync_ready = true;
    cxl_state.active = true;
    cxl_hybrid_prefault_start();
}

static void cxl_remap_state_cleanup(void)
{
    RAMBlock *block;

    if (!cxl_state.active && !cxl_state.publish_mutex_ready) {
        return;
    }

    cxl_state.phase = CXL_HYBRID_PHASE_CLEANUP;

    cxl_hybrid_prefault_wait_before_postcopy();
    cxl_sender_access_shutdown();

    if (cxl_state.active) {
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
                                                    block_last_page,
                                                    page_idx + 1);
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
    }

    if (cxl_state.mmap_base) {
        munmap(cxl_state.mmap_base, cxl_state.mmap_size);
    }
    if (cxl_state.fd >= 0) {
        close(cxl_state.fd);
    }
    if (cxl_state.sender_sync_ready) {
        qemu_cond_destroy(&cxl_state.sender_sync_cond);
        qemu_mutex_destroy(&cxl_state.sender_sync_mutex);
    }
    if (cxl_state.backing_rate_mutex_initialized) {
        qemu_mutex_destroy(&cxl_state.backing_rate_mutex);
    }
    cxl_hybrid_latch_cleanup_snapshot();
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_destroy(&cxl_state.publish_mutex);
    }
    g_free(cxl_state.warm_last_miss_ramblock);
    g_free(cxl_state.last_publish_request.ramblock);
    g_free(cxl_state.last_publish_wait_begin.ramblock);
    g_free(cxl_state.last_publish_wait_complete.ramblock);
    g_free(cxl_state.migrated_bmap);
    g_free(cxl_state.warm_sent_bmap);
    g_free(cxl_state.dst_sent_bmap);
    g_free(cxl_state.cxl_visible_bmap);
    g_free(cxl_state.remaining_bmap);
    g_free(cxl_state.warm_dirty_bmap);
    g_free(cxl_state.pending_remap_bmap);
    g_free(cxl_state.clean_epoch_seen_bmap);
    g_free(cxl_state.clean_candidate_bmap);
    g_free(cxl_state.clean_inflight_bmap);
    g_free(cxl_state.remapped_bmap);
    g_free(cxl_state.remapped_pages_bmap);
    g_free(cxl_state.published_page_state);
    cxl_rdma_sidecar_stop();
    cxl_hybrid_rdma_sidecar_global_destroy();
    cxl_hybrid_prefault_reset();
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

static bool cxl_lookup_source_remap_region(size_t region_idx,
                                           RAMBlock **blockp,
                                           ram_addr_t *block_offsetp)
{
    RAMBlock *block;
    ram_addr_t global_offset = region_idx * cxl_state.source_remap_granule;

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

static bool cxl_source_remap_region_is_clean(size_t region_idx,
                                             RAMBlock **blockp,
                                             ram_addr_t *block_offsetp,
                                             size_t *first_pagep,
                                             size_t *npagesp,
                                             uint64_t *unmigrated_pagesp,
                                             uint64_t *dirty_pagesp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    size_t first_page;
    size_t npages;
    size_t block_first_page;
    uint64_t unmigrated_pages = 0;
    uint64_t dirty_pages = 0;
    size_t page;

    if (region_idx >= cxl_state.source_remap_regions ||
        !cxl_lookup_source_remap_region(region_idx, &block, &block_offset)) {
        return false;
    }

    cxl_source_remap_region_page_span(block, block_offset, &first_page,
                                      &npages);
    block_first_page = block->offset >> TARGET_PAGE_BITS;
    if (!block->bmap || first_page < block_first_page) {
        return false;
    }

    for (page = 0; page < npages; page++) {
        if (!test_bit(first_page + page, cxl_state.migrated_bmap)) {
            unmigrated_pages++;
        }
    }
    dirty_pages = bitmap_count_one_with_offset(block->bmap,
                                               first_page - block_first_page,
                                               npages);

    if (unmigrated_pagesp) {
        *unmigrated_pagesp = unmigrated_pages;
    }
    if (dirty_pagesp) {
        *dirty_pagesp = dirty_pages;
    }

    if (unmigrated_pages || dirty_pages) {
        return false;
    }

    if (blockp) {
        *blockp = block;
    }
    if (block_offsetp) {
        *block_offsetp = block_offset;
    }
    if (first_pagep) {
        *first_pagep = first_page;
    }
    if (npagesp) {
        *npagesp = npages;
    }
    return true;
}

static bool cxl_clean_remap_lookup_region(size_t region_idx,
                                          RAMBlock **blockp,
                                          ram_addr_t *block_offsetp,
                                          size_t *first_pagep,
                                          size_t *npagesp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    ram_addr_t global_offset = region_idx * cxl_state.source_remap_granule;
    size_t first_page;
    size_t npages;

    if (region_idx >= cxl_state.source_remap_regions) {
        qatomic_inc(&cxl_state.skip_out_of_range);
        return false;
    }

    RCU_READ_LOCK_GUARD();
    if (!cxl_lookup_source_remap_region(region_idx, &block, &block_offset)) {
        qatomic_inc(&cxl_state.skip_out_of_range);
        return false;
    }

    if (!QEMU_IS_ALIGNED(global_offset, cxl_state.source_remap_granule) ||
        !QEMU_IS_ALIGNED(block_offset, cxl_state.source_remap_granule)) {
        qatomic_inc(&cxl_state.skip_misaligned);
        return false;
    }

    if (block_offset + cxl_state.source_remap_granule > block->used_length) {
        qatomic_inc(&cxl_state.skip_partial_region);
        return false;
    }

    cxl_source_remap_region_page_span(block, block_offset, &first_page,
                                      &npages);
    if (!npages) {
        qatomic_inc(&cxl_state.skip_partial_region);
        return false;
    }

    *blockp = block;
    *block_offsetp = block_offset;
    *first_pagep = first_page;
    *npagesp = npages;
    return true;
}

static bool cxl_clean_remap_region_dirty_now(RAMBlock *block,
                                             size_t first_page,
                                             size_t npages)
{
    size_t block_first_page;

    if (!block || !block->bmap) {
        return true;
    }

    block_first_page = block->offset >> TARGET_PAGE_BITS;
    if (first_page < block_first_page) {
        return true;
    }

    return bitmap_count_one_with_offset(block->bmap,
                                        first_page - block_first_page,
                                        npages) != 0;
}

static const char *cxl_clean_remap_debug_mode(void)
{
    const char *mode = g_getenv(CXL_CLEAN_REMAP_DEBUG_ENV);

    return mode && mode[0] ? mode : NULL;
}

static bool cxl_clean_remap_debug_scan_only(void)
{
    return cxl_hybrid_clean_remap_debug_scan_only(
        cxl_clean_remap_debug_mode());
}

static bool cxl_clean_remap_debug_copy_only(void)
{
    return cxl_hybrid_clean_remap_debug_copy_only(
        cxl_clean_remap_debug_mode());
}

static bool cxl_clean_remap_debug_read_only(void)
{
    return cxl_hybrid_clean_remap_debug_read_only(
        cxl_clean_remap_debug_mode());
}

static bool cxl_clean_remap_debug_write_only(void)
{
    return cxl_hybrid_clean_remap_debug_write_only(
        cxl_clean_remap_debug_mode());
}

static bool cxl_clean_remap_debug_no_publish(void)
{
    return cxl_clean_remap_debug_copy_only() ||
           cxl_clean_remap_debug_read_only() ||
           cxl_clean_remap_debug_write_only();
}

static bool cxl_clean_remap_debug_defer_remap(void)
{
    return cxl_hybrid_clean_remap_debug_defer_remap(
        cxl_clean_remap_debug_mode());
}

static int cxl_clean_remap_prefault_region(uint8_t *dst, size_t region_len,
                                           Error **errp)
{
    CXLCleanRemapPrefaultMode mode =
        migrate_cxl_clean_remap_prefault_mode();
    uint64_t start_ns;
    uint64_t elapsed_ns;
    int ret = 0;

    (void)errp;

    if (!dst || !region_len ||
        !cxl_hybrid_clean_remap_prefault_enabled(mode) ||
        cxl_clean_remap_debug_no_publish()) {
        return 0;
    }

    start_ns = cxl_now_ns();
    if (mode == CXL_CLEAN_REMAP_PREFAULT_MODE_MADVISE) {
        if (qemu_madvise(dst, region_len, QEMU_MADV_POPULATE_WRITE)) {
            ret = -errno;
        }
    } else if (mode == CXL_CLEAN_REMAP_PREFAULT_MODE_TOUCH) {
        size_t page_size = qemu_real_host_page_size();
        size_t offset;

        for (offset = 0; offset < region_len; offset += page_size) {
            volatile uint8_t *page = dst + offset;
            uint8_t value = *page;

            *page = value;
        }
    }

    elapsed_ns = cxl_now_ns() - start_ns;
    qatomic_add(&cxl_state.clean_remap_prefault_time_ns, elapsed_ns);
    if (ret < 0) {
        qatomic_inc(&cxl_state.clean_remap_prefault_errors);
        return 0;
    }

    qatomic_add(&cxl_state.clean_remap_prefault_bytes, region_len);
    return 0;
}

static int cxl_clean_remap_copy_region(RAMBlock *block,
                                       ram_addr_t block_offset,
                                       size_t region_len,
                                       uint64_t budget_used,
                                       Error **errp)
{
    uint64_t cxl_offset;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint8_t *src;
    uint8_t *dst;
    int ret;

    if (!block || block_offset + region_len > block->used_length) {
        error_setg(errp, "invalid CXL clean-remap copy region");
        return -EINVAL;
    }

    cxl_offset = block->pages_offset + block_offset;
    if (!cxl_state.mmap_base ||
        cxl_offset + region_len > cxl_state.mmap_size) {
        error_setg(errp,
                   "CXL clean-remap copy %s/0x%" PRIx64
                   " missing writable CXL backing",
                   block->idstr, (uint64_t)block_offset);
        return -ENODEV;
    }

    src = block->host + block_offset;
    dst = (uint8_t *)cxl_state.mmap_base + cxl_offset;

    ret = cxl_clean_remap_prefault_region(dst, region_len, errp);
    if (ret < 0) {
        return ret;
    }

    trace_cxl_hybrid_clean_remap_copy_begin(block->idstr, block_offset,
                                            region_len, budget_used);
    start_ns = cxl_now_ns();
    trace_cxl_hybrid_clean_remap_copy_begin_ts(
        start_ns, block->idstr, block_offset, region_len, budget_used);
    if (cxl_clean_remap_debug_read_only()) {
        g_autofree uint8_t *scratch = g_malloc(region_len);

        memcpy(scratch, src, region_len);
    } else if (cxl_clean_remap_debug_write_only()) {
        g_autofree uint8_t *scratch = g_malloc0(region_len);

        memcpy(dst, scratch, region_len);
    } else {
        memcpy(dst, src, region_len);
    }
    elapsed_ns = cxl_now_ns() - start_ns;
    qatomic_add(&cxl_state.clean_remap_copy_time_ns, elapsed_ns);
    qatomic_add(&cxl_state.publish_memcpy_time_ns, elapsed_ns);
    qatomic_add(&cxl_state.publish_memcpy_bytes, region_len);
    qatomic_add(&cxl_state.clean_remap_copy_bytes, region_len);
    trace_cxl_hybrid_clean_remap_copy_end(block->idstr, block_offset,
                                          region_len, 0, elapsed_ns);
    trace_cxl_hybrid_clean_remap_copy_end_ts(
        cxl_now_ns(), block->idstr, block_offset, region_len, 0, elapsed_ns);
    return 0;
}

static void cxl_clean_remap_publish_and_remap(size_t region_idx,
                                              RAMBlock *block,
                                              ram_addr_t block_offset,
                                              size_t first_page,
                                              size_t npages)
{
    size_t page;

    for (page = 0; page < npages; page++) {
        size_t page_idx = first_page + page;

        cxl_hybrid_mark_page_staged(page_idx);
        cxl_hybrid_mark_page_remaining(page_idx);
    }

    cxl_try_remap_region(region_idx);
    if (cxl_state.remapped_bmap &&
        test_bit(region_idx, cxl_state.remapped_bmap)) {
        cxl_hybrid_account_dst_pages_sent(
            block, block_offset, cxl_state.source_remap_granule,
            cxl_hybrid_fault_publish_generation());
    }
}

void cxl_hybrid_clean_remap_scan_region(RAMBlock *block,
                                        ram_addr_t block_offset,
                                        size_t region_len,
                                        bool dirty_now)
{
    size_t region_idx;
    bool epoch_seen;
    bool already_remapped;
    bool in_flight;
    bool candidate;

    if (!cxl_hybrid_clean_remap_enabled() || !block ||
        !cxl_state.clean_epoch_seen_bmap ||
        !cxl_state.clean_candidate_bmap ||
        !cxl_state.clean_inflight_bmap ||
        !cxl_state.remapped_bmap) {
        return;
    }

    region_idx = cxl_source_remap_region_index(block->offset + block_offset);
    if (region_idx >= cxl_state.source_remap_regions) {
        qatomic_inc(&cxl_state.skip_out_of_range);
        return;
    }

    epoch_seen = test_bit(region_idx, cxl_state.clean_epoch_seen_bmap);
    already_remapped = test_bit(region_idx, cxl_state.remapped_bmap);
    in_flight = test_bit(region_idx, cxl_state.clean_inflight_bmap);
    candidate = cxl_hybrid_clean_remap_region_is_candidate(
        epoch_seen, dirty_now, already_remapped, in_flight);

    set_bit_atomic(region_idx, cxl_state.clean_epoch_seen_bmap);
    if (dirty_now) {
        clear_bit_atomic(region_idx, cxl_state.clean_candidate_bmap);
    } else if (candidate) {
        if (!test_and_set_bit(region_idx, cxl_state.clean_candidate_bmap)) {
            qatomic_inc(&cxl_state.clean_remap_candidate_regions);
        }
    }

    trace_cxl_hybrid_clean_remap_scan_region(block->idstr, block_offset,
                                             region_len, epoch_seen,
                                             dirty_now, candidate);
}

bool cxl_hybrid_clean_remap_has_candidates(void)
{
    return cxl_hybrid_clean_remap_enabled() &&
           cxl_state.clean_candidate_bmap &&
           find_first_bit(cxl_state.clean_candidate_bmap,
                          cxl_state.source_remap_regions) <
           cxl_state.source_remap_regions;
}

static void cxl_clean_remap_clear_inflight_regions(void)
{
    size_t region_idx;

    if (!cxl_state.clean_inflight_bmap) {
        return;
    }

    while ((region_idx = find_first_bit(cxl_state.clean_inflight_bmap,
                                        cxl_state.source_remap_regions)) <
           cxl_state.source_remap_regions) {
        clear_bit_atomic(region_idx, cxl_state.clean_inflight_bmap);
    }
}

static void cxl_clean_remap_finalize_inflight_regions(void)
{
    size_t region_idx;
    uint64_t pause_start_ns;

    if (!cxl_state.clean_inflight_bmap ||
        find_first_bit(cxl_state.clean_inflight_bmap,
                       cxl_state.source_remap_regions) >=
        cxl_state.source_remap_regions) {
        return;
    }

    BQL_LOCK_GUARD();

    pause_all_vcpus();
    qatomic_inc(&cxl_state.remap_pause_calls);
    pause_start_ns = cxl_now_ns();
    ram_cxl_hybrid_sync_dirty_bitmap();

    while ((region_idx = find_first_bit(cxl_state.clean_inflight_bmap,
                                        cxl_state.source_remap_regions)) <
           cxl_state.source_remap_regions) {
        RAMBlock *block;
        ram_addr_t block_offset;
        size_t first_page;
        size_t npages;
        uint64_t region_len = cxl_state.source_remap_granule;

        if (!test_and_clear_bit(region_idx, cxl_state.clean_inflight_bmap)) {
            continue;
        }

        if (!cxl_clean_remap_lookup_region(region_idx, &block,
                                           &block_offset, &first_page,
                                           &npages)) {
            continue;
        }
        if (cxl_clean_remap_region_dirty_now(block, first_page, npages)) {
            qatomic_inc(&cxl_state.clean_remap_abandoned_dirty);
            trace_cxl_hybrid_clean_remap_abandon_dirty(block->idstr,
                                                       block_offset,
                                                       region_len);
            continue;
        }

        cxl_clean_remap_publish_and_remap(region_idx, block, block_offset,
                                          first_page, npages);
    }

    resume_all_vcpus();
    qatomic_add(&cxl_state.remap_pause_time_ns,
                cxl_now_ns() - pause_start_ns);
}

int cxl_hybrid_clean_remap_drain(Error **errp)
{
    uint64_t budget = migrate_cxl_clean_remap_copy_budget();
    uint64_t throttle_us = migrate_cxl_clean_remap_throttle_us();
    uint64_t used = 0;
    size_t region_idx;

    if (!cxl_hybrid_clean_remap_enabled() ||
        !cxl_state.clean_candidate_bmap) {
        return 0;
    }

    if (cxl_clean_remap_debug_scan_only()) {
        return 0;
    }

    if (!cxl_quiesce_senders_begin()) {
        return 0;
    }

    while ((region_idx = find_first_bit(cxl_state.clean_candidate_bmap,
                                        cxl_state.source_remap_regions)) <
           cxl_state.source_remap_regions) {
        RAMBlock *block;
        ram_addr_t block_offset;
        size_t first_page;
        size_t npages;
        uint64_t region_len = cxl_state.source_remap_granule;
        int ret;

        if (!cxl_hybrid_clean_remap_budget_allows(budget, used, region_len)) {
            qatomic_inc(&cxl_state.clean_remap_budget_exhaustions);
            trace_cxl_hybrid_clean_remap_budget_exhausted(budget, used);
            break;
        }

        clear_bit_atomic(region_idx, cxl_state.clean_candidate_bmap);
        if (test_and_set_bit(region_idx, cxl_state.clean_inflight_bmap)) {
            continue;
        }

        if (!cxl_clean_remap_lookup_region(region_idx, &block, &block_offset,
                                           &first_page, &npages)) {
            clear_bit_atomic(region_idx, cxl_state.clean_inflight_bmap);
            continue;
        }

        ret = cxl_clean_remap_copy_region(block, block_offset, region_len,
                                          used, errp);
        if (ret < 0) {
            clear_bit_atomic(region_idx, cxl_state.clean_inflight_bmap);
            cxl_quiesce_senders_end();
            return ret;
        }
        used += region_len;
        if (cxl_hybrid_clean_remap_should_throttle(throttle_us, true)) {
            uint64_t throttle_start_ns = cxl_now_ns();
            uint64_t throttle_end_ns;

            trace_cxl_hybrid_clean_remap_throttle_begin(throttle_start_ns,
                                                        throttle_us);
            g_usleep((gulong)MIN(throttle_us, (uint64_t)G_MAXULONG));
            throttle_end_ns = cxl_now_ns();
            trace_cxl_hybrid_clean_remap_throttle_end(
                throttle_end_ns, throttle_us,
                throttle_end_ns - throttle_start_ns);
        }
    }

    if (used) {
        if (cxl_clean_remap_debug_no_publish()) {
            cxl_clean_remap_clear_inflight_regions();
        } else if (!cxl_clean_remap_debug_defer_remap()) {
            cxl_clean_remap_finalize_inflight_regions();
        }
    }

    cxl_quiesce_senders_end();
    return 0;
}

void cxl_hybrid_clean_remap_finalize_deferred(void)
{
    if (cxl_hybrid_clean_remap_enabled() &&
        cxl_clean_remap_debug_defer_remap()) {
        cxl_clean_remap_finalize_inflight_regions();
    }
}

bool cxl_hybrid_clean_remap_defer_remap_enabled(void)
{
    return cxl_hybrid_clean_remap_enabled() &&
           cxl_clean_remap_debug_defer_remap();
}

bool cxl_hybrid_clean_remap_region_inflight(RAMBlock *block,
                                            ram_addr_t block_offset)
{
    size_t region_idx;

    if (!cxl_hybrid_clean_remap_defer_remap_enabled() ||
        !block || !cxl_state.clean_inflight_bmap ||
        !cxl_state.source_remap_granule) {
        return false;
    }

    region_idx = cxl_source_remap_region_index(block->offset + block_offset);
    if (region_idx >= cxl_state.source_remap_regions) {
        return false;
    }

    return test_bit(region_idx, cxl_state.clean_inflight_bmap);
}

static bool cxl_pending_remaps_have_clean_region(void)
{
    size_t region_idx;
    uint64_t pending_regions = 0;
    uint64_t clean_regions = 0;
    uint64_t unmigrated_pages = 0;
    uint64_t dirty_pages = 0;

    region_idx = find_first_bit(cxl_state.pending_remap_bmap,
                                cxl_state.source_remap_regions);
    while (region_idx < cxl_state.source_remap_regions) {
        uint64_t region_unmigrated_pages = 0;
        uint64_t region_dirty_pages = 0;

        pending_regions++;
        if (cxl_source_remap_region_is_clean(region_idx, NULL, NULL, NULL,
                                             NULL, &region_unmigrated_pages,
                                             &region_dirty_pages)) {
            clean_regions++;
        } else {
            unmigrated_pages += region_unmigrated_pages;
            dirty_pages += region_dirty_pages;
        }
        region_idx = find_next_bit(cxl_state.pending_remap_bmap,
                                   cxl_state.source_remap_regions,
                                   region_idx + 1);
    }

    qatomic_set(&cxl_state.pending_remap_regions, pending_regions);
    qatomic_set(&cxl_state.clean_pending_remap_regions, clean_regions);
    qatomic_set(&cxl_state.pending_remap_unmigrated_pages, unmigrated_pages);
    qatomic_set(&cxl_state.pending_remap_dirty_pages, dirty_pages);
    return clean_regions > 0;
}

static void cxl_try_remap_region(size_t region_idx)
{
    RAMBlock *block;
    size_t first_page;
    size_t npages;
    size_t region_len;
    ram_addr_t block_offset;
    ram_addr_t global_offset = region_idx * cxl_state.source_remap_granule;
    off_t cxl_offset;
    void *host_addr;
    void *ret;
    uint64_t t_start;

    if (!cxl_remap_active()) {
        return;
    }

    if (region_idx >= cxl_state.source_remap_regions) {
        qatomic_inc(&cxl_state.skip_out_of_range);
        return;
    }

    if (!cxl_lookup_source_remap_region(region_idx, &block, &block_offset)) {
        qatomic_inc(&cxl_state.skip_out_of_range);
        return;
    }

    if (!QEMU_IS_ALIGNED(global_offset, cxl_state.source_remap_granule) ||
        !QEMU_IS_ALIGNED(block_offset, cxl_state.source_remap_granule)) {
        qatomic_inc(&cxl_state.skip_misaligned);
        return;
    }

    if (block_offset + cxl_state.source_remap_granule > block->used_length) {
        qatomic_inc(&cxl_state.skip_partial_region);
        return;
    }

    if (test_bit(region_idx, cxl_state.remapped_bmap)) {
        qatomic_inc(&cxl_state.skip_already_remapped);
        return;
    }
    if (!cxl_hybrid_region_cxl_bulk_allowed(region_idx) ||
        !cxl_hybrid_region_try_own_cxl(region_idx)) {
        return;
    }

    qatomic_inc(&cxl_state.remap_page_check_calls);
    t_start = cxl_now_ns();
    if (!cxl_source_remap_region_is_clean(region_idx, &block, &block_offset,
                                          &first_page, &npages, NULL, NULL)) {
        qatomic_add(&cxl_state.remap_page_check_time_ns,
                    cxl_now_ns() - t_start);
        qatomic_inc(&cxl_state.skip_not_fully_migrated);
        return;
    }
    qatomic_add(&cxl_state.remap_page_check_time_ns, cxl_now_ns() - t_start);

    host_addr = block->host + block_offset;
    cxl_offset = block->pages_offset + block_offset;
    region_len = cxl_state.source_remap_granule;
    qatomic_inc(&cxl_state.remap_attempts);

    t_start = cxl_now_ns();
    ret = mmap(host_addr, region_len,
               PROT_READ | PROT_WRITE,
               MAP_FIXED | MAP_SHARED,
               cxl_state.fd, cxl_offset);
    qatomic_add(&cxl_state.remap_syscall_time_ns, cxl_now_ns() - t_start);
    if (ret == MAP_FAILED) {
        qatomic_inc(&cxl_state.remap_failures);
        warn_report("CXL remap failed at region %zu (offset 0x%lx): %s",
                    region_idx, (unsigned long)cxl_offset, strerror(errno));
        return;
    }

    cxl_mark_pages_remapped(region_idx, first_page, npages, cxl_offset);
    qatomic_inc(&cxl_state.remap_successes);
    qatomic_inc(&cxl_state.remapped_regions);
}

static void cxl_process_pending_remaps(void)
{
    size_t region_idx;
    bool profile_enabled =
        trace_event_get_state(TRACE_CXL_HYBRID_REMAP_DRAIN_PROFILE);
    uint64_t total_start_ns = 0;
    uint64_t op_start_ns = 0;
    uint64_t first_dirty_sync_ns = 0;
    uint64_t clean_scan_ns = 0;
    uint64_t quiesce_ns = 0;
    uint64_t pause_remap_ns = 0;
    uint64_t second_dirty_sync_ns = 0;
    uint64_t remap_loop_ns = 0;
    uint64_t regions_seen = 0;
    uint64_t regions_attempted = 0;
    uint64_t t_start;

    if (!cxl_remap_active()) {
        return;
    }

    region_idx = find_first_bit(cxl_state.pending_remap_bmap,
                                cxl_state.source_remap_regions);
    if (region_idx >= cxl_state.source_remap_regions) {
        return;
    }

    if (profile_enabled) {
        total_start_ns = cxl_now_ns();
        op_start_ns = total_start_ns;
    }
    ram_cxl_hybrid_sync_dirty_bitmap();
    if (profile_enabled) {
        first_dirty_sync_ns = cxl_now_ns() - op_start_ns;
        op_start_ns = cxl_now_ns();
    }
    if (!cxl_pending_remaps_have_clean_region()) {
        if (profile_enabled) {
            clean_scan_ns = cxl_now_ns() - op_start_ns;
            trace_cxl_hybrid_remap_drain_profile(
                cxl_now_ns(), cxl_now_ns() - total_start_ns,
                first_dirty_sync_ns, clean_scan_ns, quiesce_ns,
                pause_remap_ns, second_dirty_sync_ns, remap_loop_ns,
                regions_seen, regions_attempted);
        }
        return;
    }
    if (profile_enabled) {
        clean_scan_ns = cxl_now_ns() - op_start_ns;
        op_start_ns = cxl_now_ns();
    }

    if (!cxl_quiesce_senders_begin()) {
        if (profile_enabled) {
            quiesce_ns = cxl_now_ns() - op_start_ns;
            trace_cxl_hybrid_remap_drain_profile(
                cxl_now_ns(), cxl_now_ns() - total_start_ns,
                first_dirty_sync_ns, clean_scan_ns, quiesce_ns,
                pause_remap_ns, second_dirty_sync_ns, remap_loop_ns,
                regions_seen, regions_attempted);
        }
        return;
    }
    if (profile_enabled) {
        quiesce_ns = cxl_now_ns() - op_start_ns;
    }
    t_start = cxl_now_ns();
    pause_all_vcpus();
    qatomic_inc(&cxl_state.remap_pause_calls);
    if (profile_enabled) {
        op_start_ns = cxl_now_ns();
    }
    ram_cxl_hybrid_sync_dirty_bitmap();
    if (profile_enabled) {
        second_dirty_sync_ns = cxl_now_ns() - op_start_ns;
        op_start_ns = cxl_now_ns();
    }

    while ((region_idx = find_first_bit(cxl_state.pending_remap_bmap,
                                        cxl_state.source_remap_regions)) <
           cxl_state.source_remap_regions) {
        regions_seen++;
        if (!test_and_clear_bit(region_idx, cxl_state.pending_remap_bmap)) {
            continue;
        }
        regions_attempted++;
        cxl_try_remap_region(region_idx);
    }
    if (profile_enabled) {
        remap_loop_ns = cxl_now_ns() - op_start_ns;
    }

    resume_all_vcpus();
    pause_remap_ns = cxl_now_ns() - t_start;
    qatomic_add(&cxl_state.remap_pause_time_ns, pause_remap_ns);
    cxl_quiesce_senders_end();

    if (profile_enabled) {
        trace_cxl_hybrid_remap_drain_profile(
            cxl_now_ns(), cxl_now_ns() - total_start_ns, first_dirty_sync_ns,
            clean_scan_ns, quiesce_ns, pause_remap_ns, second_dirty_sync_ns,
            remap_loop_ns, regions_seen, regions_attempted);
    }
}

void cxl_hybrid_drain_source_remaps(void)
{
    Error *local_err = NULL;

    if (!cxl_state.hybrid_enabled ||
        cxl_state.phase != CXL_HYBRID_PHASE_PRECOPY_BRAKE ||
        !cxl_state.active || !cxl_state.pending_remap_bmap) {
        return;
    }

    if (cxl_hybrid_clean_remap_enabled()) {
        if (cxl_hybrid_clean_remap_drain(&local_err) < 0) {
            error_report_err(local_err);
        }
        return;
    }

    cxl_process_pending_remaps();
}

static void cxl_try_remap_range(RAMBlock *block, ram_addr_t block_offset,
                                size_t len)
{
    ram_addr_t range_start;
    ram_addr_t range_end;
    ram_addr_t region_offset;
    uint64_t t_start;

    if (!cxl_remap_active() || len == 0) {
        return;
    }

    qatomic_inc(&cxl_state.remap_scan_calls);
    t_start = cxl_now_ns();
    range_start = QEMU_ALIGN_DOWN(block_offset,
                                  cxl_state.source_remap_granule);
    range_end = MIN(block_offset + len, block->used_length);
    range_end = ROUND_UP(range_end, cxl_state.source_remap_granule);

    for (region_offset = range_start;
         region_offset < range_end && region_offset < block->used_length;
         region_offset += cxl_state.source_remap_granule) {
        size_t region_idx =
            cxl_source_remap_region_index(block->offset + region_offset);

        if (region_idx >= cxl_state.source_remap_regions) {
            qatomic_inc(&cxl_state.skip_out_of_range);
            continue;
        }
        if (test_bit(region_idx, cxl_state.remapped_bmap)) {
            continue;
        }
        if (!cxl_hybrid_region_cxl_bulk_allowed(region_idx)) {
            continue;
        }

        set_bit_atomic(region_idx, cxl_state.pending_remap_bmap);
    }
    qatomic_add(&cxl_state.remap_scan_time_ns, cxl_now_ns() - t_start);
}

static int cxl_ramblock_write_pages(QIOChannel *ioc, RAMBlock *block,
                                    const struct iovec *iov, int niov,
                                    uintptr_t base_offset, Error **errp)
{
    size_t iov_index = 0;
    size_t iov_offset = 0;
    uintptr_t page_offset = base_offset;

    while (iov_index < niov) {
        struct iovec out_iov[64];
        ram_addr_t out_offset = 0;
        size_t out_niov = 0;
        uint64_t t_start;
        ssize_t ret;

        while (iov_index < niov) {
            uint32_t generation = cxl_hybrid_fault_publish_generation();
            bool destination_owned =
                cxl_hybrid_source_region_owned_by_destination_generation(
                    block, page_offset, generation);
            bool source_remapped =
                cxl_hybrid_range_is_remapped(block, page_offset,
                                             TARGET_PAGE_SIZE);
            bool page_visible =
                cxl_hybrid_source_page_visible(block, page_offset, generation);
            MigrationPostcopyCXLRAMStreamWriteAction action =
                migration_postcopy_cxl_ram_stream_write_action(
                    destination_owned, source_remapped, page_visible);

            if (action == MIGRATION_POSTCOPY_CXL_RAM_STREAM_ERROR) {
                error_setg(errp,
                           "CXL hybrid mapped-RAM source write refused for "
                           "destination-owned invisible page %s/0x%" PRIxPTR,
                           block->idstr, page_offset);
                return -1;
            }

            if (action == MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE ||
                action ==
                    MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_ALREADY_VISIBLE) {
                trace_cxl_hybrid_ram_stream_skip_visible(block->idstr,
                                                        page_offset);
                page_offset += TARGET_PAGE_SIZE;
                iov_offset += TARGET_PAGE_SIZE;
                if (iov_offset == iov[iov_index].iov_len) {
                    iov_index++;
                    iov_offset = 0;
                }
                if (out_niov) {
                    break;
                }
                continue;
            }

            if (!out_niov) {
                out_offset = page_offset;
            }
            out_iov[out_niov++] = (struct iovec) {
                .iov_base = (uint8_t *)iov[iov_index].iov_base + iov_offset,
                .iov_len = TARGET_PAGE_SIZE,
            };
            page_offset += TARGET_PAGE_SIZE;
            iov_offset += TARGET_PAGE_SIZE;
            if (iov_offset == iov[iov_index].iov_len) {
                iov_index++;
                iov_offset = 0;
            }
            if (out_niov == G_N_ELEMENTS(out_iov)) {
                break;
            }
        }

        if (!out_niov) {
            continue;
        }

        cxl_backing_rate_limit(iov_size(out_iov, out_niov));
        t_start = cxl_now_ns();
        ret = qio_channel_pwritev(ioc, out_iov, out_niov,
                                  block->pages_offset + out_offset, errp);
        qatomic_add(&cxl_state.backing_write_time_ns,
                    cxl_now_ns() - t_start);
        if (ret < 0) {
            return -1;
        }
        qatomic_inc(&cxl_state.backing_write_calls);
        qatomic_add(&cxl_state.backing_write_bytes, (uint64_t)ret);
    }

    return 0;
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

        if (cxl_state.active) {
            ret = cxl_ramblock_write_pages(ioc, block, &iov[slice_idx],
                                           slice_num, offset, errp);
        } else {
            cxl_backing_rate_limit(iov_size(&iov[slice_idx], slice_num));
            t_start = cxl_now_ns();
            ret = qio_channel_pwritev(ioc, &iov[slice_idx], slice_num,
                                      block->pages_offset + offset, errp);
            qatomic_add(&cxl_state.backing_write_time_ns,
                        cxl_now_ns() - t_start);
        }
        if (ret < 0) {
            break;
        }
        if (!cxl_state.active && ret > 0) {
            qatomic_inc(&cxl_state.backing_write_calls);
            qatomic_add(&cxl_state.backing_write_bytes, (uint64_t)ret);
        }

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
                    cxl_hybrid_mark_page_staged(first_page + p);
                    cxl_hybrid_mark_page_remaining(first_page + p);
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

    if (!cxl_remap_active()) {
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

    if (!cxl_remap_active() || !block->bmap) {
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

void cxl_hybrid_sync_rdma_dirty_for_postcopy(void)
{
    RAMBlock *block;

    if (!migrate_cxl_rdma_sidecar()) {
        return;
    }

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH_NOT_IGNORED(block) {
        unsigned long pages;
        ram_addr_t offset;

        if (!block->bmap || !cxl_state.remap_granule ||
            !QEMU_IS_ALIGNED(cxl_state.remap_granule, TARGET_PAGE_SIZE)) {
            continue;
        }

        pages = DIV_ROUND_UP(block->used_length, TARGET_PAGE_SIZE);
        for (offset = 0;
             offset + cxl_state.remap_granule <= block->used_length;
             offset += cxl_state.remap_granule) {
            size_t region_idx;
            unsigned long first_page = offset >> TARGET_PAGE_BITS;
            unsigned long npages = cxl_state.remap_granule >> TARGET_PAGE_BITS;

            if (!cxl_hybrid_region_index_from_block_offset(block, offset,
                                                           &region_idx) ||
                first_page >= pages) {
                continue;
            }

            npages = MIN(npages, pages - first_page);
            if (bitmap_count_one_with_offset(block->bmap, first_page,
                                             npages)) {
                cxl_hybrid_invalidate_region_rdma_ready(region_idx);
            }
        }
    }
}

bool cxl_hybrid_init_source(void)
{
    Error *local_err = NULL;
    int ret;

    if (!cxl_state.hybrid_enabled) {
        return false;
    }
    if (!migrate_cxl_shared_backing()) {
        error_report("CXL hybrid postcopy requires x-cxl-shared-backing=true");
        return false;
    }
    ret = cxl_hybrid_control_init_source(&local_err);
    if (ret) {
        error_report_err(local_err);
        return false;
    }
    return true;
}

void cxl_hybrid_enter_phase(CXLHybridPhase phase,
                            CXLMigrationSwitchReason reason,
                            uint64_t iteration)
{
    CXLHybridPhase old_phase = cxl_state.phase;

    if (!cxl_state.hybrid_enabled || cxl_state.phase == phase) {
        if (reason != CXL_MIGRATION_SWITCH_REASON_NONE) {
            cxl_state.switch_reason = reason;
            cxl_state.switch_iteration = iteration;
        }
        return;
    }

    cxl_state.phase = phase;
    qatomic_inc(&cxl_state.phase_transitions);
    trace_cxl_hybrid_phase_transition(old_phase, phase, reason, iteration);
    if (reason != CXL_MIGRATION_SWITCH_REASON_NONE) {
        cxl_state.switch_reason = reason;
        cxl_state.switch_iteration = iteration;
    }
}

void cxl_hybrid_cleanup_source(void)
{
    cxl_hybrid_control_cleanup_source();
    cxl_hybrid_fault_publish_generation_end_source_run();
    cxl_cleanup_outgoing_migration();
}

bool cxl_hybrid_enabled(void)
{
    return cxl_state.hybrid_enabled;
}

CXLHybridPhase cxl_hybrid_phase(void)
{
    return cxl_state.phase;
}

uint64_t cxl_hybrid_source_staged_pages(void)
{
    if (!cxl_state.hybrid_enabled) {
        return 0;
    }

    return qatomic_read(&cxl_state.staged_pages);
}

uint8_t cxl_hybrid_source_remap_coverage(void)
{
    uint64_t staged_pages = cxl_hybrid_source_staged_pages();
    uint64_t remapped_pages;

    if (!cxl_state.hybrid_enabled || !staged_pages ||
        !cxl_state.remapped_pages_bmap) {
        return 0;
    }

    remapped_pages = bitmap_count_one(cxl_state.remapped_pages_bmap,
                                      cxl_state.total_pages);
    return cxl_hybrid_calculate_source_remap_coverage(staged_pages,
                                                      remapped_pages);
}

bool cxl_hybrid_clean_remap_enabled(void)
{
    return cxl_state.hybrid_enabled &&
           cxl_state.active &&
           migrate_cxl_clean_remap_enable() &&
           cxl_state.mmap_base &&
           cxl_state.remaining_bmap &&
           cxl_state.clean_epoch_seen_bmap &&
           cxl_state.clean_candidate_bmap &&
           cxl_state.clean_inflight_bmap &&
           cxl_state.remapped_bmap &&
           cxl_state.remapped_pages_bmap &&
           cxl_state.source_remap_granule;
}

uint64_t cxl_hybrid_clean_remap_pending_bytes(void)
{
    uint64_t remaining_pages;

    if (!cxl_hybrid_clean_remap_enabled() || !cxl_state.remaining_bmap) {
        return 0;
    }

    remaining_pages = bitmap_count_one(cxl_state.remaining_bmap,
                                       cxl_state.total_pages);
    return remaining_pages * TARGET_PAGE_SIZE;
}

uint8_t cxl_hybrid_clean_remap_coverage(void)
{
    uint64_t remapped_pages;

    if (!cxl_hybrid_clean_remap_enabled() || !cxl_state.total_pages ||
        !cxl_state.remapped_pages_bmap) {
        return 0;
    }

    remapped_pages = bitmap_count_one(cxl_state.remapped_pages_bmap,
                                      cxl_state.total_pages);
    return cxl_hybrid_calculate_source_remap_coverage(cxl_state.total_pages,
                                                      remapped_pages);
}

uint64_t cxl_hybrid_source_remap_granule(void)
{
    return cxl_state.source_remap_granule;
}

void cxl_hybrid_clean_remap_note_scan(void)
{
    if (cxl_hybrid_clean_remap_enabled()) {
        qatomic_inc(&cxl_state.clean_remap_scan_calls);
    }
}
