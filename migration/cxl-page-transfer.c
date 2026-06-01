#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qapi/error.h"
#include "migration/cxl.h"

#define CXL_HYBRID_TRANSFER_PAGE_BYTES (UINT64_C(1) << 12)

bool cxl_hybrid_fault_region_plan(uint64_t first_page,
                                  uint32_t nr_pages,
                                  uint64_t demand_page,
                                  CXLHybridFaultRegionPlan *plan)
{
    uint64_t end_page;

    if (!plan || !nr_pages || nr_pages > UINT64_MAX - first_page) {
        return false;
    }

    end_page = first_page + nr_pages;
    if (demand_page < first_page || demand_page >= end_page) {
        return false;
    }

    plan->demand_page = demand_page;
    plan->prefetch_first_page = first_page;
    plan->prefetch_nr_pages = nr_pages;
    plan->prefetch_skip_page = demand_page;
    return true;
}

bool cxl_hybrid_fault_request_demand_visible(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const CXLHybridFaultRequestRecord *record)
{
    uint64_t page_index;

    if (!record) {
        return false;
    }

    if (record->flags & CXL_HYBRID_FAULT_REQUEST_F_REGION) {
        CXLHybridFaultRegionPlan plan = { 0 };

        if (!cxl_hybrid_fault_region_plan(record->page_index,
                                          record->nr_pages,
                                          record->demand_page,
                                          &plan)) {
            return false;
        }
        page_index = plan.demand_page;
    } else {
        page_index = record->page_index;
    }

    return cxl_hybrid_control_page_visible(hdr, visible_bitmap, page_index,
                                           record->generation);
}

int cxl_hybrid_fault_request_completed_status(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const CXLHybridFaultRequestRecord *record,
    Error **errp)
{
    if (!record) {
        error_setg(errp, "CXL hybrid completed request status missing record");
        return -EINVAL;
    }

    if (cxl_hybrid_fault_request_demand_visible(hdr, visible_bitmap, record)) {
        return 0;
    }

    if (record->flags & CXL_HYBRID_FAULT_REQUEST_F_REGION) {
        error_setg(errp,
                   "CXL hybrid source completed before region demand page "
                   "%" PRIu64 " became visible "
                   "(first-page=%" PRIu64 " pages=%u)",
                   record->demand_page, record->page_index, record->nr_pages);
    } else {
        error_setg(errp,
                   "CXL hybrid source completed before page %" PRIu64
                   " became visible",
                   record->page_index);
    }
    return -ENOENT;
}

CXLHybridTransferClass cxl_hybrid_scheduler_choose_zero_page_lane(
    const CXLHybridSchedulerPolicy *policy)
{
    (void)policy;

    return CXL_HYBRID_TRANSFER_CXL_LOW;
}

CXLHybridTransferClass cxl_hybrid_scheduler_choose_bulk_lane(
    const CXLHybridSchedulerPolicy *policy,
    uint64_t page_index)
{
    uint64_t total_share;

    if (!policy || !policy->rdma_budget_pages) {
        return CXL_HYBRID_TRANSFER_CXL_LOW;
    }
    if (!policy->cxl_background_pages) {
        return CXL_HYBRID_TRANSFER_RDMA_BULK;
    }
    if (policy->rdma_budget_pages == policy->cxl_background_pages) {
        return (page_index & 1) ? CXL_HYBRID_TRANSFER_RDMA_BULK :
               CXL_HYBRID_TRANSFER_CXL_LOW;
    }

    total_share = policy->rdma_budget_pages + policy->cxl_background_pages;
    if (!total_share || total_share < policy->rdma_budget_pages) {
        return CXL_HYBRID_TRANSFER_CXL_LOW;
    }

    return (page_index % total_share) < policy->cxl_background_pages ?
           CXL_HYBRID_TRANSFER_CXL_LOW :
           CXL_HYBRID_TRANSFER_RDMA_BULK;
}

void cxl_hybrid_transfer_queue_init_for_test(CXLHybridTransferQueue *queue)
{
    int i;

    memset(queue, 0, sizeof(*queue));
    qemu_mutex_init(&queue->lock);
    queue->lock_ready = true;
    for (i = 0; i < CXL_HYBRID_TRANSFER_CLASS_COUNT; i++) {
        g_queue_init(&queue->classes[i]);
    }
}

void cxl_hybrid_transfer_queue_destroy_for_test(CXLHybridTransferQueue *queue)
{
    int i;

    if (!queue || !queue->lock_ready) {
        return;
    }
    for (i = 0; i < CXL_HYBRID_TRANSFER_CLASS_COUNT; i++) {
        g_queue_clear_full(&queue->classes[i], g_free);
    }
    qemu_mutex_destroy(&queue->lock);
    queue->lock_ready = false;
}

