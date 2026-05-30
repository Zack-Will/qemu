#include "qemu/osdep.h"
#include "migration/cxl.h"

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
