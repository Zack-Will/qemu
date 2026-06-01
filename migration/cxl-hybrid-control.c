#include "qemu/osdep.h"
#include "migration/cxl.h"
#include "migration/misc.h"
#include "migration/options.h"
#include "migration/ram.h"
#include "io/channel-cxl.h"
#include "qemu/atomic.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/thread.h"
#include "system/ramblock.h"
#include "trace.h"

#define CXL_HYBRID_CXL_WORKER_MAX_BATCH 64

typedef struct CXLHybridControlRegion {
    void *map_base;
    size_t map_len;
    off_t map_offset;
    int fd;
    CXLHybridControlHeader *hdr;
    unsigned long *visible_bitmap;
    uint64_t *page_state;
    unsigned long *visible_region_bitmap;
    unsigned long *owned_region_bitmap;
    CXLHybridFaultRequestRecord *request_ring;
    uint64_t total_pages;
    uint64_t total_regions;
    uint64_t region_granule;
    uint32_t visible_page_words;
    uint32_t page_state_words;
    uint32_t visible_region_words;
    uint32_t owned_region_words;
    uint32_t region_granule_shift;
    uint32_t request_ring_entries;
    uint32_t users;
} CXLHybridControlRegion;

typedef struct CXLHybridControlState {
    CXLHybridControlHeader *hdr;
    void *map_base;
    size_t map_len;
    unsigned long *visible_bitmap;
    uint64_t *page_state;
    uint32_t page_state_words;
    unsigned long *visible_region_bitmap;
    unsigned long *owned_region_bitmap;
    CXLHybridFaultRequestRecord *request_ring;
    CXLHybridTransferQueue transfer_queue;
    QemuThread request_worker;
    QemuThread cxl_worker;
    uint64_t active_cxl_work;
    uint32_t dirty_seq;
    bool request_worker_running;
    bool cxl_worker_running;
    bool shutdown;
} CXLHybridControlState;

static CXLHybridControlRegion cxl_hybrid_control_region = {
    .fd = -1,
};
static CXLHybridControlState cxl_hybrid_control_source;
static CXLHybridControlState cxl_hybrid_control_destination;
static QemuMutex cxl_hybrid_control_request_lock;
static bool cxl_hybrid_control_locks_ready;

static inline uint64_t cxl_hybrid_ctrl_now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static bool cxl_hybrid_ctrl_try_dequeue_request(CXLHybridControlState *state,
                                                CXLHybridFaultRequestRecord *record);

static uint64_t cxl_hybrid_ctrl_total_pages(void)
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

static uint32_t cxl_hybrid_ctrl_visible_page_words(uint64_t total_pages)
{
    size_t words = cxl_hybrid_control_visible_bitmap_words(total_pages);

    if (words > UINT32_MAX) {
        return UINT32_MAX;
    }

    return words;
}

static uint64_t cxl_hybrid_ctrl_region_granule(uint64_t align,
                                               uint64_t total_ram)
{
    return cxl_hybrid_choose_fault_region_granule(align, 0, total_ram);
}

static uint64_t cxl_hybrid_ctrl_total_regions(uint64_t total_pages,
                                              uint64_t region_granule)
{
    uint64_t pages_per_region;

    if (!region_granule) {
        return total_pages;
    }

    pages_per_region = DIV_ROUND_UP(region_granule, TARGET_PAGE_SIZE);
    pages_per_region = MAX(pages_per_region, 1);
    return DIV_ROUND_UP(total_pages, pages_per_region);
}

static uint32_t cxl_hybrid_ctrl_owned_region_words(uint64_t total_regions)
{
    size_t words =
        cxl_hybrid_control_owned_region_bitmap_words(total_regions);

    if (words > UINT32_MAX) {
        return UINT32_MAX;
    }

    return words;
}

static uint32_t cxl_hybrid_ctrl_visible_region_words(uint64_t total_regions)
{
    size_t words =
        cxl_hybrid_control_visible_region_bitmap_words(total_regions);

    if (words > UINT32_MAX) {
        return UINT32_MAX;
    }

    return words;
}

static uint32_t cxl_hybrid_ctrl_ring_entries(uint32_t order)
{
    return 1U << order;
}

uint64_t cxl_hybrid_fault_control_region_bytes(void)
{
    uint64_t total_pages = cxl_hybrid_ctrl_total_pages();
    uint64_t total_regions =
        cxl_hybrid_ctrl_total_regions(total_pages, TARGET_PAGE_SIZE);
    size_t visible_bitmap_bytes =
        cxl_hybrid_control_visible_bitmap_bytes(total_pages);
    size_t page_state_bytes =
        cxl_hybrid_control_page_state_bytes(total_pages);
    size_t visible_region_bitmap_bytes =
        cxl_hybrid_control_visible_region_bitmap_bytes(total_regions);
    size_t owned_region_bitmap_bytes =
        cxl_hybrid_control_owned_region_bitmap_bytes(total_regions);
    size_t request_bytes =
        (size_t)cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_REQUEST_ORDER) *
        sizeof(CXLHybridFaultRequestRecord);

    return ROUND_UP(sizeof(CXLHybridControlHeader) + visible_bitmap_bytes +
                    page_state_bytes + visible_region_bitmap_bytes +
                    owned_region_bitmap_bytes + request_bytes,
                    (size_t)qemu_real_host_page_size());
}

static int cxl_hybrid_ctrl_validate_region_span(
    const CXLHybridControlHeader *hdr,
    uint64_t first_page,
    uint32_t nr_pages,
    uint64_t *region_indexp,
    Error **errp)
{
    uint64_t region_index = UINT64_MAX;

    if (!hdr) {
        error_setg(errp, "CXL hybrid fault control header is not initialized");
        return -EINVAL;
    }

    if (!nr_pages) {
        error_setg(errp, "CXL hybrid fault region request has zero pages");
        return -EINVAL;
    }

    if (first_page >= hdr->total_pages ||
        nr_pages > hdr->total_pages - first_page) {
        error_setg(errp,
                   "CXL hybrid fault region request out of range "
                   "(first-page=%" PRIu64 " pages=%u "
                   "total-pages=%" PRIu64 ")",
                   first_page, nr_pages, hdr->total_pages);
        return -ERANGE;
    }

    if (!cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                              &region_index)) {
        error_setg(errp,
                   "CXL hybrid fault region request is not a complete region "
                   "(first-page=%" PRIu64 " pages=%u)",
                   first_page, nr_pages);
        return -EINVAL;
    }

    if (region_indexp) {
        *region_indexp = region_index;
    }
    return 0;
}

uint64_t cxl_hybrid_fault_control_region_allocation_bytes(uint64_t align)
{
    uint64_t raw = cxl_hybrid_fault_control_region_bytes();
    uint64_t granule = MAX(align, (uint64_t)qemu_real_host_page_size());

    return ROUND_UP(raw, granule);
}

static off_t cxl_hybrid_ctrl_region_offset(uint64_t align)
{
    return cxl_hybrid_reserved_region_bytes(align, false);
}

static void cxl_hybrid_ctrl_ensure_locks(void)
{
    if (cxl_hybrid_control_locks_ready) {
        return;
    }

    qemu_mutex_init(&cxl_hybrid_control_request_lock);
    cxl_hybrid_control_locks_ready = true;
}

static void cxl_hybrid_ctrl_bind_state(CXLHybridControlState *state)
{
    state->hdr = cxl_hybrid_control_region.hdr;
    state->map_base = cxl_hybrid_control_region.map_base;
    state->map_len = cxl_hybrid_control_region.map_len;
    state->visible_bitmap = cxl_hybrid_control_region.visible_bitmap;
    state->page_state = cxl_hybrid_control_region.page_state;
    state->page_state_words = cxl_hybrid_control_region.page_state_words;
    state->visible_region_bitmap =
        cxl_hybrid_control_region.visible_region_bitmap;
    state->owned_region_bitmap =
        cxl_hybrid_control_region.owned_region_bitmap;
    state->request_ring = cxl_hybrid_control_region.request_ring;
    cxl_hybrid_transfer_queue_init_for_test(&state->transfer_queue);
    qatomic_set(&state->active_cxl_work, 0);
    qatomic_set(&state->dirty_seq, 0);
    state->shutdown = false;
}

