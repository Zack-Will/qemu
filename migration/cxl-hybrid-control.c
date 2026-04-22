#include "qemu/osdep.h"
#include "migration/cxl.h"
#include "migration/misc.h"
#include "migration/options.h"
#include "migration/ram.h"
#include "io/channel-cxl.h"
#include "qemu/atomic.h"
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
    CXLHybridFaultRequestRecord *request_ring;
    CXLHybridFaultReadyRecord *ready_ring;
    uint64_t total_pages;
    uint32_t visible_page_words;
    uint32_t request_ring_entries;
    uint32_t ready_ring_entries;
    uint32_t users;
} CXLHybridControlRegion;

typedef struct CXLHybridControlState {
    CXLHybridControlHeader *hdr;
    void *map_base;
    size_t map_len;
    unsigned long *visible_bitmap;
    CXLHybridFaultRequestRecord *request_ring;
    CXLHybridFaultReadyRecord *ready_ring;
    QemuThread request_worker;
    QemuThread ready_poller;
    bool request_worker_running;
    bool ready_poller_running;
    bool shutdown;
} CXLHybridControlState;

static CXLHybridControlRegion cxl_hybrid_control_region = {
    .fd = -1,
};
static CXLHybridControlState cxl_hybrid_control_source;
static CXLHybridControlState cxl_hybrid_control_destination;
static QemuMutex cxl_hybrid_control_request_lock;
static QemuMutex cxl_hybrid_control_ready_lock;
static bool cxl_hybrid_control_locks_ready;

static bool cxl_hybrid_ctrl_try_dequeue_request(CXLHybridControlState *state,
                                                CXLHybridFaultRequestRecord *record);
static bool cxl_hybrid_ctrl_try_dequeue_ready(CXLHybridControlState *state,
                                              CXLHybridFaultReadyRecord *record);

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

static uint32_t cxl_hybrid_ctrl_ring_entries(uint32_t order)
{
    return 1U << order;
}

uint64_t cxl_hybrid_fault_control_region_bytes(void)
{
    uint64_t total_pages = cxl_hybrid_ctrl_total_pages();
    size_t visible_bitmap_bytes =
        cxl_hybrid_control_visible_bitmap_bytes(total_pages);
    size_t request_bytes =
        (size_t)cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_REQUEST_ORDER) *
        sizeof(CXLHybridFaultRequestRecord);
    size_t ready_bytes =
        (size_t)cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_READY_ORDER) *
        sizeof(CXLHybridFaultReadyRecord);

    return ROUND_UP(sizeof(CXLHybridControlHeader) + visible_bitmap_bytes +
                    request_bytes + ready_bytes,
                    (size_t)qemu_real_host_page_size());
}

static off_t cxl_hybrid_ctrl_region_offset(uint64_t align)
{
    return cxl_hybrid_mapped_ram_required_bytes(align);
}

static void cxl_hybrid_ctrl_ensure_locks(void)
{
    if (cxl_hybrid_control_locks_ready) {
        return;
    }

    qemu_mutex_init(&cxl_hybrid_control_request_lock);
    qemu_mutex_init(&cxl_hybrid_control_ready_lock);
    cxl_hybrid_control_locks_ready = true;
}

