/*
 * Unit tests for CXL hybrid fault region geometry helpers.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "system/ramblock.h"
#include "migration/cxl.h"
#include "migration/cxl-rdma.h"

#define TEST_TARGET_PAGE_SIZE (4 * KiB)

bool cxl_hybrid_rdma_sidecar_get_backing(void **basep, size_t *sizep)
{
    if (basep) {
        *basep = NULL;
    }
    if (sizep) {
        *sizep = 0;
    }
    return false;
}

static void assert_region_compute_error(uint64_t block_global_base,
                                        uint64_t block_used_len,
                                        uint64_t block_cxl_pages_offset,
                                        uint64_t fault_block_offset,
                                        uint64_t granule,
                                        uint64_t target_page_size,
                                        int expected_ret,
                                        const char *expected_error)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    Error *err = NULL;

    g_assert_cmpint(cxl_hybrid_fault_region_compute(block_global_base,
                                                    block_used_len,
                                                    block_cxl_pages_offset,
                                                    fault_block_offset,
                                                    granule,
                                                    target_page_size,
                                                    &g, &err), ==,
                    expected_ret);
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==, expected_error);
    error_free(err);
}

static void test_region_geometry_first_2mib(void)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    Error *err = NULL;

    g_assert_cmpint(cxl_hybrid_fault_region_compute(0, 4 * MiB, 64 * MiB,
                                                    0x1234, 2 * MiB,
                                                    TEST_TARGET_PAGE_SIZE,
                                                    &g, &err), ==, 0);
    g_assert_null(err);
    g_assert_cmphex(g.block_offset, ==, 0);
    g_assert_cmphex(g.region_len, ==, 2 * MiB);
    g_assert_cmphex(g.cxl_offset, ==, 64 * MiB);
    g_assert_cmpuint(g.first_page_index, ==, 0);
    g_assert_cmpuint(g.nr_pages, ==, (2 * MiB) / TEST_TARGET_PAGE_SIZE);
}

static void test_region_geometry_second_2mib_with_global_base(void)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    Error *err = NULL;

    g_assert_cmpint(cxl_hybrid_fault_region_compute(8 * MiB, 6 * MiB,
                                                    128 * MiB, 3 * MiB,
                                                    2 * MiB,
                                                    TEST_TARGET_PAGE_SIZE,
                                                    &g, &err), ==, 0);
    g_assert_null(err);
    g_assert_cmphex(g.global_offset, ==, 10 * MiB);
    g_assert_cmphex(g.block_offset, ==, 2 * MiB);
    g_assert_cmphex(g.cxl_offset, ==, 130 * MiB);
    g_assert_cmpuint(g.first_page_index, ==,
                     (10 * MiB) / TEST_TARGET_PAGE_SIZE);
}

static void test_region_geometry_rejects_region_crossing_ramblock_start(void)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    Error *err = NULL;

    g_assert_cmpint(cxl_hybrid_fault_region_compute(1 * MiB, 4 * MiB, 0,
                                                    0, 2 * MiB,
                                                    TEST_TARGET_PAGE_SIZE,
                                                    &g, &err), ==, -EINVAL);
    g_assert_nonnull(err);
    error_free(err);
}

static void test_region_geometry_rejects_partial_tail(void)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    Error *err = NULL;

    g_assert_cmpint(cxl_hybrid_fault_region_compute(0, 3 * MiB, 0,
                                                    2 * MiB, 2 * MiB,
                                                    TEST_TARGET_PAGE_SIZE,
                                                    &g, &err), ==, -EINVAL);
    g_assert_nonnull(err);
    error_free(err);
}

static void test_region_geometry_can_cover_whole_aligned_region(void)
{
    g_assert_true(cxl_hybrid_fault_region_can_cover(0, 4 * MiB, 64 * MiB,
                                                    0x1234, 2 * MiB,
                                                    TEST_TARGET_PAGE_SIZE));
}

static void test_region_geometry_cannot_cover_small_ramblock(void)
{
    g_assert_false(cxl_hybrid_fault_region_can_cover(0, 1 * MiB, 64 * MiB,
                                                     0, 2 * MiB,
                                                     TEST_TARGET_PAGE_SIZE));
}

static void test_region_geometry_rejects_unaligned_cxl_offset(void)
{
    CXLHybridFaultRegionGeometry g = { 0 };
    Error *err = NULL;

    g_assert_cmpint(cxl_hybrid_fault_region_compute(0, 4 * MiB, 4 * KiB,
                                                    0, 2 * MiB,
                                                    TEST_TARGET_PAGE_SIZE,
                                                    &g, &err), ==, -EINVAL);
    g_assert_nonnull(err);
    error_free(err);
}

static void test_region_geometry_rejects_invalid_granule_and_page(void)
{
    static const struct {
        const char *name;
        uint64_t granule;
        uint64_t target_page_size;
    } cases[] = {
        { "zero granule", 0, TEST_TARGET_PAGE_SIZE },
        { "non-power-of-two granule", 3 * MiB, TEST_TARGET_PAGE_SIZE },
        { "granule smaller than page", 2 * KiB, TEST_TARGET_PAGE_SIZE },
        { "non-power-of-two page", 2 * MiB, 6 * KiB },
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_test_message("case: %s", cases[i].name);
        assert_region_compute_error(0, 4 * MiB, 0, 0,
                                    cases[i].granule,
                                    cases[i].target_page_size,
                                    -EINVAL,
                                    "invalid CXL hybrid fault region granule");
    }
}

static void test_region_geometry_rejects_out_of_range_fault_offset(void)
{
    assert_region_compute_error(0, 4 * MiB, 0, 4 * MiB, 2 * MiB,
                                TEST_TARGET_PAGE_SIZE, -EINVAL,
                                "CXL hybrid fault offset is outside the RAMBlock");
}

static void test_region_geometry_rejects_cxl_offset_wrap(void)
{
    assert_region_compute_error(0, 4 * MiB,
                                QEMU_ALIGN_DOWN(UINT64_MAX, 2 * MiB),
                                2 * MiB, 2 * MiB,
                                TEST_TARGET_PAGE_SIZE, -EOVERFLOW,
                                "CXL hybrid fault region CXL offset overflows");
}

static void test_region_geometry_rejects_page_count_overflow(void)
{
    assert_region_compute_error(0, 1ULL << 44, 0, 0, 1ULL << 44, 1,
                                -EOVERFLOW,
                                "CXL hybrid fault region page count overflows");
}

static void test_region_geometry_rejects_misaligned_block_offset(void)
{
    assert_region_compute_error(1 * MiB, 6 * MiB, 0, 1 * MiB, 2 * MiB,
                                TEST_TARGET_PAGE_SIZE, -EINVAL,
                                "CXL hybrid fault region is not DAX-granule aligned");
}

static void test_dst_remap_page_index_accepts_4k_aligned_span_offsets(void)
{
    size_t page_idx = UINT64_MAX;

    g_assert_true(cxl_hybrid_dst_remap_offset_page_index(
        16384, 0, 64 * MiB, 0x220000, TEST_TARGET_PAGE_SIZE, &page_idx));
    g_assert_cmpuint(page_idx, ==, 0x220000 / TEST_TARGET_PAGE_SIZE);

    g_assert_true(cxl_hybrid_dst_remap_offset_page_index(
        16384, 0, 64 * MiB, 0x2f7000, TEST_TARGET_PAGE_SIZE, &page_idx));
    g_assert_cmpuint(page_idx, ==, 0x2f7000 / TEST_TARGET_PAGE_SIZE);

    g_assert_false(cxl_hybrid_dst_remap_offset_page_index(
        16384, 0, 64 * MiB, 0x220001, TEST_TARGET_PAGE_SIZE, &page_idx));
    g_assert_false(cxl_hybrid_dst_remap_offset_page_index(
        16384, 0, 64 * MiB, 64 * MiB, TEST_TARGET_PAGE_SIZE, &page_idx));
}

static void test_region_mode_disables_fault_burst(void)
{
    g_assert_false(cxl_hybrid_fault_resolve_mode_emits_burst(
        CXL_HYBRID_FAULT_RESOLVE_MODE_REGION_REMAP));
    g_assert_false(cxl_hybrid_fault_resolve_mode_emits_burst(
        CXL_HYBRID_FAULT_RESOLVE_MODE_REGION_REMAP_FALLBACK_COPY));
    g_assert_true(cxl_hybrid_fault_resolve_mode_emits_burst(
        CXL_HYBRID_FAULT_RESOLVE_MODE_COPY));
}

static void test_region_fallback_copy_poisons_region(void)
{
    CXLHybridDstRegionState state = { 0 };

    cxl_hybrid_dst_region_state_init_for_test(&state, 8);
    g_assert_false(cxl_hybrid_dst_region_copy_owned(&state, 2));
    cxl_hybrid_dst_region_mark_copy_owned(&state, 2);
    g_assert_true(cxl_hybrid_dst_region_copy_owned(&state, 2));
    g_assert_false(cxl_hybrid_dst_region_can_remap(&state, 2));
    cxl_hybrid_dst_region_state_destroy_for_test(&state);
}

static void test_region_remap_reservation_blocks_copy_poison(void)
{
    CXLHybridDstRegionState state = { 0 };

    cxl_hybrid_dst_region_state_init_for_test(&state, 8);
    g_assert_true(cxl_hybrid_dst_region_try_begin_remap(&state, 2));
    g_assert_false(cxl_hybrid_dst_region_can_remap(&state, 2));
    g_assert_false(cxl_hybrid_dst_region_try_mark_copy_owned(&state, 2));
    cxl_hybrid_dst_region_finish_remap(&state, 2, true);
    g_assert_false(cxl_hybrid_dst_region_copy_owned(&state, 2));
    g_assert_false(cxl_hybrid_dst_region_can_remap(&state, 2));
    cxl_hybrid_dst_region_state_destroy_for_test(&state);
}

static void test_region_copy_poison_blocks_remap_reservation(void)
{
    CXLHybridDstRegionState state = { 0 };

    cxl_hybrid_dst_region_state_init_for_test(&state, 8);
    g_assert_true(cxl_hybrid_dst_region_try_mark_copy_owned(&state, 2));
    g_assert_false(cxl_hybrid_dst_region_try_begin_remap(&state, 2));
    cxl_hybrid_dst_region_state_destroy_for_test(&state);
}

static void test_mapped_ram_pages_alignment_includes_region_granule(void)
{
    g_assert_cmpuint(cxl_hybrid_mapped_ram_pages_offset_alignment(
                         1 * MiB, 4 * KiB, 2 * MiB, true), ==, 2 * MiB);
    g_assert_cmpuint(cxl_hybrid_mapped_ram_pages_offset_alignment(
                         1 * MiB, 4 * KiB, 2 * MiB, false), ==, 1 * MiB);
}

static void test_source_remap_granule_is_independent_from_fault_region(void)
{
    uint64_t fault_granule;
    uint64_t source_granule;
    uint64_t pages_align;

    fault_granule = cxl_hybrid_choose_fault_region_granule(
        2 * MiB, 0, 64 * MiB);
    source_granule = cxl_hybrid_choose_source_remap_granule(
        TEST_TARGET_PAGE_SIZE, 256 * KiB, 64 * MiB);
    pages_align = cxl_hybrid_mapped_ram_pages_offset_alignment(
        1 * MiB, 2 * MiB, fault_granule, true);

    g_assert_cmpuint(fault_granule, ==, 2 * MiB);
    g_assert_cmpuint(source_granule, ==, 256 * KiB);
    g_assert_cmpuint(pages_align, ==, 2 * MiB);
}

static void test_source_remap_default_is_64k_with_4k_dax_alignment(void)
{
    uint64_t source_granule;

    source_granule = cxl_hybrid_choose_source_remap_granule(
        TEST_TARGET_PAGE_SIZE, 0, 64 * MiB);

    g_assert_cmpuint(source_granule, ==, 64 * KiB);
}

static void test_fault_region_default_is_2mib_with_4k_dax_alignment(void)
{
    uint64_t fault_granule;

    fault_granule = cxl_hybrid_choose_fault_region_granule(
        4 * KiB, 0, 64 * MiB);

    g_assert_cmpuint(fault_granule, ==, 2 * MiB);
}

static void test_mapped_ram_layout_returns_page_data_spans(void)
{
    uint64_t offset = 0;
    uint64_t pages_offset;
    uint64_t pages_len;

    g_assert_true(cxl_hybrid_mapped_ram_layout_next(
                      &offset, 64 * MiB, 2 * MiB, TEST_TARGET_PAGE_SIZE,
                      &pages_offset, &pages_len));
    g_assert_cmpuint(pages_offset, ==, 2 * MiB);
    g_assert_cmpuint(pages_len, ==, 64 * MiB);
    g_assert_cmpuint(offset, ==, 66 * MiB);

    g_assert_true(cxl_hybrid_mapped_ram_layout_next(
                      &offset, 16 * MiB, 2 * MiB, TEST_TARGET_PAGE_SIZE,
                      &pages_offset, &pages_len));
    g_assert_cmpuint(pages_offset, ==, 68 * MiB);
    g_assert_cmpuint(pages_len, ==, 16 * MiB);
    g_assert_cmpuint(offset, ==, 84 * MiB);
}

static void test_source_remap_region_requires_staged_clean_pages(void)
{
    unsigned long migrated[BITS_TO_LONGS(128)] = { 0 };
    unsigned long dirty[BITS_TO_LONGS(128)] = { 0 };

    bitmap_set(migrated, 32, 16);
    g_assert_true(cxl_hybrid_source_remap_region_clean(migrated, 32,
                                                       dirty, 32, 16));

    bitmap_clear(migrated, 40, 1);
    g_assert_false(cxl_hybrid_source_remap_region_clean(migrated, 32,
                                                        dirty, 32, 16));

    bitmap_set(migrated, 40, 1);
    bitmap_set(dirty, 47, 1);
    g_assert_false(cxl_hybrid_source_remap_region_clean(migrated, 32,
                                                        dirty, 32, 16));

    bitmap_clear(dirty, 47, 1);
    bitmap_set(dirty, 31, 1);
    bitmap_set(dirty, 48, 1);
    g_assert_true(cxl_hybrid_source_remap_region_clean(migrated, 32,
                                                       dirty, 32, 16));

    g_assert_false(cxl_hybrid_source_remap_region_clean(NULL, 32, dirty,
                                                        32, 16));
    g_assert_false(cxl_hybrid_source_remap_region_clean(migrated, 32, NULL,
                                                        32, 16));
    g_assert_false(cxl_hybrid_source_remap_region_clean(migrated, 32,
                                                        dirty, 32, 0));
}

static void test_clean_remap_budget_zero_one_region(void)
{
    g_assert_true(cxl_hybrid_clean_remap_budget_allows(0, 0, 256 * KiB));
    g_assert_false(cxl_hybrid_clean_remap_budget_allows(0, 1, 256 * KiB));
    g_assert_false(cxl_hybrid_clean_remap_budget_allows(0, 256 * KiB,
                                                        256 * KiB));
}

static void test_clean_remap_budget_multiple_regions(void)
{
    g_assert_true(cxl_hybrid_clean_remap_budget_allows(1 * MiB, 0,
                                                       256 * KiB));
    g_assert_true(cxl_hybrid_clean_remap_budget_allows(128 * KiB, 0,
                                                       256 * KiB));
    g_assert_true(cxl_hybrid_clean_remap_budget_allows(1 * MiB, 768 * KiB,
                                                       256 * KiB));
    g_assert_false(cxl_hybrid_clean_remap_budget_allows(1 * MiB, 768 * KiB,
                                                        512 * KiB));
    g_assert_false(cxl_hybrid_clean_remap_budget_allows(UINT64_MAX,
                                                        UINT64_MAX,
                                                        1));
}

static void test_clean_remap_throttle_requires_nonzero_delay_and_copy(void)
{
    g_assert_false(cxl_hybrid_clean_remap_should_throttle(0, false));
    g_assert_false(cxl_hybrid_clean_remap_should_throttle(0, true));
    g_assert_false(cxl_hybrid_clean_remap_should_throttle(200, false));
    g_assert_true(cxl_hybrid_clean_remap_should_throttle(200, true));
}

static void test_clean_remap_candidate_requires_second_clean_scan(void)
{
    g_assert_true(cxl_hybrid_clean_remap_region_is_candidate(true, false,
                                                             false, false));
    g_assert_false(cxl_hybrid_clean_remap_region_is_candidate(false, false,
                                                              false, false));
    g_assert_false(cxl_hybrid_clean_remap_region_is_candidate(true, true,
                                                              false, false));
    g_assert_false(cxl_hybrid_clean_remap_region_is_candidate(true, false,
                                                              true, false));
    g_assert_false(cxl_hybrid_clean_remap_region_is_candidate(true, false,
                                                              false, true));
}

static void test_clean_remap_debug_mode_helpers(void)
{
    g_assert_false(cxl_hybrid_clean_remap_debug_scan_only(NULL));
    g_assert_false(cxl_hybrid_clean_remap_debug_copy_only(NULL));
    g_assert_false(cxl_hybrid_clean_remap_debug_read_only(NULL));
    g_assert_false(cxl_hybrid_clean_remap_debug_write_only(NULL));
    g_assert_false(cxl_hybrid_clean_remap_debug_scan_only(""));
    g_assert_false(cxl_hybrid_clean_remap_debug_copy_only(""));
    g_assert_false(cxl_hybrid_clean_remap_debug_read_only(""));
    g_assert_false(cxl_hybrid_clean_remap_debug_write_only(""));
    g_assert_false(cxl_hybrid_clean_remap_debug_scan_only("normal"));
    g_assert_false(cxl_hybrid_clean_remap_debug_copy_only("normal"));
    g_assert_false(cxl_hybrid_clean_remap_debug_read_only("normal"));
    g_assert_false(cxl_hybrid_clean_remap_debug_write_only("normal"));

    g_assert_true(cxl_hybrid_clean_remap_debug_scan_only("scan-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_copy_only("scan-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_read_only("scan-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_write_only("scan-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_scan_only("copy-only"));
    g_assert_true(cxl_hybrid_clean_remap_debug_copy_only("copy-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_read_only("copy-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_write_only("copy-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_scan_only("read-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_copy_only("read-only"));
    g_assert_true(cxl_hybrid_clean_remap_debug_read_only("read-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_write_only("read-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_scan_only("write-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_copy_only("write-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_read_only("write-only"));
    g_assert_true(cxl_hybrid_clean_remap_debug_write_only("write-only"));
    g_assert_false(cxl_hybrid_clean_remap_debug_scan_only("defer-remap"));
    g_assert_false(cxl_hybrid_clean_remap_debug_copy_only("defer-remap"));
    g_assert_false(cxl_hybrid_clean_remap_debug_read_only("defer-remap"));
    g_assert_false(cxl_hybrid_clean_remap_debug_write_only("defer-remap"));
    g_assert_true(cxl_hybrid_clean_remap_debug_defer_remap("defer-remap"));
}

static void test_clean_remap_prefault_mode_helpers(void)
{
    g_assert_false(cxl_hybrid_clean_remap_prefault_enabled(
                       CXL_CLEAN_REMAP_PREFAULT_MODE_OFF));
    g_assert_true(cxl_hybrid_clean_remap_prefault_enabled(
                      CXL_CLEAN_REMAP_PREFAULT_MODE_MADVISE));
    g_assert_true(cxl_hybrid_clean_remap_prefault_enabled(
                      CXL_CLEAN_REMAP_PREFAULT_MODE_TOUCH));
    g_assert_false(cxl_hybrid_clean_remap_prefault_valid(
                       CXL_CLEAN_REMAP_PREFAULT_MODE__MAX));
}

static void test_warm_page_eligible_excludes_already_visible_pages(void)
{
    unsigned long migrated[BITS_TO_LONGS(128)] = { 0 };
    unsigned long warm_sent[BITS_TO_LONGS(128)] = { 0 };
    unsigned long dst_sent[BITS_TO_LONGS(128)] = { 0 };
    unsigned long visible[BITS_TO_LONGS(128)] = { 0 };

    bitmap_set(migrated, 42, 1);
    g_assert_true(cxl_hybrid_warm_page_eligible_for_push(migrated, warm_sent,
                                                         dst_sent, visible,
                                                         42));

    bitmap_set(dst_sent, 42, 1);
    g_assert_false(cxl_hybrid_warm_page_eligible_for_push(migrated, warm_sent,
                                                          dst_sent, visible,
                                                          42));

    bitmap_clear(dst_sent, 42, 1);
    bitmap_set(visible, 42, 1);
    g_assert_false(cxl_hybrid_warm_page_eligible_for_push(migrated, warm_sent,
                                                          dst_sent, visible,
                                                          42));

    bitmap_clear(visible, 42, 1);
    bitmap_set(warm_sent, 42, 1);
    g_assert_false(cxl_hybrid_warm_page_eligible_for_push(migrated, warm_sent,
                                                          dst_sent, visible,
                                                          42));

    g_assert_false(cxl_hybrid_warm_page_eligible_for_push(NULL, warm_sent,
                                                          dst_sent, visible,
                                                          42));
}

static void test_dirty_requeue_clears_stale_sent_state(void)
{
    unsigned long migrated[BITS_TO_LONGS(128)] = { 0 };
    unsigned long warm_sent[BITS_TO_LONGS(128)] = { 0 };
    unsigned long dst_sent[BITS_TO_LONGS(128)] = { 0 };
    unsigned long visible[BITS_TO_LONGS(128)] = { 0 };
    unsigned long remaining[BITS_TO_LONGS(128)] = { 0 };
    unsigned long warm_dirty[BITS_TO_LONGS(128)] = { 0 };

    bitmap_set(migrated, 42, 1);
    bitmap_set(warm_sent, 42, 1);
    bitmap_set(dst_sent, 42, 1);
    bitmap_set(visible, 42, 1);

    cxl_hybrid_dirty_requeue_page_for_push(warm_sent, dst_sent, visible,
                                           remaining, warm_dirty, 42, true);

    g_assert_false(test_bit(42, warm_sent));
    g_assert_false(test_bit(42, dst_sent));
    g_assert_false(test_bit(42, visible));
    g_assert_true(test_bit(42, remaining));
    g_assert_true(test_bit(42, warm_dirty));
    g_assert_true(cxl_hybrid_warm_page_eligible_for_push(migrated, warm_sent,
                                                         dst_sent, visible,
                                                         42));
}

static void test_rdma_descriptor_claims_only_requested_pages(void)
{
    uint64_t page_state[8];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 2;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }
    page_state[3] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);

    g_assert_true(cxl_hybrid_rdma_descriptor_claim_pages_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 8, generation));
    g_assert_cmpuint(desc.first_page, ==, 0);
    g_assert_cmpuint(desc.nr_pages, ==, 8);
    g_assert_cmpuint(desc.claimed_pages, ==, 7);
    g_assert_false(cxl_hybrid_rdma_descriptor_page_claimed(&desc, 3));
    g_assert_true(cxl_hybrid_rdma_descriptor_page_claimed(&desc, 4));

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_claim_contiguous_stops_before_clean_page(void)
{
    uint64_t page_state[8];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 2;
    uint32_t claimed = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }
    page_state[3] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);

    g_assert_true(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 8, generation,
        &claimed));
    g_assert_cmpuint(claimed, ==, 3);
    g_assert_cmpuint(desc.claimed_pages, ==, 3);
    g_assert_true(cxl_hybrid_rdma_descriptor_page_claimed(&desc, 0));
    g_assert_true(cxl_hybrid_rdma_descriptor_page_claimed(&desc, 1));
    g_assert_true(cxl_hybrid_rdma_descriptor_page_claimed(&desc, 2));

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_claim_contiguous_rejects_sparse_prefix(void)
{
    uint64_t page_state[4];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 2;
    uint32_t claimed = 1;

    page_state[0] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    for (size_t i = 1; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }

    g_assert_false(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation,
        &claimed));
    g_assert_cmpuint(claimed, ==, 0);
    g_assert_null(desc.claimed_bmap);
    g_assert_null(desc.completed_bmap);
    g_assert_null(desc.stale_bmap);
    g_assert_null(desc.claims);

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_claim_contiguous_reuse_drops_old_claims(void)
{
    uint64_t page_state[8];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 2;
    uint32_t claimed = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }

    g_assert_true(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation,
        &claimed));
    g_assert_cmpuint(claimed, ==, 4);
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[0]), ==,
                     CXL_HYBRID_PAGE_STATE_IN_FLIGHT);

    g_assert_true(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 4, 2, generation,
        &claimed));
    g_assert_cmpuint(claimed, ==, 2);
    g_assert_cmpuint(desc.first_page, ==, 4);
    g_assert_cmpuint(desc.nr_pages, ==, 2);
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[0]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[1]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[4]), ==,
                     CXL_HYBRID_PAGE_STATE_IN_FLIGHT);

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_claim_contiguous_invalid_reclaims_old_claims(void)
{
    uint64_t page_state[8];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 2;
    uint32_t claimed = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }

    g_assert_true(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation,
        &claimed));
    g_assert_cmpuint(claimed, ==, 4);
    g_assert_nonnull(desc.claimed_bmap);
    g_assert_nonnull(desc.claims);

    claimed = 99;
    g_assert_false(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 0, generation,
        &claimed));
    g_assert_cmpuint(claimed, ==, 0);
    g_assert_null(desc.claimed_bmap);
    g_assert_null(desc.completed_bmap);
    g_assert_null(desc.stale_bmap);
    g_assert_null(desc.claims);
    for (size_t i = 0; i < 4; i++) {
        g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[i]), ==,
                         CXL_HYBRID_PAGE_STATE_DIRTY);
    }

    g_assert_true(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 2, 2, generation,
        &claimed));
    g_assert_cmpuint(claimed, ==, 2);

    claimed = 99;
    g_assert_false(cxl_hybrid_rdma_descriptor_claim_contiguous_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), G_N_ELEMENTS(page_state),
        2, generation, &claimed));
    g_assert_cmpuint(claimed, ==, 0);
    g_assert_null(desc.claimed_bmap);
    g_assert_null(desc.completed_bmap);
    g_assert_null(desc.stale_bmap);
    g_assert_null(desc.claims);
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[2]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[3]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_completion_ignores_stale_page(void)
{
    uint64_t page_state[4];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 6;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }
    g_assert_true(cxl_hybrid_rdma_descriptor_claim_pages_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation));
    cxl_hybrid_page_state_mark_dirty(&page_state[1], generation, 99);

    cxl_hybrid_rdma_descriptor_complete_pages_for_test(&desc, page_state,
                                                       G_N_ELEMENTS(page_state));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[0], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[1]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[2], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
    g_assert_cmpuint(desc.completed_pages, ==, 3);
    g_assert_cmpuint(desc.stale_pages, ==, 1);

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_no_claims_clears_descriptor(void)
{
    uint64_t page_state[4];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 4;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }
    g_assert_true(cxl_hybrid_rdma_descriptor_claim_pages_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation));
    g_assert_nonnull(desc.claimed_bmap);
    g_assert_nonnull(desc.claims);

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_published(
            generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    }
    g_assert_false(cxl_hybrid_rdma_descriptor_claim_pages_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation));
    g_assert_null(desc.claimed_bmap);
    g_assert_null(desc.claims);
    g_assert_cmpuint(desc.claimed_pages, ==, 0);

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_completion_consumes_claims_once(void)
{
    uint64_t page_state[4];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 7;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }
    g_assert_true(cxl_hybrid_rdma_descriptor_claim_pages_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation));
    cxl_hybrid_page_state_mark_dirty(&page_state[1], generation, 99);

    cxl_hybrid_rdma_descriptor_complete_pages_for_test(&desc, page_state,
                                                       G_N_ELEMENTS(page_state));
    g_assert_cmpuint(desc.completed_pages, ==, 3);
    g_assert_cmpuint(desc.stale_pages, ==, 1);

    cxl_hybrid_rdma_descriptor_complete_pages_for_test(&desc, page_state,
                                                       G_N_ELEMENTS(page_state));
    g_assert_cmpuint(desc.completed_pages, ==, 3);
    g_assert_cmpuint(desc.stale_pages, ==, 1);

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_posted_page_blocks_cxl_publish_until_completion(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(8, 1);
    CXLHybridPageClaim rdma_claim = { 0 };
    CXLHybridPageClaim cxl_claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&slot, 8,
                                                       &rdma_claim));
    g_assert_false(cxl_hybrid_page_state_claim_for_cxl(&slot, 8,
                                                       &cxl_claim));
    g_assert_true(cxl_hybrid_page_state_complete_rdma(&slot, &rdma_claim));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 8, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
}

static void test_rdma_admission_rejects_full_dynamic_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_admission_state_init(&state, 32, 2 * MiB);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 8);
    g_assert_false(snap.accept_rdma);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 8);
    g_assert_cmpuint(snap.sq_capacity_regions, ==, 32);
    g_assert_cmpuint(snap.inflight_len, ==, 8);

    g_assert_false(cxl_rdma_sidecar_admission_try_reserve(
        &state, true, true, false, false, false, 0, 8, &reservation, &snap));
    g_assert_false(reservation.valid);
    g_assert_cmpuint(state.overflow_cxl_regions, ==, 1);
}

static void test_rdma_claim_identity_distinguishes_postcopy_from_region(void)
{
    CXLHybridRDMABulkClaim precopy = {
        .kind = CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION,
        .region_index = 7,
        .claim_id = 101,
    };
    CXLHybridRDMABulkClaim postcopy = {
        .kind = CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN,
        .region_index = UINT64_MAX,
        .claim_id = 102,
    };
    CXLHybridRDMABulkClaim postcopy_with_region_id = {
        .kind = CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN,
        .region_index = 7,
        .claim_id = 103,
    };

    g_assert_true(cxl_hybrid_rdma_claim_has_region_ownership(&precopy));
    g_assert_false(cxl_hybrid_rdma_claim_has_region_ownership(&postcopy));
    g_assert_false(cxl_hybrid_rdma_claim_has_region_ownership(
        &postcopy_with_region_id));
    g_assert_cmpuint(cxl_hybrid_rdma_claim_wrid(&precopy), ==, 101);
    g_assert_cmpuint(cxl_hybrid_rdma_claim_wrid(&postcopy), ==, 102);
    g_assert_cmpuint(cxl_hybrid_rdma_claim_wrid(&postcopy_with_region_id),
                     ==, 103);
}

static void test_rdma_claim_identity_rejects_zero_wrid(void)
{
    CXLHybridRDMABulkClaim claim = {
        .kind = CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN,
        .region_index = UINT64_MAX,
        .claim_id = 0,
    };

    g_assert_cmpuint(cxl_hybrid_rdma_claim_wrid(&claim), ==, UINT64_MAX);
}

static void test_rdma_postcopy_span_caps_at_2mib(void)
{
    uint32_t pages = cxl_hybrid_rdma_postcopy_span_max_pages_for_test(
        4096, 8 * MiB, 0);

    g_assert_cmpuint(pages, ==, 512);
}

static void test_rdma_postcopy_span_respects_block_tail(void)
{
    uint32_t pages = cxl_hybrid_rdma_postcopy_span_max_pages_for_test(
        4096, 2 * MiB + 16 * KiB, 2 * MiB - 8 * KiB);

    g_assert_cmpuint(pages, ==, 6);
}

static void test_rdma_admission_reserve_and_cancel_updates_outstanding(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_admission_state_init(&state, 4, 2 * MiB);

    g_assert_true(cxl_rdma_sidecar_admission_try_reserve(
        &state, true, true, false, false, false, 0, 0, &reservation, &snap));
    g_assert_true(reservation.valid);
    g_assert_cmpuint(state.reserved_regions, ==, 1);
    g_assert_cmpuint(state.accepted_regions, ==, 1);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);
    g_assert_true(snap.accept_rdma);
    g_assert_cmpuint(snap.outstanding_regions, ==, 1);

    cxl_rdma_sidecar_admission_cancel_reserve(&state, &reservation);
    g_assert_false(reservation.valid);
    g_assert_cmpuint(state.reserved_regions, ==, 0);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);
    g_assert_true(snap.accept_rdma);
}

static void test_rdma_admission_outstanding_wrap_rejects_rdma(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);
    cxl_rdma_sidecar_admission_note_completion(&state, 8 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 8 * MiB, 1000000);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, UINT32_MAX, 2);

    g_assert_cmpuint(snap.dynamic_window_regions, >, 1);
    g_assert_cmpuint(snap.outstanding_regions, ==, UINT32_MAX);
    g_assert_false(snap.accept_rdma);
}

static void test_rdma_admission_completion_grows_window_and_bdp(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);
    g_assert_cmpuint(snap.bdp_estimate_regions, ==, 0);

    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(state.dynamic_window_regions, ==, 8);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 8);
    g_assert_cmpuint(snap.bdp_estimate_regions, >=, 1);
    g_assert_cmpfloat(snap.goodput_ewma_bytes_per_ns, >, 0.0);
    g_assert_cmpuint(snap.completion_latency_ewma_ns, ==, 1000000);
}

static void test_rdma_admission_starts_with_bounded_probe_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 64, 2 * MiB);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(state.dynamic_window_regions, ==, 8);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 8);
    g_assert_cmpuint(snap.sq_capacity_regions, ==, 64);
    g_assert_true(snap.accept_rdma);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 8);
    g_assert_false(snap.accept_rdma);
}

static void test_rdma_admission_can_grow_past_initial_probe_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 64, 2 * MiB);

    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(state.dynamic_window_regions, ==, 10);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 10);
    g_assert_cmpuint(snap.sq_capacity_regions, ==, 64);
}

static void test_rdma_admission_bdp_caps_effective_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);

    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 3);

    g_assert_cmpuint(snap.bdp_estimate_regions, ==, 1);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 8);
    g_assert_true(snap.accept_rdma);

    cxl_rdma_sidecar_admission_note_completion(&state, MiB / 2, 4000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 2);

    g_assert_cmpuint(snap.bdp_estimate_regions, ==, 2);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 2);
    g_assert_false(snap.accept_rdma);
    g_assert_false(cxl_rdma_sidecar_admission_try_reserve(
        &state, true, true, false, false, false, 0, 2, &reservation, &snap));
    g_assert_false(reservation.valid);
    g_assert_cmpuint(state.overflow_cxl_regions, ==, 1);
}

static void test_rdma_admission_bdp_clamps_without_u32_wrap(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 1);

    cxl_rdma_sidecar_admission_note_completion(
        &state, (uint64_t)UINT32_MAX + 4, 1);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(snap.bdp_estimate_regions, ==, 8);
}

static void test_rdma_admission_bdp_ceil_division_does_not_wrap(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 3);
    state.goodput_ewma_bytes_per_ns = (double)UINT64_MAX;
    state.completion_latency_ewma_ns = 1;

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(snap.bdp_estimate_regions, ==, 8);
}

static void test_rdma_admission_goodput_regression_halves_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);

    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 4 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 4 * MiB, 1000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);
    g_assert_cmpuint(state.dynamic_window_regions, ==, 8);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 8);

    cxl_rdma_sidecar_admission_note_completion(&state, 1 * MiB, 4000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(state.dynamic_window_regions, ==, 4);
    g_assert_cmpuint(snap.dynamic_window_regions, <=,
                     snap.bdp_estimate_regions);
    g_assert_cmpuint(state.goodput_drop_events, ==, 1);
}

static void test_rdma_admission_small_jitter_does_not_drop_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);

    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1050000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 3);

    g_assert_cmpuint(state.goodput_drop_events, ==, 0);
    g_assert_cmpuint(state.dynamic_window_regions, ==, 8);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 8);
    g_assert_true(snap.accept_rdma);
}

static void test_rdma_admission_epoch_goodput_keeps_probe_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 32, 2 * MiB);

    cxl_rdma_sidecar_admission_note_completion(&state, 64 * MiB, 6000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(state.goodput_drop_events, ==, 0);
    g_assert_cmpuint(state.dynamic_window_regions, ==, 9);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 9);
    g_assert_cmpuint(snap.bdp_estimate_regions, ==, 32);
}

static void test_rdma_sidecar_active_epoch_accounts_wall_time(void)
{
    CXLHybridRDMASidecarBulkStats stats = { 0 };

    cxl_rdma_sidecar_bulk_stats_note_active_epoch(&stats, 4 * MiB, 1000000);
    cxl_rdma_sidecar_bulk_stats_note_active_epoch(&stats, 8 * MiB, 2000000);
    cxl_rdma_sidecar_bulk_stats_note_active_epoch(&stats, 0, 3000000);
    cxl_rdma_sidecar_bulk_stats_note_active_epoch(&stats, 4 * MiB, 0);
    cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch(
        &stats, 12 * MiB, 1500000);
    cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch(
        &stats, 0, 1500000);
    cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch(
        &stats, 12 * MiB, 0);
    cxl_rdma_sidecar_bulk_stats_note_publish_time(&stats, 700000);
    cxl_rdma_sidecar_bulk_stats_note_publish_time(&stats, 300000);
    cxl_rdma_sidecar_bulk_stats_note_publish_time(&stats, 0);

    g_assert_cmpuint(stats.page_state_rdma_active_time_ns, ==, 3000000);
    g_assert_cmpuint(stats.page_state_rdma_transport_active_time_ns, ==,
                     1500000);
    g_assert_cmpuint(stats.page_state_rdma_publish_time_ns, ==, 1000000);
    g_assert_cmpuint(stats.page_state_rdma_completed_time_ns, ==, 0);
}

static void test_rdma_sidecar_phase_counters_split_claim_kinds(void)
{
    CXLHybridRDMASidecarBulkStats stats = { 0 };

    cxl_rdma_sidecar_bulk_stats_note_active_epoch_for_kind(
        &stats, CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION, 4 * MiB, 1000000);
    cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch_for_kind(
        &stats, CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION, 4 * MiB, 700000);
    cxl_rdma_sidecar_bulk_stats_note_active_epoch_for_kind(
        &stats, CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN, 8 * MiB, 2000000);
    cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch_for_kind(
        &stats, CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN, 8 * MiB, 1500000);

    g_assert_cmpuint(stats.page_state_rdma_active_time_ns, ==, 3000000);
    g_assert_cmpuint(stats.page_state_rdma_transport_active_time_ns, ==,
                     2200000);
    g_assert_cmpuint(stats.page_state_rdma_precopy_active_time_ns, ==,
                     1000000);
    g_assert_cmpuint(stats.page_state_rdma_precopy_transport_active_time_ns,
                     ==, 700000);
    g_assert_cmpuint(stats.page_state_rdma_postcopy_dirty_active_time_ns, ==,
                     2000000);
    g_assert_cmpuint(
        stats.page_state_rdma_postcopy_dirty_transport_active_time_ns, ==,
        1500000);
}

static void test_rdma_sidecar_effective_inflight_capacity_matches_pin_mode(void)
{
    g_assert_cmpuint(cxl_rdma_sidecar_effective_inflight_capacity(64, 32, true),
                     ==, 32);
    g_assert_cmpuint(cxl_rdma_sidecar_effective_inflight_capacity(64, 32, false),
                     ==, 1);
    g_assert_cmpuint(cxl_rdma_sidecar_effective_inflight_capacity(4, 32, true),
                     ==, 4);
    g_assert_cmpuint(cxl_rdma_sidecar_effective_inflight_capacity(0, 0, true),
                     ==, 1);
}

static void test_rdma_sidecar_drain_keeps_scheduling_queued_claims(void)
{
    g_assert_true(cxl_rdma_sidecar_schedule_allowed(false, true, false));
    g_assert_true(cxl_rdma_sidecar_schedule_allowed(false, false, true));
    g_assert_false(cxl_rdma_sidecar_schedule_allowed(false, false, false));
    g_assert_false(cxl_rdma_sidecar_schedule_allowed(true, true, true));
}

static void test_rdma_schedule_allows_postcopy_dirty_claim(void)
{
    g_assert_false(cxl_rdma_sidecar_claim_schedule_allowed(
        CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION, true, false, false));
    g_assert_true(cxl_rdma_sidecar_claim_schedule_allowed(
        CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN, true, false, false));
    g_assert_true(cxl_rdma_sidecar_claim_schedule_allowed(
        CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN, true, false, true));
    g_assert_false(cxl_rdma_sidecar_claim_schedule_allowed(
        CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN, true, true, true));
}

static void test_rdma_postcopy_dirty_admission_uses_actual_span_bytes(void)
{
    CXLHybridRDMAPostcopyDirtyAdmissionState state;
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot snap;
    CXLHybridRDMAPostcopyDirtyAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
        &state, 4, 8 * MiB, 64 * KiB);

    g_assert_true(cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve(
        &state, true, true, false, false, false, 128 * KiB,
        0, 0, 0, 0, &reservation, &snap));
    g_assert_true(reservation.valid);
    g_assert_cmpuint(reservation.bytes, ==, 128 * KiB);
    g_assert_cmpuint(state.reserved_wr, ==, 1);
    g_assert_cmpuint(state.reserved_bytes, ==, 128 * KiB);
    g_assert_cmpuint(snap.outstanding_wr, ==, 1);
    g_assert_cmpuint(snap.outstanding_bytes, ==, 128 * KiB);
    g_assert_cmpuint(state.accepted_spans, ==, 1);

    cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
        &state, &reservation);
    g_assert_false(reservation.valid);
    g_assert_cmpuint(state.reserved_wr, ==, 0);
    g_assert_cmpuint(state.reserved_bytes, ==, 0);
}

static void test_rdma_postcopy_dirty_admission_rejects_tiny_span(void)
{
    CXLHybridRDMAPostcopyDirtyAdmissionState state;
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot snap;
    CXLHybridRDMAPostcopyDirtyAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
        &state, 4, 8 * MiB, 64 * KiB);

    g_assert_false(cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve(
        &state, true, true, false, false, false, 4 * KiB,
        0, 0, 0, 0, &reservation, &snap));
    g_assert_false(reservation.valid);
    g_assert_false(snap.accept_rdma);
    g_assert_cmpuint(state.min_span_cxl_spans, ==, 1);
}

static void test_rdma_postcopy_dirty_admission_rejects_full_byte_window(void)
{
    CXLHybridRDMAPostcopyDirtyAdmissionState state;
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot snap;
    CXLHybridRDMAPostcopyDirtyAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
        &state, 8, 512 * KiB, 64 * KiB);

    g_assert_false(cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve(
        &state, true, true, false, false, false, 128 * KiB,
        0, 4, 0, 512 * KiB, &reservation, &snap));
    g_assert_false(reservation.valid);
    g_assert_false(snap.accept_rdma);
    g_assert_cmpuint(snap.outstanding_wr, ==, 4);
    g_assert_cmpuint(snap.outstanding_bytes, ==, 512 * KiB);
    g_assert_cmpuint(state.overflow_cxl_spans, ==, 1);
}

static void test_rdma_postcopy_dirty_admission_ignores_preexisting_bulk_bytes(void)
{
    CXLHybridRDMAPostcopyDirtyAdmissionState state;
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot snap;
    CXLHybridRDMAPostcopyDirtyAdmissionReservation reservation = { 0 };
    CXLHybridRDMABulkClaim inflight[] = {
        {
            .kind = CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION,
            .bytes = 2 * MiB,
        },
        {
            .kind = CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION,
            .bytes = 2 * MiB,
        },
    };

    cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
        &state, 4, 512 * KiB, 4 * KiB);

    g_assert_true(
        cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve_for_claims_for_test(
            &state, true, true, false, false, false, 64 * KiB,
            NULL, 0, inflight, G_N_ELEMENTS(inflight), &reservation, &snap));
    g_assert_true(reservation.valid);
    g_assert_true(snap.accept_rdma);
    g_assert_cmpuint(snap.inflight_wr, ==, 0);
    g_assert_cmpuint(snap.inflight_bytes, ==, 0);
    g_assert_cmpuint(snap.outstanding_bytes, ==, 64 * KiB);

    cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
        &state, &reservation);
}

static void test_rdma_postcopy_dirty_admission_ignores_precopy_drain(void)
{
    CXLHybridRDMAPostcopyDirtyAdmissionState state;
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot snap;
    CXLHybridRDMAPostcopyDirtyAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
        &state, 4, 8 * MiB, 4 * KiB);

    g_assert_true(cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve(
        &state, true, true, false, true, false, 64 * KiB,
        0, 0, 0, 0, &reservation, &snap));
    g_assert_true(reservation.valid);
    g_assert_true(snap.accept_rdma);
    g_assert_cmpuint(state.accepted_spans, ==, 1);

    cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
        &state, &reservation);
}

static void test_rdma_post_send_enomem_waits_when_inflight_exists(void)
{
    g_assert_true(cxl_rdma_sidecar_should_wait_for_sq_room_for_test(ENOMEM, 1));
    g_assert_false(cxl_rdma_sidecar_should_wait_for_sq_room_for_test(ENOMEM, 0));
    g_assert_false(cxl_rdma_sidecar_should_wait_for_sq_room_for_test(EINVAL, 1));
}

static void test_rdma_cq_wait_repolls_after_timeout(void)
{
    g_assert_true(cxl_rdma_sidecar_should_poll_after_cq_wait_for_test(0));
    g_assert_true(cxl_rdma_sidecar_should_poll_after_cq_wait_for_test(1));
    g_assert_false(cxl_rdma_sidecar_should_poll_after_cq_wait_for_test(-1));
}

static void test_rdma_cq_stale_event_retries_wait(void)
{
    g_assert_true(cxl_rdma_sidecar_should_retry_after_stale_cq_event_for_test(1,
                                                                              0));
    g_assert_false(cxl_rdma_sidecar_should_retry_after_stale_cq_event_for_test(1,
                                                                               1));
    g_assert_false(cxl_rdma_sidecar_should_retry_after_stale_cq_event_for_test(0,
                                                                               0));
    g_assert_false(cxl_rdma_sidecar_should_retry_after_stale_cq_event_for_test(-1,
                                                                               0));
}

static void test_rdma_sidecar_accounting_counts_ready_invalidate_and_republish(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    CXLHybridRDMASidecarStats stats = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_mark_ready(&state, 2));
    cxl_hybrid_rdma_sidecar_get_stats(&state, &stats);
    g_assert_cmpuint(stats.rdma_ready_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_ready_pages, ==, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_invalidate_ready(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_invalidate_ready(&state, 2));
    cxl_hybrid_rdma_sidecar_get_stats(&state, &stats);
    g_assert_cmpuint(stats.rdma_invalidated_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_ready_pages_lost, ==, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_note_cxl_republish(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_note_cxl_republish(&state, 2));
    cxl_hybrid_rdma_sidecar_get_stats(&state, &stats);
    g_assert_cmpuint(stats.cxl_republish_regions_due_to_rdma_invalidate, ==, 1);
    g_assert_cmpuint(stats.cxl_republish_pages_due_to_rdma_invalidate, ==, 512);

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_inflight_does_not_block_cxl(void)
{
    CXLHybridRDMASidecarState state = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_inflight(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_note_cxl_publish(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 3));
    g_assert_false(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_stale(&state, 3));

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_selector_accepts_dirty_bulk_region(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    unsigned long *dirty = bitmap_new(8 * 512);
    uint64_t region = UINT64_MAX;

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);
    bitmap_set(dirty, 2 * 512, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_pick_pending_region_for_test(
        &state, dirty, 8 * 512, &region));
    g_assert_cmpuint(region, ==, 2);
    g_assert_true(cxl_hybrid_rdma_sidecar_region_inflight(&state, 2));

    g_free(dirty);
    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_selector_skips_clean_visible_region(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    unsigned long *dirty = bitmap_new(8 * 512);
    uint64_t region = UINT64_MAX;

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);
    bitmap_set(dirty, 0, 512);
    bitmap_set(dirty, 1 * 512, 512);
    g_assert_true(cxl_hybrid_rdma_sidecar_note_cxl_publish(&state, 0));

    g_assert_true(cxl_hybrid_rdma_sidecar_pick_pending_region_for_test(
        &state, dirty, 8 * 512, &region));
    g_assert_cmpuint(region, ==, 1);
    g_assert_false(cxl_hybrid_rdma_sidecar_region_inflight(&state, 0));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_inflight(&state, 1));

    g_free(dirty);
    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_inflight_does_not_cap_total_coverage(void)
{
    CXLHybridRDMASidecarState state = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 33, 512);

    for (uint64_t i = 0; i < 33; i++) {
        g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, i));
    }

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_dirty_invalidation_clears_ready(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    CXLHybridRDMASidecarStats stats = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 2));

    g_assert_true(cxl_hybrid_rdma_sidecar_invalidate_ready(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_region_is_rdma_owned(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));
    cxl_hybrid_rdma_sidecar_get_stats(&state, &stats);
    g_assert_cmpuint(stats.rdma_invalidated_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_ready_pages_lost, ==, 512);

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_commit_only_current_ready_regions(void)
{
    CXLHybridRDMASidecarState state = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_invalidate_ready(&state, 2));

    g_assert_true(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 1));
    g_assert_false(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_committed(&state, 1));
    g_assert_false(cxl_hybrid_rdma_sidecar_region_committed(&state, 2));

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_brake_fallback_requires_brake_enabled(void)
{
    g_assert_false(cxl_hybrid_rdma_brake_fallback_enabled(false, false));
    g_assert_false(cxl_hybrid_rdma_brake_fallback_enabled(false, true));
    g_assert_false(cxl_hybrid_rdma_brake_fallback_enabled(true, false));
    g_assert_true(cxl_hybrid_rdma_brake_fallback_enabled(true, true));
}

static void test_rdma_sidecar_transport_stats_accounting(void)
{
    CXLHybridRDMASidecarStats stats;

    cxl_hybrid_reset_rdma_sidecar_stats_for_test();
    cxl_hybrid_account_rdma_sidecar_connect(1000);
    cxl_hybrid_account_rdma_sidecar_registered(2 * MiB);
    cxl_hybrid_account_rdma_sidecar_posted(123, 3, 2 * MiB);
    cxl_hybrid_account_rdma_sidecar_completed(123, 3, 2 * MiB);
    cxl_hybrid_account_rdma_sidecar_stale(4, 2 * MiB, true);
    cxl_hybrid_account_rdma_sidecar_failed(5);
    cxl_hybrid_account_rdma_sidecar_no_candidate();
    cxl_hybrid_set_rdma_sidecar_inflight_hint(7);
    cxl_hybrid_get_rdma_sidecar_stats(&stats);

    g_assert_cmpuint(stats.rdma_sidecar_connect_time_ns, ==, 1000);
    g_assert_cmpuint(stats.rdma_sidecar_registered_bytes, ==, 2 * MiB);
    g_assert_cmpuint(stats.rdma_sidecar_posted_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_posted_bytes, ==, 2 * MiB);
    g_assert_cmpuint(stats.rdma_sidecar_completed_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_completed_bytes, ==, 2 * MiB);
    g_assert_cmpuint(stats.rdma_sidecar_stale_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_cxl_race_lost_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_failed_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_no_candidate_events, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_max_inflight_regions, ==, 7);
    g_assert_true(stats.rdma_sidecar_failed);

    cxl_hybrid_reset_rdma_sidecar_stats_for_test();
}

static void test_rdma_sidecar_requires_transport_for_completion(void)
{
    CXLHybridRDMASidecarBulkStats stats = { 0 };

    cxl_rdma_sidecar_stop();
    cxl_rdma_sidecar_get_stats(&stats);
    g_assert_cmpuint(stats.rdma_bulk_regions, ==, 0);
    g_assert_cmpuint(stats.rdma_bulk_bytes, ==, 0);
}

static void test_rdma_sidecar_ready_commit_and_dirty_invalidate(void)
{
    CXLHybridRDMASidecarState state = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_committed(&state, 1));

    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_invalidate_ready(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_invalidated(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(&state, 2));

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_dirty_sync_invalidates_ready_regions(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    unsigned long dirty[BITS_TO_LONGS(2048)] = { 0 };
    CXLHybridRDMASidecarStats stats = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 4, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 2));

    bitmap_set(dirty, 2 * 512 + 17, 1);
    g_assert_cmpuint(cxl_hybrid_rdma_sidecar_invalidate_dirty_ready_regions(
                         &state, dirty, 2048), ==, 1);

    g_assert_true(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 1));
    g_assert_false(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_is_cxl_owned(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));

    cxl_hybrid_rdma_sidecar_get_stats(&state, &stats);
    g_assert_cmpuint(stats.rdma_invalidated_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_ready_pages_lost, ==, 512);

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_invalidated_region_republishes_once(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    CXLHybridRDMASidecarStats stats = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 4, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_invalidate_ready(&state, 2));

    g_assert_true(cxl_hybrid_rdma_sidecar_region_is_cxl_owned(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_note_cxl_republish(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_note_cxl_republish(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_try_own_region(&state, 2));

    cxl_hybrid_rdma_sidecar_get_stats(&state, &stats);
    g_assert_cmpuint(stats.cxl_republish_regions_due_to_rdma_invalidate, ==, 1);
    g_assert_cmpuint(stats.cxl_republish_pages_due_to_rdma_invalidate, ==, 512);

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_global_republish_is_idempotent(void)
{
    CXLHybridRDMASidecarStats stats = { 0 };

    cxl_hybrid_reset_rdma_sidecar_stats_for_test();
    cxl_hybrid_rdma_sidecar_global_init(4, 512);

    g_assert_true(cxl_hybrid_region_try_own_rdma(2));
    cxl_hybrid_mark_region_rdma_ready(2);
    cxl_hybrid_invalidate_region_rdma_ready(2);

    g_assert_true(cxl_hybrid_region_note_cxl_republish(2));
    cxl_hybrid_get_rdma_sidecar_stats(&stats);
    g_assert_cmpuint(stats.cxl_republish_regions_due_to_rdma_invalidate, ==, 1);
    g_assert_cmpuint(stats.cxl_republish_pages_due_to_rdma_invalidate, ==, 512);

    g_assert_false(cxl_hybrid_region_note_cxl_republish(2));
    cxl_hybrid_get_rdma_sidecar_stats(&stats);
    g_assert_cmpuint(stats.cxl_republish_regions_due_to_rdma_invalidate, ==, 1);
    g_assert_cmpuint(stats.cxl_republish_pages_due_to_rdma_invalidate, ==, 512);

    cxl_hybrid_rdma_sidecar_global_destroy();
    cxl_hybrid_reset_rdma_sidecar_stats_for_test();
}

static void test_rdma_sidecar_switch_commits_only_final_clean_ready_regions(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    unsigned long dirty[BITS_TO_LONGS(2048)] = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 4, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 0));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 0));
    g_assert_true(cxl_hybrid_rdma_sidecar_try_own_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_mark_ready(&state, 1));

    bitmap_set(dirty, 512 + 4, 1);
    g_assert_cmpuint(cxl_hybrid_rdma_sidecar_invalidate_dirty_ready_regions(
                         &state, dirty, 2048), ==, 1);

    g_assert_true(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 0));
    g_assert_false(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_committed(&state, 0));
    g_assert_false(cxl_hybrid_rdma_sidecar_region_committed(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_invalidated(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_is_cxl_owned(&state, 1));

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cxl/region/first-2mib",
                    test_region_geometry_first_2mib);
    g_test_add_func("/cxl/region/second-2mib",
                    test_region_geometry_second_2mib_with_global_base);
    g_test_add_func("/cxl/region/reject-cross-start",
                    test_region_geometry_rejects_region_crossing_ramblock_start);
    g_test_add_func("/cxl/region/reject-partial-tail",
                    test_region_geometry_rejects_partial_tail);
    g_test_add_func("/cxl/region/can-cover-whole-aligned-region",
                    test_region_geometry_can_cover_whole_aligned_region);
    g_test_add_func("/cxl/region/cannot-cover-small-ramblock",
                    test_region_geometry_cannot_cover_small_ramblock);
    g_test_add_func("/cxl/region/reject-unaligned-cxl",
                    test_region_geometry_rejects_unaligned_cxl_offset);
    g_test_add_func("/cxl/region/reject-invalid-granule-page",
                    test_region_geometry_rejects_invalid_granule_and_page);
    g_test_add_func("/cxl/region/reject-out-of-range-fault-offset",
                    test_region_geometry_rejects_out_of_range_fault_offset);
    g_test_add_func("/cxl/region/reject-cxl-offset-wrap",
                    test_region_geometry_rejects_cxl_offset_wrap);
    g_test_add_func("/cxl/region/reject-page-count-overflow",
                    test_region_geometry_rejects_page_count_overflow);
    g_test_add_func("/cxl/region/reject-misaligned-block-offset",
                    test_region_geometry_rejects_misaligned_block_offset);
    g_test_add_func("/cxl/region/dst-remap-page-index-4k-offsets",
                    test_dst_remap_page_index_accepts_4k_aligned_span_offsets);
    g_test_add_func("/cxl/region/mode-disables-fault-burst",
                    test_region_mode_disables_fault_burst);
    g_test_add_func("/cxl/region/fallback-copy-poisons-region",
                    test_region_fallback_copy_poisons_region);
    g_test_add_func("/cxl/region/remap-reservation-blocks-copy-poison",
                    test_region_remap_reservation_blocks_copy_poison);
    g_test_add_func("/cxl/region/copy-poison-blocks-remap-reservation",
                    test_region_copy_poison_blocks_remap_reservation);
    g_test_add_func("/cxl/region/mapped-ram-pages-alignment",
                    test_mapped_ram_pages_alignment_includes_region_granule);
    g_test_add_func("/cxl/region/source-remap-independent-from-fault-region",
                    test_source_remap_granule_is_independent_from_fault_region);
    g_test_add_func("/cxl/region/source-default-64k-with-4k-dax",
                    test_source_remap_default_is_64k_with_4k_dax_alignment);
    g_test_add_func("/cxl/region/fault-default-2mib-with-4k-dax",
                    test_fault_region_default_is_2mib_with_4k_dax_alignment);
    g_test_add_func("/cxl/region/mapped-ram-layout-page-spans",
                    test_mapped_ram_layout_returns_page_data_spans);
    g_test_add_func("/cxl/region/source-remap-requires-staged-clean-pages",
                    test_source_remap_region_requires_staged_clean_pages);
    g_test_add_func("/cxl/region/clean-remap-budget-zero-one-region",
                    test_clean_remap_budget_zero_one_region);
    g_test_add_func("/cxl/region/clean-remap-budget-multiple-regions",
                    test_clean_remap_budget_multiple_regions);
    g_test_add_func("/cxl/region/clean-remap-throttle-requires-delay-and-copy",
                    test_clean_remap_throttle_requires_nonzero_delay_and_copy);
    g_test_add_func("/cxl/region/clean-remap-candidate-requires-second-clean-scan",
                    test_clean_remap_candidate_requires_second_clean_scan);
    g_test_add_func("/cxl/region/clean-remap-debug-mode-helpers",
                    test_clean_remap_debug_mode_helpers);
    g_test_add_func("/cxl/region/clean-remap-prefault-mode-helpers",
                    test_clean_remap_prefault_mode_helpers);
    g_test_add_func("/cxl/region/warm-page-eligible-excludes-already-visible-pages",
                    test_warm_page_eligible_excludes_already_visible_pages);
    g_test_add_func("/cxl/region/dirty-requeue-clears-stale-sent-state",
                    test_dirty_requeue_clears_stale_sent_state);
    g_test_add_func("/cxl/region/rdma-descriptor-claims-requested-pages",
                    test_rdma_descriptor_claims_only_requested_pages);
    g_test_add_func("/cxl/region/rdma-descriptor-contiguous-stops-before-clean",
                    test_rdma_descriptor_claim_contiguous_stops_before_clean_page);
    g_test_add_func("/cxl/region/rdma-descriptor-contiguous-rejects-sparse-prefix",
                    test_rdma_descriptor_claim_contiguous_rejects_sparse_prefix);
    g_test_add_func("/cxl/region/rdma-descriptor-contiguous-reuse-drops-old-claims",
                    test_rdma_descriptor_claim_contiguous_reuse_drops_old_claims);
    g_test_add_func("/cxl/region/rdma-descriptor-contiguous-invalid-reclaims-old-claims",
                    test_rdma_descriptor_claim_contiguous_invalid_reclaims_old_claims);
    g_test_add_func("/cxl/region/rdma-descriptor-completion-ignores-stale-page",
                    test_rdma_descriptor_completion_ignores_stale_page);
    g_test_add_func("/cxl/region/rdma-descriptor-no-claims-clears-descriptor",
                    test_rdma_descriptor_no_claims_clears_descriptor);
    g_test_add_func("/cxl/region/rdma-descriptor-completion-consumes-once",
                    test_rdma_descriptor_completion_consumes_claims_once);
    g_test_add_func("/cxl/region/rdma-posted-page-blocks-cxl-publish",
                    test_rdma_posted_page_blocks_cxl_publish_until_completion);
    g_test_add_func("/cxl/region/rdma-admission-rejects-full-window",
                    test_rdma_admission_rejects_full_dynamic_window);
    g_test_add_func("/cxl/region/rdma-claim-identity-distinguishes-postcopy",
                    test_rdma_claim_identity_distinguishes_postcopy_from_region);
    g_test_add_func("/cxl/region/rdma-claim-identity-rejects-zero-wrid",
                    test_rdma_claim_identity_rejects_zero_wrid);
    g_test_add_func("/cxl/region/rdma-postcopy-span-caps-at-2mib",
                    test_rdma_postcopy_span_caps_at_2mib);
    g_test_add_func("/cxl/region/rdma-postcopy-span-respects-block-tail",
                    test_rdma_postcopy_span_respects_block_tail);
    g_test_add_func("/cxl/region/rdma-admission-reserve-cancel",
                    test_rdma_admission_reserve_and_cancel_updates_outstanding);
    g_test_add_func("/cxl/region/rdma-admission-outstanding-wrap-rejects-rdma",
                    test_rdma_admission_outstanding_wrap_rejects_rdma);
    g_test_add_func("/cxl/region/rdma-admission-completion-grows-window",
                    test_rdma_admission_completion_grows_window_and_bdp);
    g_test_add_func("/cxl/region/rdma-admission-bounded-probe-window",
                    test_rdma_admission_starts_with_bounded_probe_window);
    g_test_add_func("/cxl/region/rdma-admission-grows-past-probe-window",
                    test_rdma_admission_can_grow_past_initial_probe_window);
    g_test_add_func("/cxl/region/rdma-admission-bdp-caps-effective-window",
                    test_rdma_admission_bdp_caps_effective_window);
    g_test_add_func("/cxl/region/rdma-admission-bdp-clamps-without-u32-wrap",
                    test_rdma_admission_bdp_clamps_without_u32_wrap);
    g_test_add_func("/cxl/region/rdma-admission-bdp-ceil-division-no-wrap",
                    test_rdma_admission_bdp_ceil_division_does_not_wrap);
    g_test_add_func("/cxl/region/rdma-admission-goodput-regression-halves-window",
                    test_rdma_admission_goodput_regression_halves_window);
    g_test_add_func("/cxl/region/rdma-admission-small-jitter-keeps-window",
                    test_rdma_admission_small_jitter_does_not_drop_window);
    g_test_add_func("/cxl/region/rdma-admission-epoch-goodput-keeps-window",
                    test_rdma_admission_epoch_goodput_keeps_probe_window);
    g_test_add_func("/cxl/region/rdma-sidecar-active-epoch-wall-time",
                    test_rdma_sidecar_active_epoch_accounts_wall_time);
    g_test_add_func("/cxl/region/rdma-sidecar-phase-counters",
                    test_rdma_sidecar_phase_counters_split_claim_kinds);
    g_test_add_func("/cxl/region/rdma-sidecar-effective-inflight-capacity",
                    test_rdma_sidecar_effective_inflight_capacity_matches_pin_mode);
    g_test_add_func("/cxl/region/rdma-sidecar-drain-schedules-queued-claims",
                    test_rdma_sidecar_drain_keeps_scheduling_queued_claims);
    g_test_add_func("/cxl/region/rdma-schedule-allows-postcopy-dirty",
                    test_rdma_schedule_allows_postcopy_dirty_claim);
    g_test_add_func("/cxl/region/rdma-postcopy-dirty-admission-actual-bytes",
                    test_rdma_postcopy_dirty_admission_uses_actual_span_bytes);
    g_test_add_func("/cxl/region/rdma-postcopy-dirty-admission-tiny-cxl",
                    test_rdma_postcopy_dirty_admission_rejects_tiny_span);
    g_test_add_func("/cxl/region/rdma-postcopy-dirty-admission-byte-window",
                    test_rdma_postcopy_dirty_admission_rejects_full_byte_window);
    g_test_add_func("/cxl/region/rdma-postcopy-dirty-admission-dirty-only",
                    test_rdma_postcopy_dirty_admission_ignores_preexisting_bulk_bytes);
    g_test_add_func("/cxl/region/rdma-postcopy-dirty-admission-ignores-precopy-drain",
                    test_rdma_postcopy_dirty_admission_ignores_precopy_drain);
    g_test_add_func("/cxl/region/rdma-post-send-enomem-waits",
                    test_rdma_post_send_enomem_waits_when_inflight_exists);
    g_test_add_func("/cxl/region/rdma-cq-wait-repolls-after-timeout",
                    test_rdma_cq_wait_repolls_after_timeout);
    g_test_add_func("/cxl/region/rdma-cq-stale-event-retries-wait",
                    test_rdma_cq_stale_event_retries_wait);
    g_test_add_func("/cxl/region/rdma-sidecar-accounting",
                    test_rdma_sidecar_accounting_counts_ready_invalidate_and_republish);
    g_test_add_func("/cxl/region/rdma-sidecar-inflight-does-not-block-cxl",
                    test_rdma_sidecar_inflight_does_not_block_cxl);
    g_test_add_func("/cxl/region/rdma-sidecar-selector-accepts-dirty-bulk-region",
                    test_rdma_sidecar_selector_accepts_dirty_bulk_region);
    g_test_add_func("/cxl/region/rdma-sidecar-selector-skips-clean-visible-region",
                    test_rdma_sidecar_selector_skips_clean_visible_region);
    g_test_add_func("/cxl/region/rdma-sidecar-inflight-does-not-cap-total-coverage",
                    test_rdma_sidecar_inflight_does_not_cap_total_coverage);
    g_test_add_func("/cxl/region/rdma-sidecar-dirty-invalidation-clears-ready",
                    test_rdma_sidecar_dirty_invalidation_clears_ready);
    g_test_add_func("/cxl/region/rdma-sidecar-commit-current-ready",
                    test_rdma_sidecar_commit_only_current_ready_regions);
    g_test_add_func("/cxl/region/rdma-sidecar-brake-fallback-gated",
                    test_rdma_sidecar_brake_fallback_requires_brake_enabled);
    g_test_add_func("/cxl/region/rdma-sidecar-transport-stats-accounting",
                    test_rdma_sidecar_transport_stats_accounting);
    g_test_add_func("/cxl/region/rdma-sidecar-requires-transport",
                    test_rdma_sidecar_requires_transport_for_completion);
    g_test_add_func("/cxl/region/rdma-sidecar-ready-commit-dirty-invalidate",
                    test_rdma_sidecar_ready_commit_and_dirty_invalidate);
    g_test_add_func("/cxl/region/rdma-sidecar-dirty-sync-invalidates-ready",
                    test_rdma_sidecar_dirty_sync_invalidates_ready_regions);
    g_test_add_func("/cxl/region/rdma-sidecar-invalidated-region-republishes-once",
                    test_rdma_sidecar_invalidated_region_republishes_once);
    g_test_add_func("/cxl/region/rdma-sidecar-global-republish-idempotent",
                    test_rdma_sidecar_global_republish_is_idempotent);
    g_test_add_func("/cxl/region/rdma-sidecar-switch-commits-final-clean",
                    test_rdma_sidecar_switch_commits_only_final_clean_ready_regions);
    return g_test_run();
}
