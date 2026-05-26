#include "qemu/osdep.h"
#include "anemoi/pool.h"
#include "anemoi/runtime.h"
#include "anemoi/uffd.h"
#include "qemu/error-report.h"
#include "system/ramblock.h"
#include "system/runstate.h"

typedef struct AnemoiRuntimeBlock {
    RAMBlock *rb;
    void *host;
    uint64_t length;
    uint64_t gfn_base;
    const char *idstr;
} AnemoiRuntimeBlock;

struct AnemoiRuntime {
    uint32_t vmid;
    uint64_t guest_pages;
    uint64_t local_cache_pages;
    AnemoiBackend *backend;
    bool backend_owned;
    AnemoiCache *cache;
    AnemoiRMap *rmap;
    AnemoiLM *lm;
    AnemoiFaultService *fault_service;
    AnemoiRuntimeBlock *blocks;
    uint32_t nr_blocks;
    AnemoiCacheRegion *regions;
    AnemoiFaultRange *fault_ranges;
    bool vm_was_running;
    bool auto_resume;
};

typedef struct AnemoiCollectState {
    GArray *blocks;
    uint64_t guest_pages;
    Error **errp;
} AnemoiCollectState;

static int anemoi_runtime_collect_block(RAMBlock *rb, void *opaque)
{
    AnemoiCollectState *state = opaque;
    AnemoiRuntimeBlock block;
    uint64_t pages;
    ram_addr_t ram_offset;

    if (!qemu_ram_is_migratable(rb)) {
        return 0;
    }

    memset(&block, 0, sizeof(block));
    block.host = qemu_ram_get_host_addr(rb);
    block.length = qemu_ram_get_used_length(rb);
    block.idstr = qemu_ram_get_idstr(rb);
    block.rb = rb;

    if (!block.host || !block.length) {
        return 0;
    }
    if (qemu_ram_pagesize(rb) != ANEMOI_PAGE_SIZE) {
        error_setg(state->errp,
                   "Anemoi requires 4 KiB RAMBlock page size for %s, got %zu",
                   block.idstr, qemu_ram_pagesize(rb));
        return -1;
    }
    if (!QEMU_IS_ALIGNED((uintptr_t)block.host, ANEMOI_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(block.length, ANEMOI_PAGE_SIZE)) {
        error_setg(state->errp, "Anemoi RAMBlock %s is not 4 KiB aligned",
                   block.idstr);
        return -1;
    }

    ram_offset = qemu_ram_get_offset(rb);
    if (!QEMU_IS_ALIGNED(ram_offset, ANEMOI_PAGE_SIZE)) {
        error_setg(state->errp, "Anemoi RAMBlock %s has unaligned RAM offset",
                   block.idstr);
        return -1;
    }

    pages = block.length / ANEMOI_PAGE_SIZE;
    block.gfn_base = ram_offset / ANEMOI_PAGE_SIZE;
    if (block.gfn_base > UINT64_MAX - pages) {
        error_setg(state->errp, "Anemoi RAMBlock %s GFN range overflows",
                   block.idstr);
        return -1;
    }

    state->guest_pages = MAX(state->guest_pages, block.gfn_base + pages);
    g_array_append_val(state->blocks, block);
    return 0;
}

static int anemoi_runtime_collect_blocks(AnemoiRuntime *runtime,
                                         Error **errp)
{
    AnemoiCollectState state = {
        .blocks = g_array_new(false, true, sizeof(AnemoiRuntimeBlock)),
        .errp = errp,
    };

    if (qemu_ram_foreach_block(anemoi_runtime_collect_block, &state) != 0) {
        g_array_free(state.blocks, true);
        return -1;
    }
    if (!state.blocks->len || !state.guest_pages) {
        g_array_free(state.blocks, true);
        error_setg(errp, "Anemoi found no migratable RAMBlocks");
        return -1;
    }

    runtime->guest_pages = state.guest_pages;
    runtime->nr_blocks = state.blocks->len;
    runtime->blocks = (AnemoiRuntimeBlock *)g_array_free(state.blocks, false);
    return 0;
}

static void anemoi_runtime_build_ranges(AnemoiRuntime *runtime)
{
    runtime->regions = g_new0(AnemoiCacheRegion, runtime->nr_blocks);
    runtime->fault_ranges = g_new0(AnemoiFaultRange, runtime->nr_blocks);

    for (uint32_t i = 0; i < runtime->nr_blocks; i++) {
        AnemoiRuntimeBlock *block = &runtime->blocks[i];

        runtime->regions[i] = (AnemoiCacheRegion) {
            .hva_base = (uintptr_t)block->host,
            .hva_size = block->length,
            .gfn_base = block->gfn_base,
        };
        runtime->fault_ranges[i] = (AnemoiFaultRange) {
            .host = block->host,
            .length = block->length,
            .idstr = block->idstr,
        };
    }
}

static int anemoi_runtime_create_default_backend(AnemoiRuntime *runtime,
                                                 Error **errp)
{
    AnemoiPoolMemoryConfig cfg = {
        .vm_capacity = runtime->vmid + 1,
        .pages_per_vm = runtime->guest_pages,
    };

    if (runtime->vmid == UINT32_MAX) {
        error_setg(errp, "Anemoi vmid %u is too large for memory backend",
                   runtime->vmid);
        return -1;
    }

    runtime->backend = anemoi_pool_backend_new_memory(&cfg, errp);
    if (!runtime->backend) {
        return -1;
    }
    runtime->backend_owned = true;
    return 0;
}

static int anemoi_runtime_create_rdma_backend(AnemoiRuntime *runtime,
                                              const AnemoiRDMAConfig *cfg,
                                              Error **errp)
{
    AnemoiRDMAConfig rdma_cfg = *cfg;

    if (!rdma_cfg.tcp_port) {
        rdma_cfg.tcp_port = ANEMOI_RDMA_DEFAULT_TCP_PORT;
    }
    if (!rdma_cfg.ib_port) {
        rdma_cfg.ib_port = ANEMOI_RDMA_DEFAULT_IB_PORT;
    }
    if (!rdma_cfg.vm_capacity) {
        if (runtime->vmid == UINT32_MAX) {
            error_setg(errp, "Anemoi vmid %u is too large for RDMA backend",
                       runtime->vmid);
            return -1;
        }
        rdma_cfg.vm_capacity = runtime->vmid + 1;
    }
    if (!rdma_cfg.pages_per_vm) {
        rdma_cfg.pages_per_vm = runtime->guest_pages;
    }

    runtime->backend = anemoi_rdma_backend_new(&rdma_cfg, errp);
    if (!runtime->backend) {
        return -1;
    }
    runtime->backend_owned = true;
    return 0;
}

static int anemoi_runtime_create_backend(AnemoiRuntime *runtime,
                                         const AnemoiRuntimeConfig *cfg,
                                         Error **errp)
{
    if (runtime->backend) {
        return 0;
    }

    switch (cfg->backend_kind) {
    case ANEMOI_RUNTIME_BACKEND_MEMORY:
        return anemoi_runtime_create_default_backend(runtime, errp);
    case ANEMOI_RUNTIME_BACKEND_RDMA:
        return anemoi_runtime_create_rdma_backend(runtime, &cfg->rdma, errp);
    default:
        error_setg(errp, "unknown Anemoi backend kind %d",
                   cfg->backend_kind);
        return -1;
    }
}

static int anemoi_runtime_seed_backend(AnemoiRuntime *runtime, Error **errp)
{
    for (uint32_t i = 0; i < runtime->nr_blocks; i++) {
        AnemoiRuntimeBlock *block = &runtime->blocks[i];
        uint8_t *host = block->host;

        for (uint64_t off = 0; off < block->length; off += ANEMOI_PAGE_SIZE) {
            uint64_t gfn = block->gfn_base + off / ANEMOI_PAGE_SIZE;

            if (anemoi_backend_writeback(runtime->backend, runtime->vmid, gfn,
                                         host + off, errp) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int anemoi_runtime_discard_ramblocks(AnemoiRuntime *runtime,
                                            Error **errp)
{
    for (uint32_t i = 0; i < runtime->nr_blocks; i++) {
        AnemoiRuntimeBlock *block = &runtime->blocks[i];

        if (ram_block_discard_range(block->rb, 0, block->length) != 0) {
            error_setg(errp, "failed to discard Anemoi RAMBlock %s",
                       block->idstr);
            return -1;
        }
    }
    return 0;
}

static int anemoi_runtime_create_core(AnemoiRuntime *runtime, Error **errp)
{
    AnemoiCacheConfig cache_cfg = {
        .vmid = runtime->vmid,
        .guest_pages = runtime->guest_pages,
        .local_cache_pages = runtime->local_cache_pages,
        .regions = runtime->regions,
        .nr_regions = runtime->nr_blocks,
        .backend = runtime->backend,
    };
    AnemoiLMConfig lm_cfg = {
        .vmid = runtime->vmid,
        .guest_pages = runtime->guest_pages,
    };

    runtime->rmap = anemoi_rmap_new(runtime->guest_pages, true, errp);
    if (!runtime->rmap) {
        return -1;
    }

    cache_cfg.rmap = runtime->rmap;
    runtime->cache = anemoi_cache_new(&cache_cfg, errp);
    if (!runtime->cache) {
        return -1;
    }

    lm_cfg.cache = runtime->cache;
    lm_cfg.rmap = runtime->rmap;
    runtime->lm = anemoi_lm_new(&lm_cfg, errp);
    return runtime->lm ? 0 : -1;
}

static int anemoi_runtime_start_fault_service(AnemoiRuntime *runtime,
                                              Error **errp)
{
    AnemoiFaultServiceConfig cfg = {
        .cache = runtime->cache,
        .ranges = runtime->fault_ranges,
        .nr_ranges = runtime->nr_blocks,
        .discard_on_start = false,
    };

    runtime->fault_service = anemoi_fault_service_start(&cfg, errp);
    return runtime->fault_service ? 0 : -1;
}

AnemoiRuntime *anemoi_runtime_start(const AnemoiRuntimeConfig *cfg,
                                    Error **errp)
{
    AnemoiRuntime *runtime;

    if (!cfg) {
        error_setg(errp, "invalid Anemoi runtime configuration");
        return NULL;
    }

    runtime = g_new0(AnemoiRuntime, 1);
    runtime->vmid = cfg->vmid;
    runtime->local_cache_pages = cfg->local_cache_pages;
    runtime->backend = cfg->backend;
    runtime->backend_owned = cfg->backend_owned;
    runtime->auto_resume = cfg->auto_resume;

    if (cfg->auto_pause && runstate_is_running()) {
        if (vm_stop(RUN_STATE_PAUSED) != 0) {
            error_setg(errp, "failed to pause VM for Anemoi boot sequence");
            goto fail;
        }
        runtime->vm_was_running = true;
    }

    if (anemoi_runtime_collect_blocks(runtime, errp) != 0) {
        goto fail_resume;
    }
    if (!runtime->local_cache_pages) {
        runtime->local_cache_pages = runtime->guest_pages;
    }
    if (runtime->local_cache_pages > runtime->guest_pages) {
        error_setg(errp, "Anemoi local cache pages exceed guest pages");
        goto fail_resume;
    }

    anemoi_runtime_build_ranges(runtime);

    if (anemoi_runtime_create_backend(runtime, cfg, errp) != 0) {
        goto fail_resume;
    }
    if (anemoi_runtime_seed_backend(runtime, errp) != 0 ||
        anemoi_runtime_create_core(runtime, errp) != 0 ||
        anemoi_runtime_start_fault_service(runtime, errp) != 0 ||
        anemoi_runtime_discard_ramblocks(runtime, errp) != 0) {
        goto fail_resume;
    }

    if (runtime->vm_was_running && runtime->auto_resume) {
        vm_start();
    }
    return runtime;

fail_resume:
fail:
    anemoi_runtime_stop(runtime);
    return NULL;
}

void anemoi_runtime_stop(AnemoiRuntime *runtime)
{
    if (!runtime) {
        return;
    }
    if (runtime->fault_service) {
        anemoi_fault_service_stop(runtime->fault_service);
    }
    anemoi_lm_free(runtime->lm);
    anemoi_rmap_free(runtime->rmap);
    anemoi_cache_free(runtime->cache);
    if (runtime->backend_owned) {
        anemoi_backend_shutdown(runtime->backend);
    }
    g_free(runtime->fault_ranges);
    g_free(runtime->regions);
    g_free(runtime->blocks);
    g_free(runtime);
}

AnemoiLM *anemoi_runtime_lm(AnemoiRuntime *runtime)
{
    return runtime ? runtime->lm : NULL;
}

AnemoiCache *anemoi_runtime_cache(AnemoiRuntime *runtime)
{
    return runtime ? runtime->cache : NULL;
}

AnemoiBackend *anemoi_runtime_backend(AnemoiRuntime *runtime)
{
    return runtime ? runtime->backend : NULL;
}

int anemoi_runtime_prepare_switchover(AnemoiRuntime *runtime,
                                      AnemoiLMBranch branch, Error **errp)
{
    if (!runtime || !runtime->lm) {
        error_setg(errp, "Anemoi runtime is not active");
        return -1;
    }
    return anemoi_lm_prepare_switchover(runtime->lm, branch, errp);
}

void anemoi_runtime_get_stats(AnemoiRuntime *runtime,
                              AnemoiRuntimeStats *stats)
{
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!runtime) {
        return;
    }

    stats->vmid = runtime->vmid;
    stats->guest_pages = runtime->guest_pages;
    stats->local_cache_pages = runtime->local_cache_pages;
    stats->nr_ramblocks = runtime->nr_blocks;
    stats->fault_service_failed =
        anemoi_fault_service_failed(runtime->fault_service);

    if (runtime->cache) {
        const AnemoiCacheStats *cache_stats =
            anemoi_cache_stats(runtime->cache);

        if (cache_stats) {
            stats->cache = *cache_stats;
        }
    }
    if (runtime->backend) {
        stats->backend = runtime->backend->stats;
    }
}
