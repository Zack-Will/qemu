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

size_t cxl_hybrid_control_visible_region_bitmap_words(uint64_t regions)
{
    return BITS_TO_LONGS(regions);
}

size_t cxl_hybrid_control_visible_region_bitmap_bytes(uint64_t regions)
{
    return cxl_hybrid_control_visible_region_bitmap_words(regions) *
           sizeof(unsigned long);
}

size_t cxl_hybrid_control_owned_region_bitmap_words(uint64_t regions)
{
    return BITS_TO_LONGS(regions);
}

size_t cxl_hybrid_control_owned_region_bitmap_bytes(uint64_t regions)
{
    return cxl_hybrid_control_owned_region_bitmap_words(regions) *
           sizeof(unsigned long);
}

uint32_t cxl_hybrid_control_region_granule_shift(uint64_t region_granule)
{
    if (!region_granule || !is_power_of_2(region_granule)) {
        return 0;
    }

    return ctz64(region_granule);
}

uint32_t cxl_hybrid_control_generation(const CXLHybridControlHeader *hdr)
{
    return hdr ? qatomic_load_acquire(&hdr->generation) : 0;
}

bool cxl_hybrid_control_generation_matches(const CXLHybridControlHeader *hdr,
                                           uint32_t generation)
{
    return hdr && cxl_hybrid_control_generation(hdr) == generation;
}

bool cxl_hybrid_control_abort_generation(CXLHybridControlHeader *hdr,
                                         uint32_t generation)
{
    uint32_t next_generation = generation == UINT32_MAX ? 0 : generation + 1;

    return hdr &&
           qatomic_cmpxchg(&hdr->generation, generation, next_generation) ==
           generation;
}

uint32_t cxl_hybrid_select_fault_publish_generation(bool incoming_valid,
                                                    uint32_t incoming_generation,
                                                    bool source_run_valid,
                                                    uint32_t source_run_generation,
                                                    uint64_t phase_transitions)
{
    if (incoming_valid) {
        return incoming_generation;
    }

    if (source_run_valid) {
        return source_run_generation;
    }

    return (uint32_t)phase_transitions;
}

uint64_t cxl_hybrid_control_source_write_count(
    const CXLHybridControlHeader *hdr)
{
    return hdr ? qatomic_load_acquire(&hdr->source_write_count) : 0;
}

uint64_t cxl_hybrid_control_source_write_begin(CXLHybridControlHeader *hdr)
{
    return hdr ? qatomic_inc_fetch(&hdr->source_write_count) : 0;
}

uint64_t cxl_hybrid_control_source_write_end(CXLHybridControlHeader *hdr)
{
    return hdr ? qatomic_dec_fetch(&hdr->source_write_count) : 0;
}

bool cxl_hybrid_control_fault_pressure(const CXLHybridControlHeader *hdr,
                                       uint32_t generation)
{
    uint64_t prod;
    uint64_t cons;

    if (!cxl_hybrid_control_generation_matches(hdr, generation)) {
        return false;
    }

    prod = qatomic_load_acquire(&hdr->request_prod);
    cons = qatomic_load_acquire(&hdr->request_cons);
    if (prod != cons) {
        return true;
    }

    if (qatomic_load_acquire(&hdr->active_request_count)) {
        return true;
    }

    return qatomic_load_acquire(&hdr->active_enqueue_count) != 0;
}

bool cxl_hybrid_control_page_range_resolved(uint64_t first_page,
                                            uint32_t nr_pages,
                                            CXLHybridPageResolveFunc resolve,
                                            void *opaque,
                                            uint64_t *unresolved_page)
{
    uint32_t page;

    if (!resolve) {
        if (unresolved_page) {
            *unresolved_page = first_page;
        }
        return false;
    }

    for (page = 0; page < nr_pages; page++) {
        uint64_t page_index = first_page + page;

        if (!resolve(page_index, opaque)) {
            if (unresolved_page) {
                *unresolved_page = page_index;
            }
            return false;
        }
    }

    return true;
}

static bool cxl_hybrid_control_page_in_range(const CXLHybridControlHeader *hdr,
                                             uint64_t page_index)
{
    if (!hdr) {
        return false;
    }

    return page_index < hdr->total_pages;
}

static bool cxl_hybrid_control_region_in_range(const CXLHybridControlHeader *hdr,
                                               uint64_t region_index)
{
    if (!hdr) {
        return false;
    }

    return region_index < hdr->total_regions;
}

static bool cxl_hybrid_control_page_range_in_range(
    const CXLHybridControlHeader *hdr,
    uint64_t first_page,
    uint32_t nr_pages)
{
    if (!hdr || !nr_pages || first_page >= hdr->total_pages) {
        return false;
    }

    return nr_pages <= hdr->total_pages - first_page;
}

