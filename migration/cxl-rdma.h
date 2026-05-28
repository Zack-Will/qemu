/*
 * CXL hybrid RDMA sidecar transport helpers.
 */

#ifndef MIGRATION_CXL_RDMA_H
#define MIGRATION_CXL_RDMA_H

#include "qapi/qapi-types-migration.h"
#include "system/ramlist.h"

typedef struct CXLHybridRDMABulkClaim CXLHybridRDMABulkClaim;

typedef struct CXLHybridRDMASidecarOps {
    bool (*migration_running)(void *opaque);
    bool (*migration_postcopy)(void *opaque);
    bool (*migration_failed)(void *opaque);
    bool (*claim_bulk_region)(CXLHybridRDMABulkClaim *claim, void *opaque);
    void (*drop_bulk_claim)(const CXLHybridRDMABulkClaim *claim, void *opaque);
    void (*propagate_error)(Error *err, void *opaque);
    int (*foreach_ramblock)(RAMBlockIterFunc func, void *iter_opaque,
                            void *opaque);
    void *opaque;
} CXLHybridRDMASidecarOps;

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
    const CXLHybridRDMASidecarOps *ops;
} CXLHybridRDMASidecarConfig;

int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp);
bool cxl_rdma_sidecar_start_async(const CXLHybridRDMASidecarConfig *cfg,
                                  Error **errp);
void cxl_rdma_sidecar_stop(void);
bool cxl_rdma_sidecar_running(void);
void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats);

#endif