static void cxl_hybrid_ctrl_release_region(void)
{
    if (cxl_hybrid_control_region.map_base) {
        munmap(cxl_hybrid_control_region.map_base, cxl_hybrid_control_region.map_len);
    }
    if (cxl_hybrid_control_region.fd >= 0) {
        close(cxl_hybrid_control_region.fd);
    }
    memset(&cxl_hybrid_control_region, 0, sizeof(cxl_hybrid_control_region));
    cxl_hybrid_control_region.fd = -1;
}

static void cxl_hybrid_ctrl_unbind_state(CXLHybridControlState *state)
{
    if (!state->hdr) {
        return;
    }

    if (state->transfer_queue.lock_ready) {
        cxl_hybrid_transfer_queue_destroy_for_test(&state->transfer_queue);
    }
    memset(state, 0, sizeof(*state));
    assert(cxl_hybrid_control_region.users > 0);
    cxl_hybrid_control_region.users--;
    if (!cxl_hybrid_control_region.users) {
        cxl_hybrid_ctrl_release_region();
    }
}

static int cxl_hybrid_ctrl_region_ensure(Error **errp)
{
    QIOChannelCXL *cioc = NULL;
    CXLHybridControlHeader *hdr;
    void *map_base;
    size_t map_len;
    off_t map_offset;
    uint64_t total_pages;
    uint64_t total_ram;
    uint64_t total_regions;
    uint64_t region_granule;
    uint32_t visible_page_words;
    size_t page_state_words;
    uint32_t visible_region_words;
    uint32_t owned_region_words;
    size_t visible_bitmap_bytes;
    size_t page_state_bytes;
    size_t visible_region_bitmap_bytes;
    size_t owned_region_bitmap_bytes;
    int fd;
    int prot;
    const char *path = migrate_cxl_path();

    if (cxl_hybrid_control_region.map_base) {
        return 0;
    }

    if (!path || !path[0]) {
        error_setg(errp,
                   "CXL hybrid fault control plane requires x-cxl-path");
        return -EINVAL;
    }
    if (!migrate_cxl_shared_backing()) {
        error_setg(errp,
                   "CXL hybrid postcopy requires x-cxl-shared-backing=true");
        return -EINVAL;
    }

    cioc = qio_channel_cxl_new_path(path, O_RDWR | O_CLOEXEC, errp);
    if (!cioc) {
        return -errno;
    }

    total_pages = cxl_hybrid_ctrl_total_pages();
    page_state_words = cxl_hybrid_control_page_state_words(total_pages);
    if (page_state_words == SIZE_MAX) {
        error_setg(errp,
                   "CXL hybrid fault control page-state array supports at "
                   "most %u pages (total-pages=%" PRIu64 ")",
                   UINT32_MAX, total_pages);
        object_unref(OBJECT(cioc));
        return -EOVERFLOW;
    }

    map_len = cxl_hybrid_fault_control_region_allocation_bytes(cioc->align);
    total_ram = ram_bytes_total();
    region_granule = cxl_hybrid_ctrl_region_granule(cioc->align, total_ram);
    total_regions = cxl_hybrid_ctrl_total_regions(total_pages,
                                                  region_granule);
    visible_page_words = cxl_hybrid_ctrl_visible_page_words(total_pages);
    visible_region_words =
        cxl_hybrid_ctrl_visible_region_words(total_regions);
    owned_region_words = cxl_hybrid_ctrl_owned_region_words(total_regions);
    visible_bitmap_bytes =
        cxl_hybrid_control_visible_bitmap_bytes(total_pages);
    page_state_bytes = cxl_hybrid_control_page_state_bytes(total_pages);
    visible_region_bitmap_bytes =
        cxl_hybrid_control_visible_region_bitmap_bytes(total_regions);
    owned_region_bitmap_bytes =
        cxl_hybrid_control_owned_region_bitmap_bytes(total_regions);
    map_offset = cxl_hybrid_ctrl_region_offset(cioc->align);
    if (cioc->map_size < (uint64_t)map_offset ||
        cioc->map_size - (uint64_t)map_offset < map_len) {
        error_setg(errp,
                   "CXL backing is too small for the hybrid fault control region");
        object_unref(OBJECT(cioc));
        return -ENOSPC;
    }

    fd = dup(cioc->fd);
    object_unref(OBJECT(cioc));
    if (fd < 0) {
        error_setg_errno(errp, errno,
                         "Failed to dup CXL hybrid fault control backing");
        return -errno;
    }

    prot = PROT_READ | PROT_WRITE;

    map_base = mmap(NULL, map_len, prot, MAP_SHARED, fd, map_offset);
    if (map_base == MAP_FAILED) {
        int saved_errno = errno;

        close(fd);
        error_setg_errno(errp, saved_errno,
                         "Failed to mmap CXL hybrid fault control region");
        return -saved_errno;
    }

    hdr = map_base;
    cxl_hybrid_control_region.map_base = map_base;
    cxl_hybrid_control_region.map_len = map_len;
    cxl_hybrid_control_region.map_offset = map_offset;
    cxl_hybrid_control_region.fd = fd;
    cxl_hybrid_control_region.hdr = hdr;
    cxl_hybrid_control_region.total_pages = total_pages;
    cxl_hybrid_control_region.total_regions = total_regions;
    cxl_hybrid_control_region.region_granule = region_granule;
    cxl_hybrid_control_region.visible_page_words = visible_page_words;
    cxl_hybrid_control_region.page_state_words = (uint32_t)page_state_words;
    cxl_hybrid_control_region.visible_region_words = visible_region_words;
    cxl_hybrid_control_region.owned_region_words = owned_region_words;
    cxl_hybrid_control_region.region_granule_shift =
        cxl_hybrid_control_region_granule_shift(region_granule);
    cxl_hybrid_control_region.request_ring_entries =
        cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_REQUEST_ORDER);
    cxl_hybrid_control_region.visible_bitmap = (unsigned long *)(hdr + 1);
    cxl_hybrid_control_region.page_state =
        (uint64_t *)((char *)cxl_hybrid_control_region.visible_bitmap +
                    visible_bitmap_bytes);
    cxl_hybrid_control_region.visible_region_bitmap =
        (unsigned long *)((char *)cxl_hybrid_control_region.page_state +
                         page_state_bytes);
    cxl_hybrid_control_region.owned_region_bitmap =
        (unsigned long *)((char *)
                          cxl_hybrid_control_region.visible_region_bitmap +
                          visible_region_bitmap_bytes);
    cxl_hybrid_control_region.request_ring =
        (CXLHybridFaultRequestRecord *)
        ((char *)cxl_hybrid_control_region.owned_region_bitmap +
         owned_region_bitmap_bytes);

    return 0;
}

static int cxl_hybrid_ctrl_state_init(CXLHybridControlState *state,
                                      Error **errp)
{
    int ret;

    if (state->hdr) {
        return 0;
    }

    cxl_hybrid_ctrl_ensure_locks();
    ret = cxl_hybrid_ctrl_region_ensure(errp);
    if (ret) {
        return ret;
    }

    cxl_hybrid_ctrl_bind_state(state);
    cxl_hybrid_control_region.users++;
    return 0;
}

static bool cxl_hybrid_ctrl_state_ready(const CXLHybridControlState *state)
{
    return state->hdr && state->visible_bitmap &&
           state->visible_region_bitmap && state->owned_region_bitmap &&
           state->request_ring &&
           state->hdr->magic == CXL_HYBRID_CTRL_MAGIC &&
           state->hdr->version == CXL_HYBRID_CTRL_VERSION &&
           state->hdr->request_ring_order == CXL_HYBRID_CTRL_REQUEST_ORDER;
}

static bool cxl_hybrid_ctrl_state_run_completed(
    const CXLHybridControlState *state,
    uint32_t generation)
{
    uint32_t completed;

    if (!state->hdr) {
        return false;
    }

    completed = qatomic_load_acquire(&state->hdr->completed_generation);
    return completed == generation;
}

static bool cxl_hybrid_ctrl_state_quiescing(
    const CXLHybridControlState *state,
    uint32_t generation)
{
    uint32_t flags;

    if (!state->hdr ||
        !cxl_hybrid_control_generation_matches(state->hdr, generation)) {
        return false;
    }

    flags = qatomic_load_acquire(&state->hdr->completion_flags);
    return flags & CXL_HYBRID_CTRL_COMPLETION_F_QUIESCE;
}

