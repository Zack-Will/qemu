/*
 * CXL hybrid RDMA sidecar transport helpers.
 */

#ifndef MIGRATION_CXL_RDMA_H
#define MIGRATION_CXL_RDMA_H

#include "qapi/qapi-types-migration.h"

typedef struct CXLHybridRDMASidecarBulkStats {
    uint64_t rdma_bulk_regions;
    uint64_t rdma_bulk_bytes;
} CXLHybridRDMASidecarBulkStats;

typedef struct CXLHybridRDMASidecarConfig {
    const MigrationAddress *addr;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint8_t max_cover_percent;
    bool pin_all;
    bool incoming;
} CXLHybridRDMASidecarConfig;

int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp);
void cxl_rdma_sidecar_stop(void);
bool cxl_rdma_sidecar_running(void);
void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats);

#endif