static void cxl_hybrid_ctrl_bind_state(CXLHybridControlState *state)
{
    state->hdr = cxl_hybrid_control_region.hdr;
    state->map_base = cxl_hybrid_control_region.map_base;
    state->map_len = cxl_hybrid_control_region.map_len;
    state->visible_bitmap = cxl_hybrid_control_region.visible_bitmap;
    state->request_ring = cxl_hybrid_control_region.request_ring;
    state->ready_ring = cxl_hybrid_control_region.ready_ring;
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
    uint32_t visible_page_words;
    size_t visible_bitmap_bytes;
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
                   "x-cxl-fault-control-plane=cxl requires x-cxl-shared-backing=true");
        return -EINVAL;
    }

    cioc = qio_channel_cxl_new_path(path, O_RDWR | O_CLOEXEC, errp);
    if (!cioc) {
        return -errno;
    }

    map_len = cxl_hybrid_fault_control_region_bytes();
    total_pages = cxl_hybrid_ctrl_total_pages();
    visible_page_words = cxl_hybrid_ctrl_visible_page_words(total_pages);
    visible_bitmap_bytes =
        cxl_hybrid_control_visible_bitmap_bytes(total_pages);
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
    cxl_hybrid_control_region.visible_page_words = visible_page_words;
    cxl_hybrid_control_region.request_ring_entries =
        cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_REQUEST_ORDER);
    cxl_hybrid_control_region.ready_ring_entries =
        cxl_hybrid_ctrl_ring_entries(CXL_HYBRID_CTRL_READY_ORDER);
    cxl_hybrid_control_region.visible_bitmap = (unsigned long *)(hdr + 1);
    cxl_hybrid_control_region.request_ring =
        (CXLHybridFaultRequestRecord *)((char *)cxl_hybrid_control_region.visible_bitmap +
                                        visible_bitmap_bytes);
    cxl_hybrid_control_region.ready_ring =
        (CXLHybridFaultReadyRecord *)(
            cxl_hybrid_control_region.request_ring +
            cxl_hybrid_control_region.request_ring_entries);

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
    return state->hdr && state->visible_bitmap && state->request_ring &&
           state->ready_ring &&
           state->hdr->magic == CXL_HYBRID_CTRL_MAGIC &&
           state->hdr->version == CXL_HYBRID_CTRL_VERSION &&
           state->hdr->request_ring_order == CXL_HYBRID_CTRL_REQUEST_ORDER &&
           state->hdr->ready_ring_order == CXL_HYBRID_CTRL_READY_ORDER;
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

            if (record.generation != state->hdr->generation) {
                continue;
            }
            if (!cxl_hybrid_lookup_global_page(record.page_index, &block,
                                               &block_offset)) {
                continue;
            }

            cxl_hybrid_note_publish_request_received(qemu_ram_get_idstr(block),
                                                     block_offset,
                                                     record.generation,
                                                     record.request_ts_ns);
            if (cxl_hybrid_publish_fault_request_core(qemu_ram_get_idstr(block),
                                                      block_offset,
                                                      TARGET_PAGE_SIZE,
                                                      record.generation,
                                                      true,
                                                      &(CXLHybridFaultReadyRecord){ 0 },
                                                      NULL,
                                                      &local_err)) {
                error_free(local_err);
            }
            continue;
        }
        g_usleep(50);
    }
    trace_cxl_hybrid_ctrl_request_worker_stop();
    return NULL;
}

