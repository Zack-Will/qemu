/*
 * AnemoiM pool backend.
 *
 * The memory backend is a single-process sanity backend for early P1 tests.
 * The production baseline backend uses the same ABI with RDMA transport.
 */

#ifndef ANEMOI_POOL_H
#define ANEMOI_POOL_H

#include "anemoi/backend.h"
#include "anemoi/constants.h"
#include "qapi/error.h"

typedef struct AnemoiPoolMemoryConfig {
    uint32_t vm_capacity;
    uint64_t pages_per_vm;
} AnemoiPoolMemoryConfig;

AnemoiBackend *anemoi_pool_backend_new_memory(
    const AnemoiPoolMemoryConfig *cfg, Error **errp);

uint64_t anemoi_pool_page_offset(uint64_t pages_per_vm, uint32_t vmid,
                                 uint64_t gfn);

#endif /* ANEMOI_POOL_H */
