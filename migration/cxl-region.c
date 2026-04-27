/*
 * Pure helpers for CXL hybrid fault region geometry.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "migration/cxl.h"

bool cxl_hybrid_fault_resolve_mode_emits_burst(
    CXLHybridFaultResolveMode mode)
{
    return mode == CXL_HYBRID_FAULT_RESOLVE_MODE_COPY;
}

int cxl_hybrid_fault_region_compute(uint64_t block_global_base,
                                    uint64_t block_used_len,
                                    uint64_t block_cxl_pages_offset,
                                    uint64_t fault_block_offset,
                                    uint64_t granule,
                                    uint64_t target_page_size,
                                    CXLHybridFaultRegionGeometry *out,
                                    Error **errp)
{
    uint64_t fault_global;
    uint64_t region_global;
    uint64_t region_block_offset;
    uint64_t cxl_offset;
    uint64_t nr_pages;

    if (!out || !granule || !target_page_size ||
        !is_power_of_2(granule) ||
        !is_power_of_2(target_page_size) ||
        granule < target_page_size ||
        granule % target_page_size) {
        error_setg(errp, "invalid CXL hybrid fault region granule");
        return -EINVAL;
    }

    if (fault_block_offset >= block_used_len ||
        block_global_base > UINT64_MAX - fault_block_offset) {
        error_setg(errp, "CXL hybrid fault offset is outside the RAMBlock");
        return -EINVAL;
    }

    fault_global = block_global_base + fault_block_offset;
    region_global = QEMU_ALIGN_DOWN(fault_global, granule);
    if (region_global < block_global_base) {
        error_setg(errp, "CXL hybrid fault region crosses RAMBlock start");
        return -EINVAL;
    }

    region_block_offset = region_global - block_global_base;
    if (region_block_offset > block_used_len ||
        granule > block_used_len - region_block_offset) {
        error_setg(errp, "CXL hybrid fault region crosses RAMBlock end");
        return -EINVAL;
    }

    if (!QEMU_IS_ALIGNED(region_global, granule) ||
        !QEMU_IS_ALIGNED(region_block_offset, granule) ||
        !QEMU_IS_ALIGNED(block_cxl_pages_offset, granule)) {
        error_setg(errp, "CXL hybrid fault region is not DAX-granule aligned");
        return -EINVAL;
    }

    if (block_cxl_pages_offset > UINT64_MAX - region_block_offset) {
        error_setg(errp, "CXL hybrid fault region CXL offset overflows");
        return -EOVERFLOW;
    }

    cxl_offset = block_cxl_pages_offset + region_block_offset;
    if (!QEMU_IS_ALIGNED(cxl_offset, granule)) {
        error_setg(errp, "CXL hybrid fault region CXL offset is not aligned");
        return -EINVAL;
    }

    nr_pages = granule / target_page_size;
    if (nr_pages > UINT32_MAX) {
        error_setg(errp, "CXL hybrid fault region page count overflows");
        return -EOVERFLOW;
    }

    *out = (CXLHybridFaultRegionGeometry) {
        .global_offset = region_global,
        .block_offset = region_block_offset,
        .cxl_offset = cxl_offset,
        .region_len = granule,
        .first_page_index = region_global / target_page_size,
        .nr_pages = (uint32_t)nr_pages,
        .region_index = region_global / granule,
    };
    return 0;
}