static void *cxl_hybrid_ctrl_ready_poller_thread(void *opaque)
{
    CXLHybridControlState *state = opaque;
    CXLHybridFaultReadyRecord record;

    trace_cxl_hybrid_ctrl_ready_poller_start();
    while (!qatomic_read(&state->shutdown)) {
        if (cxl_hybrid_ctrl_dequeue_fault_ready(&record)) {
            Error *local_err = NULL;

            if (record.generation != state->hdr->generation) {
                continue;
            }
            cxl_hybrid_handle_fault_ready_record(&record, &local_err);
            error_free(local_err);
            continue;
        }
        g_usleep(50);
    }
    trace_cxl_hybrid_ctrl_ready_poller_stop();
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

static void cxl_hybrid_ctrl_start_ready_poller(CXLHybridControlState *state)
{
    if (state->ready_poller_running) {
        return;
    }

    qatomic_set(&state->shutdown, false);
    qemu_thread_create(&state->ready_poller, "cxl-ctrl-ready",
                       cxl_hybrid_ctrl_ready_poller_thread, state,
                       QEMU_THREAD_JOINABLE);
    state->ready_poller_running = true;
}

static void cxl_hybrid_ctrl_stop_ready_poller(CXLHybridControlState *state)
{
    if (!state->ready_poller_running) {
        return;
    }

    qatomic_set(&state->shutdown, true);
    qemu_thread_join(&state->ready_poller);
    state->ready_poller_running = false;
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

static void cxl_hybrid_ctrl_publish_ready(CXLHybridFaultReadyRecord *slot,
                                          const CXLHybridFaultReadyRecord *src,
                                          uint64_t seq)
{
    *slot = *src;
    smp_wmb();
    qatomic_set(&slot->seq, seq);
}

static int cxl_hybrid_ctrl_try_enqueue_request(CXLHybridControlState *state,
                                               const CXLHybridFaultRequestRecord *record,
                                               Error **errp)
{
    CXLHybridFaultRequestRecord entry = { 0 };
    CXLHybridFaultRequestRecord *slot;
    uint64_t cons;
    uint64_t prod;
    uint64_t seq;
    uint64_t mask;
    int ret = 0;

    if (!cxl_hybrid_ctrl_state_ready(state)) {
        error_setg(errp,
                   "CXL hybrid fault request ring is not initialized");
        return -EINVAL;
    }

    qemu_mutex_lock(&cxl_hybrid_control_request_lock);
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
    smp_wmb();
    qatomic_set(&state->hdr->request_prod, seq);

out:
    qemu_mutex_unlock(&cxl_hybrid_control_request_lock);
    return ret;
}

static int cxl_hybrid_ctrl_try_enqueue_ready(CXLHybridControlState *state,
                                             const CXLHybridFaultReadyRecord *record,
                                             Error **errp)
{
    CXLHybridFaultReadyRecord entry = { 0 };
    CXLHybridFaultReadyRecord *slot;
    uint64_t cons;
    uint64_t prod;
    uint64_t seq;
    uint64_t mask;
    int ret = 0;

    if (!cxl_hybrid_ctrl_state_ready(state)) {
        error_setg(errp, "CXL hybrid fault ready ring is not initialized");
        return -EINVAL;
    }

    qemu_mutex_lock(&cxl_hybrid_control_ready_lock);
    prod = qatomic_read(&state->hdr->ready_prod);
    cons = qatomic_read(&state->hdr->ready_cons);
    if (prod - cons >= cxl_hybrid_control_region.ready_ring_entries) {
        error_setg(errp, "CXL hybrid fault ready ring is full");
        ret = -ENOSPC;
        goto out;
    }

    seq = prod + 1;
    mask = cxl_hybrid_control_region.ready_ring_entries - 1;
    slot = &state->ready_ring[prod & mask];
    entry = *record;
    entry.seq = 0;
    cxl_hybrid_ctrl_publish_ready(slot, &entry, seq);
    smp_wmb();
    qatomic_set(&state->hdr->ready_prod, seq);

out:
    qemu_mutex_unlock(&cxl_hybrid_control_ready_lock);
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
    qatomic_set(&state->hdr->request_cons, expected_seq);
    return true;
}

static bool cxl_hybrid_ctrl_try_dequeue_ready(CXLHybridControlState *state,
                                              CXLHybridFaultReadyRecord *record)
{
    CXLHybridFaultReadyRecord *slot;
    uint64_t expected_seq;
    uint64_t mask;
    uint64_t prod;
    uint64_t cons;

    if (!record || !cxl_hybrid_ctrl_state_ready(state)) {
        return false;
    }

    cons = qatomic_read(&state->hdr->ready_cons);
    prod = qatomic_read(&state->hdr->ready_prod);
    if (cons == prod) {
        return false;
    }

    expected_seq = cons + 1;
    mask = cxl_hybrid_control_region.ready_ring_entries - 1;
    slot = &state->ready_ring[cons & mask];
    if (qatomic_read(&slot->seq) != expected_seq) {
        return false;
    }

    smp_rmb();
    *record = *slot;
    qatomic_set(&state->hdr->ready_cons, expected_seq);
    return true;
}

int cxl_hybrid_control_init_source(Error **errp)
{
    int ret;

    if (!migrate_cxl_fault_control_plane_cxl()) {
        return 0;
    }

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_source, errp);
    if (ret) {
        return ret;
    }

    return 0;
}

int cxl_hybrid_control_init_destination(Error **errp)
{
    int ret;

    if (!migrate_cxl_fault_control_plane_cxl()) {
        return 0;
    }

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_destination, errp);
    if (ret) {
        return ret;
    }

    return 0;
}

