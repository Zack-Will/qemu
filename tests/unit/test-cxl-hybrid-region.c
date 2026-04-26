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
    return g_test_run();
}
