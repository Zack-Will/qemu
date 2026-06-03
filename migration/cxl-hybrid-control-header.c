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

size_t cxl_hybrid_control_page_state_words(uint64_t pages)
{
    if (pages > UINT32_MAX) {
        return SIZE_MAX;
    }

    return pages;
}

size_t cxl_hybrid_control_page_state_bytes(uint64_t pages)
{
    return pages * sizeof(uint64_t);
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

bool cxl_hybrid_control_page_location(const CXLHybridControlHeader *hdr,
                                      const unsigned long *visible_bitmap,
                                      const uint64_t *page_state,
                                      uint64_t page_index,
                                      uint32_t generation,
                                      CXLHybridPageLocation *locationp)
{
    uint64_t word;
    CXLHybridPageLocation location;

    if (!locationp ||
        !cxl_hybrid_control_page_visible(hdr, visible_bitmap, page_index,
                                         generation) ||
        !page_state || page_index >= hdr->page_state_words) {
        return false;
    }

    word = qatomic_load_acquire(&page_state[page_index]);
    if (cxl_hybrid_page_state_kind(word) != CXL_HYBRID_PAGE_STATE_PUBLISHED ||
        cxl_hybrid_page_state_generation(word) != (generation & 0xffff)) {
        return false;
    }

    location = cxl_hybrid_page_state_location(word);
    if (location == CXL_HYBRID_PAGE_LOCATION_NONE) {
        return false;
    }

    *locationp = location;
    return true;
}

bool cxl_hybrid_control_page_requires_destination_install(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    bool received,
    CXLHybridPageLocation *locationp)
{
    CXLHybridPageLocation location = CXL_HYBRID_PAGE_LOCATION_NONE;

    if (locationp) {
        *locationp = CXL_HYBRID_PAGE_LOCATION_NONE;
    }
    if (received ||
        !cxl_hybrid_control_page_location(hdr, visible_bitmap, page_state,
                                          page_index, generation, &location)) {
        return false;
    }

    if (location != CXL_HYBRID_PAGE_LOCATION_CXL &&
        location != CXL_HYBRID_PAGE_LOCATION_DST_LOCAL) {
        return false;
    }

    if (locationp) {
        *locationp = location;
    }
    return true;
}

bool cxl_hybrid_control_page_requires_postcopy_discard(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation)
{
    uint64_t word;
    CXLHybridPageStateKind kind;

    (void)visible_bitmap;

    if (!hdr || !page_state ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_in_range(hdr, page_index) ||
        page_index >= hdr->page_state_words) {
        return false;
    }

    word = qatomic_load_acquire(&page_state[page_index]);
    if (cxl_hybrid_page_state_generation(word) != (generation & 0xffff)) {
        return false;
    }

    kind = cxl_hybrid_page_state_kind(word);
    if (kind == CXL_HYBRID_PAGE_STATE_PUBLISHED) {
        return cxl_hybrid_page_state_location(word) ==
               CXL_HYBRID_PAGE_LOCATION_CXL;
    }
    if (kind == CXL_HYBRID_PAGE_STATE_IN_FLIGHT) {
        return cxl_hybrid_page_state_owner(word) ==
               CXL_HYBRID_PAGE_OWNER_CXL;
    }

    return false;
}

bool cxl_hybrid_control_cxl_remap_span(const CXLHybridControlHeader *hdr,
                                       const uint64_t *page_state,
                                       uint64_t fault_page,
                                       uint32_t generation,
                                       uint32_t max_pages,
                                       CXLHybridRemapSpan *span)
{
    uint64_t total_pages;

    if (!hdr || !page_state || !span ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_in_range(hdr, fault_page) ||
        fault_page >= hdr->page_state_words) {
        return false;
    }

    total_pages = MIN(hdr->total_pages, (uint64_t)hdr->page_state_words);
    return cxl_hybrid_page_state_longest_cxl_span(
        page_state, total_pages, fault_page, generation, max_pages, span);
}

bool cxl_hybrid_fault_place_result_satisfied(int ret, bool received)
{
    return ret == 0 || (ret == -EEXIST && received);
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
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    CXLHybridPageLocation location)
{
    if (!cxl_hybrid_control_generation_matches(hdr, generation)) {
        return;
    }

    if (!visible_bitmap ||
        !cxl_hybrid_control_page_in_range(hdr, page_index)) {
        return;
    }

    if (page_state && page_index < hdr->page_state_words) {
        smp_mb_release();
        qatomic_set(&page_state[page_index],
                    cxl_hybrid_page_state_make_published(generation,
                                                         location, 0));
    } else {
        smp_mb_release();
    }
    set_bit_atomic(page_index, visible_bitmap);
}

bool cxl_hybrid_control_complete_cxl_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim)
{
    if (!visible_bitmap || !page_state || !claim ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_in_range(hdr, page_index) ||
        page_index >= hdr->page_state_words) {
        return false;
    }

    if (!cxl_hybrid_page_state_complete_cxl(&page_state[page_index], claim)) {
        return false;
    }

    /* The page-state CAS published CXL data; expose the visible bit after it. */
    smp_mb_release();
    set_bit_atomic(page_index, visible_bitmap);
    return true;
}

