/*
 * Unit tests for CXL hybrid control header lifecycle helpers.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/cxl.h"
#include "qemu/bitmap.h"

static void test_header_reset_clears_visible_bitmap(void)
{
    CXLHybridControlHeader hdr = {
        .magic = 0xffffffffU,
        .version = 0xffffU,
        .flags = 0xffffU,
        .request_ring_order = 77,
        .ready_ring_order = 88,
        .generation = 99,
        .visible_page_words = 1234,
        .request_prod = 10,
        .request_cons = 9,
        .ready_prod = 20,
        .ready_cons = 19,
    };
    unsigned long visible_bitmap[2] = { ~0UL, ~0UL };

    cxl_hybrid_control_reset_run_state(&hdr, visible_bitmap, 2,
                                       G_N_ELEMENTS(visible_bitmap));

    g_assert_cmphex(hdr.magic, ==, CXL_HYBRID_CTRL_MAGIC);
    g_assert_cmpuint(hdr.version, ==, CXL_HYBRID_CTRL_VERSION);
    g_assert_cmpuint(hdr.flags, ==, 0);
    g_assert_cmpuint(hdr.request_ring_order, ==, CXL_HYBRID_CTRL_REQUEST_ORDER);
    g_assert_cmpuint(hdr.ready_ring_order, ==, CXL_HYBRID_CTRL_READY_ORDER);
    g_assert_cmpuint(hdr.generation, ==, 2);
    g_assert_cmpuint(hdr.visible_page_words, ==, G_N_ELEMENTS(visible_bitmap));
    g_assert_cmpuint(hdr.request_prod, ==, 0);
    g_assert_cmpuint(hdr.request_cons, ==, 0);
    g_assert_cmpuint(hdr.ready_prod, ==, 0);
    g_assert_cmpuint(hdr.ready_cons, ==, 0);
    g_assert_cmphex(visible_bitmap[0], ==, 0);
    g_assert_cmphex(visible_bitmap[1], ==, 0);
}

static void test_visible_bitmap_bytes_round_up_by_ulong(void)
{
    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_words(0), ==, 0);
    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_bytes(0), ==, 0);

    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_words(1), ==, 1);
    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_bytes(1), ==,
                     sizeof(unsigned long));

    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_words(BITS_PER_LONG),
                     ==, 1);
    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_bytes(BITS_PER_LONG),
                     ==, sizeof(unsigned long));

    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_words(BITS_PER_LONG + 1),
                     ==, 2);
    g_assert_cmpuint(cxl_hybrid_control_visible_bitmap_bytes(BITS_PER_LONG + 1),
                     ==, 2 * sizeof(unsigned long));
}

static void test_visible_bitmap_respects_generation(void)
{
    CXLHybridControlHeader hdr = {
        .generation = 7,
        .visible_page_words = 2,
    };
    unsigned long visible_bitmap[2] = { 0 };
    const uint64_t page_index = BITS_PER_LONG + 5;
    const uint64_t out_of_range = hdr.visible_page_words * BITS_PER_LONG;

    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible_bitmap,
                                                   page_index, 6));
    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible_bitmap,
                                                   page_index, 7));

    cxl_hybrid_control_mark_page_visible(&hdr, visible_bitmap, page_index);
    g_assert_true(cxl_hybrid_control_page_visible(&hdr, visible_bitmap,
                                                  page_index, 7));

    cxl_hybrid_control_clear_page_visible(&hdr, visible_bitmap, page_index);
    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible_bitmap,
                                                   page_index, 7));

    cxl_hybrid_control_mark_page_visible(&hdr, visible_bitmap, out_of_range);
    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible_bitmap,
                                                   out_of_range, 7));
    g_assert_cmphex(visible_bitmap[0], ==, 0);
    g_assert_cmphex(visible_bitmap[1], ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cxl-hybrid-control/header-reset-clears-visible-bitmap",
                    test_header_reset_clears_visible_bitmap);
    g_test_add_func("/cxl-hybrid-control/visible-bitmap-bytes-round-up-by-ulong",
                    test_visible_bitmap_bytes_round_up_by_ulong);
    g_test_add_func("/cxl-hybrid-control/visible-bitmap-respects-generation",
                    test_visible_bitmap_respects_generation);
    return g_test_run();
}
