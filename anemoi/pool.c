#include "qemu/osdep.h"
#include "anemoi/pool.h"
#include "qemu/host-utils.h"

typedef struct AnemoiMemoryPool {
    uint32_t vm_capacity;
    uint64_t pages_per_vm;
    uint8_t *bytes;
    uint64_t total_pages;
} AnemoiMemoryPool;

uint64_t anemoi_pool_page_offset(uint64_t pages_per_vm, uint32_t vmid,
                                 uint64_t gfn)
{
    return (uint64_t)vmid * pages_per_vm + gfn;
}

static bool anemoi_memory_pool_translate(AnemoiMemoryPool *pool,
                                         uint32_t vmid, uint64_t gfn,
                                         uint64_t *page_index,
                                         Error **errp)
{
    uint64_t idx;

    if (vmid >= pool->vm_capacity) {
        error_setg(errp, "AnemoiM vmid %u exceeds capacity %u",
                   vmid, pool->vm_capacity);
        return false;
    }
    if (gfn >= pool->pages_per_vm) {
        error_setg(errp, "AnemoiM gfn %" PRIu64
                   " exceeds pages_per_vm %" PRIu64,
                   gfn, pool->pages_per_vm);
        return false;
    }
    idx = anemoi_pool_page_offset(pool->pages_per_vm, vmid, gfn);
    if (idx >= pool->total_pages) {
        error_setg(errp, "AnemoiM translated page index out of bounds");
        return false;
    }
    *page_index = idx;
    return true;
}

static int anemoi_memory_fetch(AnemoiBackend *backend, uint32_t vmid,
                               uint64_t gfn, void *dst_4k, Error **errp)
{
    AnemoiMemoryPool *pool = backend->opaque;
    uint64_t idx;

    if (!anemoi_memory_pool_translate(pool, vmid, gfn, &idx, errp)) {
        return -1;
    }
    memcpy(dst_4k, pool->bytes + idx * ANEMOI_PAGE_SIZE, ANEMOI_PAGE_SIZE);
    return 0;
}

static int anemoi_memory_writeback(AnemoiBackend *backend, uint32_t vmid,
                                   uint64_t gfn, const void *src_4k,
                                   Error **errp)
{
    AnemoiMemoryPool *pool = backend->opaque;
    uint64_t idx;

    if (!anemoi_memory_pool_translate(pool, vmid, gfn, &idx, errp)) {
        return -1;
    }
    memcpy(pool->bytes + idx * ANEMOI_PAGE_SIZE, src_4k, ANEMOI_PAGE_SIZE);
    return 0;
}

static void anemoi_memory_shutdown(AnemoiBackend *backend)
{
    AnemoiMemoryPool *pool;

    if (!backend) {
        return;
    }
    pool = backend->opaque;
    if (pool) {
        g_free(pool->bytes);
        g_free(pool);
    }
    g_free(backend);
}

static const AnemoiBackendOps anemoi_memory_ops = {
    .fetch = anemoi_memory_fetch,
    .writeback = anemoi_memory_writeback,
    .shutdown = anemoi_memory_shutdown,
};

AnemoiBackend *anemoi_pool_backend_new_memory(
    const AnemoiPoolMemoryConfig *cfg, Error **errp)
{
    AnemoiBackend *backend;
    AnemoiMemoryPool *pool;
    uint64_t total_pages;
    uint64_t total_bytes;

    if (!cfg || !cfg->vm_capacity || !cfg->pages_per_vm) {
        error_setg(errp, "invalid Anemoi memory pool configuration");
        return NULL;
    }
    if (umul64_overflow((uint64_t)cfg->vm_capacity, cfg->pages_per_vm,
                        &total_pages) ||
        umul64_overflow(total_pages, ANEMOI_PAGE_SIZE, &total_bytes) ||
        total_bytes > SIZE_MAX) {
        error_setg(errp, "Anemoi memory pool size overflows host size_t");
        return NULL;
    }

    pool = g_new0(AnemoiMemoryPool, 1);
    pool->vm_capacity = cfg->vm_capacity;
    pool->pages_per_vm = cfg->pages_per_vm;
    pool->total_pages = total_pages;
    pool->bytes = g_try_malloc0((size_t)total_bytes);
    if (!pool->bytes) {
        g_free(pool);
        error_setg(errp, "failed to allocate %" PRIu64
                   " bytes for Anemoi memory pool", total_bytes);
        return NULL;
    }

    backend = g_new0(AnemoiBackend, 1);
    backend->name = "anemoi-memory-pool";
    backend->kind = ANEMOI_BACKEND_MEMORY;
    backend->page_size = ANEMOI_PAGE_SIZE;
    backend->ops = &anemoi_memory_ops;
    backend->opaque = pool;
    return backend;
}
