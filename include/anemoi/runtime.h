/*
 * QEMU-facing Anemoi runtime.
 */

#ifndef ANEMOI_RUNTIME_H
#define ANEMOI_RUNTIME_H

#include "anemoi/backend.h"
#include "anemoi/cache.h"
#include "anemoi/lm.h"
#include "anemoi/rdma.h"
#include "qapi/error.h"

typedef struct AnemoiRuntime AnemoiRuntime;

typedef enum AnemoiRuntimeBackendKind {
    ANEMOI_RUNTIME_BACKEND_MEMORY = 0,
    ANEMOI_RUNTIME_BACKEND_RDMA,
} AnemoiRuntimeBackendKind;

typedef enum AnemoiRuntimeBootMode {
    ANEMOI_RUNTIME_BOOT_SOURCE_SEED = 0,
    ANEMOI_RUNTIME_BOOT_DESTINATION_ATTACH,
} AnemoiRuntimeBootMode;

typedef struct AnemoiRuntimeConfig {
    uint32_t vmid;
    uint64_t local_cache_pages;
    AnemoiRuntimeBootMode boot_mode;
    AnemoiRuntimeBackendKind backend_kind;
    AnemoiRDMAConfig rdma;
    AnemoiBackend *backend;
    bool backend_owned;
    bool auto_pause;
    bool auto_resume;
} AnemoiRuntimeConfig;

typedef struct AnemoiRuntimeStats {
    uint32_t vmid;
    AnemoiRuntimeBootMode boot_mode;
    uint64_t guest_pages;
    uint64_t local_cache_pages;
    uint64_t nr_ramblocks;
    bool fault_service_failed;
    AnemoiCacheStats cache;
    AnemoiBackendStats backend;
} AnemoiRuntimeStats;

AnemoiRuntime *anemoi_runtime_start(const AnemoiRuntimeConfig *cfg,
                                    Error **errp);
void anemoi_runtime_stop(AnemoiRuntime *runtime);

AnemoiLM *anemoi_runtime_lm(AnemoiRuntime *runtime);
AnemoiCache *anemoi_runtime_cache(AnemoiRuntime *runtime);
AnemoiBackend *anemoi_runtime_backend(AnemoiRuntime *runtime);

int anemoi_runtime_prepare_switchover(AnemoiRuntime *runtime,
                                      AnemoiLMBranch branch, Error **errp);
void anemoi_runtime_get_stats(AnemoiRuntime *runtime,
                              AnemoiRuntimeStats *stats);

#endif /* ANEMOI_RUNTIME_H */