static int cxl_hybrid_ctrl_request_completed_status(
    const CXLHybridControlState *state,
    const CXLHybridFaultRequestRecord *record,
    Error **errp)
{
    return cxl_hybrid_fault_request_completed_status(state->hdr,
                                                     state->visible_bitmap,
                                                     record, errp);
}

static uint64_t cxl_hybrid_ctrl_active_request_count(
    const CXLHybridControlState *state)
{
    return state->hdr ? qatomic_load_acquire(&state->hdr->active_request_count)
                      : 0;
}

static uint64_t cxl_hybrid_ctrl_active_enqueue_count(
    const CXLHybridControlState *state)
{
    return state->hdr ? qatomic_load_acquire(&state->hdr->active_enqueue_count)
                      : 0;
}

static bool cxl_hybrid_ctrl_cxl_queue_empty(CXLHybridControlState *state)
{
    return cxl_hybrid_transfer_queue_depth(&state->transfer_queue,
                                           CXL_HYBRID_TRANSFER_CXL_HIGH) == 0 &&
           cxl_hybrid_transfer_queue_depth(&state->transfer_queue,
                                           CXL_HYBRID_TRANSFER_CXL_LOW) == 0 &&
           qatomic_load_acquire(&state->active_cxl_work) == 0;
}

static void cxl_hybrid_ctrl_active_enqueue_begin(CXLHybridControlState *state)
{
    qatomic_inc(&state->hdr->active_enqueue_count);
}

static void cxl_hybrid_ctrl_active_enqueue_end(CXLHybridControlState *state)
{
    qatomic_dec(&state->hdr->active_enqueue_count);
}

static void cxl_hybrid_ctrl_active_request_begin(
    CXLHybridControlState *state)
{
    qatomic_inc(&state->hdr->active_request_count);
}

static void cxl_hybrid_ctrl_active_request_end(
    CXLHybridControlState *state)
{
    qatomic_dec(&state->hdr->active_request_count);
}

static void cxl_hybrid_ctrl_abort_generation(CXLHybridControlState *state,
                                             uint32_t generation)
{
    if (state->hdr) {
        cxl_hybrid_control_abort_generation(state->hdr, generation);
    }
}

static void cxl_hybrid_ctrl_enqueue_fault_region_prefetch(
    const CXLHybridFaultRegionPlan *plan,
    uint32_t generation)
{
    uint64_t end_page;

    if (!plan || !plan->prefetch_nr_pages) {
        return;
    }

    end_page = plan->prefetch_first_page + plan->prefetch_nr_pages;
    for (uint64_t page = plan->prefetch_first_page; page < end_page; page++) {
        RAMBlock *prefetch_block;
        ram_addr_t prefetch_offset;
        uint64_t cxl_offset;

        if (page == plan->prefetch_skip_page ||
            !cxl_hybrid_lookup_global_page(page, &prefetch_block,
                                           &prefetch_offset) ||
            !cxl_hybrid_source_page_cxl_offset(
                qemu_ram_get_idstr(prefetch_block), prefetch_offset,
                &cxl_offset)) {
            continue;
        }

        cxl_hybrid_ctrl_enqueue_cxl_page(prefetch_block, prefetch_offset, page,
                                         cxl_offset, generation,
                                         CXL_HYBRID_TRANSFER_CXL_HIGH);
    }
}

static void *cxl_hybrid_ctrl_request_worker_thread(void *opaque)
{
    CXLHybridControlState *state = opaque;
    CXLHybridFaultRequestRecord record;
    RAMBlock *block;
    ram_addr_t block_offset;

    trace_cxl_hybrid_ctrl_request_worker_start();
    while (!qatomic_read(&state->shutdown)) {
        if (cxl_hybrid_ctrl_dequeue_fault_request(&record)) {
            Error *local_err = NULL;

            if (!cxl_hybrid_control_generation_matches(state->hdr,
                                                       record.generation)) {
                goto request_done;
            }
            if (record.flags & CXL_HYBRID_FAULT_REQUEST_F_REGION) {
                CXLHybridFaultRegionPlan plan = { 0 };

                trace_cxl_hybrid_region_request_dequeue(record.page_index,
                                                        record.nr_pages,
                                                        record.generation);
                if (cxl_hybrid_ctrl_validate_region_span(
                        state->hdr, record.page_index, record.nr_pages,
                        NULL, &local_err)) {
                    if (local_err) {
                        error_report_err(local_err);
                    }
                    cxl_hybrid_ctrl_abort_generation(state, record.generation);
                    goto request_done;
                }
                if (!cxl_hybrid_fault_region_plan(record.page_index,
                                                  record.nr_pages,
                                                  record.demand_page,
                                                  &plan) ||
                    !cxl_hybrid_lookup_global_page(plan.demand_page, &block,
                                                   &block_offset)) {
                    cxl_hybrid_ctrl_abort_generation(state, record.generation);
                    goto request_done;
                }

                cxl_hybrid_note_publish_request_received(qemu_ram_get_idstr(block),
                                                         block_offset,
                                                         record.generation,
                                                         record.request_ts_ns);
                if (cxl_hybrid_publish_fault_request_core(
                        qemu_ram_get_idstr(block),
                        block_offset,
                        TARGET_PAGE_SIZE,
                        record.generation,
                        false,
                        &local_err)) {
                    if (local_err) {
                        error_report_err(local_err);
                    }
                    cxl_hybrid_ctrl_abort_generation(state, record.generation);
                    goto request_done;
                }
                cxl_hybrid_ctrl_enqueue_fault_region_prefetch(
                    &plan, record.generation);
                goto request_done;
            }
            if (!cxl_hybrid_lookup_global_page(record.page_index, &block,
                                               &block_offset)) {
                cxl_hybrid_ctrl_abort_generation(state, record.generation);
                goto request_done;
            }

            cxl_hybrid_note_publish_request_received(qemu_ram_get_idstr(block),
                                                     block_offset,
                                                     record.generation,
                                                     record.request_ts_ns);
            if (cxl_hybrid_publish_fault_request_core(
                    qemu_ram_get_idstr(block),
                    block_offset,
                    TARGET_PAGE_SIZE,
                    record.generation,
                    !migrate_cxl_fault_resolve_uses_region(),
                    &local_err)) {
                if (local_err) {
                    error_report_err(local_err);
                }
                cxl_hybrid_ctrl_abort_generation(state, record.generation);
            }
request_done:
            cxl_hybrid_ctrl_active_request_end(state);
            continue;
        }
        g_usleep(50);
    }
    trace_cxl_hybrid_ctrl_request_worker_stop();
    return NULL;
}

static void cxl_hybrid_ctrl_start_request_worker(CXLHybridControlState *state)
{
    if (state->request_worker_running) {
        return;
    }

    qatomic_set(&state->shutdown, false);
    qemu_thread_create(&state->request_worker, "cxl-ctrl-req",
                       cxl_hybrid_ctrl_request_worker_thread, state,
                       QEMU_THREAD_JOINABLE);
    state->request_worker_running = true;
}

static void cxl_hybrid_ctrl_stop_request_worker(CXLHybridControlState *state)
{
    if (!state->request_worker_running) {
        return;
    }

    qatomic_set(&state->shutdown, true);
    qemu_thread_join(&state->request_worker);
    state->request_worker_running = false;
    qatomic_set(&state->shutdown, false);
}

static void cxl_hybrid_ctrl_drop_cxl_descriptor(CXLHybridControlState *state,
                                                const CXLHybridPageDescriptor *desc)
{
    if (!state->page_state || !desc || !desc->has_claim ||
        desc->page_index >= state->page_state_words) {
        return;
    }

    cxl_hybrid_page_state_drop_claim(&state->page_state[desc->page_index],
                                     &desc->claim);
}

