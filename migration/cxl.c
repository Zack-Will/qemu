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
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-migration.h"
#include "channel.h"
#include "cxl.h"
#include "migration.h"
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

typedef struct CXLHybridPendingReady {
    CXLHybridPublishNotify notify;
    uint64_t queued_at_ns;
    bool fault_primary;
    QSIMPLEQ_ENTRY(CXLHybridPendingReady) next;
} CXLHybridPendingReady;

typedef struct CXLHybridWarmDescBatchBuilder {
    MigrationState *s;
    CXLHybridWarmDescBatch batch;
    size_t encoded_len;
    uint32_t pages;
    bool have_open_range;
    CXLHybridWarmDescRange open_range;
} CXLHybridWarmDescBatchBuilder;

typedef struct CXLHybridLastPublishRequestInfo {
    uint64_t count;
    uint64_t guest_offset;
    uint32_t generation;
    char *ramblock;
} CXLHybridLastPublishRequestInfo;

typedef struct CXLHybridLastPublishReadyInfo {
    uint64_t count;
    uint64_t guest_offset;
    uint64_t cxl_offset;
    uint32_t generation;
    char *ramblock;
} CXLHybridLastPublishReadyInfo;

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

typedef struct CXLHybridFaultWaitRecord {
    uint64_t wait_begin_ns;
    uint64_t ready_recv_ns;
} CXLHybridFaultWaitRecord;

static CXLIncomingMetadataState cxl_incoming_meta_state;
static size_t cxl_global_page_index(RAMBlock *block, ram_addr_t block_offset);
static size_t cxl_global_region_index(ram_addr_t global_offset);
static int cxl_destination_staging_init(Error **errp);
static void cxl_destination_staging_cleanup(void);
static bool cxl_hybrid_page_eligible(size_t page_idx);
static uint64_t cxl_hybrid_prefetch_rate_limit(void);
static void cxl_hybrid_warm_sleep(uint32_t pages_sent);
static bool cxl_hybrid_warm_disabled(void);
static int cxl_hybrid_stream_ready_consumer(
    const CXLHybridFaultReadyRecord *record, Error **errp);
static void cxl_hybrid_queue_publish_ready(const char *ramblock,
                                           uint64_t guest_offset,
                                           uint64_t cxl_offset,
                                           uint32_t page_len,
                                           uint32_t generation,
                                           bool fault_primary);
static char *cxl_hybrid_publish_wait_key(const char *ramblock,
                                         uint64_t guest_offset,
                                         uint32_t generation);
static void cxl_hybrid_invalidate_published_page(const char *ramblock,
                                                 uint64_t guest_offset);
static bool cxl_hybrid_range_is_remapped(RAMBlock *block,
                                         uint64_t guest_offset,
                                         uint32_t page_len);
static void cxl_hybrid_ensure_publish_mutex(void);
static void cxl_hybrid_ensure_fault_wait_records(void);

#define CXL_HYBRID_WARM_DESC_BATCH_HEADER_LEN (4 + 4)
#define CXL_HYBRID_WARM_DESC_BATCH_ENTRY_HEADER_LEN (1 + 8 + 8 + 4 + 4)
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
    unsigned long *remapped_bmap;
    unsigned long *remapped_pages_bmap;
    QEMUBH *remap_bh;
    size_t total_pages;
    size_t total_regions;
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
    uint64_t phase_transitions;
    uint64_t switch_iteration;
    uint64_t source_heat_updates;
    uint64_t source_warm_queue_pages;
    uint64_t source_warm_sent_pages;
    uint64_t source_warm_sent_bytes;
    uint64_t source_warm_desc_sent_pages;
    uint64_t source_warm_desc_sent_bytes;
    uint64_t source_warm_payload_fallback_pages;
    uint64_t source_warm_desc_skip_unremapped;
    uint64_t source_warm_skip_received;
    uint64_t source_warm_skip_unstaged;
    uint64_t source_warm_last_miss_offset;
    uint64_t warm_publish_pages;
    uint64_t warm_push_publish_pages;
    uint64_t fault_primary_publish_pages;
    uint64_t fault_burst_publish_pages;
    uint64_t pending_ready_publish_pages;
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
    uint64_t fault_primary_ready_drain_samples;
    uint64_t fault_primary_ready_drain_time_ns;
    uint64_t max_fault_primary_ready_drain_time_ns;
    uint64_t fault_primary_ready_write_samples;
    uint64_t fault_primary_ready_write_time_ns;
    uint64_t max_fault_primary_ready_write_time_ns;
    uint64_t fault_primary_ready_recv_samples;
    uint64_t fault_primary_ready_recv_time_ns;
    uint64_t max_fault_primary_ready_recv_time_ns;
    uint64_t fault_primary_ready_handle_samples;
    uint64_t fault_primary_ready_handle_time_ns;
    uint64_t max_fault_primary_ready_handle_time_ns;
    uint64_t fault_primary_ready_send_samples;
    uint64_t fault_primary_ready_send_time_ns;
    uint64_t max_fault_primary_ready_send_time_ns;
    uint64_t fault_wait_ready_recv_samples;
    uint64_t fault_wait_ready_recv_time_ns;
    uint64_t max_fault_wait_ready_recv_time_ns;
    uint64_t fault_wait_after_ready_recv_samples;
    uint64_t fault_wait_after_ready_recv_time_ns;
    uint64_t max_fault_wait_after_ready_recv_time_ns;
    uint64_t pending_publish_ready;
    uint64_t completion_pending_publish_ready;
    uint64_t publish_ready_sent_pages;
    CXLHybridLastPublishRequestInfo last_publish_request;
    CXLHybridLastPublishReadyInfo last_publish_ready;
    CXLHybridLastPublishReadyInfo last_completion_publish_ready;
    CXLHybridLastPublishReadyInfo last_publish_ready_recv;
    CXLHybridLastPublishWaitInfo last_publish_wait_begin;
    CXLHybridLastPublishWaitInfo last_publish_wait_complete;
    GHashTable *fault_wait_records;
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
        uint64_t pending_ready_publish_pages;
        uint64_t completion_publish_pages;
        uint64_t publish_ready_sent_pages;
        CXLHybridPhase phase;
    } iter_begin;
    uint64_t last_iterate_ram_pages;
    uint64_t last_iterate_staged_pages_delta;
    uint64_t last_iterate_warm_push_pages;
    uint64_t last_iterate_fault_primary_pages;
    uint64_t last_iterate_fault_burst_pages;
    uint64_t last_iterate_publish_ready_pages;
    CXLHybridPhase last_iterate_phase;
    size_t warm_scan_cursor;
    ram_addr_t warm_last_miss_offset;
    char *warm_last_miss_ramblock;
    CXLHybridPublishedPageEntry *published_page_state;
    QSIMPLEQ_HEAD(, CXLHybridPendingReady) pending_ready;
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
    .pending_ready = QSIMPLEQ_HEAD_INITIALIZER(cxl_state.pending_ready),
};

static void cxl_remap_state_init(int fd, uint64_t align, int64_t dev_size);
static void cxl_process_pending_remaps_bh(void *opaque);

#define CXL_BACKING_ALIGN_FALLBACK (2 * 1024 * 1024)
#define CXL_REMAP_GRANULE_DEFAULT (64 * 1024)
#define CXL_ROLLBACK_COPY_CHUNK (2 * 1024 * 1024)
#define CXL_WRITE_REDIRECT_ENV "QEMU_CXL_WRITE_REDIRECT"
#define CXL_REMAP_GRANULE_ENV "QEMU_CXL_REMAP_GRANULE"
#define CXL_HYBRID_WARM_DISABLE_ENV "QEMU_CXL_HYBRID_WARM_DISABLE"
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