void cxl_hybrid_transfer_queue_push(CXLHybridTransferQueue *queue,
                                    CXLHybridTransferClass klass,
                                    const CXLHybridPageDescriptor *desc)
{
    CXLHybridPageDescriptor *copy;

    if (!queue || !queue->lock_ready || !desc || (int)klass < 0 ||
        klass >= CXL_HYBRID_TRANSFER_CLASS_COUNT) {
        return;
    }

    copy = g_new(CXLHybridPageDescriptor, 1);
    *copy = *desc;
    qemu_mutex_lock(&queue->lock);
    g_queue_push_tail(&queue->classes[klass], copy);
    qemu_mutex_unlock(&queue->lock);
}

bool cxl_hybrid_transfer_queue_pop(CXLHybridTransferQueue *queue,
                                   CXLHybridPageDescriptor *desc)
{
    static const CXLHybridTransferClass order[] = {
        CXL_HYBRID_TRANSFER_CXL_HIGH,
        CXL_HYBRID_TRANSFER_CXL_LOW,
        CXL_HYBRID_TRANSFER_RDMA_BULK,
        CXL_HYBRID_TRANSFER_RDMA_PREFETCH,
    };
    CXLHybridPageDescriptor *copy;
    size_t i;

    if (!queue || !queue->lock_ready || !desc) {
        return false;
    }

    qemu_mutex_lock(&queue->lock);
    for (i = 0; i < G_N_ELEMENTS(order); i++) {
        copy = g_queue_pop_head(&queue->classes[order[i]]);
        if (copy) {
            *desc = *copy;
            g_free(copy);
            qemu_mutex_unlock(&queue->lock);
            return true;
        }
    }
    qemu_mutex_unlock(&queue->lock);
    return false;
}

static bool cxl_hybrid_transfer_queue_pop_ordered(
    CXLHybridTransferQueue *queue,
    const CXLHybridTransferClass *order,
    size_t order_len,
    CXLHybridPageDescriptor *desc,
    CXLHybridTransferClass *klass)
{
    CXLHybridPageDescriptor *copy;

    if (!queue || !queue->lock_ready || !desc || !order || !order_len) {
        return false;
    }

    qemu_mutex_lock(&queue->lock);
    for (size_t i = 0; i < order_len; i++) {
        copy = g_queue_pop_head(&queue->classes[order[i]]);
        if (copy) {
            *desc = *copy;
            if (klass) {
                *klass = order[i];
            }
            g_free(copy);
            qemu_mutex_unlock(&queue->lock);
            return true;
        }
    }
    qemu_mutex_unlock(&queue->lock);
    return false;
}

bool cxl_hybrid_transfer_queue_pop_cxl(CXLHybridTransferQueue *queue,
                                       CXLHybridPageDescriptor *desc,
                                       CXLHybridTransferClass *klass)
{
    static const CXLHybridTransferClass order[] = {
        CXL_HYBRID_TRANSFER_CXL_HIGH,
        CXL_HYBRID_TRANSFER_CXL_LOW,
    };

    return cxl_hybrid_transfer_queue_pop_ordered(
        queue, order, G_N_ELEMENTS(order), desc, klass);
}

static bool cxl_hybrid_transfer_cxl_descs_mergeable(
    const CXLHybridPageDescriptor *prev,
    const CXLHybridPageDescriptor *next)
{
    if (!prev || !next || prev->nr_pages != 1 || next->nr_pages != 1 ||
        prev->block != next->block || prev->generation != next->generation) {
        return false;
    }

    return next->page_index == prev->page_index + 1 &&
           next->block_offset ==
               prev->block_offset + CXL_HYBRID_TRANSFER_PAGE_BYTES &&
           next->cxl_offset == prev->cxl_offset + CXL_HYBRID_TRANSFER_PAGE_BYTES;
}

uint32_t cxl_hybrid_transfer_queue_pop_cxl_batch(
    CXLHybridTransferQueue *queue,
    CXLHybridPageDescriptor *descs,
    uint32_t max_descs,
    CXLHybridTransferClass *klass)
{
    static const CXLHybridTransferClass order[] = {
        CXL_HYBRID_TRANSFER_CXL_HIGH,
        CXL_HYBRID_TRANSFER_CXL_LOW,
    };
    CXLHybridPageDescriptor *copy;
    CXLHybridTransferClass selected = CXL_HYBRID_TRANSFER_CLASS_COUNT;
    uint32_t count = 0;

    if (!queue || !queue->lock_ready || !descs || !max_descs) {
        return 0;
    }

    qemu_mutex_lock(&queue->lock);
    for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
        if (!g_queue_is_empty(&queue->classes[order[i]])) {
            selected = order[i];
            break;
        }
    }
    if (selected == CXL_HYBRID_TRANSFER_CLASS_COUNT) {
        qemu_mutex_unlock(&queue->lock);
        return 0;
    }

    copy = g_queue_pop_head(&queue->classes[selected]);
    assert(copy);
    descs[count++] = *copy;
    g_free(copy);

    while (count < max_descs) {
        copy = g_queue_peek_head(&queue->classes[selected]);
        if (!cxl_hybrid_transfer_cxl_descs_mergeable(&descs[count - 1],
                                                     copy)) {
            break;
        }
        copy = g_queue_pop_head(&queue->classes[selected]);
        assert(copy);
        descs[count++] = *copy;
        g_free(copy);
    }

    if (klass) {
        *klass = selected;
    }
    qemu_mutex_unlock(&queue->lock);
    return count;
}