static bool cxl_hybrid_ctrl_process_cxl_descriptor(
    CXLHybridControlState *state,
    const CXLHybridPageDescriptor *desc,
    int *retp,
    uint64_t *copied_bytes)
{
    Error *local_err = NULL;
    bool completed;
    int ret;

    if (retp) {
        *retp = -EINVAL;
    }
    if (copied_bytes) {
        *copied_bytes = 0;
    }

    if (!desc || !desc->block || !desc->has_claim || desc->nr_pages != 1 ||
        desc->page_index >= state->page_state_words) {
        cxl_hybrid_ctrl_drop_cxl_descriptor(state, desc);
        return false;
    }

    ret = cxl_hybrid_copy_page_to_stable_cxl(desc->block, desc->block_offset,
                                             desc->cxl_offset,
                                             TARGET_PAGE_SIZE, &local_err);
    if (retp) {
        *retp = ret;
    }
    if (ret) {
        if (local_err) {
            error_report_err(local_err);
        }
        cxl_hybrid_ctrl_drop_cxl_descriptor(state, desc);
        return false;
    }
    if (copied_bytes) {
        *copied_bytes = TARGET_PAGE_SIZE;
    }

    completed = cxl_hybrid_control_complete_cxl_page_visible_generation(
        state->hdr, state->visible_bitmap, state->page_state,
        desc->page_index, desc->generation, &desc->claim);
    if (!completed) {
        cxl_hybrid_ctrl_drop_cxl_descriptor(state, desc);
    }
    return completed;
}

static bool cxl_hybrid_ctrl_cxl_batch_valid(CXLHybridControlState *state,
                                            const CXLHybridPageDescriptor *descs,
                                            uint32_t count)
{
    if (!state || !descs || !count) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        const CXLHybridPageDescriptor *desc = &descs[i];

        if (!desc->block || !desc->has_claim || desc->nr_pages != 1 ||
            desc->page_index >= state->page_state_words) {
            return false;
        }
        if (i > 0) {
            const CXLHybridPageDescriptor *prev = &descs[i - 1];

            if (desc->block != prev->block ||
                desc->generation != prev->generation ||
                desc->page_index != prev->page_index + 1 ||
                desc->block_offset != prev->block_offset + TARGET_PAGE_SIZE ||
                desc->cxl_offset != prev->cxl_offset + TARGET_PAGE_SIZE) {
                return false;
            }
        }
    }
    return true;
}

static void cxl_hybrid_ctrl_process_cxl_batch_fallback(
    CXLHybridControlState *state,
    const CXLHybridPageDescriptor *descs,
    uint32_t count,
    int *rets,
    uint64_t *copied_bytes,
    bool *completed)
{
    for (uint32_t i = 0; i < count; i++) {
        completed[i] = cxl_hybrid_ctrl_process_cxl_descriptor(
            state, &descs[i], &rets[i], &copied_bytes[i]);
    }
}

static void cxl_hybrid_ctrl_process_cxl_batch(
    CXLHybridControlState *state,
    const CXLHybridPageDescriptor *descs,
    uint32_t count,
    int *rets,
    uint64_t *copied_bytes,
    bool *completed)
{
    Error *local_err = NULL;
    uint32_t span_bytes;
    int ret;

    for (uint32_t i = 0; i < count; i++) {
        rets[i] = -EINVAL;
        copied_bytes[i] = 0;
        completed[i] = false;
    }

    if (!count) {
        return;
    }
    if (count == 1 ||
        !cxl_hybrid_ctrl_cxl_batch_valid(state, descs, count)) {
        cxl_hybrid_ctrl_process_cxl_batch_fallback(
            state, descs, count, rets, copied_bytes, completed);
        return;
    }

    span_bytes = count * TARGET_PAGE_SIZE;
    ret = cxl_hybrid_copy_page_to_stable_cxl(descs[0].block,
                                             descs[0].block_offset,
                                             descs[0].cxl_offset,
                                             span_bytes,
                                             &local_err);
    if (ret) {
        if (local_err) {
            error_report_err(local_err);
        }
        for (uint32_t i = 0; i < count; i++) {
            rets[i] = ret;
            cxl_hybrid_ctrl_drop_cxl_descriptor(state, &descs[i]);
        }
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        rets[i] = 0;
        copied_bytes[i] = TARGET_PAGE_SIZE;
        completed[i] = cxl_hybrid_control_complete_cxl_page_visible_generation(
            state->hdr, state->visible_bitmap, state->page_state,
            descs[i].page_index, descs[i].generation, &descs[i].claim);
        if (!completed[i]) {
            cxl_hybrid_ctrl_drop_cxl_descriptor(state, &descs[i]);
        }
    }
}

static void *cxl_hybrid_ctrl_cxl_worker_thread(void *opaque)
{
    CXLHybridControlState *state = opaque;
    CXLHybridPageDescriptor descs[CXL_HYBRID_CXL_WORKER_MAX_BATCH];
    uint64_t copied_bytes[CXL_HYBRID_CXL_WORKER_MAX_BATCH];
    bool completed[CXL_HYBRID_CXL_WORKER_MAX_BATCH];
    int rets[CXL_HYBRID_CXL_WORKER_MAX_BATCH];
    CXLHybridTransferClass klass;

    while (!qatomic_read(&state->shutdown)) {
        uint32_t count = cxl_hybrid_transfer_queue_pop_cxl_batch(
            &state->transfer_queue, descs, G_N_ELEMENTS(descs), &klass);

        if (count) {
            uint64_t start_ns;
            uint64_t end_ns;
            uint64_t elapsed_ns;

            qatomic_inc(&state->active_cxl_work);
            start_ns = cxl_hybrid_ctrl_now_ns();
            cxl_hybrid_ctrl_process_cxl_batch(
                state, descs, count, rets, copied_bytes, completed);
            end_ns = cxl_hybrid_ctrl_now_ns();
            elapsed_ns = (end_ns - start_ns) / count;
            for (uint32_t i = 0; i < count; i++) {
                trace_cxl_hybrid_cxl_worker_complete(
                    end_ns, descs[i].page_index, descs[i].generation,
                    (uint32_t)klass, copied_bytes[i], elapsed_ns, rets[i],
                    completed[i]);
            }
            qatomic_dec(&state->active_cxl_work);
            continue;
        }
        g_usleep(50);
    }
    return NULL;
}

static void cxl_hybrid_ctrl_start_cxl_worker(CXLHybridControlState *state)
{
    if (state->cxl_worker_running) {
        return;
    }

    qatomic_set(&state->shutdown, false);
    qemu_thread_create(&state->cxl_worker, "cxl-ctrl-cxl",
                       cxl_hybrid_ctrl_cxl_worker_thread, state,
                       QEMU_THREAD_JOINABLE);
    state->cxl_worker_running = true;
}

static void cxl_hybrid_ctrl_stop_cxl_worker(CXLHybridControlState *state)
{
    if (!state->cxl_worker_running) {
        return;
    }

    qatomic_set(&state->shutdown, true);
    qemu_thread_join(&state->cxl_worker);
    state->cxl_worker_running = false;
    qatomic_set(&state->shutdown, false);
}

static void cxl_hybrid_ctrl_publish_request(CXLHybridFaultRequestRecord *slot,
                                            const CXLHybridFaultRequestRecord *src,
                                            uint64_t seq)
{
    *slot = *src;
    smp_wmb();
    qatomic_set(&slot->seq, seq);
}

static int cxl_hybrid_ctrl_try_enqueue_request(CXLHybridControlState *state,
                                               const CXLHybridFaultRequestRecord *record,
                                               bool *queuedp,
                                               Error **errp)
{
    CXLHybridFaultRequestRecord entry = { 0 };
    CXLHybridFaultRequestRecord *slot;
    uint64_t cons;
    uint64_t prod;
    uint64_t seq;
    uint64_t mask;
    int ret = 0;

    if (queuedp) {
        *queuedp = false;
    }
    if (!cxl_hybrid_ctrl_state_ready(state)) {
        error_setg(errp,
                   "CXL hybrid fault request ring is not initialized");
        return -EINVAL;
    }

    cxl_hybrid_ctrl_active_enqueue_begin(state);
    smp_mb();

    qemu_mutex_lock(&cxl_hybrid_control_request_lock);
    if (cxl_hybrid_ctrl_state_run_completed(state, record->generation)) {
        ret = cxl_hybrid_ctrl_request_completed_status(state, record, errp);
        goto out;
    }
    if (cxl_hybrid_ctrl_state_quiescing(state, record->generation)) {
        ret = 0;
        goto out;
    }

    prod = qatomic_read(&state->hdr->request_prod);
    cons = qatomic_read(&state->hdr->request_cons);
    if (prod - cons >= cxl_hybrid_control_region.request_ring_entries) {
        error_setg(errp, "CXL hybrid fault request ring is full");
        ret = -ENOSPC;
        goto out;
    }

    seq = prod + 1;
    mask = cxl_hybrid_control_region.request_ring_entries - 1;
    slot = &state->request_ring[prod & mask];
    entry = *record;
    entry.seq = 0;
    cxl_hybrid_ctrl_publish_request(slot, &entry, seq);
    qatomic_store_release(&state->hdr->request_prod, seq);
    if (queuedp) {
        *queuedp = true;
    }

out:
    qemu_mutex_unlock(&cxl_hybrid_control_request_lock);
    smp_mb();
    cxl_hybrid_ctrl_active_enqueue_end(state);
    return ret;
}

