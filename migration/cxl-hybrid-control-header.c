/*
 * Pure helpers for the CXL hybrid fault control header.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/cxl.h"
#include "qemu/atomic.h"
#include "qemu/bitmap.h"

size_t cxl_hybrid_control_visible_bitmap_words(uint64_t pages)
{
    return BITS_TO_LONGS(pages);
}

size_t cxl_hybrid_control_visible_bitmap_bytes(uint64_t pages)
{
    return cxl_hybrid_control_visible_bitmap_words(pages) *
           sizeof(unsigned long);
}

static bool cxl_hybrid_control_page_in_range(const CXLHybridControlHeader *hdr,
                                             uint64_t page_index)
{
    uint64_t nr_pages;

    if (!hdr) {
        return false;
    }

    nr_pages = (uint64_t)hdr->visible_page_words * BITS_PER_LONG;
    return page_index < nr_pages;
}

bool cxl_hybrid_control_page_visible(const CXLHybridControlHeader *hdr,
                                     const unsigned long *visible_bitmap,
                                     uint64_t page_index,
                                     uint32_t generation)
{
    if (!hdr || !visible_bitmap || generation != hdr->generation ||
        !cxl_hybrid_control_page_in_range(hdr, page_index)) {
        return false;
    }

    if (!test_bit(page_index, visible_bitmap)) {
        return false;
    }

    smp_rmb();
    return true;
}

void cxl_hybrid_control_mark_page_visible(const CXLHybridControlHeader *hdr,
                                          unsigned long *visible_bitmap,
                                          uint64_t page_index)
{
    if (!visible_bitmap ||
        !cxl_hybrid_control_page_in_range(hdr, page_index)) {
        return;
    }

    set_bit(page_index, visible_bitmap);
}

void cxl_hybrid_control_clear_page_visible(const CXLHybridControlHeader *hdr,
                                           unsigned long *visible_bitmap,
                                           uint64_t page_index)
{
    if (!visible_bitmap ||
        !cxl_hybrid_control_page_in_range(hdr, page_index)) {
        return;
    }

    clear_bit(page_index, visible_bitmap);
}

void cxl_hybrid_control_reset_run_state(CXLHybridControlHeader *hdr,
                                        unsigned long *visible_bitmap,
                                        uint32_t generation,
                                        uint32_t visible_page_words)
{
    g_assert(hdr);

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = CXL_HYBRID_CTRL_MAGIC;
    hdr->version = CXL_HYBRID_CTRL_VERSION;
    hdr->request_ring_order = CXL_HYBRID_CTRL_REQUEST_ORDER;
    hdr->ready_ring_order = CXL_HYBRID_CTRL_READY_ORDER;
    hdr->generation = generation;
    hdr->visible_page_words = visible_page_words;

    if (visible_bitmap && visible_page_words) {
        memset(visible_bitmap, 0,
               (size_t)visible_page_words * sizeof(*visible_bitmap));
    }
}

void cxl_hybrid_control_reset_header_for_run(CXLHybridControlHeader *hdr,
                                             uint32_t generation)
{
    cxl_hybrid_control_reset_run_state(hdr, NULL, generation, 0);
}
