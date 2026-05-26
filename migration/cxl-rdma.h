/*
 * CXL hybrid RDMA sidecar measurement helpers.
 *
 * This is a measurement-only spike: accepted regions are tracked as
 * whole-region RDMA work, but this layer does not publish destination
 * visibility.
 */

#ifndef MIGRATION_CXL_RDMA_H
#define MIGRATION_CXL_RDMA_H

typedef struct CXLHybridRDMASidecarBulkStats {
    uint64_t rdma_bulk_regions;
    uint64_t rdma_bulk_bytes;
} CXLHybridRDMASidecarBulkStats;

void cxl_rdma_sidecar_init(uint64_t total_regions,
                           uint64_t bytes_per_region,
                           uint64_t pages_per_region);
void cxl_rdma_sidecar_destroy(void);
bool cxl_rdma_sidecar_submit_region(uint64_t region_index,
                                    const void *src,
                                    void *dst);
bool cxl_rdma_sidecar_complete_owned_region(uint64_t region_index,
                                            const void *src,
                                            void *dst);
bool cxl_rdma_sidecar_submit_shadow_region(uint64_t region_index);
void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats);

#endif