bool cxl_hybrid_control_complete_rdma_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim)
{
    if (!visible_bitmap || !page_state || !claim ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_in_range(hdr, page_index) ||
        page_index >= hdr->page_state_words) {
        return false;
    }

    if (!cxl_hybrid_page_state_complete_rdma(&page_state[page_index], claim)) {
        return false;
    }

    smp_mb_release();
    set_bit_atomic(page_index, visible_bitmap);
    return true;
}

bool cxl_hybrid_control_complete_zero_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim)
{
    if (!visible_bitmap || !page_state || !claim ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_in_range(hdr, page_index) ||
        page_index >= hdr->page_state_words) {
        return false;
    }

    if (!cxl_hybrid_page_state_complete_zero(&page_state[page_index],
                                             claim)) {
        return false;
    }

    smp_mb_release();
    set_bit_atomic(page_index, visible_bitmap);
    return true;
}

void cxl_hybrid_control_mark_pages_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t first_page,
    uint64_t nr_pages,
    uint32_t generation,
    CXLHybridPageLocation location)
{
    if (!visible_bitmap || !nr_pages ||
        nr_pages > LONG_MAX ||
        first_page > LONG_MAX ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        first_page >= hdr->total_pages ||
        nr_pages > hdr->total_pages - first_page) {
        return;
    }

    for (uint64_t page = first_page; page < first_page + nr_pages; page++) {
        cxl_hybrid_control_mark_page_visible_generation(
            hdr, visible_bitmap, page_state, page, generation, location);
    }
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

void cxl_hybrid_control_mark_page_dirty_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    uint32_t dirty_seq)
{
    if (!visible_bitmap || !page_state ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_page_in_range(hdr, page_index) ||
        page_index >= hdr->page_state_words) {
        return;
    }

    clear_bit_atomic(page_index, visible_bitmap);
    cxl_hybrid_page_state_mark_dirty(&page_state[page_index], generation,
                                     dirty_seq);
}

uint64_t cxl_hybrid_control_mark_dirty_pages_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    const unsigned long *dirty_bitmap,
    uint64_t dirty_first_page,
    uint64_t state_first_page,
    uint64_t nr_pages,
    uint32_t generation,
    uint32_t dirty_seq)
{
    uint64_t marked = 0;

    if (!visible_bitmap || !page_state || !dirty_bitmap || !nr_pages ||
        dirty_first_page > LONG_MAX ||
        state_first_page > LONG_MAX ||
        nr_pages > LONG_MAX ||
        dirty_first_page > (uint64_t)LONG_MAX - (nr_pages - 1) ||
        state_first_page > (uint64_t)LONG_MAX - (nr_pages - 1) ||
        dirty_first_page + nr_pages < dirty_first_page ||
        state_first_page + nr_pages < state_first_page ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        state_first_page >= hdr->total_pages ||
        nr_pages > hdr->total_pages - state_first_page ||
        state_first_page + nr_pages > hdr->page_state_words) {
        return 0;
    }

    for (uint64_t i = 0; i < nr_pages; i++) {
        if (!test_bit(dirty_first_page + i, dirty_bitmap)) {
            continue;
        }
        cxl_hybrid_control_mark_page_dirty_generation(
            hdr, visible_bitmap, page_state, state_first_page + i,
            generation, dirty_seq);
        marked++;
    }

    return marked;
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
    uint64_t *page_state,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation,
    CXLHybridPageLocation location)
{
    uint64_t region_index = UINT64_MAX;

    if (!visible_bitmap || !visible_region_bitmap ||
        !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !cxl_hybrid_control_region_span_index(hdr, first_page, nr_pages,
                                              &region_index)) {
        return false;
    }

    cxl_hybrid_control_mark_pages_visible_generation(
        hdr, visible_bitmap, page_state, first_page, nr_pages, generation,
        location);
    cxl_hybrid_control_mark_region_visible(hdr, visible_region_bitmap,
                                           region_index);
    return true;
}

void cxl_hybrid_control_reset_run_state(CXLHybridControlHeader *hdr,
                                        unsigned long *visible_bitmap,
                                        uint64_t visible_pages,
                                        uint64_t *page_state,
                                        uint64_t page_state_words,
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
    hdr->page_state_words = page_state_words;
    hdr->page_state_word_size = sizeof(uint64_t);
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
    if (page_state) {
        for (uint64_t page = 0; page < page_state_words; page++) {
            page_state[page] =
                cxl_hybrid_page_state_make_not_sent(generation);
        }
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
    cxl_hybrid_control_reset_run_state(hdr, NULL, 0, NULL, 0, NULL, 0, NULL,
                                       0, 0, 0, generation);
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