int cxl_hybrid_control_begin_source_run(Error **errp)
{
    int ret;
    uint32_t generation;

    if (!migrate_cxl_fault_control_plane_cxl()) {
        return 0;
    }

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_source, errp);
    if (ret) {
        return ret;
    }

    generation = cxl_hybrid_fault_publish_generation();
    cxl_hybrid_control_reset_run_state(cxl_hybrid_control_source.hdr,
                                       cxl_hybrid_control_source.visible_bitmap,
                                       generation,
                                       cxl_hybrid_control_region.visible_page_words);
    cxl_hybrid_ctrl_start_request_worker(&cxl_hybrid_control_source);
    return 0;
}

int cxl_hybrid_control_activate_destination(Error **errp)
{
    int ret;
    CXLHybridControlHeader *hdr;
    uint32_t generation;
    size_t expected_visible_page_words;

    if (!migrate_cxl_fault_control_plane_cxl()) {
        return 0;
    }

    ret = cxl_hybrid_ctrl_state_init(&cxl_hybrid_control_destination, errp);
    if (ret) {
        return ret;
    }

    if (!cxl_hybrid_ctrl_state_ready(&cxl_hybrid_control_destination)) {
        error_setg(errp, "CXL hybrid fault control header is not initialized");
        return -EINVAL;
    }

    hdr = cxl_hybrid_control_destination.hdr;
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

    generation = cxl_hybrid_fault_publish_generation();
    if (hdr->generation != generation) {
        error_setg(errp,
                   "CXL hybrid fault control generation mismatch "
                   "(header=%u expected=%u)",
                   hdr->generation,
                   generation);
        return -EINVAL;
    }

    cxl_hybrid_ctrl_start_ready_poller(&cxl_hybrid_control_destination);
    return 0;
}

void cxl_hybrid_control_cleanup_source(void)
{
    cxl_hybrid_ctrl_stop_request_worker(&cxl_hybrid_control_source);
    cxl_hybrid_ctrl_unbind_state(&cxl_hybrid_control_source);
}

void cxl_hybrid_control_cleanup_destination(void)
{
    cxl_hybrid_ctrl_stop_ready_poller(&cxl_hybrid_control_destination);
    cxl_hybrid_ctrl_unbind_state(&cxl_hybrid_control_destination);
}

bool cxl_hybrid_ctrl_page_visible(uint64_t page_index, uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_destination;

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

    (void)generation;
    cxl_hybrid_control_mark_page_visible(state->hdr, state->visible_bitmap,
                                         page_index);
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
            cxl_hybrid_control_destination.hdr->generation != generation) {
            error_setg(errp,
                       "CXL hybrid visible page generation mismatch "
                       "(header=%u expected=%u)",
                       cxl_hybrid_control_destination.hdr ?
                       cxl_hybrid_control_destination.hdr->generation : 0,
                       generation);
            return -EINVAL;
        }

        g_usleep(50);
    }

    return 0;
}

int cxl_hybrid_ctrl_enqueue_fault_request(uint64_t page_index,
                                          uint32_t generation,
                                          uint64_t request_ts_ns,
                                          Error **errp)
{
    CXLHybridFaultRequestRecord record = {
        .page_index = page_index,
        .generation = generation,
        .request_ts_ns = request_ts_ns,
    };

    return cxl_hybrid_ctrl_try_enqueue_request(
        &cxl_hybrid_control_destination, &record, errp);
}

bool cxl_hybrid_ctrl_dequeue_fault_request(CXLHybridFaultRequestRecord *record)
{
    return cxl_hybrid_ctrl_try_dequeue_request(&cxl_hybrid_control_source,
                                               record);
}

int cxl_hybrid_ctrl_enqueue_fault_ready(
    const CXLHybridFaultReadyRecord *record, Error **errp)
{
    return cxl_hybrid_ctrl_try_enqueue_ready(&cxl_hybrid_control_source,
                                             record, errp);
}

bool cxl_hybrid_ctrl_dequeue_fault_ready(CXLHybridFaultReadyRecord *record)
{
    return cxl_hybrid_ctrl_try_dequeue_ready(
        &cxl_hybrid_control_destination, record);
}
