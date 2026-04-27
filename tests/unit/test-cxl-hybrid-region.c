/*
 * Unit tests for CXL hybrid fault region geometry helpers.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
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
    return g_test_run();
}
