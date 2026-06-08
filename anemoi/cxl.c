#include "qemu/osdep.h"
#include "anemoi/cxl.h"
#include "anemoi/cxl-dax.h"
#include "anemoi/pool.h"
#include "qemu/host-utils.h"

typedef struct AnemoiCxlPool {
    uint32_t vm_capacity;
    uint64_t pages_per_vm;
    uint64_t total_pages;
    uint64_t total_bytes;
    AnemoiCxlDaxRegion dax;
} AnemoiCxlPool;

static bool anemoi_cxl_pool_translate(AnemoiCxlPool *pool,
                                      uint32_t vmid, uint64_t gfn,
                                      uint64_t *page_index, Error **errp)
{
    uint64_t idx;

    if (vmid >= pool->vm_capacity) {
        error_setg(errp, "Anemoi CXL vmid %u exceeds capacity %u",
                   vmid, pool->vm_capacity);
        return false;
    }
    if (gfn >= pool->pages_per_vm) {
        error_setg(errp, "Anemoi CXL gfn %" PRIu64
                   " exceeds pages_per_vm %" PRIu64,
                   gfn, pool->pages_per_vm);
        return false;
    }
    idx = anemoi_pool_page_offset(pool->pages_per_vm, vmid, gfn);
    if (idx >= pool->total_pages) {
        error_setg(errp, "Anemoi CXL translated page index out of bounds");
        return false;
    }
    *page_index = idx;
    return true;
}

static int anemoi_cxl_fetch(AnemoiBackend *backend, uint32_t vmid,
                            uint64_t gfn, void *dst_4k, Error **errp)
{
    AnemoiCxlPool *pool = backend->opaque;
    uint64_t idx;

    if (!anemoi_cxl_pool_translate(pool, vmid, gfn, &idx, errp)) {
        return -1;
    }
    memcpy(dst_4k, (uint8_t *)pool->dax.addr + idx * ANEMOI_PAGE_SIZE,
           ANEMOI_PAGE_SIZE);
    return 0;
}

static int anemoi_cxl_writeback(AnemoiBackend *backend, uint32_t vmid,
                                uint64_t gfn, const void *src_4k,
                                Error **errp)
{
    AnemoiCxlPool *pool = backend->opaque;
    uint64_t idx;

    if (!anemoi_cxl_pool_translate(pool, vmid, gfn, &idx, errp)) {
        return -1;
    }
    memcpy((uint8_t *)pool->dax.addr + idx * ANEMOI_PAGE_SIZE, src_4k,
           ANEMOI_PAGE_SIZE);
    return 0;
}

static void anemoi_cxl_shutdown(AnemoiBackend *backend)
{
    AnemoiCxlPool *pool;

    if (!backend) {
        return;
    }
    pool = backend->opaque;
    if (pool) {
        anemoi_cxl_dax_close(&pool->dax);
        g_free(pool);
    }
    g_free(backend);
}

static const AnemoiBackendOps anemoi_cxl_ops = {
    .fetch = anemoi_cxl_fetch,
    .writeback = anemoi_cxl_writeback,
    .shutdown = anemoi_cxl_shutdown,
};

AnemoiBackend *anemoi_pool_backend_new_cxl(const AnemoiCxlConfig *cfg,
                                           Error **errp)
{
    AnemoiBackend *backend;
    AnemoiCxlPool *pool;
    uint64_t total_pages;
    uint64_t total_bytes;

    if (!cfg || !cfg->dax_path || !cfg->vm_capacity || !cfg->pages_per_vm) {
        error_setg(errp, "invalid Anemoi CXL pool configuration");
        return NULL;
    }
    if (umul64_overflow((uint64_t)cfg->vm_capacity, cfg->pages_per_vm,
                        &total_pages) ||
        umul64_overflow(total_pages, ANEMOI_PAGE_SIZE, &total_bytes) ||
        total_bytes > SIZE_MAX) {
        error_setg(errp, "Anemoi CXL pool size overflows host size_t");
        return NULL;
    }

    pool = g_new0(AnemoiCxlPool, 1);
    pool->vm_capacity = cfg->vm_capacity;
    pool->pages_per_vm = cfg->pages_per_vm;
    pool->total_pages = total_pages;
    pool->total_bytes = total_bytes;

    if (anemoi_cxl_dax_open(&pool->dax, cfg->dax_path, total_bytes,
                            cfg->use_map_sync, cfg->numa_node, errp) != 0) {
        g_free(pool);
        return NULL;
    }

    backend = g_new0(AnemoiBackend, 1);
    backend->name = "anemoi-cxl-dax-pool";
    backend->kind = ANEMOI_BACKEND_CXL;
    backend->page_size = ANEMOI_PAGE_SIZE;
    backend->ops = &anemoi_cxl_ops;
    backend->opaque = pool;
    return backend;
}