static bool cxl_hybrid_ctrl_try_dequeue_request(CXLHybridControlState *state,
                                                CXLHybridFaultRequestRecord *record)
{
    CXLHybridFaultRequestRecord *slot;
    uint64_t expected_seq;
    uint64_t mask;
    uint64_t prod;
    uint64_t cons;

    if (!record || !cxl_hybrid_ctrl_state_ready(state)) {
        return false;
    }

    cons = qatomic_read(&state->hdr->request_cons);
    prod = qatomic_read(&state->hdr->request_prod);
    if (cons == prod) {
        return false;
    }

    expected_seq = cons + 1;
    mask = cxl_hybrid_control_region.request_ring_entries - 1;
    slot = &state->request_ring[cons & mask];
    if (qatomic_read(&slot->seq) != expected_seq) {
        return false;
    }

    smp_rmb();
    *record = *slot;
    cxl_hybrid_ctrl_active_request_begin(state);
    qatomic_store_release(&state->hdr->request_cons, expected_seq);
    return true;
}

int cxl_hybrid_control_init_source(Error **errp)
{
    int ret;

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_source, errp);
    if (ret) {
        return ret;
    }

    return 0;
}

int cxl_hybrid_control_init_destination(Error **errp)
{
    int ret;

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_destination, errp);
    if (ret) {
        return ret;
    }

    return 0;
}

int cxl_hybrid_control_begin_source_run(Error **errp)
{
    unsigned long *visible_region_bitmap;
    unsigned long *owned_region_bitmap;
    int ret;
    uint32_t generation;

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_source, errp);
    if (ret) {
        return ret;
    }

    generation = cxl_hybrid_fault_publish_generation_begin_source_run();
    visible_region_bitmap =
        cxl_hybrid_control_source.visible_region_bitmap;
    owned_region_bitmap = cxl_hybrid_control_source.owned_region_bitmap;
    cxl_hybrid_control_reset_run_state(cxl_hybrid_control_source.hdr,
                                       cxl_hybrid_control_source.visible_bitmap,
                                       cxl_hybrid_control_region.total_pages,
                                       cxl_hybrid_control_source.page_state,
                                       cxl_hybrid_control_source.page_state_words,
                                       visible_region_bitmap,
                                       cxl_hybrid_control_region.total_regions,
                                       owned_region_bitmap,
                                       cxl_hybrid_control_region.total_regions,
                                       cxl_hybrid_control_region.region_granule,
                                       TARGET_PAGE_BITS,
                                       generation);
    qatomic_set(&cxl_hybrid_control_source.dirty_seq, 0);
    cxl_hybrid_ctrl_start_request_worker(&cxl_hybrid_control_source);
    cxl_hybrid_ctrl_start_cxl_worker(&cxl_hybrid_control_source);
    return 0;
}

int cxl_hybrid_control_complete_source_run(Error **errp)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;
    uint32_t generation;

    if (!cxl_hybrid_ctrl_state_ready(state)) {
        error_setg(errp,
                   "CXL hybrid fault control source is not initialized");
        return -EINVAL;
    }

    generation = cxl_hybrid_fault_publish_generation();
    if (!cxl_hybrid_control_generation_matches(state->hdr, generation)) {
        error_setg(errp,
                   "CXL hybrid fault control completion generation mismatch "
                   "(header=%u expected=%u)",
                   cxl_hybrid_control_generation(state->hdr), generation);
        return -EINVAL;
    }

    qatomic_or(&state->hdr->completion_flags,
               CXL_HYBRID_CTRL_COMPLETION_F_QUIESCE);

    while (cxl_hybrid_ctrl_active_enqueue_count(state) != 0) {
        if (!cxl_hybrid_control_generation_matches(state->hdr, generation)) {
            error_setg(errp,
                       "CXL hybrid fault control generation changed while "
                       "waiting for active enqueues");
            return -EINVAL;
        }
        cpu_relax();
    }

    while (qatomic_load_acquire(&state->hdr->request_cons) !=
           qatomic_load_acquire(&state->hdr->request_prod)) {
        if (!cxl_hybrid_control_generation_matches(state->hdr, generation)) {
            error_setg(errp,
                       "CXL hybrid fault control generation changed while "
                       "draining requests");
            return -EINVAL;
        }
        cpu_relax();
    }

    while (cxl_hybrid_ctrl_active_request_count(state) != 0) {
        if (!cxl_hybrid_control_generation_matches(state->hdr, generation)) {
            error_setg(errp,
                       "CXL hybrid fault control generation changed while "
                       "waiting for active requests");
            return -EINVAL;
        }
        cpu_relax();
    }

    while (cxl_hybrid_control_source_write_count(state->hdr) != 0) {
        if (!cxl_hybrid_control_generation_matches(state->hdr, generation)) {
            error_setg(errp,
                       "CXL hybrid fault control generation changed while "
                       "waiting for source writes");
            return -EINVAL;
        }
        cpu_relax();
    }

    while (!cxl_hybrid_ctrl_cxl_queue_empty(state)) {
        if (!cxl_hybrid_control_generation_matches(state->hdr, generation)) {
            error_setg(errp,
                       "CXL hybrid fault control generation changed while "
                       "waiting for CXL worker");
            return -EINVAL;
        }
        cpu_relax();
    }

    qatomic_store_release(&state->hdr->completed_generation, generation);
    return 0;
}

void cxl_hybrid_ctrl_trace_page_state_snapshot(const char *tag)
{
    CXLHybridControlState *state = cxl_hybrid_control_source.hdr ?
        &cxl_hybrid_control_source : &cxl_hybrid_control_destination;
    CXLHybridPageStateSnapshot snapshot = { 0 };
    uint64_t now_ns;
    uint32_t generation;
    uint32_t completed_generation;
    uint32_t flags;
    uint64_t request_prod;
    uint64_t request_cons;
    uint64_t request_backlog = 0;
    uint64_t cxl_high_depth = 0;
    uint64_t cxl_low_depth = 0;

    if (!tag) {
        tag = "unknown";
    }
    now_ns = cxl_hybrid_ctrl_now_ns();
    if (!state->hdr || !state->page_state) {
        trace_cxl_hybrid_page_state_snapshot_summary(
            tag, now_ns, 0, 0, 0, 0, 0, 0, 0, 0);
        trace_cxl_hybrid_page_state_snapshot_state(
            tag, now_ns, 0, 0, 0, 0, 0, 0, 0, 0);
        trace_cxl_hybrid_page_state_snapshot_runtime(
            tag, now_ns, 0, 0, 0, 0, 0, 0, 0, 0);
        return;
    }

    generation = cxl_hybrid_control_generation(state->hdr);
    completed_generation =
        qatomic_load_acquire(&state->hdr->completed_generation);
    flags = qatomic_load_acquire(&state->hdr->completion_flags);
    request_prod = qatomic_load_acquire(&state->hdr->request_prod);
    request_cons = qatomic_load_acquire(&state->hdr->request_cons);
    if (request_prod >= request_cons) {
        request_backlog = request_prod - request_cons;
    }
    if (state->transfer_queue.lock_ready) {
        cxl_high_depth = cxl_hybrid_transfer_queue_depth(
            &state->transfer_queue, CXL_HYBRID_TRANSFER_CXL_HIGH);
        cxl_low_depth = cxl_hybrid_transfer_queue_depth(
            &state->transfer_queue, CXL_HYBRID_TRANSFER_CXL_LOW);
    }

    cxl_hybrid_page_state_snapshot(state->page_state,
                                   state->visible_bitmap,
                                   state->hdr->page_state_words,
                                   generation, &snapshot);
    trace_cxl_hybrid_page_state_snapshot_summary(
        tag, now_ns, generation, completed_generation, flags,
        snapshot.total_pages, snapshot.visible, snapshot.generation_mismatch,
        snapshot.other, request_backlog);
    trace_cxl_hybrid_page_state_snapshot_state(
        tag, now_ns, snapshot.not_sent, snapshot.dirty, snapshot.in_flight,
        snapshot.in_flight_cxl, snapshot.in_flight_rdma, snapshot.published,
        snapshot.published_cxl, snapshot.published_dst_local);
    trace_cxl_hybrid_page_state_snapshot_runtime(
        tag, now_ns, snapshot.published_zero, snapshot.published_invisible,
        cxl_high_depth, cxl_low_depth,
        qatomic_load_acquire(&state->active_cxl_work),
        cxl_hybrid_control_source_write_count(state->hdr),
        cxl_hybrid_ctrl_active_enqueue_count(state),
        cxl_hybrid_ctrl_active_request_count(state));
}

