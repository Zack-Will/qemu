/*
 * Unit tests for CXL hybrid control header lifecycle helpers.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/cxl.h"

static void test_header_reset_for_run(void)
{
    CXLHybridControlHeader hdr = {
        .magic = 0xffffffffU,
        .version = 0xffffU,
        .flags = 0xffffU,
        .request_ring_order = 77,
        .ready_ring_order = 88,
        .generation = 99,
        .reserved0 = 1234,
        .request_prod = 10,
        .request_cons = 9,
        .ready_prod = 20,
        .ready_cons = 19,
    };

    cxl_hybrid_control_reset_header_for_run(&hdr, 2);

    g_assert_cmphex(hdr.magic, ==, CXL_HYBRID_CTRL_MAGIC);
    g_assert_cmpuint(hdr.version, ==, CXL_HYBRID_CTRL_VERSION);
    g_assert_cmpuint(hdr.flags, ==, 0);
    g_assert_cmpuint(hdr.request_ring_order, ==, CXL_HYBRID_CTRL_REQUEST_ORDER);
    g_assert_cmpuint(hdr.ready_ring_order, ==, CXL_HYBRID_CTRL_READY_ORDER);
    g_assert_cmpuint(hdr.generation, ==, 2);
    g_assert_cmpuint(hdr.reserved0, ==, 0);
    g_assert_cmpuint(hdr.request_prod, ==, 0);
    g_assert_cmpuint(hdr.request_cons, ==, 0);
    g_assert_cmpuint(hdr.ready_prod, ==, 0);
    g_assert_cmpuint(hdr.ready_cons, ==, 0);
}

static void test_header_reset_updates_generation(void)
{
    CXLHybridControlHeader hdr = { 0 };

    cxl_hybrid_control_reset_header_for_run(&hdr, 2);
    hdr.request_prod = 3;
    hdr.request_cons = 1;
    hdr.ready_prod = 7;
    hdr.ready_cons = 6;

    cxl_hybrid_control_reset_header_for_run(&hdr, 3);

    g_assert_cmpuint(hdr.generation, ==, 3);
    g_assert_cmpuint(hdr.request_prod, ==, 0);
    g_assert_cmpuint(hdr.request_cons, ==, 0);
    g_assert_cmpuint(hdr.ready_prod, ==, 0);
    g_assert_cmpuint(hdr.ready_cons, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cxl-hybrid-control/header-reset-for-run",
                    test_header_reset_for_run);
    g_test_add_func("/cxl-hybrid-control/header-reset-updates-generation",
                    test_header_reset_updates_generation);
    return g_test_run();
}
