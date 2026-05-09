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

typedef struct CXLHybridControlRegion {
    void *map_base;
    size_t map_len;
    off_t map_offset;
    int fd;
    CXLHybridControlHeader *hdr;
    unsigned long *visible_bitmap;
    unsigned long *visible_region_bitmap;
    unsigned long *owned_region_bitmap;
    CXLHybridFaultRequestRecord *request_ring;
    uint64_t total_pages;
    uint64_t total_regions;
    uint64_t region_granule;
    uint32_t visible_page_words;
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
    unsigned long *visible_region_bitmap;
    unsigned long *owned_region_bitmap;
    CXLHybridFaultRequestRecord *request_ring;
    QemuThread request_worker;
    bool request_worker_running;
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
    size_t visible_region_bitmap_bytes =
        cxl_hybrid_control_visible_region_bitmap_bytes(total_regions);
    size_t owned_region_bitmap_bytes =
        cxl_hybrid_control_owned_region_bitmap_bytes(total_regions);
    size_t request_bytes =
        (size_t)cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_REQUEST_ORDER) *
        sizeof(CXLHybridFaultRequestRecord);

    return ROUND_UP(sizeof(CXLHybridControlHeader) + visible_bitmap_bytes +
                    visible_region_bitmap_bytes + owned_region_bitmap_bytes +
                    request_bytes,
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
    state->visible_region_bitmap =
        cxl_hybrid_control_region.visible_region_bitmap;
    state->owned_region_bitmap =
        cxl_hybrid_control_region.owned_region_bitmap;
    state->request_ring = cxl_hybrid_control_region.request_ring;
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
    uint32_t visible_region_words;
    uint32_t owned_region_words;
    size_t visible_bitmap_bytes;
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

    map_len = cxl_hybrid_fault_control_region_allocation_bytes(cioc->align);
    total_pages = cxl_hybrid_ctrl_total_pages();
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
    cxl_hybrid_control_region.visible_region_words = visible_region_words;
    cxl_hybrid_control_region.owned_region_words = owned_region_words;
    cxl_hybrid_control_region.region_granule_shift =
        cxl_hybrid_control_region_granule_shift(region_granule);
    cxl_hybrid_control_region.request_ring_entries =
        cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_REQUEST_ORDER);
    cxl_hybrid_control_region.visible_bitmap = (unsigned long *)(hdr + 1);
    cxl_hybrid_control_region.visible_region_bitmap =
        (unsigned long *)((char *)cxl_hybrid_control_region.visible_bitmap +
                         visible_bitmap_bytes);
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

static bool cxl_hybrid_ctrl_request_visible(
    const CXLHybridControlState *state,
    const CXLHybridFaultRequestRecord *record)
{
    if (record->flags & CXL_HYBRID_FAULT_REQUEST_F_REGION) {
        return cxl_hybrid_control_region_visible_or_synthesize(
            state->hdr, state->visible_bitmap, state->visible_region_bitmap,
            record->page_index, record->nr_pages, record->generation);
    }

    return cxl_hybrid_control_page_visible(state->hdr, state->visible_bitmap,
                                           record->page_index,
                                           record->generation);
}

static int cxl_hybrid_ctrl_request_completed_status(
    const CXLHybridControlState *state,
    const CXLHybridFaultRequestRecord *record,
    Error **errp)
{
    if (cxl_hybrid_ctrl_request_visible(state, record)) {
        return 0;
    }

    if (record->flags & CXL_HYBRID_FAULT_REQUEST_F_REGION) {
        error_setg(errp,
                   "CXL hybrid source completed before region "
                   "(first-page=%" PRIu64 " pages=%u) became visible",
                   record->page_index, record->nr_pages);
    } else {
        error_setg(errp,
                   "CXL hybrid source completed before page %" PRIu64
                   " became visible",
                   record->page_index);
    }
    return -ENOENT;
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
                if (cxl_hybrid_publish_fault_region_request_core(
                        record.page_index, record.nr_pages,
                        record.generation, record.request_ts_ns,
                        &local_err)) {
                    if (local_err) {
                        error_report_err(local_err);
                    }
                    cxl_hybrid_ctrl_abort_generation(state, record.generation);
                }
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
                                       visible_region_bitmap,
                                       cxl_hybrid_control_region.total_regions,
                                       owned_region_bitmap,
                                       cxl_hybrid_control_region.total_regions,
                                       cxl_hybrid_control_region.region_granule,
                                       TARGET_PAGE_BITS,
                                       generation);
    cxl_hybrid_ctrl_start_request_worker(&cxl_hybrid_control_source);
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

    qatomic_store_release(&state->hdr->completed_generation, generation);
    return 0;
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

void cxl_hybrid_ctrl_set_page_visible(uint64_t page_index,
                                      uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;

    if (!state->hdr || !state->visible_bitmap) {
        return;
    }

    cxl_hybrid_control_mark_page_visible_generation(
        state->hdr, state->visible_bitmap, page_index, generation);
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
        state->hdr, state->visible_bitmap, first_page, nr_pages, generation);
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
        first_page, nr_pages, generation);
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
    ret = cxl_hybrid_ctrl_validate_region_span(
        state->hdr, first_page, nr_pages, &region_index, errp);
    if (ret) {
        trace_cxl_hybrid_region_wait_complete(first_page, nr_pages,
                                              generation, ret,
                                              cxl_hybrid_ctrl_now_ns() -
                                              start_ns);
        return ret;
    }
    if (!state->visible_region_bitmap) {
        error_setg(errp, "CXL hybrid visible region bitmap is not initialized");
        trace_cxl_hybrid_region_wait_complete(first_page, nr_pages,
                                              generation, -EINVAL,
                                              cxl_hybrid_ctrl_now_ns() -
                                              start_ns);
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
    trace_cxl_hybrid_region_wait_complete(first_page, nr_pages, generation,
                                          ret,
                                          cxl_hybrid_ctrl_now_ns() - start_ns);

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
                                                 uint32_t generation,
                                                 uint64_t request_ts_ns,
                                                 bool *queuedp,
                                                 Error **errp)
{
    CXLHybridFaultRequestRecord record = {
        .page_index = first_page,
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
