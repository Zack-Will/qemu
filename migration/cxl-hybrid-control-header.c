/*
 * Pure helpers for the CXL hybrid fault control header.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/cxl.h"

void cxl_hybrid_control_reset_header_for_run(CXLHybridControlHeader *hdr,
                                             uint32_t generation)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = CXL_HYBRID_CTRL_MAGIC;
    hdr->version = CXL_HYBRID_CTRL_VERSION;
    hdr->request_ring_order = CXL_HYBRID_CTRL_REQUEST_ORDER;
    hdr->ready_ring_order = CXL_HYBRID_CTRL_READY_ORDER;
    hdr->generation = generation;
}