bool cxl_hybrid_control_region_span_index(const CXLHybridControlHeader *hdr,
                                          uint64_t first_page,
                                          uint32_t nr_pages,
                                          uint64_t *region_indexp)
{
    uint64_t pages_per_region;
    uint64_t expected_pages;
    uint64_t region_index;

    if (!hdr || !hdr->target_page_shift ||
        hdr->region_granule_shift < hdr->target_page_shift ||
        hdr->region_granule_shift - hdr->target_page_shift >= 64) {
        return false;
    }

    pages_per_region = 1ULL << (hdr->region_granule_shift -
                                hdr->target_page_shift);
    if (!pages_per_region || first_page % pages_per_region) {
        return false;
    }

    region_index = first_page / pages_per_region;
    if (!cxl_hybrid_control_region_in_range(hdr, region_index)) {
        return false;
    }

    expected_pages = MIN(pages_per_region, hdr->total_pages - first_page);
    if (nr_pages != expected_pages) {
        return false;
    }

    if (region_indexp) {
        *region_indexp = region_index;
    }
    return true;
}

bool cxl_hybrid_control_region_span_valid(const CXLHybridControlHeader *hdr,
                                          uint64_t first_page,
                                          uint32_t nr_pages)
{
    return cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                                NULL);
}

bool cxl_hybrid_control_page_visible(const CXLHybridControlHeader *hdr,
                                     const unsigned long *visible_bitmap,
                                     uint64_t page_index,
                                     uint32_t generation)
{
    if (!hdr || !visible_bitmap ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_in_range(hdr, page_index)) {
        return false;
    }

    if (!test_bit(page_index, visible_bitmap)) {
        return false;
    }

    smp_rmb();
    return true;
}

bool cxl_hybrid_control_region_visible(const CXLHybridControlHeader *hdr,
                                       const unsigned long *visible_bitmap,
                                       const unsigned long *visible_region_bitmap,
                                       uint64_t first_page,
                                       uint32_t nr_pages,
                                       uint32_t generation)
{
    uint64_t region_index = UINT64_MAX;
    uint64_t page;

    if (!hdr || !nr_pages ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_range_in_range(hdr, first_page, nr_pages)) {
        return false;
    }

    if (visible_region_bitmap &&
        cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                             &region_index) &&
        test_bit(region_index, visible_region_bitmap)) {
        smp_rmb();
        return true;
    }

    if (!visible_bitmap) {
        return false;
    }

    for (page = 0; page < nr_pages; page++) {
        if (!test_bit(first_page + page, visible_bitmap)) {
            return false;
        }
    }

    smp_rmb();
    return true;
}

bool cxl_hybrid_control_region_bit_visible(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_region_bitmap,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation)
{
    uint64_t region_index = UINT64_MAX;

    if (!hdr || !visible_region_bitmap || !nr_pages ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_range_in_range(hdr, first_page, nr_pages) ||
        !cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                              &region_index)) {
        return false;
    }

    if (!test_bit(region_index, visible_region_bitmap)) {
        return false;
    }

    smp_rmb();
    return true;
}

bool cxl_hybrid_control_region_visible_or_synthesize(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    unsigned long *visible_region_bitmap,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation)
{
    uint64_t region_index = UINT64_MAX;
    uint64_t page;

    if (cxl_hybrid_control_region_bit_visible(hdr, visible_region_bitmap,
                                              first_page, nr_pages,
                                              generation)) {
        return true;
    }
    if (!hdr || !visible_bitmap || !visible_region_bitmap ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                              &region_index)) {
        return false;
    }

    for (page = 0; page < nr_pages; page++) {
        if (!test_bit(first_page + page, visible_bitmap)) {
            return false;
        }
    }

    cxl_hybrid_control_mark_region_visible(hdr, visible_region_bitmap,
                                           region_index);
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

    /* Publish page data before making the visibility bit observable. */
    smp_mb_release();
    set_bit_atomic(page_index, visible_bitmap);
}

void cxl_hybrid_control_mark_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t page_index,
    uint32_t generation)
{
    if (!cxl_hybrid_control_generation_matches(hdr, generation)) {
        return;
    }

    cxl_hybrid_control_mark_page_visible(hdr, visible_bitmap, page_index);
}

void cxl_hybrid_control_mark_pages_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t first_page,
    uint64_t nr_pages,
    uint32_t generation)
{
    if (!visible_bitmap || !nr_pages ||
        nr_pages > LONG_MAX ||
        first_page > LONG_MAX ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        first_page >= hdr->total_pages ||
        nr_pages > hdr->total_pages - first_page) {
        return;
    }

    /* Publish page data before making the visibility range observable. */
    smp_mb_release();
    bitmap_set_atomic(visible_bitmap, first_page, nr_pages);
}

void cxl_hybrid_control_clear_page_visible(const CXLHybridControlHeader *hdr,
                                           unsigned long *visible_bitmap,
                                           uint64_t page_index)
{
    if (!visible_bitmap ||
        !cxl_hybrid_control_page_in_range(hdr, page_index)) {
        return;
    }

    clear_bit_atomic(page_index, visible_bitmap);
}