bool cxl_hybrid_control_source_run_completed(uint32_t generation)
{
    CXLHybridControlState *state = cxl_hybrid_control_destination.hdr ?
        &cxl_hybrid_control_destination : &cxl_hybrid_control_source;

    if (!state->hdr) {
        return false;
    }

    return cxl_hybrid_ctrl_state_run_completed(state, generation);
}

int cxl_hybrid_control_activate_destination(Error **errp)
{
    int ret;
    CXLHybridControlHeader *hdr;
    uint64_t region_granule;
    uint32_t generation;
    size_t expected_visible_page_words;
    size_t expected_page_state_words;
    size_t expected_owned_region_words;
    uint32_t expected_region_granule_shift;

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_destination, errp);
    if (ret) {
        return ret;
    }

    if (!cxl_hybrid_ctrl_state_ready(&cxl_hybrid_control_destination)) {
        error_setg(errp,
                   "CXL hybrid fault control header is not initialized");
        return -EINVAL;
    }

    hdr = cxl_hybrid_control_destination.hdr;
    region_granule = cxl_hybrid_control_region.region_granule;
    expected_visible_page_words =
        cxl_hybrid_control_visible_bitmap_words(cxl_hybrid_ctrl_total_pages());
    if (hdr->visible_page_words != expected_visible_page_words) {
        error_setg(errp,
                   "CXL hybrid fault control visible bitmap mismatch "
                   "(header=%u expected=%zu)",
                   hdr->visible_page_words,
                   expected_visible_page_words);
        return -EINVAL;
    }
    if (hdr->total_pages != cxl_hybrid_control_region.total_pages) {
        error_setg(errp,
                   "CXL hybrid fault control total pages mismatch "
                   "(header=%" PRIu64 " expected=%" PRIu64 ")",
                   hdr->total_pages,
                   cxl_hybrid_control_region.total_pages);
        return -EINVAL;
    }
    expected_page_state_words =
        cxl_hybrid_control_page_state_words(cxl_hybrid_ctrl_total_pages());
    if (expected_page_state_words == SIZE_MAX) {
        error_setg(errp,
                   "CXL hybrid fault control page-state array supports at "
                   "most %u pages",
                   UINT32_MAX);
        return -EOVERFLOW;
    }
    if (hdr->page_state_words != expected_page_state_words) {
        error_setg(errp,
                   "CXL hybrid fault control page state mismatch "
                   "(header=%u expected=%zu)",
                   hdr->page_state_words,
                   expected_page_state_words);
        return -EINVAL;
    }
    if (hdr->page_state_word_size != sizeof(uint64_t)) {
        error_setg(errp,
                   "CXL hybrid fault control page state word size mismatch "
                   "(header=%u expected=%zu)",
                   hdr->page_state_word_size,
                   sizeof(uint64_t));
        return -EINVAL;
    }
    expected_owned_region_words =
        cxl_hybrid_control_owned_region_bitmap_words(
            cxl_hybrid_ctrl_total_regions(cxl_hybrid_ctrl_total_pages(),
                                          region_granule));
    if (hdr->owned_region_words != expected_owned_region_words) {
        error_setg(errp,
                   "CXL hybrid fault control owned region bitmap mismatch "
                   "(header=%u expected=%zu)",
                   hdr->owned_region_words,
                   expected_owned_region_words);
        return -EINVAL;
    }
    if (hdr->total_regions != cxl_hybrid_control_region.total_regions) {
        error_setg(errp,
                   "CXL hybrid fault control total regions mismatch "
                   "(header=%" PRIu64 " expected=%" PRIu64 ")",
                   hdr->total_regions,
                   cxl_hybrid_control_region.total_regions);
        return -EINVAL;
    }
    if (hdr->region_granule != region_granule) {
        error_setg(errp,
                   "CXL hybrid fault control region granule mismatch "
                   "(header=%" PRIu64 " expected=%" PRIu64 ")",
                   hdr->region_granule, region_granule);
        return -EINVAL;
    }
    expected_region_granule_shift =
        cxl_hybrid_control_region_granule_shift(region_granule);
    if (hdr->region_granule_shift != expected_region_granule_shift) {
        error_setg(errp,
                   "CXL hybrid fault control region granule shift mismatch "
                   "(header=%u expected=%u)",
                   hdr->region_granule_shift,
                   expected_region_granule_shift);
        return -EINVAL;
    }

    generation = cxl_hybrid_fault_publish_generation();
    if (!cxl_hybrid_control_generation_matches(hdr, generation)) {
        error_setg(errp,
                   "CXL hybrid fault control generation mismatch "
                   "(header=%u expected=%u)",
                   cxl_hybrid_control_generation(hdr),
                   generation);
        return -EINVAL;
    }

    return 0;
}

void cxl_hybrid_control_cleanup_source(void)
{
    cxl_hybrid_ctrl_stop_request_worker(&cxl_hybrid_control_source);
    cxl_hybrid_ctrl_stop_cxl_worker(&cxl_hybrid_control_source);
    cxl_hybrid_ctrl_unbind_state(&cxl_hybrid_control_source);
}

void cxl_hybrid_control_cleanup_destination(void)
{
    cxl_hybrid_ctrl_unbind_state(&cxl_hybrid_control_destination);
}

bool cxl_hybrid_ctrl_page_visible(uint64_t page_index, uint32_t generation)
{
    CXLHybridControlState *state = cxl_hybrid_control_destination.hdr ?
        &cxl_hybrid_control_destination : &cxl_hybrid_control_source;

    return cxl_hybrid_control_page_visible(state->hdr, state->visible_bitmap,
                                           page_index, generation);
}

bool cxl_hybrid_ctrl_page_location(uint64_t page_index, uint32_t generation,
                                   CXLHybridPageLocation *locationp)
{
    CXLHybridControlState *state = cxl_hybrid_control_destination.hdr ?
        &cxl_hybrid_control_destination : &cxl_hybrid_control_source;

    return cxl_hybrid_control_page_location(state->hdr, state->visible_bitmap,
                                            state->page_state, page_index,
                                            generation, locationp);
}

bool cxl_hybrid_ctrl_page_requires_destination_install(
    uint64_t page_index,
    uint32_t generation,
    bool received,
    CXLHybridPageLocation *locationp)
{
    CXLHybridControlState *state = cxl_hybrid_control_destination.hdr ?
        &cxl_hybrid_control_destination : &cxl_hybrid_control_source;

    return cxl_hybrid_control_page_requires_destination_install(
        state->hdr, state->visible_bitmap, state->page_state, page_index,
        generation, received, locationp);
}

bool cxl_hybrid_ctrl_page_requires_postcopy_discard(uint64_t page_index,
                                                    uint32_t generation)
{
    CXLHybridControlState *state = cxl_hybrid_control_destination.hdr ?
        &cxl_hybrid_control_destination : &cxl_hybrid_control_source;

    return cxl_hybrid_control_page_requires_postcopy_discard(
        state->hdr, state->visible_bitmap, state->page_state, page_index,
        generation);
}

