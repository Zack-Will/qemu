/*
 * Unit tests for CXL hybrid control header lifecycle helpers.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "migration/cxl.h"
#include "qemu/bitmap.h"
#include "glib/gstdio.h"

#define TEST_ALIGN_2M (2 * MiB)

static void test_align_mapping_bytes_rounds_up_to_device_align(void)
{
    uint64_t raw = 3 * MiB + 4096;
    uint64_t alloc = cxl_hybrid_align_mapping_bytes(raw, TEST_ALIGN_2M);

    g_assert_cmpuint(alloc, >=, raw);
    g_assert_cmpuint(alloc % TEST_ALIGN_2M, ==, 0);
    g_assert_cmpuint(alloc, ==, 4 * MiB);
}

static void test_staging_shared_map_supports_fixed_extent_io(void)
{
    g_autofree char *path = NULL;
    CXLHybridMetadataEntry entry = {
        .ramblock = (char *)"rb0",
        .offset = 0,
        .length = 4096,
    };
    CXLHybridMetadata meta = {
        .version = CXL_HYBRID_METADATA_VERSION,
        .generation = 1,
        .nr_entries = 1,
        .entries = &entry,
    };
    struct stat st;
    Error *err = NULL;
    int fd;
    int ret;
    uint8_t in[4096];
    uint8_t out[4096];

    fd = g_file_open_tmp("test-cxl-hybrid-staging-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 8 * MiB), ==, 0);

    ret = cxl_hybrid_dst_staging_init_fixed_fd(fd, 2 * MiB, 2 * MiB,
                                               8 * MiB, true, &err);
    if (err) {
        error_free_or_abort(&err);
    }
    g_assert_cmpint(ret, ==, 0);

    ret = cxl_hybrid_dst_staging_apply_metadata(&meta, &err);
    if (err) {
        error_free_or_abort(&err);
    }
    g_assert_cmpint(ret, ==, 0);

    memset(in, 0x5a, sizeof(in));
    memset(out, 0, sizeof(out));

    ret = cxl_hybrid_dst_staging_store_page("rb0", 0, in, sizeof(in), &err);
    if (err) {
        error_free_or_abort(&err);
    }
    g_assert_cmpint(ret, ==, 0);

    ret = cxl_hybrid_dst_staging_read_page("rb0", 0, out, sizeof(out), &err);
    if (err) {
        error_free_or_abort(&err);
    }
    g_assert_cmpint(ret, ==, 0);
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));

    g_assert_cmpint(fstat(fd, &st), ==, 0);
    g_assert_cmpuint(st.st_size, ==, 8 * MiB);

    cxl_hybrid_dst_staging_cleanup();
    close(fd);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_staging_shared_map_rejects_extent_overflow_without_growing(void)
{
    g_autofree char *path = NULL;
    struct stat st;
    Error *err = NULL;
    int fd;
    int ret;

    fd = g_file_open_tmp("test-cxl-hybrid-staging-overflow-XXXXXX",
                         &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 2 * MiB), ==, 0);

    ret = cxl_hybrid_dst_staging_init_fixed_fd(fd, 2 * MiB, 1 * MiB,
                                               2 * MiB, true, &err);
    g_assert_cmpint(ret, <, 0);
    g_assert_nonnull(err);
    error_free(err);

    g_assert_cmpint(fstat(fd, &st), ==, 0);
    g_assert_cmpuint(st.st_size, ==, 2 * MiB);

    close(fd);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_staging_shared_map_rejects_base_beyond_extent(void)
{
    g_autofree char *path = NULL;
    struct stat st;
    Error *err = NULL;
    int fd;
    int ret;

    fd = g_file_open_tmp("test-cxl-hybrid-staging-base-overflow-XXXXXX",
                         &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 2 * MiB), ==, 0);

    errno = 0;
    ret = cxl_hybrid_dst_staging_init_fixed_fd(fd, 4096, 3 * MiB,
                                               2 * MiB, true, &err);
    g_assert_cmpint(ret, ==, -EOVERFLOW);
    g_assert_nonnull(err);
    error_free(err);

    g_assert_cmpint(fstat(fd, &st), ==, 0);
    g_assert_cmpuint(st.st_size, ==, 2 * MiB);

    close(fd);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

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
        .owned_region_words = 5678,
        .region_granule_shift = 12,
        .total_pages = 999,
        .total_regions = 888,
        .region_granule = 4096,
        .request_prod = 10,
        .request_cons = 9,
        .active_enqueue_count = 4,
        .active_request_count = 3,
        .ready_prod = 20,
        .ready_cons = 19,
        .source_write_count = 33,
        .completed_generation = 99,
        .completion_flags = CXL_HYBRID_CTRL_COMPLETION_F_QUIESCE,
    };
    unsigned long visible_bitmap[2] = { ~0UL, ~0UL };
    unsigned long owned_bitmap[1] = { ~0UL };

    cxl_hybrid_control_reset_run_state(&hdr, visible_bitmap,
                                       G_N_ELEMENTS(visible_bitmap) *
                                       BITS_PER_LONG,
                                       owned_bitmap,
                                       G_N_ELEMENTS(owned_bitmap) *
                                       BITS_PER_LONG,
                                       64 * 1024,
                                       2);

    g_assert_cmphex(hdr.magic, ==, CXL_HYBRID_CTRL_MAGIC);
    g_assert_cmpuint(hdr.version, ==, CXL_HYBRID_CTRL_VERSION);
    g_assert_cmpuint(hdr.flags, ==, 0);
    g_assert_cmpuint(hdr.request_ring_order, ==, CXL_HYBRID_CTRL_REQUEST_ORDER);
    g_assert_cmpuint(hdr.ready_ring_order, ==, CXL_HYBRID_CTRL_READY_ORDER);
    g_assert_cmpuint(hdr.generation, ==, 2);
    g_assert_cmpuint(hdr.visible_page_words, ==, G_N_ELEMENTS(visible_bitmap));
    g_assert_cmpuint(hdr.owned_region_words, ==, G_N_ELEMENTS(owned_bitmap));
    g_assert_cmpuint(hdr.region_granule_shift, ==, 16);
    g_assert_cmpuint(hdr.total_pages, ==,
                     G_N_ELEMENTS(visible_bitmap) * BITS_PER_LONG);
    g_assert_cmpuint(hdr.total_regions, ==,
                     G_N_ELEMENTS(owned_bitmap) * BITS_PER_LONG);
    g_assert_cmpuint(hdr.region_granule, ==, 64 * 1024);
    g_assert_cmpuint(hdr.request_prod, ==, 0);
    g_assert_cmpuint(hdr.request_cons, ==, 0);
    g_assert_cmpuint(hdr.active_enqueue_count, ==, 0);
    g_assert_cmpuint(hdr.active_request_count, ==, 0);
    g_assert_cmpuint(hdr.ready_prod, ==, 0);
    g_assert_cmpuint(hdr.ready_cons, ==, 0);
    g_assert_cmpuint(hdr.source_write_count, ==, 0);
    g_assert_cmpuint(hdr.completed_generation, ==, 0);
    g_assert_cmpuint(hdr.completion_flags, ==, 0);
    g_assert_cmphex(visible_bitmap[0], ==, 0);
    g_assert_cmphex(visible_bitmap[1], ==, 0);
    g_assert_cmphex(owned_bitmap[0], ==, 0);
}

static void test_header_abort_generation_only_matches_expected(void)
{
    CXLHybridControlHeader hdr = {
        .generation = 7,
    };

    g_assert_false(cxl_hybrid_control_abort_generation(&hdr, 6));
    g_assert_cmpuint(cxl_hybrid_control_generation(&hdr), ==, 7);

    g_assert_true(cxl_hybrid_control_abort_generation(&hdr, 7));
    g_assert_cmpuint(cxl_hybrid_control_generation(&hdr), ==, 8);
    g_assert_false(cxl_hybrid_control_generation_matches(&hdr, 7));
    g_assert_true(cxl_hybrid_control_generation_matches(&hdr, 8));
}

static void test_header_abort_generation_wraps(void)
{
    CXLHybridControlHeader hdr = {
        .generation = UINT32_MAX,
    };

    g_assert_true(cxl_hybrid_control_abort_generation(&hdr, UINT32_MAX));
    g_assert_cmpuint(cxl_hybrid_control_generation(&hdr), ==, 0);
}

static void test_header_source_write_count_balances(void)
{
    CXLHybridControlHeader hdr = { 0 };

    g_assert_cmpuint(cxl_hybrid_control_source_write_count(&hdr), ==, 0);
    g_assert_cmpuint(cxl_hybrid_control_source_write_begin(&hdr), ==, 1);
    g_assert_cmpuint(cxl_hybrid_control_source_write_begin(&hdr), ==, 2);
    g_assert_cmpuint(cxl_hybrid_control_source_write_count(&hdr), ==, 2);
    g_assert_cmpuint(cxl_hybrid_control_source_write_end(&hdr), ==, 1);
    g_assert_cmpuint(cxl_hybrid_control_source_write_end(&hdr), ==, 0);
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

static void test_owned_region_bitmap_bytes_round_up_by_ulong(void)
{
    g_assert_cmpuint(cxl_hybrid_control_owned_region_bitmap_words(0), ==, 0);
    g_assert_cmpuint(cxl_hybrid_control_owned_region_bitmap_bytes(0), ==, 0);

    g_assert_cmpuint(cxl_hybrid_control_owned_region_bitmap_words(1), ==, 1);
    g_assert_cmpuint(cxl_hybrid_control_owned_region_bitmap_bytes(1), ==,
                     sizeof(unsigned long));

    g_assert_cmpuint(
        cxl_hybrid_control_owned_region_bitmap_words(BITS_PER_LONG), ==, 1);
    g_assert_cmpuint(
        cxl_hybrid_control_owned_region_bitmap_bytes(BITS_PER_LONG),
        ==, sizeof(unsigned long));

    g_assert_cmpuint(
        cxl_hybrid_control_owned_region_bitmap_words(BITS_PER_LONG + 1),
        ==, 2);
    g_assert_cmpuint(
        cxl_hybrid_control_owned_region_bitmap_bytes(BITS_PER_LONG + 1),
        ==, 2 * sizeof(unsigned long));
}

static void test_visible_bitmap_respects_generation(void)
{
    CXLHybridControlHeader hdr = {
        .generation = 7,
        .visible_page_words = 2,
        .total_pages = 2 * BITS_PER_LONG,
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

static void test_visible_bitmap_rejects_padded_tail(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(BITS_PER_LONG + 1)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, BITS_PER_LONG + 1,
                                       NULL, 0, 0, 8);

    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible, 0, 0,
                                                     8));
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible,
                                                     UINT64_MAX, 2, 8));
    cxl_hybrid_control_mark_page_visible(&hdr, visible, BITS_PER_LONG + 1);
    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible,
                                                   BITS_PER_LONG + 1, 8));
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible,
                                                     BITS_PER_LONG, 2, 8));
}

static void test_region_owned_bitmap_respects_generation(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024, owned, 8,
                                       64 * 1024, 11);

    g_assert_false(cxl_hybrid_control_region_owned(&hdr, owned, 3, 10));
    g_assert_false(cxl_hybrid_control_region_owned(&hdr, owned, 3, 11));
    cxl_hybrid_control_mark_region_owned(&hdr, owned, 3);
    g_assert_true(cxl_hybrid_control_region_owned(&hdr, owned, 3, 11));
    g_assert_false(cxl_hybrid_control_region_owned(&hdr, owned, 3, 12));
}

static void test_region_owned_bitmap_rejects_padded_tail(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long owned[BITS_TO_LONGS(BITS_PER_LONG + 1)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, NULL, 0, owned,
                                       BITS_PER_LONG + 1, 64 * 1024, 9);

    cxl_hybrid_control_mark_region_owned(&hdr, owned, BITS_PER_LONG + 1);
    g_assert_false(cxl_hybrid_control_region_owned(&hdr, owned,
                                                   BITS_PER_LONG + 1, 9));
}

static void test_region_visibility_requires_all_pages(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024, owned, 8,
                                       64 * 1024, 4);

    cxl_hybrid_control_mark_page_visible(&hdr, visible, 10);
    cxl_hybrid_control_mark_page_visible(&hdr, visible, 11);
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible, 10, 3,
                                                     4));
    cxl_hybrid_control_mark_page_visible(&hdr, visible, 12);
    g_assert_true(cxl_hybrid_control_region_visible(&hdr, visible, 10, 3,
                                                    4));
}

static bool test_resolve_page_bitmap(uint64_t page_index, void *opaque)
{
    const unsigned long *resolved = opaque;

    return test_bit(page_index, resolved);
}

static void test_region_resolution_rejects_hole(void)
{
    unsigned long resolved[BITS_TO_LONGS(64)] = { 0 };
    uint64_t unresolved_page = UINT64_MAX;

    bitmap_set(resolved, 10, 2);
    bitmap_set(resolved, 13, 1);

    g_assert_false(cxl_hybrid_control_page_range_resolved(
        10, 4, test_resolve_page_bitmap, resolved, &unresolved_page));
    g_assert_cmpuint(unresolved_page, ==, 12);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cxl-hybrid-control/align-mapping-bytes-rounds-up-to-device-align",
                    test_align_mapping_bytes_rounds_up_to_device_align);
    g_test_add_func("/cxl-hybrid-control/header-reset-clears-visible-bitmap",
                    test_header_reset_clears_visible_bitmap);
    g_test_add_func("/cxl-hybrid-control/header-abort-generation",
                    test_header_abort_generation_only_matches_expected);
    g_test_add_func("/cxl-hybrid-control/header-abort-generation-wraps",
                    test_header_abort_generation_wraps);
    g_test_add_func("/cxl-hybrid-control/header-source-write-count-balances",
                    test_header_source_write_count_balances);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-supports-fixed-extent-io",
                    test_staging_shared_map_supports_fixed_extent_io);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-rejects-extent-overflow-without-growing",
                    test_staging_shared_map_rejects_extent_overflow_without_growing);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-rejects-base-beyond-extent",
                    test_staging_shared_map_rejects_base_beyond_extent);
    g_test_add_func("/cxl-hybrid-control/visible-bitmap-bytes-round-up-by-ulong",
                    test_visible_bitmap_bytes_round_up_by_ulong);
    g_test_add_func("/cxl-hybrid-control/owned-region-bitmap-bytes-round-up-by-ulong",
                    test_owned_region_bitmap_bytes_round_up_by_ulong);
    g_test_add_func("/cxl-hybrid-control/visible-bitmap-respects-generation",
                    test_visible_bitmap_respects_generation);
    g_test_add_func("/cxl-hybrid-control/visible-bitmap-rejects-padded-tail",
                    test_visible_bitmap_rejects_padded_tail);
    g_test_add_func("/cxl-hybrid-control/region-owned-bitmap-respects-generation",
                    test_region_owned_bitmap_respects_generation);
    g_test_add_func("/cxl-hybrid-control/region-owned-bitmap-rejects-padded-tail",
                    test_region_owned_bitmap_rejects_padded_tail);
    g_test_add_func("/cxl-hybrid-control/region-visibility-requires-all-pages",
                    test_region_visibility_requires_all_pages);
    g_test_add_func("/cxl-hybrid-control/region-resolution-rejects-hole",
                    test_region_resolution_rejects_hole);
    return g_test_run();
}