static char *cxl_hybrid_publish_wait_key(const char *ramblock,
                                         uint64_t guest_offset,
                                         uint32_t generation)
{
    return g_strdup_printf("%s:0x%" PRIx64 ":%u", ramblock, guest_offset,
                           generation);
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

static CXLHybridFaultWaitRecord *
cxl_hybrid_lookup_fault_wait_record_locked(const char *ramblock,
                                           uint64_t guest_offset,
                                           uint32_t generation,
                                           bool create)
{
    CXLHybridFaultWaitRecord *record;
    g_autofree char *key = NULL;

    if (!cxl_state.fault_wait_records || !ramblock) {
        return NULL;
    }

    key = cxl_hybrid_publish_wait_key(ramblock, guest_offset, generation);
    record = g_hash_table_lookup(cxl_state.fault_wait_records, key);
    if (!record && create) {
        record = g_new0(CXLHybridFaultWaitRecord, 1);
        g_hash_table_insert(cxl_state.fault_wait_records,
                            g_steal_pointer(&key), record);
    }
    return record;
}

static uint64_t
cxl_hybrid_take_fault_wait_ready_recv_ns_locked(const char *ramblock,
                                                uint64_t guest_offset,
                                                uint32_t generation)
{
    CXLHybridFaultWaitRecord *record;
    g_autofree char *key = NULL;
    uint64_t ready_recv_ns = 0;

    if (!cxl_state.fault_wait_records || !ramblock) {
        return 0;
    }

    key = cxl_hybrid_publish_wait_key(ramblock, guest_offset, generation);
    record = g_hash_table_lookup(cxl_state.fault_wait_records, key);
    if (record) {
        ready_recv_ns = record->ready_recv_ns;
        g_hash_table_remove(cxl_state.fault_wait_records, key);
    }

    return ready_recv_ns;
}

static void cxl_hybrid_ensure_publish_mutex(void)
{
    if (cxl_state.publish_mutex_ready) {
        return;
    }

    qemu_mutex_init(&cxl_state.publish_mutex);
    cxl_state.publish_mutex_ready = true;
}

static void cxl_hybrid_ensure_fault_wait_records(void)
{
    if (!cxl_state.fault_wait_records) {
        cxl_state.fault_wait_records = g_hash_table_new_full(g_str_hash,
                                                             g_str_equal,
                                                             g_free,
                                                             g_free);
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

static void cxl_hybrid_set_last_publish_ready(
    CXLHybridLastPublishReadyInfo *info,
    const char *ramblock,
    uint64_t guest_offset,
    uint64_t cxl_offset,
    uint32_t generation,
    uint64_t count)
{
    if (!info) {
        return;
    }

    g_free(info->ramblock);
    info->ramblock = g_strdup(ramblock);
    info->guest_offset = guest_offset;
    info->cxl_offset = cxl_offset;
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
    case CXL_HYBRID_PUBLISH_SOURCE_PENDING_READY:
        qatomic_add(&cxl_state.pending_ready_publish_pages, npages);
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
    cxl_state.iter_begin.pending_ready_publish_pages =
        qatomic_read(&cxl_state.pending_ready_publish_pages);
    cxl_state.iter_begin.completion_publish_pages =
        qatomic_read(&cxl_state.completion_publish_pages);
    cxl_state.iter_begin.publish_ready_sent_pages =
        qatomic_read(&cxl_state.publish_ready_sent_pages);
    cxl_state.iter_begin.phase = cxl_state.phase;
}

void cxl_hybrid_iteration_snapshot_end(uint64_t ram_pages)
{
    uint64_t staged_pages = cxl_hybrid_current_staged_pages();
    uint64_t warm_push = qatomic_read(&cxl_state.warm_push_publish_pages);
    uint64_t fault_primary = qatomic_read(&cxl_state.fault_primary_publish_pages);
    uint64_t fault_burst = qatomic_read(&cxl_state.fault_burst_publish_pages);
    uint64_t pending_ready = qatomic_read(&cxl_state.pending_ready_publish_pages);
    uint64_t completion = qatomic_read(&cxl_state.completion_publish_pages);
    uint64_t publish_ready_sent_pages =
        qatomic_read(&cxl_state.publish_ready_sent_pages);

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
    qatomic_set(&cxl_state.last_iterate_publish_ready_pages,
                publish_ready_sent_pages - cxl_state.iter_begin.publish_ready_sent_pages);
    qatomic_set(&cxl_state.last_iterate_phase, cxl_state.iter_begin.phase);

    trace_cxl_hybrid_iteration_profile(
        qatomic_read(&cxl_state.last_iterate_phase),
        qatomic_read(&cxl_state.last_iterate_ram_pages),
        qatomic_read(&cxl_state.last_iterate_staged_pages_delta),
        staged_pages,
        cxl_state.total_pages,
        qatomic_read(&cxl_state.last_iterate_warm_push_pages),
        qatomic_read(&cxl_state.last_iterate_fault_primary_pages),
        qatomic_read(&cxl_state.last_iterate_fault_burst_pages),
        qatomic_read(&cxl_state.last_iterate_publish_ready_pages));

    /*
     * Keep completion-originated republishes in their own cumulative counter.
     * They are not reported as a distinct per-iteration field yet because the
     * user-facing question is about warm push and fault-triggered traffic.
     */
    (void)(pending_ready - cxl_state.iter_begin.pending_ready_publish_pages);
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

static CXLMigrationPublishReadyInfo *cxl_hybrid_export_last_publish_ready(
    const CXLHybridLastPublishReadyInfo *src)
{
    CXLMigrationPublishReadyInfo *dst;

    if (!src || !src->ramblock || !src->count) {
        return NULL;
    }

    dst = g_new0(CXLMigrationPublishReadyInfo, 1);
    dst->count = src->count;
    dst->ramblock = g_strdup(src->ramblock);
    dst->guest_offset = src->guest_offset;
    dst->cxl_offset = src->cxl_offset;
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
        .generation = (uint32_t)qatomic_read(&cxl_state.phase_transitions),
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
    if (migrate_cxl_fault_control_plane_cxl()) {
        ret = cxl_hybrid_control_activate_destination(errp);
        if (ret) {
            cxl_hybrid_metadata_cleanup(&cxl_incoming_meta_state.last_meta);
            cxl_incoming_meta_state.valid = false;
            return ret;
        }
    }
    trace_cxl_hybrid_metadata_recv(meta.generation, meta.nr_entries, len);
    return 0;
}

void cxl_hybrid_metadata_cleanup_incoming(void)
{
    cxl_hybrid_metadata_cleanup(&cxl_incoming_meta_state.last_meta);
    cxl_incoming_meta_state.valid = false;
    cxl_destination_staging_cleanup();
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
    ret = place_page(mis, host, page, rb);
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
    if (cxl_incoming_meta_state.valid) {
        return cxl_incoming_meta_state.last_meta.generation;
    }

    return (uint32_t)qatomic_read(&cxl_state.phase_transitions);
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
    bool cxl_ctrl_fault_path;
    size_t page_index;
    size_t pagesize;
    uint32_t generation;
    uint64_t wait_start_ns;
    uint64_t wait_time_ns;
    uint64_t cxl_offset;
    uint64_t ready_recv_ns = 0;
    uint64_t wait_ready_recv_time_ns = 0;
    uint64_t wait_after_ready_recv_time_ns = 0;
    int ret;

    if (!mis || !rb || !place_page) {
        error_setg(errp, "CXL hybrid fault wait missing arguments");
        return -EINVAL;
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
        }
        mark_postcopy_blocktime_begin(haddr, tid, rb);
    }

    generation = cxl_hybrid_fault_publish_generation();
    wait_start_ns = cxl_now_ns();
    cxl_ctrl_fault_path = migrate_cxl_fault_control_plane_cxl();
    if (!cxl_ctrl_fault_path && cxl_state.publish_mutex_ready) {
        CXLHybridFaultWaitRecord *wait_record;

        qemu_mutex_lock(&cxl_state.publish_mutex);
        wait_record = cxl_hybrid_lookup_fault_wait_record_locked(
            ramblock, offset, generation, true);
        if (wait_record) {
            wait_record->wait_begin_ns = wait_start_ns;
        }
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    if (cxl_ctrl_fault_path) {
        page_index = cxl_global_page_index(rb, offset);
        if (!cxl_hybrid_ctrl_page_visible(page_index, generation)) {
            ret = cxl_hybrid_ctrl_enqueue_fault_request(page_index, generation,
                                                        wait_start_ns, errp);
            publish_req_sent = (ret == 0);
            if (ret) {
                return ret;
            }
        }
    } else {
        ret = migrate_send_rp_cxl_publish_req(mis, rb, offset,
                                              pagesize,
                                              generation,
                                              &publish_req_sent);
        if (ret) {
            if (cxl_state.publish_mutex_ready) {
                qemu_mutex_lock(&cxl_state.publish_mutex);
                cxl_hybrid_take_fault_wait_ready_recv_ns_locked(
                    ramblock, offset, generation);
                qemu_mutex_unlock(&cxl_state.publish_mutex);
            }
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
    if (cxl_ctrl_fault_path) {
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
            ret = cxl_hybrid_dst_staging_register_external_page(
                ramblock, offset, cxl_offset, pagesize, errp);
        }
    } else {
        ret = cxl_hybrid_dst_staging_wait_range_present(
            ramblock, offset, pagesize, errp);
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
        if (!cxl_ctrl_fault_path) {
            ready_recv_ns = cxl_hybrid_take_fault_wait_ready_recv_ns_locked(
                ramblock, offset, generation);
        }
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    if (!cxl_ctrl_fault_path && ready_recv_ns) {
        if (ready_recv_ns > wait_start_ns) {
            wait_ready_recv_time_ns = ready_recv_ns - wait_start_ns;
            if (wait_ready_recv_time_ns > wait_time_ns) {
                wait_ready_recv_time_ns = wait_time_ns;
            }
        }
        wait_after_ready_recv_time_ns = wait_time_ns -
            wait_ready_recv_time_ns;
        cxl_hybrid_record_timing(&cxl_state.fault_wait_ready_recv_samples,
                                 &cxl_state.fault_wait_ready_recv_time_ns,
                                 &cxl_state.max_fault_wait_ready_recv_time_ns,
                                 wait_ready_recv_time_ns);
        cxl_hybrid_record_timing(
            &cxl_state.fault_wait_after_ready_recv_samples,
            &cxl_state.fault_wait_after_ready_recv_time_ns,
            &cxl_state.max_fault_wait_after_ready_recv_time_ns,
            wait_after_ready_recv_time_ns);
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
                   "CXL hybrid publish-ready did not make %s/0x%" PRIx64
                   " available",
                   ramblock, (uint64_t)offset);
        return -ENOENT;
    }

    return 0;
}

static int cxl_destination_staging_init(Error **errp)
{
    QIOChannelCXL *cioc;
    int ret;
    const char *path = migrate_cxl_path();
    uint64_t base_offset;
    uint64_t total_capacity;

    if (!migrate_cxl_hybrid()) {
        return 0;
    }

    if (!path) {
        error_setg(errp, "CXL hybrid destination staging requires cxl-path");
        return -EINVAL;
    }

    cioc = qio_channel_cxl_new_path(path, O_RDWR | O_CLOEXEC, errp);
    if (!cioc) {
        return -EINVAL;
    }

    if (cioc->map_size == 0) {
        error_setg(errp, "CXL hybrid destination staging size is unknown");
        object_unref(OBJECT(cioc));
        return -EINVAL;
    }

    total_capacity = cioc->map_size;
    base_offset = cxl_hybrid_mapped_ram_required_bytes(cioc->align);
    if (migrate_cxl_fault_control_plane_cxl()) {
        base_offset += cxl_hybrid_fault_control_region_bytes();
    }
    if (base_offset >= total_capacity) {
        error_setg(errp,
                   "CXL hybrid destination staging has no free space after mapped-ram backing: "
                   "base=%" PRIu64 " capacity=%" PRIu64,
                   base_offset, total_capacity);
        object_unref(OBJECT(cioc));
        return -ENOSPC;
    }

    ret = cxl_hybrid_dst_staging_init_path_at(path, total_capacity - base_offset,
                                              base_offset, errp);
    object_unref(OBJECT(cioc));
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

static void cxl_destination_staging_cleanup(void)
{
    cxl_hybrid_control_cleanup_destination();
    cxl_hybrid_dst_staging_cleanup();
}

bool cxl_hybrid_init_destination(Error **errp)
{
    int ret;

    cxl_hybrid_ensure_publish_mutex();
    cxl_hybrid_ensure_fault_wait_records();
    cxl_state.hybrid_enabled = migrate_cxl_hybrid();
    if (cxl_state.hybrid_enabled &&
        cxl_state.phase == CXL_HYBRID_PHASE_DISABLED) {
        cxl_state.phase = CXL_HYBRID_PHASE_PRECOPY_BULK;
        cxl_state.phase_transitions = MAX(cxl_state.phase_transitions, 1);
    }
    ret = cxl_destination_staging_init(errp);
    if (ret) {
        return false;
    }
    if (migrate_cxl_fault_control_plane_cxl()) {
        ret = cxl_hybrid_control_init_destination(errp);
        if (ret) {
            cxl_destination_staging_cleanup();
            return false;
        }
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

static uint64_t cxl_choose_remap_granule(uint64_t align, uint64_t total_ram)
{
    const char *value = g_getenv(CXL_REMAP_GRANULE_ENV);
    uint64_t configured = migrate_cxl_brake_remap_granule();
    uint64_t granule = MAX(align, (uint64_t)CXL_REMAP_GRANULE_DEFAULT);

    if (configured) {
        granule = MAX(align, configured);
    }

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
    CXLHybridDstStagingStats dst_stats = { 0 };

    cxl_hybrid_dst_staging_get_stats(&dst_stats);
    if (!cxl_state.active && cxl_cleanup_snapshot) {
        info->x_cxl = QAPI_CLONE(CXLMigrationStats, cxl_cleanup_snapshot);
        info->x_cxl->active = false;
        info->x_cxl->phase = CXL_HYBRID_PHASE_DISABLED;
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
    info->x_cxl->staged_pages = cxl_hybrid_current_staged_pages();
    if (cxl_state.total_pages) {
        info->x_cxl->staged_pages_percent =
            ((double)info->x_cxl->staged_pages * 100.0) /
            (double)cxl_state.total_pages;
    }
    info->x_cxl->warm_desc_sent_pages =
        qatomic_read(&cxl_state.source_warm_desc_sent_pages);
    info->x_cxl->warm_desc_sent_bytes =
        qatomic_read(&cxl_state.source_warm_desc_sent_bytes);
    info->x_cxl->warm_payload_fallback_pages =
        qatomic_read(&cxl_state.source_warm_payload_fallback_pages);
    info->x_cxl->warm_desc_skip_unremapped =
        qatomic_read(&cxl_state.source_warm_desc_skip_unremapped);
    info->x_cxl->warm_publish_pages =
        qatomic_read(&cxl_state.warm_publish_pages);
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
    info->x_cxl->last_iterate_publish_ready_pages =
        qatomic_read(&cxl_state.last_iterate_publish_ready_pages);
    info->x_cxl->last_iterate_phase =
        qatomic_read(&cxl_state.last_iterate_phase);
    info->x_cxl->fault_publish_requests =
        qatomic_read(&cxl_state.fault_publish_requests);
    info->x_cxl->fault_publish_waits =
        qatomic_read(&cxl_state.fault_publish_waits);
    info->x_cxl->fault_publish_wait_time_ns =
        qatomic_read(&cxl_state.fault_publish_wait_time_ns);
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
    info->x_cxl->fault_primary_ready_drain_samples =
        qatomic_read(&cxl_state.fault_primary_ready_drain_samples);
    info->x_cxl->fault_primary_ready_drain_time_ns =
        qatomic_read(&cxl_state.fault_primary_ready_drain_time_ns);
    info->x_cxl->max_fault_primary_ready_drain_time_ns =
        qatomic_read(&cxl_state.max_fault_primary_ready_drain_time_ns);
    info->x_cxl->fault_primary_ready_write_samples =
        qatomic_read(&cxl_state.fault_primary_ready_write_samples);
    info->x_cxl->fault_primary_ready_write_time_ns =
        qatomic_read(&cxl_state.fault_primary_ready_write_time_ns);
    info->x_cxl->max_fault_primary_ready_write_time_ns =
        qatomic_read(&cxl_state.max_fault_primary_ready_write_time_ns);
    info->x_cxl->fault_primary_ready_recv_samples =
        qatomic_read(&cxl_state.fault_primary_ready_recv_samples);
    info->x_cxl->fault_primary_ready_recv_time_ns =
        qatomic_read(&cxl_state.fault_primary_ready_recv_time_ns);
    info->x_cxl->max_fault_primary_ready_recv_time_ns =
        qatomic_read(&cxl_state.max_fault_primary_ready_recv_time_ns);
    info->x_cxl->fault_primary_ready_handle_samples =
        qatomic_read(&cxl_state.fault_primary_ready_handle_samples);
    info->x_cxl->fault_primary_ready_handle_time_ns =
        qatomic_read(&cxl_state.fault_primary_ready_handle_time_ns);
    info->x_cxl->max_fault_primary_ready_handle_time_ns =
        qatomic_read(&cxl_state.max_fault_primary_ready_handle_time_ns);
    info->x_cxl->fault_primary_ready_send_samples =
        qatomic_read(&cxl_state.fault_primary_ready_send_samples);
    info->x_cxl->fault_primary_ready_send_time_ns =
        qatomic_read(&cxl_state.fault_primary_ready_send_time_ns);
    info->x_cxl->max_fault_primary_ready_send_time_ns =
        qatomic_read(&cxl_state.max_fault_primary_ready_send_time_ns);
    info->x_cxl->fault_wait_ready_recv_samples =
        qatomic_read(&cxl_state.fault_wait_ready_recv_samples);
    info->x_cxl->fault_wait_ready_recv_time_ns =
        qatomic_read(&cxl_state.fault_wait_ready_recv_time_ns);
    info->x_cxl->max_fault_wait_ready_recv_time_ns =
        qatomic_read(&cxl_state.max_fault_wait_ready_recv_time_ns);
    info->x_cxl->fault_wait_after_ready_recv_samples =
        qatomic_read(&cxl_state.fault_wait_after_ready_recv_samples);
    info->x_cxl->fault_wait_after_ready_recv_time_ns =
        qatomic_read(&cxl_state.fault_wait_after_ready_recv_time_ns);
    info->x_cxl->max_fault_wait_after_ready_recv_time_ns =
        qatomic_read(&cxl_state.max_fault_wait_after_ready_recv_time_ns);
    info->x_cxl->pending_publish_ready =
        qatomic_read(&cxl_state.pending_publish_ready);
    info->x_cxl->completion_pending_publish_ready =
        qatomic_read(&cxl_state.completion_pending_publish_ready);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    info->x_cxl->last_publish_request =
        cxl_hybrid_export_last_publish_request(
            &cxl_state.last_publish_request);
    info->x_cxl->last_publish_ready =
        cxl_hybrid_export_last_publish_ready(&cxl_state.last_publish_ready);
    info->x_cxl->last_completion_publish_ready =
        cxl_hybrid_export_last_publish_ready(
            &cxl_state.last_completion_publish_ready);
    info->x_cxl->last_publish_ready_recv =
        cxl_hybrid_export_last_publish_ready(
            &cxl_state.last_publish_ready_recv);
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
    return cxl_state.migrated_bmap &&
           test_bit(page_idx, cxl_state.migrated_bmap) &&
           (!cxl_state.warm_sent_bmap ||
            !test_bit(page_idx, cxl_state.warm_sent_bmap));
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

static void cxl_hybrid_clear_page_range(unsigned long *bitmap,
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
        clear_bit_atomic(first_page + page, bitmap);
    }
}

static int cxl_hybrid_send_selected_page(MigrationState *s, size_t page_idx,
                                         Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    uint32_t generation;
    uint64_t cxl_offset;
    uint64_t offset;
    int ret;

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

    if (cxl_state.dst_sent_bmap && test_bit(page_idx, cxl_state.dst_sent_bmap)) {
        trace_cxl_hybrid_warm_page_skip_received(block->idstr, block_offset);
        qatomic_inc(&cxl_state.source_warm_skip_received);
        return 0;
    }

    offset = block_offset;
    generation = (uint32_t)qatomic_read(&cxl_state.phase_transitions);

    switch (migrate_cxl_warm_transport()) {
    case CXL_HYBRID_WARM_TRANSPORT_CXL_OFFSET:
    case CXL_HYBRID_WARM_TRANSPORT_AUTO:
        ret = cxl_hybrid_publish_page_to_cxl(block->idstr, offset,
                                             TARGET_PAGE_SIZE, generation,
                                             CXL_HYBRID_PUBLISH_SOURCE_WARM_PUSH,
                                             &cxl_offset, errp);
        if (ret) {
            return ret;
        }
        ret = cxl_hybrid_send_warm_descriptor(s->to_dst_file, block->idstr,
                                              offset, errp);
        break;
    case CXL_HYBRID_WARM_TRANSPORT_PAYLOAD:
        error_setg(errp, "CXL-only hybrid warm push does not support payload transport");
        ret = -EINVAL;
        break;
    default:
        error_setg(errp, "Unsupported CXL warm transport mode %d",
                   migrate_cxl_warm_transport());
        ret = -EINVAL;
        break;
    }
    if (ret) {
        return ret;
    }

    set_bit_atomic(page_idx, cxl_state.warm_sent_bmap);
    clear_bit_atomic(page_idx, cxl_state.warm_dirty_bmap);
    qatomic_inc(&cxl_state.source_warm_queue_pages);
    trace_cxl_hybrid_warm_page_queued(block->idstr, block_offset);
    return 1;
}

static void cxl_hybrid_warm_desc_batch_builder_cleanup(
    CXLHybridWarmDescBatchBuilder *builder)
{
    if (!builder) {
        return;
    }

    cxl_hybrid_warm_desc_batch_cleanup(&builder->batch);
    g_free(builder->open_range.ramblock);
    builder->open_range.ramblock = NULL;
    builder->have_open_range = false;
    builder->encoded_len = CXL_HYBRID_WARM_DESC_BATCH_HEADER_LEN;
    builder->pages = 0;
}

static size_t cxl_hybrid_warm_desc_batch_entry_len(
    const CXLHybridWarmDescRange *range)
{
    return CXL_HYBRID_WARM_DESC_BATCH_ENTRY_HEADER_LEN +
           strlen(range->ramblock);
}

static int cxl_hybrid_build_warm_desc_range(const char *ramblock,
                                            uint64_t guest_offset,
                                            uint32_t page_len,
                                            CXLHybridWarmDescRange *range,
                                            Error **errp)
{
    RAMBlock *block;
    ram_addr_t global_offset;
    CXLHybridPublishedPageState publish_state = { 0 };

    if (!ramblock || !range) {
        error_setg(errp, "CXL hybrid warm descriptor range missing arguments");
        return -EINVAL;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block) {
        error_setg(errp,
                   "CXL hybrid warm descriptor range %s/0x%" PRIx64
                   " has no RAMBlock",
                   ramblock, guest_offset);
        return -ENOENT;
    }

    if (!cxl_hybrid_source_page_cxl_offset(ramblock, guest_offset,
                                           &range->cxl_offset)) {
        error_setg(errp,
                   "CXL hybrid warm descriptor range %s/0x%" PRIx64
                   " has no stable CXL offset",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    if (!cxl_hybrid_global_page_offset(block, guest_offset,
                                       page_len, &global_offset)) {
        error_setg(errp,
                   "CXL hybrid warm descriptor range %s/0x%" PRIx64
                   " has invalid global offset",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    if (!cxl_hybrid_range_is_remapped(block, guest_offset, page_len) &&
        !cxl_hybrid_get_published_page_state(ramblock, guest_offset,
                                             &publish_state)) {
        error_setg(errp,
                   "CXL hybrid warm descriptor range %s/0x%" PRIx64
                   " is neither source-remapped nor published-to-CXL",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    range->ramblock = g_strdup(ramblock);
    range->guest_offset = guest_offset;
    range->page_len = page_len;
    range->flags = CXL_HYBRID_WARM_DESC_F_SHARED_CXL;
    if (cxl_hybrid_range_is_remapped(block, guest_offset, page_len)) {
        range->flags |= CXL_HYBRID_WARM_DESC_F_SOURCE_REMAPPED;
    }

    return 0;
}

static int cxl_hybrid_warm_desc_batch_builder_flush(
    CXLHybridWarmDescBatchBuilder *builder,
    Error **errp);

static int cxl_hybrid_warm_desc_batch_builder_append_entry(
    CXLHybridWarmDescBatchBuilder *builder,
    const CXLHybridWarmDescRange *range,
    Error **errp)
{
    CXLHybridWarmDescRange *entry;
    size_t next_len;

    next_len = builder->encoded_len +
               cxl_hybrid_warm_desc_batch_entry_len(range);
    if (next_len > UINT16_MAX && builder->batch.nr_entries > 0) {
        int ret = cxl_hybrid_warm_desc_batch_builder_flush(builder, errp);

        if (ret) {
            return ret;
        }
        next_len = builder->encoded_len +
                   cxl_hybrid_warm_desc_batch_entry_len(range);
    }

    if (next_len > UINT16_MAX) {
        error_setg(errp,
                   "CXL hybrid warm descriptor batch command too large: %zu",
                   next_len);
        return -E2BIG;
    }

    builder->batch.entries = g_renew(CXLHybridWarmDescRange,
                                     builder->batch.entries,
                                     builder->batch.nr_entries + 1);
    entry = &builder->batch.entries[builder->batch.nr_entries++];
    *entry = (CXLHybridWarmDescRange) {
        .ramblock = g_strdup(range->ramblock),
        .guest_offset = range->guest_offset,
        .cxl_offset = range->cxl_offset,
        .page_len = range->page_len,
        .flags = range->flags,
    };
    builder->encoded_len = next_len;
    builder->pages += range->page_len / TARGET_PAGE_SIZE;
    return 0;
}

static int cxl_hybrid_warm_desc_batch_builder_append(
    CXLHybridWarmDescBatchBuilder *builder,
    const CXLHybridWarmDescRange *range,
    Error **errp)
{
    CXLHybridWarmDescRange *open_range;

    if (!builder || !range) {
        error_setg(errp, "CXL hybrid warm descriptor batch append missing arguments");
        return -EINVAL;
    }

    if (!builder->have_open_range) {
        builder->open_range = (CXLHybridWarmDescRange) {
            .ramblock = g_strdup(range->ramblock),
            .guest_offset = range->guest_offset,
            .cxl_offset = range->cxl_offset,
            .page_len = range->page_len,
            .flags = range->flags,
        };
        builder->have_open_range = true;
        return 0;
    }

    open_range = &builder->open_range;
    if (!strcmp(open_range->ramblock, range->ramblock) &&
        open_range->flags == range->flags &&
        open_range->guest_offset + open_range->page_len == range->guest_offset &&
        open_range->cxl_offset + open_range->page_len == range->cxl_offset) {
        open_range->page_len += range->page_len;
        return 0;
    }

    {
        int ret = cxl_hybrid_warm_desc_batch_builder_flush(builder, errp);

        if (ret) {
            return ret;
        }
    }

    builder->open_range = (CXLHybridWarmDescRange) {
        .ramblock = g_strdup(range->ramblock),
        .guest_offset = range->guest_offset,
        .cxl_offset = range->cxl_offset,
        .page_len = range->page_len,
        .flags = range->flags,
    };
    builder->have_open_range = true;
    return 0;
}

static int cxl_hybrid_warm_desc_batch_builder_flush(
    CXLHybridWarmDescBatchBuilder *builder,
    Error **errp)
{
    g_autofree uint8_t *buf = NULL;
    size_t encoded_len;
    uint32_t generation;
    uint32_t i;
    int ret;

    if (!builder) {
        error_setg(errp, "CXL hybrid warm descriptor batch flush missing builder");
        return -EINVAL;
    }

    if (builder->have_open_range) {
        ret = cxl_hybrid_warm_desc_batch_builder_append_entry(
            builder, &builder->open_range, errp);
        if (ret) {
            return ret;
        }
        g_clear_pointer(&builder->open_range.ramblock, g_free);
        builder->have_open_range = false;
    }

    if (!builder->batch.nr_entries) {
        return 0;
    }

    ret = cxl_hybrid_warm_desc_batch_encoded_len(&builder->batch, &encoded_len,
                                                 errp);
    if (ret) {
        return ret;
    }

    buf = g_malloc(encoded_len);
    ret = cxl_hybrid_warm_desc_batch_encode(&builder->batch, buf, encoded_len,
                                            errp);
    if (ret) {
        return ret;
    }

    for (i = 0; i < builder->batch.nr_entries; i++) {
        CXLHybridWarmDescRange *range = &builder->batch.entries[i];

        trace_cxl_hybrid_warm_desc_send(range->ramblock, range->guest_offset,
                                        range->cxl_offset, range->page_len);
    }

    trace_cxl_hybrid_warm_desc_batch_send(builder->batch.generation,
                                          builder->batch.nr_entries,
                                          builder->pages, encoded_len);
    qemu_savevm_send_cxl_hybrid_warm_desc_batch(builder->s->to_dst_file,
                                                builder->batch.generation,
                                                builder->batch.nr_entries,
                                                buf, encoded_len);
    ret = qemu_file_get_error(builder->s->to_dst_file);
    if (ret) {
        error_setg(errp,
                   "Failed to send CXL hybrid warm descriptor batch command");
        return ret < 0 ? ret : -EIO;
    }

    for (i = 0; i < builder->batch.nr_entries; i++) {
        CXLHybridWarmDescRange *range = &builder->batch.entries[i];

        cxl_hybrid_account_dst_page_sent(range->ramblock, range->guest_offset,
                                         range->page_len);
        qatomic_add(&cxl_state.source_warm_desc_sent_pages,
                    range->page_len / TARGET_PAGE_SIZE);
        qatomic_add(&cxl_state.source_warm_desc_sent_bytes, range->page_len);
        qatomic_add(&cxl_state.source_warm_sent_pages,
                    range->page_len / TARGET_PAGE_SIZE);
        qatomic_add(&cxl_state.source_warm_sent_bytes, range->page_len);
    }

    generation = builder->batch.generation;
    cxl_hybrid_warm_desc_batch_cleanup(&builder->batch);
    builder->batch.generation = generation;
    builder->encoded_len = CXL_HYBRID_WARM_DESC_BATCH_HEADER_LEN;
    builder->pages = 0;
    return 0;
}

static int cxl_hybrid_completion_publish_remaining_page(
    MigrationState *s,
    size_t page_idx,
    CXLHybridWarmDescBatchBuilder *builder,
    Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    CXLHybridWarmDescRange range = { 0 };
    uint32_t generation;
    int ret;

    if (!s || !builder) {
        error_setg(errp, "CXL hybrid completion missing warm descriptor batch builder");
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

    generation = builder->batch.generation;
    ret = cxl_hybrid_publish_page_to_cxl(block->idstr, block_offset,
                                         TARGET_PAGE_SIZE, generation,
                                         CXL_HYBRID_PUBLISH_SOURCE_COMPLETION,
                                         &(uint64_t){ 0 }, errp);
    if (ret) {
        return ret;
    }

    ret = cxl_hybrid_build_warm_desc_range(block->idstr, block_offset,
                                           TARGET_PAGE_SIZE, &range, errp);
    if (ret) {
        return ret;
    }

    ret = cxl_hybrid_warm_desc_batch_builder_append(builder, &range, errp);
    g_free(range.ramblock);
    if (ret) {
        return ret;
    }

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

    if (!cxl_state.hybrid_enabled || !rbname) {
        return;
    }

    cxl_hybrid_mark_page_range(cxl_state.warm_dirty_bmap, rbname, offset, len);
    block = qemu_ram_block_by_name(rbname);
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
    if (!cxl_state.hybrid_enabled || !rbname) {
        return;
    }

    cxl_hybrid_mark_page_range(cxl_state.dst_sent_bmap, rbname, offset, len);
    cxl_hybrid_mark_page_range(cxl_state.cxl_visible_bmap, rbname, offset, len);
    cxl_hybrid_clear_page_range(cxl_state.remaining_bmap, rbname, offset, len);
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
    stats->source_warm_desc_sent_pages =
        qatomic_read(&cxl_state.source_warm_desc_sent_pages);
    stats->source_warm_desc_sent_bytes =
        qatomic_read(&cxl_state.source_warm_desc_sent_bytes);
    stats->source_warm_payload_fallback_pages =
        qatomic_read(&cxl_state.source_warm_payload_fallback_pages);
    stats->source_warm_desc_skip_unremapped =
        qatomic_read(&cxl_state.source_warm_desc_skip_unremapped);
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
    memcpy(dst, src, page_len);
    return 0;
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

void cxl_hybrid_record_publish_req_recv_time(uint64_t elapsed_ns)
{
    cxl_hybrid_record_timing(&cxl_state.fault_publish_req_recv_samples,
                             &cxl_state.fault_publish_req_recv_time_ns,
                             &cxl_state.max_fault_publish_req_recv_time_ns,
                             elapsed_ns);
}

void cxl_hybrid_record_publish_ready_recv_time(uint64_t elapsed_ns)
{
    cxl_hybrid_record_timing(&cxl_state.fault_primary_ready_recv_samples,
                             &cxl_state.fault_primary_ready_recv_time_ns,
                             &cxl_state.max_fault_primary_ready_recv_time_ns,
                             elapsed_ns);
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

static int cxl_hybrid_stream_ready_consumer(
    const CXLHybridFaultReadyRecord *record, Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    bool fault_primary;

    if (!record) {
        error_setg(errp, "CXL hybrid stream ready consumer missing record");
        return -EINVAL;
    }

    if (!cxl_hybrid_lookup_global_page(record->page_index, &block,
                                       &block_offset)) {
        error_setg(errp,
                   "CXL hybrid stream ready page %" PRIu64 " no longer resolves",
                   record->page_index);
        return -ENOENT;
    }

    fault_primary = record->flags & CXL_HYBRID_FAULT_READY_F_PRIMARY;
    cxl_hybrid_queue_publish_ready(qemu_ram_get_idstr(block), block_offset,
                                   record->cxl_offset, TARGET_PAGE_SIZE,
                                   record->generation, fault_primary);
    return 0;
}

static void cxl_hybrid_queue_publish_ready(const char *ramblock,
                                           uint64_t guest_offset,
                                           uint64_t cxl_offset,
                                           uint32_t page_len,
                                           uint32_t generation,
                                           bool fault_primary)
{
    CXLHybridPendingReady *ready = g_new0(CXLHybridPendingReady, 1);
    uint64_t pending;

    ready->notify.ramblock = g_strdup(ramblock);
    ready->notify.guest_offset = guest_offset;
    ready->notify.cxl_offset = cxl_offset;
    ready->notify.page_len = page_len;
    ready->notify.generation = generation;
    ready->queued_at_ns = cxl_now_ns();
    ready->fault_primary = fault_primary;

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    QSIMPLEQ_INSERT_TAIL(&cxl_state.pending_ready, ready, next);
    pending = qatomic_inc_fetch(&cxl_state.pending_publish_ready);
    trace_cxl_hybrid_publish_ready_queue(ramblock, guest_offset, cxl_offset,
                                         generation, pending);
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    if (fault_primary) {
        migration_mark_cxl_hybrid_ready_urgent();
    }
}

static int cxl_hybrid_republish_pending_ready(CXLHybridPendingReady *ready,
                                              Error **errp)
{
    CXLHybridPublishedPageState state = { 0 };
    uint64_t cxl_offset = 0;
    int ret;

    if (!ready || !ready->notify.ramblock) {
        error_setg(errp, "CXL hybrid pending publish-ready missing arguments");
        return -EINVAL;
    }

    if (cxl_hybrid_get_published_page_state(ready->notify.ramblock,
                                            ready->notify.guest_offset,
                                            &state) &&
        state.ready &&
        state.generation == ready->notify.generation &&
        state.cxl_offset == ready->notify.cxl_offset) {
        return 0;
    }

    ret = cxl_hybrid_publish_page_to_cxl(ready->notify.ramblock,
                                         ready->notify.guest_offset,
                                         ready->notify.page_len,
                                         ready->notify.generation,
                                         CXL_HYBRID_PUBLISH_SOURCE_PENDING_READY,
                                         &cxl_offset, errp);
    if (ret) {
        return ret;
    }

    ready->notify.cxl_offset = cxl_offset;
    return 0;
}

static CXLHybridPendingReady *cxl_hybrid_pop_publish_ready(void)
{
    CXLHybridPendingReady *ready;

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    ready = QSIMPLEQ_FIRST(&cxl_state.pending_ready);
    if (ready) {
        uint64_t pending;

        QSIMPLEQ_REMOVE_HEAD(&cxl_state.pending_ready, next);
        pending = qatomic_dec_fetch(&cxl_state.pending_publish_ready);
        trace_cxl_hybrid_publish_ready_pop(ready->notify.ramblock,
                                           ready->notify.guest_offset,
                                           ready->notify.cxl_offset,
                                           ready->notify.generation,
                                           pending);
    }
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    return ready;
}

uint64_t cxl_hybrid_pending_publish_ready(void)
{
    return qatomic_read(&cxl_state.pending_publish_ready);
}

void cxl_hybrid_mark_completion_pending_publish_ready(void)
{
    qatomic_set(&cxl_state.completion_pending_publish_ready,
                qatomic_read(&cxl_state.pending_publish_ready));
}

void cxl_hybrid_mark_completion_publish_ready_flushed(void)
{
    if (!cxl_state.publish_mutex_ready) {
        return;
    }

    qemu_mutex_lock(&cxl_state.publish_mutex);
    if (cxl_state.last_publish_ready.count) {
        cxl_hybrid_set_last_publish_ready(
            &cxl_state.last_completion_publish_ready,
            cxl_state.last_publish_ready.ramblock,
            cxl_state.last_publish_ready.guest_offset,
            cxl_state.last_publish_ready.cxl_offset,
            cxl_state.last_publish_ready.generation,
            cxl_state.last_publish_ready.count);
    }
    qemu_mutex_unlock(&cxl_state.publish_mutex);
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

    if (!source_remapped && !already_ready) {
        ret = cxl_hybrid_copy_page_to_stable_cxl(block, guest_offset,
                                                 *cxl_offsetp, page_len, errp);
        if (ret) {
            qatomic_inc(&cxl_state.publish_failures);
            return ret;
        }
        qatomic_add(&cxl_state.publish_copied_pages,
                    page_len >> TARGET_PAGE_BITS);
        qatomic_add(&cxl_state.publish_copied_bytes, page_len);
    } else {
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
    cxl_hybrid_ctrl_set_page_visible(page_idx, generation);
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
                                           CXLHybridFaultReadyConsumer ready_consumer,
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
        size_t page_index;
        CXLHybridFaultReadyRecord ready = { 0 };
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
        page_index = cxl_global_page_index(block, neighbor_offset);
        ready.page_index = page_index;
        ready.cxl_offset = cxl_offset;
        ready.generation = generation;
        ready.flags = CXL_HYBRID_FAULT_READY_F_BURST_NEIGHBOR;
        if (cxl_hybrid_range_is_remapped(block, neighbor_offset,
                                         TARGET_PAGE_SIZE)) {
            ready.flags |= CXL_HYBRID_FAULT_READY_F_SOURCE_REMAPPED;
        }
        ready.ready_ts_ns = cxl_now_ns();

        if (ready_consumer) {
            ret = ready_consumer(&ready, &local_err);
            if (ret) {
                warn_report_err(local_err);
            }
        }
    }
}

int cxl_hybrid_handle_publish_request(const char *ramblock,
                                      uint64_t guest_offset,
                                      uint32_t page_len,
                                      uint32_t generation,
                                      uint64_t req_recv_ns,
                                      Error **errp)
{
    CXLHybridFaultReadyRecord primary_ready = { 0 };
    int ret;

    cxl_hybrid_note_publish_request_received(ramblock, guest_offset,
                                             generation, req_recv_ns);

    ret = cxl_hybrid_publish_fault_request_core(ramblock, guest_offset,
                                                page_len, generation, true,
                                                &primary_ready,
                                                cxl_hybrid_stream_ready_consumer,
                                                errp);
    if (ret) {
        return ret;
    }
    return 0;
}

int cxl_hybrid_publish_fault_request_core(const char *ramblock,
                                          uint64_t guest_offset,
                                          uint32_t page_len,
                                          uint32_t generation,
                                          bool emit_burst,
                                          CXLHybridFaultReadyRecord *primary_ready,
                                          CXLHybridFaultReadyConsumer ready_consumer,
                                          Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    size_t page_index;
    uint64_t cxl_offset;
    uint64_t handle_start_ns;
    uint64_t primary_start_ns;
    uint64_t burst_start_ns;
    uint64_t handle_time_ns;
    uint64_t primary_time_ns;
    uint64_t burst_time_ns;
    int ret;

    if (!ramblock || !primary_ready) {
        error_setg(errp, "CXL hybrid publish fault request missing arguments");
        return -EINVAL;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block || !offset_in_ramblock(block, guest_offset) ||
        guest_offset + page_len > block->used_length) {
        error_setg(errp, "CXL hybrid publish fault request page is invalid");
        return -EINVAL;
    }

    block_offset = guest_offset;
    page_index = cxl_global_page_index(block, block_offset);
    handle_start_ns = cxl_now_ns();
    primary_start_ns = handle_start_ns;
    ret = cxl_hybrid_publish_page_to_cxl(ramblock, guest_offset, page_len,
                                         generation,
                                         CXL_HYBRID_PUBLISH_SOURCE_FAULT_PRIMARY,
                                         &cxl_offset, errp);
    if (ret) {
        return ret;
    }

    memset(primary_ready, 0, sizeof(*primary_ready));
    primary_ready->page_index = page_index;
    primary_ready->cxl_offset = cxl_offset;
    primary_ready->generation = generation;
    primary_ready->flags = CXL_HYBRID_FAULT_READY_F_PRIMARY;
    if (cxl_hybrid_range_is_remapped(block, guest_offset, page_len)) {
        primary_ready->flags |= CXL_HYBRID_FAULT_READY_F_SOURCE_REMAPPED;
    }
    primary_ready->ready_ts_ns = cxl_now_ns();

    if (ready_consumer) {
        ret = ready_consumer(primary_ready, errp);
        if (ret) {
            return ret;
        }
    }

    primary_time_ns = cxl_now_ns() - primary_start_ns;
    cxl_hybrid_record_timing(&cxl_state.fault_publish_primary_samples,
                             &cxl_state.fault_publish_primary_time_ns,
                             &cxl_state.max_fault_publish_primary_time_ns,
                             primary_time_ns);

    if (emit_burst) {
        burst_start_ns = cxl_now_ns();
        cxl_hybrid_publish_fault_burst(ramblock, guest_offset, page_len,
                                       generation, ready_consumer, errp);
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

int cxl_hybrid_handle_publish_quiesce(MigrationIncomingState *mis,
                                      Error **errp)
{
    int ret;

    if (!mis) {
        error_setg(errp, "CXL hybrid publish quiesce missing migration state");
        return -EINVAL;
    }

    mis->cxl_publish_request_quiesce = true;
    ret = migrate_send_rp_cxl_publish_quiesce_ack(mis);
    if (ret) {
        error_setg(errp, "Failed to send CXL hybrid publish quiesce ACK");
        return ret < 0 ? ret : -EIO;
    }

    return 0;
}

int cxl_hybrid_send_pending_publish_ready(QEMUFile *f, Error **errp)
{
    CXLHybridPendingReady *ready;
    int sent = 0;

    if (!f) {
        return 0;
    }

    trace_cxl_hybrid_publish_ready_drain_begin(
        qatomic_read(&cxl_state.pending_publish_ready));
    if (migration_cxl_hybrid_ready_urgent()) {
        migration_clear_cxl_hybrid_ready_urgent();
    }
    while ((ready = cxl_hybrid_pop_publish_ready()) != NULL) {
        g_autofree uint8_t *buf = NULL;
        uint64_t drain_start_ns;
        size_t len;
        int ret;

        drain_start_ns = cxl_now_ns();
        if (ready->fault_primary) {
            cxl_hybrid_record_timing(
                &cxl_state.fault_primary_ready_drain_samples,
                &cxl_state.fault_primary_ready_drain_time_ns,
                &cxl_state.max_fault_primary_ready_drain_time_ns,
                drain_start_ns - ready->queued_at_ns);
        }
        ret = cxl_hybrid_republish_pending_ready(ready, errp);
        if (!ret) {
            ret = cxl_hybrid_publish_notify_encoded_len(&ready->notify, &len,
                                                        errp);
        }
        if (!ret) {
            buf = g_malloc(len);
            ret = cxl_hybrid_publish_notify_encode(&ready->notify, buf, len,
                                                   errp);
        }
        if (!ret) {
            uint64_t ready_complete_ns;
            uint64_t ready_send_time_ns;
            uint64_t ready_write_time_ns;

            trace_cxl_hybrid_publish_ready_send(ready->notify.ramblock,
                                                ready->notify.guest_offset,
                                                ready->notify.cxl_offset,
                                                ready->notify.generation);
            qemu_savevm_send_cxl_hybrid_publish_ready(
                f, ready->notify.ramblock, ready->notify.guest_offset,
                ready->notify.cxl_offset, buf, len, ready->fault_primary);
            ret = qemu_file_get_error(f);
            if (ret) {
                error_setg(errp, "Failed to send CXL hybrid publish-ready");
            } else {
                ready_complete_ns = cxl_now_ns();
                if (ready->fault_primary) {
                    ready_send_time_ns = ready_complete_ns -
                                         ready->queued_at_ns;
                    ready_write_time_ns = ready_complete_ns - drain_start_ns;
                    cxl_hybrid_record_timing(
                        &cxl_state.fault_primary_ready_write_samples,
                        &cxl_state.fault_primary_ready_write_time_ns,
                        &cxl_state.max_fault_primary_ready_write_time_ns,
                        ready_write_time_ns);
                    cxl_hybrid_record_timing(
                        &cxl_state.fault_primary_ready_send_samples,
                        &cxl_state.fault_primary_ready_send_time_ns,
                        &cxl_state.max_fault_primary_ready_send_time_ns,
                        ready_send_time_ns);
                }
                if (cxl_state.publish_mutex_ready) {
                    qemu_mutex_lock(&cxl_state.publish_mutex);
                    cxl_hybrid_set_last_publish_ready(
                        &cxl_state.last_publish_ready,
                        ready->notify.ramblock,
                        ready->notify.guest_offset,
                        ready->notify.cxl_offset,
                        ready->notify.generation,
                        cxl_state.last_publish_ready.count + 1);
                    qemu_mutex_unlock(&cxl_state.publish_mutex);
                }
                qatomic_inc(&cxl_state.publish_ready_sent_pages);
            }
        }

        cxl_hybrid_publish_notify_cleanup(&ready->notify);
        g_free(ready);
        if (ret) {
            return ret < 0 ? ret : -EIO;
        }
        sent++;
    }

    trace_cxl_hybrid_publish_ready_drain_end(
        sent, qatomic_read(&cxl_state.pending_publish_ready));
    return sent;
}

int cxl_hybrid_handle_publish_ready(const CXLHybridPublishNotify *notify,
                                    bool fault_primary,
                                    uint64_t ready_recv_ns,
                                    Error **errp)
{
    uint64_t handle_start_ns;
    uint64_t handle_time_ns;
    uint64_t ready_complete_ns;
    int ret;

    if (!notify || !notify->ramblock) {
        error_setg(errp, "CXL hybrid publish-ready missing arguments");
        return -EINVAL;
    }

    handle_start_ns = ready_recv_ns ? ready_recv_ns : cxl_now_ns();
    ret = cxl_hybrid_dst_staging_register_external_page(
        notify->ramblock, notify->guest_offset, notify->cxl_offset,
        notify->page_len, errp);
    if (ret) {
        return ret;
    }

    ready_complete_ns = cxl_now_ns();
    if (fault_primary) {
        handle_time_ns = ready_complete_ns - handle_start_ns;
        cxl_hybrid_record_timing(&cxl_state.fault_primary_ready_handle_samples,
                                 &cxl_state.fault_primary_ready_handle_time_ns,
                                 &cxl_state.max_fault_primary_ready_handle_time_ns,
                                 handle_time_ns);
    }
    if (cxl_state.publish_mutex_ready) {
        CXLHybridFaultWaitRecord *wait_record;

        qemu_mutex_lock(&cxl_state.publish_mutex);
        cxl_hybrid_set_last_publish_ready(
            &cxl_state.last_publish_ready_recv,
            notify->ramblock,
            notify->guest_offset,
            notify->cxl_offset,
            notify->generation,
            cxl_state.last_publish_ready_recv.count + 1);
        wait_record = cxl_hybrid_lookup_fault_wait_record_locked(
            notify->ramblock, notify->guest_offset, notify->generation,
            false);
        if (wait_record && !wait_record->ready_recv_ns) {
            wait_record->ready_recv_ns = ready_complete_ns;
        }
        qemu_mutex_unlock(&cxl_state.publish_mutex);
    }
    trace_cxl_hybrid_publish_ready_recv(notify->ramblock,
                                        notify->guest_offset,
                                        notify->cxl_offset,
                                        notify->generation);
    return 0;
}

int cxl_hybrid_handle_fault_ready_record(
    const CXLHybridFaultReadyRecord *record, Error **errp)
{
    RAMBlock *block;
    ram_addr_t block_offset;
    bool fault_primary;
    uint64_t ready_recv_ns;
    uint64_t ready_recv_elapsed_ns = 0;
    CXLHybridPublishNotify notify = { 0 };

    if (!record) {
        error_setg(errp, "CXL hybrid fault ready record missing");
        return -EINVAL;
    }

    if (!cxl_hybrid_lookup_global_page(record->page_index, &block,
                                       &block_offset)) {
        error_setg(errp,
                   "CXL hybrid fault ready page %" PRIu64 " no longer resolves",
                   record->page_index);
        return -ENOENT;
    }

    fault_primary = record->flags & CXL_HYBRID_FAULT_READY_F_PRIMARY;
    ready_recv_ns = cxl_now_ns();
    notify.ramblock = (char *)qemu_ram_get_idstr(block);
    notify.guest_offset = block_offset;
    notify.cxl_offset = record->cxl_offset;
    notify.page_len = TARGET_PAGE_SIZE;
    notify.generation = record->generation;

    if (fault_primary) {
        if (record->ready_ts_ns && ready_recv_ns >= record->ready_ts_ns) {
            ready_recv_elapsed_ns = ready_recv_ns - record->ready_ts_ns;
        }
        cxl_hybrid_record_publish_ready_recv_time(ready_recv_elapsed_ns);
    }

    return cxl_hybrid_handle_publish_ready(&notify, fault_primary,
                                           ready_recv_ns, errp);
}

int cxl_hybrid_send_warm_descriptor(QEMUFile *f, const char *ramblock,
                                    uint64_t guest_offset, Error **errp)
{
    CXLHybridWarmDescriptor desc = { 0 };
    RAMBlock *block;
    ram_addr_t global_offset;
    CXLHybridPublishedPageState publish_state = { 0 };
    g_autofree uint8_t *buf = NULL;
    size_t encoded_len;
    int ret;

    if (!f || !ramblock) {
        error_setg(errp, "CXL hybrid warm descriptor missing arguments");
        return -EINVAL;
    }

    if (!migrate_cxl_shared_backing()) {
        error_setg(errp,
                   "CXL hybrid warm descriptor transport requires x-cxl-shared-backing");
        return -EPERM;
    }

    if (!cxl_hybrid_source_page_cxl_offset(ramblock, guest_offset,
                                           &desc.cxl_offset)) {
        error_setg(errp,
                   "CXL hybrid warm descriptor page %s/0x%" PRIx64 " has no stable CXL offset",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    block = qemu_ram_block_by_name(ramblock);
    if (!block) {
        error_setg(errp,
                   "CXL hybrid warm descriptor page %s/0x%" PRIx64 " has no RAMBlock",
                   ramblock, guest_offset);
        return -ENOENT;
    }

    if (!cxl_hybrid_global_page_offset(block, guest_offset,
                                       TARGET_PAGE_SIZE, &global_offset)) {
        error_setg(errp,
                   "CXL hybrid warm descriptor page %s/0x%" PRIx64
                   " has invalid global offset",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    if (!cxl_page_is_remapped(global_offset) &&
        !cxl_hybrid_get_published_page_state(ramblock, guest_offset,
                                             &publish_state)) {
        error_setg(errp,
                   "CXL hybrid warm descriptor page %s/0x%" PRIx64
                   " is neither source-remapped nor published-to-CXL",
                   ramblock, guest_offset);
        return -EINVAL;
    }

    desc.ramblock = (char *)ramblock;
    desc.guest_offset = guest_offset;
    desc.page_len = TARGET_PAGE_SIZE;
    desc.flags = CXL_HYBRID_WARM_DESC_F_SHARED_CXL;
    if (cxl_page_is_remapped(global_offset)) {
        desc.flags |= CXL_HYBRID_WARM_DESC_F_SOURCE_REMAPPED;
    }
    desc.generation = publish_state.valid ? publish_state.generation :
                      (uint32_t)qatomic_read(&cxl_state.phase_transitions);

    ret = cxl_hybrid_warm_desc_encoded_len(&desc, &encoded_len, errp);
    if (ret) {
        return ret;
    }
    if (encoded_len > UINT16_MAX) {
        error_setg(errp,
                   "CXL hybrid warm descriptor command too large: %zu",
                   encoded_len);
        return -E2BIG;
    }

    buf = g_malloc(encoded_len);
    ret = cxl_hybrid_warm_desc_encode(&desc, buf, encoded_len, errp);
    if (ret) {
        return ret;
    }

    trace_cxl_hybrid_warm_desc_send(ramblock, guest_offset,
                                    desc.cxl_offset, TARGET_PAGE_SIZE);
    qemu_savevm_send_cxl_hybrid_warm_desc(f, ramblock, guest_offset,
                                          desc.cxl_offset, buf, encoded_len);
    ret = qemu_file_get_error(f);
    if (ret) {
        error_setg(errp,
                   "Failed to send CXL hybrid warm descriptor command");
        return ret < 0 ? ret : -EIO;
    }

    cxl_hybrid_account_dst_page_sent(ramblock, guest_offset, TARGET_PAGE_SIZE);
    qatomic_inc(&cxl_state.source_warm_desc_sent_pages);
    qatomic_add(&cxl_state.source_warm_desc_sent_bytes, TARGET_PAGE_SIZE);
    qatomic_inc(&cxl_state.source_warm_sent_pages);
    qatomic_add(&cxl_state.source_warm_sent_bytes, TARGET_PAGE_SIZE);
    return 0;
}

int cxl_hybrid_send_warm_page(QEMUFile *f, const char *ramblock,
                              uint64_t offset, const uint8_t *data,
                              size_t len, Error **errp)
{
    CXLHybridWarmPage page = {
        .ramblock = (char *)ramblock,
        .offset = offset,
        .page_len = len,
        .data = (uint8_t *)data,
    };
    g_autofree uint8_t *buf = NULL;
    size_t encoded_len;
    int ret;

    ret = cxl_hybrid_warm_page_encoded_len(&page, &encoded_len, errp);
    if (ret) {
        return ret;
    }
    if (encoded_len > UINT16_MAX) {
        error_setg(errp, "CXL hybrid warm page command too large: %zu",
                   encoded_len);
        return -E2BIG;
    }

    buf = g_malloc(encoded_len);
    ret = cxl_hybrid_warm_page_encode(&page, buf, encoded_len, errp);
    if (ret) {
        return ret;
    }

    trace_cxl_hybrid_warm_page_send(ramblock, offset, len);
    qemu_savevm_send_cxl_hybrid_warm_page(f, ramblock, offset, buf, encoded_len);
    ret = qemu_file_get_error(f);
    if (ret) {
        error_setg(errp, "Failed to send CXL hybrid warm page command");
        return ret < 0 ? ret : -EIO;
    }

    qatomic_inc(&cxl_state.source_warm_sent_pages);
    qatomic_add(&cxl_state.source_warm_sent_bytes, len);
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
    CXLHybridWarmDescBatchBuilder builder = {
        .s = s,
        .batch.generation = (uint32_t)qatomic_read(&cxl_state.phase_transitions),
        .encoded_len = CXL_HYBRID_WARM_DESC_BATCH_HEADER_LEN,
    };
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
        ret = cxl_hybrid_completion_publish_remaining_page(s, page_idx,
                                                           &builder, errp);
        if (ret < 0) {
            cxl_hybrid_warm_desc_batch_builder_cleanup(&builder);
            return ret;
        }
        sent += ret;
        page_idx = find_next_bit(cxl_state.remaining_bmap,
                                 cxl_state.total_pages,
                                 page_idx + 1);
    }

    ret = cxl_hybrid_warm_desc_batch_builder_flush(&builder, errp);
    if (ret) {
        cxl_hybrid_warm_desc_batch_builder_cleanup(&builder);
        return ret;
    }

    cxl_hybrid_warm_desc_batch_builder_cleanup(&builder);
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
        uint64_t required = cxl_hybrid_mapped_ram_required_bytes(cioc->align);

        if (migrate_cxl_fault_control_plane_cxl()) {
            required += cxl_hybrid_fault_control_region_bytes();
        }

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
    uint32_t generation;

    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_lock(&cxl_state.publish_mutex);
    }
    generation = (uint32_t)qatomic_read(&cxl_state.phase_transitions);
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
            entry->generation = generation;
            entry->valid = true;
            entry->ready = true;
            entry->source_remapped = true;
        }
        cxl_hybrid_ctrl_set_page_visible(page_idx, generation);
    }
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_unlock(&cxl_state.publish_mutex);
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
    cxl_state.align = align;
    cxl_state.remap_granule = cxl_choose_remap_granule(align, total_ram);
    cxl_state.total_pages = DIV_ROUND_UP(total_ram, TARGET_PAGE_SIZE);
    cxl_state.total_regions = DIV_ROUND_UP(total_ram, cxl_state.remap_granule);
    cxl_state.hybrid_enabled = migrate_cxl_hybrid();
    cxl_state.phase = cxl_state.hybrid_enabled ?
                      CXL_HYBRID_PHASE_PRECOPY_BULK :
                      CXL_HYBRID_PHASE_DISABLED;
    cxl_state.switch_reason = CXL_MIGRATION_SWITCH_REASON_NONE;
    cxl_state.switch_iteration = 0;
    cxl_state.phase_transitions = cxl_state.hybrid_enabled ? 1 : 0;
    QSIMPLEQ_INIT(&cxl_state.pending_ready);
    cxl_hybrid_ensure_publish_mutex();
    cxl_hybrid_ensure_fault_wait_records();

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
    cxl_state.pending_remap_bmap = bitmap_new(cxl_state.total_regions);
    cxl_state.remapped_bmap = bitmap_new(cxl_state.total_regions);
    cxl_state.remapped_pages_bmap = bitmap_new(cxl_state.total_pages);
    cxl_state.published_page_state = g_new0(CXLHybridPublishedPageEntry,
                                            cxl_state.total_pages);
    cxl_state.remap_bh = qemu_bh_new(cxl_process_pending_remaps_bh, NULL);
    qemu_mutex_init(&cxl_state.sender_sync_mutex);
    qemu_cond_init(&cxl_state.sender_sync_cond);
    cxl_state.sender_sync_ready = true;
    cxl_state.active = true;
}

static void cxl_remap_state_cleanup(void)
{
    CXLHybridPendingReady *ready;
    RAMBlock *block;

    if (!cxl_state.active && !cxl_state.publish_mutex_ready) {
        return;
    }

    cxl_state.phase = CXL_HYBRID_PHASE_CLEANUP;

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
    if (cxl_state.remap_bh) {
        qemu_bh_delete(cxl_state.remap_bh);
    }
    if (cxl_state.sender_sync_ready) {
        qemu_cond_destroy(&cxl_state.sender_sync_cond);
        qemu_mutex_destroy(&cxl_state.sender_sync_mutex);
    }
    cxl_hybrid_latch_cleanup_snapshot();
    while ((ready = QSIMPLEQ_FIRST(&cxl_state.pending_ready)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&cxl_state.pending_ready, next);
        qatomic_dec(&cxl_state.pending_publish_ready);
        cxl_hybrid_publish_notify_cleanup(&ready->notify);
        g_free(ready);
    }
    if (cxl_state.publish_mutex_ready) {
        qemu_mutex_destroy(&cxl_state.publish_mutex);
    }
    g_free(cxl_state.warm_last_miss_ramblock);
    g_free(cxl_state.last_publish_request.ramblock);
    g_free(cxl_state.last_publish_ready.ramblock);
    g_free(cxl_state.last_completion_publish_ready.ramblock);
    g_free(cxl_state.last_publish_ready_recv.ramblock);
    g_free(cxl_state.last_publish_wait_begin.ramblock);
    g_free(cxl_state.last_publish_wait_complete.ramblock);
    if (cxl_state.fault_wait_records) {
        g_hash_table_unref(cxl_state.fault_wait_records);
    }
    g_free(cxl_state.migrated_bmap);
    g_free(cxl_state.warm_sent_bmap);
    g_free(cxl_state.dst_sent_bmap);
    g_free(cxl_state.cxl_visible_bmap);
    g_free(cxl_state.remaining_bmap);
    g_free(cxl_state.warm_dirty_bmap);
    g_free(cxl_state.pending_remap_bmap);
    g_free(cxl_state.remapped_bmap);
    g_free(cxl_state.remapped_pages_bmap);
    g_free(cxl_state.published_page_state);
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

    if (!cxl_remap_active()) {
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

    if (!cxl_remap_active()) {
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

    if (!cxl_remap_active() || len == 0) {
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

bool cxl_hybrid_init_source(void)
{
    Error *local_err = NULL;
    int ret;

    if (!cxl_state.hybrid_enabled) {
        return false;
    }
    if (migrate_cxl_fault_control_plane_cxl()) {
        ret = cxl_hybrid_control_init_source(&local_err);
        if (ret) {
            error_report_err(local_err);
            return false;
        }
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