void cxl_hybrid_control_mark_region_visible(const CXLHybridControlHeader *hdr,
                                            unsigned long *visible_region_bitmap,
                                            uint64_t region_index)
{
    if (!visible_region_bitmap ||
        !cxl_hybrid_control_region_in_range(hdr, region_index)) {
        return;
    }

    /* Publish region data before making the region visibility bit observable. */
    smp_mb_release();
    set_bit_atomic(region_index, visible_region_bitmap);
}

void cxl_hybrid_control_mark_region_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_region_bitmap,
    uint64_t region_index,
    uint32_t generation)
{
    if (!cxl_hybrid_control_generation_matches(hdr, generation)) {
        return;
    }

    cxl_hybrid_control_mark_region_visible(hdr, visible_region_bitmap,
                                           region_index);
}

bool cxl_hybrid_control_mark_region_visible_for_span_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_region_bitmap,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation)
{
    uint64_t region_index = UINT64_MAX;

    if (!visible_region_bitmap ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                              &region_index)) {
        return false;
    }

    cxl_hybrid_control_mark_region_visible(hdr, visible_region_bitmap,
                                           region_index);
    return true;
}

bool cxl_hybrid_control_mark_visible_region_span_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    unsigned long *visible_region_bitmap,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation)
{
    uint64_t region_index = UINT64_MAX;

    if (!visible_bitmap || !visible_region_bitmap ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                              &region_index)) {
        return false;
    }

    cxl_hybrid_control_mark_pages_visible_generation(
        hdr, visible_bitmap, first_page, nr_pages, generation);
    cxl_hybrid_control_mark_region_visible(hdr, visible_region_bitmap,
                                           region_index);
    return true;
}

void cxl_hybrid_control_reset_run_state(CXLHybridControlHeader *hdr,
                                        unsigned long *visible_bitmap,
                                        uint64_t visible_pages,
                                        unsigned long *visible_region_bitmap,
                                        uint64_t visible_regions,
                                        unsigned long *owned_region_bitmap,
                                        uint64_t total_regions,
                                        uint64_t region_granule,
                                        uint32_t target_page_shift,
                                        uint32_t generation)
{
    g_assert(hdr);

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = CXL_HYBRID_CTRL_MAGIC;
    hdr->version = CXL_HYBRID_CTRL_VERSION;
    hdr->request_ring_order = CXL_HYBRID_CTRL_REQUEST_ORDER;
    hdr->generation = generation;
    hdr->active_enqueue_count = 0;
    hdr->active_request_count = 0;
    hdr->completed_generation = 0;
    hdr->completion_flags = 0;
    hdr->visible_page_words = BITS_TO_LONGS(visible_pages);
    hdr->visible_region_words = BITS_TO_LONGS(visible_regions);
    hdr->owned_region_words = BITS_TO_LONGS(total_regions);
    hdr->region_granule_shift =
        cxl_hybrid_control_region_granule_shift(region_granule);
    hdr->target_page_shift = target_page_shift;
    hdr->total_pages = visible_pages;
    hdr->total_regions = total_regions;
    hdr->region_granule = region_granule;

    if (visible_bitmap && hdr->visible_page_words) {
        memset(visible_bitmap, 0,
               (size_t)hdr->visible_page_words * sizeof(*visible_bitmap));
    }
    if (visible_region_bitmap && hdr->visible_region_words) {
        memset(visible_region_bitmap, 0,
               (size_t)hdr->visible_region_words *
               sizeof(*visible_region_bitmap));
    }
    if (owned_region_bitmap && hdr->owned_region_words) {
        memset(owned_region_bitmap, 0,
               (size_t)hdr->owned_region_words *
               sizeof(*owned_region_bitmap));
    }
}

void cxl_hybrid_control_reset_header_for_run(CXLHybridControlHeader *hdr,
                                             uint32_t generation)
{
    cxl_hybrid_control_reset_run_state(hdr, NULL, 0, NULL, 0, NULL, 0, 0, 0,
                                       generation);
}

bool cxl_hybrid_control_region_owned(const CXLHybridControlHeader *hdr,
                                     const unsigned long *owned_bitmap,
                                     uint64_t region_index,
                                     uint32_t generation)
{
    if (!hdr || !owned_bitmap ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_region_in_range(hdr, region_index)) {
        return false;
    }

    if (!test_bit(region_index, owned_bitmap)) {
        return false;
    }

    smp_rmb();
    return true;
}

void cxl_hybrid_control_mark_region_owned(const CXLHybridControlHeader *hdr,
                                          unsigned long *owned_bitmap,
                                          uint64_t region_index)
{
    if (!owned_bitmap ||
        !cxl_hybrid_control_region_in_range(hdr, region_index)) {
        return;
    }

    smp_mb_release();
    set_bit_atomic(region_index, owned_bitmap);
}