bool cxl_hybrid_transfer_queue_pop_rdma(CXLHybridTransferQueue *queue,
                                        CXLHybridPageDescriptor *desc,
                                        CXLHybridTransferClass *klass)
{
    static const CXLHybridTransferClass order[] = {
        CXL_HYBRID_TRANSFER_RDMA_BULK,
        CXL_HYBRID_TRANSFER_RDMA_PREFETCH,
    };

    return cxl_hybrid_transfer_queue_pop_ordered(
        queue, order, G_N_ELEMENTS(order), desc, klass);
}

bool cxl_hybrid_rdma_descriptor_page_claimed(
    const CXLHybridRDMAPageDescriptor *desc,
    uint32_t page_offset)
{
    return desc && desc->claimed_bmap && page_offset < desc->nr_pages &&
           test_bit(page_offset, desc->claimed_bmap);
}

void cxl_hybrid_rdma_descriptor_destroy(CXLHybridRDMAPageDescriptor *desc)
{
    if (!desc) {
        return;
    }
    g_free(desc->claimed_bmap);
    g_free(desc->claims);
    memset(desc, 0, sizeof(*desc));
}

bool cxl_hybrid_rdma_descriptor_claim_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation)
{
    if (!desc || !page_state || !nr_pages ||
        first_page >= total_pages || nr_pages > total_pages - first_page) {
        return false;
    }

    cxl_hybrid_rdma_descriptor_destroy(desc);
    desc->first_page = first_page;
    desc->nr_pages = nr_pages;
    desc->generation = generation;
    desc->claimed_bmap = bitmap_new(nr_pages);
    desc->claims = g_new0(CXLHybridPageClaim, nr_pages);

    for (uint32_t i = 0; i < nr_pages; i++) {
        if (cxl_hybrid_page_state_claim_for_rdma(&page_state[first_page + i],
                                                 generation,
                                                 &desc->claims[i])) {
            set_bit(i, desc->claimed_bmap);
            desc->claimed_pages++;
        }
    }

    if (!desc->claimed_pages) {
        cxl_hybrid_rdma_descriptor_destroy(desc);
        return false;
    }

    return true;
}

void cxl_hybrid_rdma_descriptor_complete_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages)
{
    uint32_t limit;

    if (!desc || !page_state || desc->first_page >= total_pages) {
        return;
    }

    limit = MIN(desc->nr_pages, total_pages - desc->first_page);
    for (uint32_t i = 0; i < limit; i++) {
        if (!cxl_hybrid_rdma_descriptor_page_claimed(desc, i)) {
            continue;
        }
        if (cxl_hybrid_page_state_complete_rdma(
                &page_state[desc->first_page + i], &desc->claims[i])) {
            desc->completed_pages++;
        } else {
            desc->stale_pages++;
        }
        clear_bit(i, desc->claimed_bmap);
    }
}

void cxl_hybrid_rdma_descriptor_drop_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages)
{
    uint32_t limit;

    if (!desc || !page_state || desc->first_page >= total_pages) {
        return;
    }

    limit = MIN(desc->nr_pages, total_pages - desc->first_page);
    for (uint32_t i = 0; i < limit; i++) {
        if (!cxl_hybrid_rdma_descriptor_page_claimed(desc, i)) {
            continue;
        }
        cxl_hybrid_page_state_drop_claim(&page_state[desc->first_page + i],
                                         &desc->claims[i]);
        clear_bit(i, desc->claimed_bmap);
    }
}

void cxl_hybrid_cxl_descriptor_complete_pages_for_test(
    uint64_t *page_state,
    uint64_t total_pages,
    uint64_t first_page,
    const CXLHybridPageClaim *claims,
    uint32_t nr_pages,
    uint32_t *completedp,
    uint32_t *stalep)
{
    for (uint32_t i = 0; page_state && claims && i < nr_pages &&
         first_page + i < total_pages; i++) {
        if (cxl_hybrid_page_state_complete_cxl(&page_state[first_page + i],
                                               &claims[i])) {
            if (completedp) {
                (*completedp)++;
            }
        } else if (stalep) {
            (*stalep)++;
        }
    }
}

uint64_t cxl_hybrid_transfer_queue_depth(CXLHybridTransferQueue *queue,
                                         CXLHybridTransferClass klass)
{
    uint64_t depth;

    if (!queue || !queue->lock_ready || (int)klass < 0 ||
        klass >= CXL_HYBRID_TRANSFER_CLASS_COUNT) {
        return 0;
    }

    qemu_mutex_lock(&queue->lock);
    depth = g_queue_get_length(&queue->classes[klass]);
    qemu_mutex_unlock(&queue->lock);
    return depth;
}

void cxl_hybrid_rdma_bulk_claim_release(CXLHybridRDMABulkClaim *claim)
{
    if (!claim || !claim->page_desc) {
        return;
    }

    cxl_hybrid_rdma_descriptor_destroy(claim->page_desc);
    g_free(claim->page_desc);
    claim->page_desc = NULL;
}