void cxl_hybrid_ctrl_set_page_visible(uint64_t page_index,
                                      uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr || !state->visible_bitmap) {
        return;
    }

    cxl_hybrid_control_mark_page_visible_generation(
        state->hdr, state->visible_bitmap, state->page_state, page_index,
        generation, CXL_HYBRID_PAGE_LOCATION_CXL);
}

void cxl_hybrid_ctrl_set_pages_visible(uint64_t first_page,
                                       uint64_t nr_pages,
                                       uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr || !state->visible_bitmap) {
        return;
    }

    cxl_hybrid_control_mark_pages_visible_generation(
        state->hdr, state->visible_bitmap, state->page_state, first_page,
        nr_pages, generation, CXL_HYBRID_PAGE_LOCATION_CXL);
}

void cxl_hybrid_ctrl_mark_page_dirty(uint64_t page_index, uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;
    uint32_t dirty_seq;

    if (!state->hdr || !state->visible_bitmap || !state->page_state ||
        page_index >= state->page_state_words ||
        !cxl_hybrid_control_generation_matches(state->hdr, generation)) {
        return;
    }

    dirty_seq = qatomic_inc_fetch(&state->dirty_seq);
    if (!dirty_seq) {
        dirty_seq = qatomic_inc_fetch(&state->dirty_seq);
    }
    cxl_hybrid_control_mark_page_dirty_generation(
        state->hdr, state->visible_bitmap, state->page_state, page_index,
        generation, dirty_seq);
}

uint64_t cxl_hybrid_ctrl_mark_dirty_pages(const unsigned long *dirty_bitmap,
                                          uint64_t dirty_first_page,
                                          uint64_t state_first_page,
                                          uint64_t nr_pages,
                                          uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;
    uint32_t dirty_seq;

    if (!state->hdr || !state->visible_bitmap || !state->page_state ||
        !dirty_bitmap || !nr_pages ||
        !cxl_hybrid_control_generation_matches(state->hdr, generation)) {
        return 0;
    }

    dirty_seq = qatomic_inc_fetch(&state->dirty_seq);
    if (!dirty_seq) {
        dirty_seq = qatomic_inc_fetch(&state->dirty_seq);
    }

    return cxl_hybrid_control_mark_dirty_pages_generation(
        state->hdr, state->visible_bitmap, state->page_state, dirty_bitmap,
        dirty_first_page, state_first_page, nr_pages, generation, dirty_seq);
}

bool cxl_hybrid_ctrl_complete_rdma_page_visible(
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr || !state->visible_bitmap || !state->page_state) {
        return false;
    }

    return cxl_hybrid_control_complete_rdma_page_visible_generation(
        state->hdr, state->visible_bitmap, state->page_state, page_index,
        generation, claim);
}

bool cxl_hybrid_ctrl_enqueue_cxl_page(RAMBlock *block,
                                      ram_addr_t block_offset,
                                      uint64_t page_index,
                                      uint64_t cxl_offset,
                                      uint32_t generation,
                                      CXLHybridTransferClass klass)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;
    CXLHybridPageDescriptor desc = {
        .block = block,
        .block_offset = block_offset,
        .page_index = page_index,
        .cxl_offset = cxl_offset,
        .generation = generation,
        .nr_pages = 1,
    };

    if (!state->hdr || !state->page_state || !state->transfer_queue.lock_ready ||
        page_index >= state->page_state_words ||
        (klass != CXL_HYBRID_TRANSFER_CXL_HIGH &&
         klass != CXL_HYBRID_TRANSFER_CXL_LOW) ||
        !cxl_hybrid_control_generation_matches(state->hdr, generation) ||
        cxl_hybrid_ctrl_state_quiescing(state, generation)) {
        return false;
    }

    if (!cxl_hybrid_page_state_claim_for_cxl(&state->page_state[page_index],
                                             generation, &desc.claim)) {
        return false;
    }
    desc.has_claim = true;
    cxl_hybrid_transfer_queue_push(&state->transfer_queue, klass, &desc);
    trace_cxl_hybrid_cxl_worker_enqueue(
        cxl_hybrid_ctrl_now_ns(), page_index, generation, (uint32_t)klass);
    return true;
}

bool cxl_hybrid_ctrl_claim_rdma_pages(CXLHybridRDMAPageDescriptor *desc,
                                      RAMBlock *block,
                                      ram_addr_t block_offset,
                                      uint64_t first_page,
                                      uint32_t nr_pages,
                                      uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr || !state->page_state ||
        !cxl_hybrid_control_generation_matches(state->hdr, generation)) {
        return false;
    }
    if (!cxl_hybrid_rdma_descriptor_claim_pages_for_test(
            desc, state->page_state, state->page_state_words, first_page,
            nr_pages, generation)) {
        return false;
    }

    desc->block = block;
    desc->block_offset = block_offset;
    return true;
}

void cxl_hybrid_ctrl_drop_rdma_pages(CXLHybridRDMAPageDescriptor *desc)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr || !state->page_state || !desc) {
        return;
    }

    cxl_hybrid_rdma_descriptor_drop_pages_for_test(
        desc, state->page_state, state->page_state_words);
}

void cxl_hybrid_ctrl_complete_rdma_pages(CXLHybridRDMAPageDescriptor *desc,
                                         uint32_t *completedp,
                                         uint32_t *stalep)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;
    uint32_t limit;

    if (!state->hdr || !state->visible_bitmap || !state->page_state ||
        !desc || desc->first_page >= state->page_state_words) {
        return;
    }

    limit = MIN(desc->nr_pages, state->page_state_words - desc->first_page);
    for (uint32_t i = 0; i < limit; i++) {
        if (!cxl_hybrid_rdma_descriptor_page_claimed(desc, i)) {
            continue;
        }
        if (cxl_hybrid_control_complete_rdma_page_visible_generation(
                state->hdr, state->visible_bitmap, state->page_state,
                desc->first_page + i, desc->generation, &desc->claims[i])) {
            desc->completed_pages++;
            if (completedp) {
                (*completedp)++;
            }
        } else {
            desc->stale_pages++;
            if (stalep) {
                (*stalep)++;
            }
        }
        clear_bit(i, desc->claimed_bmap);
    }
}

void cxl_hybrid_ctrl_clear_page_visible(uint64_t page_index)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    cxl_hybrid_control_clear_page_visible(state->hdr, state->visible_bitmap,
                                          page_index);
}

int cxl_hybrid_ctrl_wait_page_visible(uint64_t page_index,
                                      uint32_t generation,
                                      Error **errp)
{
    while (!cxl_hybrid_ctrl_page_visible(page_index, generation)) {
        if (!cxl_hybrid_control_destination.hdr ||
            !cxl_hybrid_control_generation_matches(
                cxl_hybrid_control_destination.hdr, generation)) {
            error_setg(errp,
                       "CXL hybrid visible page generation mismatch "
                       "(header=%u expected=%u)",
                       cxl_hybrid_control_destination.hdr ?
                       cxl_hybrid_control_generation(
                           cxl_hybrid_control_destination.hdr) : 0,
                       generation);
            return -EINVAL;
        }
        if (cxl_hybrid_ctrl_state_run_completed(
                &cxl_hybrid_control_destination, generation)) {
            error_setg(errp,
                       "CXL hybrid source completed before page %" PRIu64
                       " became visible",
                       page_index);
            return -ENOENT;
        }

        cpu_relax();
    }

    return 0;
}

bool cxl_hybrid_ctrl_region_visible(uint64_t first_page,
                                    uint32_t nr_pages,
                                    uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_destination;

    return cxl_hybrid_control_region_visible(state->hdr,
                                             state->visible_bitmap,
                                             state->visible_region_bitmap,
                                             first_page, nr_pages,
                                             generation);
}

bool cxl_hybrid_ctrl_region_bit_visible(uint64_t first_page,
                                        uint32_t nr_pages,
                                        uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_destination;

    return cxl_hybrid_control_region_bit_visible(state->hdr,
                                                 state->visible_region_bitmap,
                                                 first_page, nr_pages,
                                                 generation);
}

void cxl_hybrid_ctrl_set_region_visible(uint64_t first_page,
                                        uint32_t nr_pages,
                                        uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    cxl_hybrid_control_mark_visible_region_span_generation(
        state->hdr, state->visible_bitmap, state->visible_region_bitmap,
        state->page_state, first_page, nr_pages, generation,
        CXL_HYBRID_PAGE_LOCATION_CXL);
}

