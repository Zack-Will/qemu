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
#include "migration/cxl.h"

#define TEST_TARGET_PAGE_SIZE (4 * KiB)

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

static void test_rdma_sidecar_accounting_counts_ready_invalidate_and_republish(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    CXLHybridRDMASidecarStats stats = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

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

static void test_rdma_sidecar_global_accounting_exports_stats(void)
{
    CXLHybridRDMASidecarStats stats = { 0 };

    cxl_hybrid_reset_rdma_sidecar_stats_for_test();

    cxl_hybrid_account_rdma_ready(7, 512);
    cxl_hybrid_account_rdma_invalidate(7, 512);
    cxl_hybrid_account_rdma_cxl_republish(7, 1024);
    cxl_hybrid_get_rdma_sidecar_stats(&stats);

    g_assert_cmpuint(stats.rdma_ready_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_ready_pages, ==, 512);
    g_assert_cmpuint(stats.rdma_invalidated_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_ready_pages_lost, ==, 512);
    g_assert_cmpuint(stats.cxl_republish_regions_due_to_rdma_invalidate, ==, 1);
    g_assert_cmpuint(stats.cxl_republish_pages_due_to_rdma_invalidate, ==, 1024);

    cxl_hybrid_reset_rdma_sidecar_stats_for_test();
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
    g_test_add_func("/cxl/region/rdma-sidecar-accounting",
                    test_rdma_sidecar_accounting_counts_ready_invalidate_and_republish);
    g_test_add_func("/cxl/region/rdma-sidecar-global-accounting",
                    test_rdma_sidecar_global_accounting_exports_stats);
    return g_test_run();
}
