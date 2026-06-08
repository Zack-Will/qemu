/*
 * Anemoi CXL devdax backend.
 */

#ifndef ANEMOI_CXL_H
#define ANEMOI_CXL_H

#include "anemoi/backend.h"

typedef struct AnemoiCxlConfig {
    const char *dax_path;
    uint32_t vm_capacity;
    uint64_t pages_per_vm;
    bool use_map_sync;
    int numa_node;
} AnemoiCxlConfig;

AnemoiBackend *anemoi_pool_backend_new_cxl(const AnemoiCxlConfig *cfg,
                                           Error **errp);

#endif /* ANEMOI_CXL_H */