bool cxl_hybrid_ctrl_synthesize_region_visible(uint64_t first_page,
                                               uint32_t nr_pages,
                                               uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    return cxl_hybrid_control_region_visible_or_synthesize(
        state->hdr, state->visible_bitmap, state->visible_region_bitmap,
        first_page, nr_pages, generation);
}

int cxl_hybrid_ctrl_wait_region_visible(uint64_t first_page,
                                        uint32_t nr_pages,
                                        uint32_t generation,
                                        Error **errp)
{
    uint64_t start_ns = cxl_hybrid_ctrl_now_ns();
    CXLHybridControlState *state = &cxl_hybrid_control_destination;
    uint64_t region_index = UINT64_MAX;
    int ret = 0;

    trace_cxl_hybrid_region_wait_begin(first_page, nr_pages, generation);
    if (trace_event_get_state(TRACE_CXL_HYBRID_REGION_WAIT_BEGIN_TS)) {
        trace_cxl_hybrid_region_wait_begin_ts(start_ns, first_page, nr_pages,
                                             generation);
    }
    ret = cxl_hybrid_ctrl_validate_region_span(
        state->hdr, first_page, nr_pages, &region_index, errp);
    if (ret) {
        uint64_t elapsed_ns = cxl_hybrid_ctrl_now_ns() - start_ns;

        trace_cxl_hybrid_region_wait_complete(first_page, nr_pages,
                                              generation, ret, elapsed_ns);
        if (trace_event_get_state(
                TRACE_CXL_HYBRID_REGION_WAIT_COMPLETE_TS)) {
            trace_cxl_hybrid_region_wait_complete_ts(
                start_ns + elapsed_ns, first_page, nr_pages, generation, ret,
                elapsed_ns);
        }
        return ret;
    }
    if (!state->visible_region_bitmap) {
        uint64_t elapsed_ns;

        error_setg(errp, "CXL hybrid visible region bitmap is not initialized");
        elapsed_ns = cxl_hybrid_ctrl_now_ns() - start_ns;
        trace_cxl_hybrid_region_wait_complete(first_page, nr_pages,
                                              generation, -EINVAL,
                                              elapsed_ns);
        if (trace_event_get_state(
                TRACE_CXL_HYBRID_REGION_WAIT_COMPLETE_TS)) {
            trace_cxl_hybrid_region_wait_complete_ts(
                start_ns + elapsed_ns, first_page, nr_pages, generation,
                -EINVAL, elapsed_ns);
        }
        return -EINVAL;
    }

    while (true) {
        if (!state->hdr ||
            !cxl_hybrid_control_generation_matches(state->hdr, generation)) {
            error_setg(errp,
                       "CXL hybrid visible region generation mismatch "
                       "(header=%u expected=%u)",
                       state->hdr ?
                       cxl_hybrid_control_generation(
                           state->hdr) : 0,
                       generation);
            ret = -EINVAL;
            break;
        }
        if (cxl_hybrid_control_region_bit_visible(
                state->hdr, state->visible_region_bitmap, first_page,
                nr_pages, generation)) {
            break;
        }
        if (cxl_hybrid_ctrl_state_run_completed(state, generation)) {
            if (cxl_hybrid_control_region_visible_or_synthesize(
                    state->hdr, state->visible_bitmap,
                    state->visible_region_bitmap, first_page, nr_pages,
                    generation)) {
                break;
            }
            error_setg(errp,
                       "CXL hybrid source completed before region "
                       "(first-page=%" PRIu64 " pages=%u) became visible",
                       first_page, nr_pages);
            ret = -ENOENT;
            break;
        }

        cpu_relax();
    }
    {
        uint64_t elapsed_ns = cxl_hybrid_ctrl_now_ns() - start_ns;

        trace_cxl_hybrid_region_wait_complete(first_page, nr_pages, generation,
                                              ret, elapsed_ns);
        if (trace_event_get_state(
                TRACE_CXL_HYBRID_REGION_WAIT_COMPLETE_TS)) {
            trace_cxl_hybrid_region_wait_complete_ts(
                start_ns + elapsed_ns, first_page, nr_pages, generation, ret,
                elapsed_ns);
        }
    }

    return ret;
}

int cxl_hybrid_ctrl_enqueue_fault_request(uint64_t page_index,
                                          uint32_t generation,
                                          uint64_t request_ts_ns,
                                          bool *queuedp,
                                          Error **errp)
{
    CXLHybridFaultRequestRecord record = {
        .page_index = page_index,
        .generation = generation,
        .request_ts_ns = request_ts_ns,
    };

    return cxl_hybrid_ctrl_try_enqueue_request(
        &cxl_hybrid_control_destination, &record, queuedp, errp);
}

int cxl_hybrid_ctrl_enqueue_fault_region_request(uint64_t first_page,
                                                 uint32_t nr_pages,
                                                 uint64_t demand_page,
                                                 uint32_t generation,
                                                 uint64_t request_ts_ns,
                                                 bool *queuedp,
                                                 Error **errp)
{
    CXLHybridFaultRegionPlan plan = { 0 };
    CXLHybridFaultRequestRecord record = {
        .page_index = first_page,
        .demand_page = demand_page,
        .generation = generation,
        .flags = CXL_HYBRID_FAULT_REQUEST_F_REGION,
        .nr_pages = nr_pages,
        .request_ts_ns = request_ts_ns,
    };
    int ret;

    if (queuedp) {
        *queuedp = false;
    }
    trace_cxl_hybrid_region_request_enqueue(first_page, nr_pages, generation);
    ret = cxl_hybrid_ctrl_validate_region_span(
        cxl_hybrid_control_destination.hdr, first_page, nr_pages, NULL, errp);
    if (ret) {
        return ret;
    }
    if (!cxl_hybrid_fault_region_plan(first_page, nr_pages, demand_page,
                                      &plan)) {
        error_setg(errp,
                   "CXL hybrid fault region demand page out of range "
                   "(first-page=%" PRIu64 " pages=%u demand-page=%" PRIu64 ")",
                   first_page, nr_pages, demand_page);
        return -EINVAL;
    }

    return cxl_hybrid_ctrl_try_enqueue_request(
        &cxl_hybrid_control_destination, &record, queuedp, errp);
}

bool cxl_hybrid_ctrl_region_owned(uint64_t region_index, uint32_t generation)
{
    CXLHybridControlState *state = cxl_hybrid_control_source.hdr ?
        &cxl_hybrid_control_source : &cxl_hybrid_control_destination;

    return cxl_hybrid_control_region_owned(state->hdr,
                                           state->owned_region_bitmap,
                                           region_index, generation);
}

void cxl_hybrid_ctrl_mark_region_owned(uint64_t region_index,
                                       uint32_t generation)
{
    CXLHybridControlState *state = cxl_hybrid_control_destination.hdr ?
        &cxl_hybrid_control_destination : &cxl_hybrid_control_source;

    if (!cxl_hybrid_control_generation_matches(state->hdr, generation)) {
        return;
    }

    cxl_hybrid_control_mark_region_owned(state->hdr,
                                         state->owned_region_bitmap,
                                         region_index);
    trace_cxl_hybrid_region_owned_set(region_index, generation);
    while (state == &cxl_hybrid_control_destination &&
           cxl_hybrid_control_generation_matches(state->hdr, generation) &&
           cxl_hybrid_control_source_write_count(state->hdr)) {
        cpu_relax();
    }
}

bool cxl_hybrid_ctrl_fault_pressure(uint32_t generation)
{
    CXLHybridControlState *state = cxl_hybrid_control_source.hdr ?
        &cxl_hybrid_control_source : &cxl_hybrid_control_destination;

    return cxl_hybrid_control_fault_pressure(state->hdr, generation);
}

void cxl_hybrid_ctrl_source_write_begin(void)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr) {
        return;
    }

    cxl_hybrid_control_source_write_begin(state->hdr);
}

void cxl_hybrid_ctrl_source_write_end(void)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr) {
        return;
    }

    cxl_hybrid_control_source_write_end(state->hdr);
}

bool cxl_hybrid_ctrl_dequeue_fault_request(CXLHybridFaultRequestRecord *record)
{
    return cxl_hybrid_ctrl_try_dequeue_request(&cxl_hybrid_control_source,
                                               record);
}
