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
    g_test_add_func("/cxl-hybrid-control/align-mapping-bytes-rounds-up-to-device-align",
                    test_align_mapping_bytes_rounds_up_to_device_align);
    g_test_add_func("/cxl-hybrid-control/header-reset-clears-visible-bitmap",
                    test_header_reset_clears_visible_bitmap);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-supports-fixed-extent-io",
                    test_staging_shared_map_supports_fixed_extent_io);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-rejects-extent-overflow-without-growing",
                    test_staging_shared_map_rejects_extent_overflow_without_growing);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-rejects-base-beyond-extent",
                    test_staging_shared_map_rejects_base_beyond_extent);
    g_test_add_func("/cxl-hybrid-control/visible-bitmap-bytes-round-up-by-ulong",
                    test_visible_bitmap_bytes_round_up_by_ulong);
    g_test_add_func("/cxl-hybrid-control/visible-bitmap-respects-generation",
                    test_visible_bitmap_respects_generation);
    return g_test_run();
}
