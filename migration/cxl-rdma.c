/*
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "cxl.h"
#include "cxl-rdma.h"
#include "trace.h"

static struct CXLHybridRDMASidecarBulkState {
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    CXLHybridRDMASidecarBulkStats stats;
    bool initialized;
} cxl_rdma_sidecar;

void cxl_rdma_sidecar_init(uint64_t total_regions,
                           uint64_t bytes_per_region,
                           uint64_t pages_per_region)
{
    memset(&cxl_rdma_sidecar, 0, sizeof(cxl_rdma_sidecar));
    cxl_rdma_sidecar.total_regions = total_regions;
    cxl_rdma_sidecar.bytes_per_region = bytes_per_region;
    cxl_rdma_sidecar.pages_per_region = pages_per_region;
    cxl_rdma_sidecar.initialized = total_regions && bytes_per_region &&
                                   pages_per_region;
}

void cxl_rdma_sidecar_destroy(void)
{
    memset(&cxl_rdma_sidecar, 0, sizeof(cxl_rdma_sidecar));
}

bool cxl_rdma_sidecar_submit_shadow_region(uint64_t region_index)
{
    if (!cxl_rdma_sidecar.initialized ||
        region_index >= cxl_rdma_sidecar.total_regions ||
        !cxl_hybrid_region_try_own_rdma(region_index)) {
        return false;
    }

    qatomic_inc(&cxl_rdma_sidecar.stats.rdma_bulk_regions);
    qatomic_add(&cxl_rdma_sidecar.stats.rdma_bulk_bytes,
                cxl_rdma_sidecar.bytes_per_region);
    trace_cxl_hybrid_rdma_bulk_region(region_index,
                                      cxl_rdma_sidecar.bytes_per_region);

    cxl_hybrid_mark_region_rdma_ready(region_index);
    return true;
}

void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats)
{
    if (!stats) {
        return;
    }

    stats->rdma_bulk_regions =
        qatomic_read(&cxl_rdma_sidecar.stats.rdma_bulk_regions);
    stats->rdma_bulk_bytes =
        qatomic_read(&cxl_rdma_sidecar.stats.rdma_bulk_bytes);
}
