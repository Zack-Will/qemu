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

#ifdef CXL_HYBRID_STREAM_FAULT_COOLDOWN_US_ENV
#error "CXL hybrid stream fault cooldown debug knob must not be compiled in"
#endif

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
        .generation = 99,
        .visible_page_words = 1234,
        .visible_region_words = 2345,
        .owned_region_words = 5678,
        .region_granule_shift = 12,
        .target_page_shift = 12,
        .total_pages = 999,
        .total_regions = 888,
        .region_granule = 4096,
        .request_prod = 10,
        .request_cons = 9,
        .active_enqueue_count = 4,
        .active_request_count = 3,
        .source_write_count = 33,
        .completed_generation = 99,
        .completion_flags = CXL_HYBRID_CTRL_COMPLETION_F_QUIESCE,
    };
    unsigned long visible_bitmap[2] = { ~0UL, ~0UL };
    unsigned long visible_region_bitmap[1] = { ~0UL };
    unsigned long owned_bitmap[1] = { ~0UL };

    cxl_hybrid_control_reset_run_state(&hdr, visible_bitmap,
                                       G_N_ELEMENTS(visible_bitmap) *
                                       BITS_PER_LONG,
                                       NULL, 0,
                                       visible_region_bitmap,
                                       G_N_ELEMENTS(visible_region_bitmap) *
                                       BITS_PER_LONG,
                                       owned_bitmap,
                                       G_N_ELEMENTS(owned_bitmap) *
                                       BITS_PER_LONG,
                                       64 * 1024,
                                       12,
                                       2);

    g_assert_cmphex(hdr.magic, ==, CXL_HYBRID_CTRL_MAGIC);
    g_assert_cmpuint(hdr.version, ==, CXL_HYBRID_CTRL_VERSION);
    g_assert_cmpuint(hdr.flags, ==, 0);
    g_assert_cmpuint(hdr.request_ring_order, ==, CXL_HYBRID_CTRL_REQUEST_ORDER);
    g_assert_cmpuint(hdr.generation, ==, 2);
    g_assert_cmpuint(hdr.visible_page_words, ==, G_N_ELEMENTS(visible_bitmap));
    g_assert_cmpuint(hdr.visible_region_words, ==,
                     G_N_ELEMENTS(visible_region_bitmap));
    g_assert_cmpuint(hdr.owned_region_words, ==, G_N_ELEMENTS(owned_bitmap));
    g_assert_cmpuint(hdr.region_granule_shift, ==, 16);
    g_assert_cmpuint(hdr.target_page_shift, ==, 12);
    g_assert_cmpuint(hdr.total_pages, ==,
                     G_N_ELEMENTS(visible_bitmap) * BITS_PER_LONG);
    g_assert_cmpuint(hdr.total_regions, ==,
                     G_N_ELEMENTS(owned_bitmap) * BITS_PER_LONG);
    g_assert_cmpuint(hdr.region_granule, ==, 64 * 1024);
    g_assert_cmpuint(hdr.request_prod, ==, 0);
    g_assert_cmpuint(hdr.request_cons, ==, 0);
    g_assert_cmpuint(hdr.active_enqueue_count, ==, 0);
    g_assert_cmpuint(hdr.active_request_count, ==, 0);
    g_assert_cmpuint(hdr.source_write_count, ==, 0);
    g_assert_cmpuint(hdr.completed_generation, ==, 0);
    g_assert_cmpuint(hdr.completion_flags, ==, 0);
    g_assert_cmphex(visible_bitmap[0], ==, 0);
    g_assert_cmphex(visible_bitmap[1], ==, 0);
    g_assert_cmphex(visible_region_bitmap[0], ==, 0);
    g_assert_cmphex(owned_bitmap[0], ==, 0);
}

static void test_page_state_bitmap_bytes_match_total_pages(void)
{
    g_assert_cmpuint(cxl_hybrid_control_page_state_words(0), ==, 0);
    g_assert_cmpuint(cxl_hybrid_control_page_state_words(1), ==, 1);
    g_assert_cmpuint(cxl_hybrid_control_page_state_words(UINT32_MAX), ==,
                     UINT32_MAX);
    g_assert_cmpuint(cxl_hybrid_control_page_state_words(
                         (uint64_t)UINT32_MAX + 1), ==, SIZE_MAX);

    g_assert_cmpuint(cxl_hybrid_control_page_state_bytes(0), ==, 0);
    g_assert_cmpuint(cxl_hybrid_control_page_state_bytes(1), ==,
                     sizeof(uint64_t));
    g_assert_cmpuint(cxl_hybrid_control_page_state_bytes(65), ==,
                     65 * sizeof(uint64_t));
}

static void test_header_reset_initializes_page_state_words(void)
{
    CXLHybridControlHeader hdr = { 0 };
    uint64_t page_state[4] = { UINT64_MAX, UINT64_MAX, UINT64_MAX,
                               UINT64_MAX };

    cxl_hybrid_control_reset_run_state(&hdr, NULL,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, 5);

    g_assert_cmpuint(hdr.page_state_words, ==, G_N_ELEMENTS(page_state));
    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[i]), ==,
                         CXL_HYBRID_PAGE_STATE_NOT_SENT);
        g_assert_cmpuint(cxl_hybrid_page_state_generation(page_state[i]), ==,
                         5);
    }
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

static void test_header_fault_pressure_tracks_pending_and_active_requests(void)
{
    CXLHybridControlHeader hdr = {
        .generation = 4,
    };

    g_assert_false(cxl_hybrid_control_fault_pressure(&hdr, 4));

    hdr.request_prod = 11;
    hdr.request_cons = 10;
    g_assert_true(cxl_hybrid_control_fault_pressure(&hdr, 4));

    hdr.request_cons = 11;
    g_assert_false(cxl_hybrid_control_fault_pressure(&hdr, 4));

    hdr.active_request_count = 1;
    g_assert_true(cxl_hybrid_control_fault_pressure(&hdr, 4));

    hdr.active_request_count = 0;
    hdr.active_enqueue_count = 1;
    g_assert_true(cxl_hybrid_control_fault_pressure(&hdr, 4));

    g_assert_false(cxl_hybrid_control_fault_pressure(&hdr, 5));
}

static void test_page_state_claim_and_complete_cxl(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_not_sent(7);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_CXL, 7, &claim));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_IN_FLIGHT);
    g_assert_cmpuint(cxl_hybrid_page_state_owner(slot), ==,
                     CXL_HYBRID_PAGE_OWNER_CXL);

    g_assert_true(cxl_hybrid_page_state_complete(
        &slot, &claim, CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_PUBLISHED);
    g_assert_cmpuint(cxl_hybrid_page_state_location(slot), ==,
                     CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 7, CXL_HYBRID_PAGE_LOCATION_CXL));
}

static void test_page_state_dirty_makes_rdma_completion_stale(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(3, 11);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_RDMA, 3, &claim));
    cxl_hybrid_page_state_mark_dirty(&slot, 3, 12);
    g_assert_false(cxl_hybrid_page_state_complete(
        &slot, &claim, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_dirty_seq(slot), ==, 12);
}

static void test_page_state_rejects_double_claim(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_not_sent(9);
    CXLHybridPageClaim cxl_claim = { 0 };
    CXLHybridPageClaim rdma_claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_CXL, 9, &cxl_claim));
    g_assert_false(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_RDMA, 9, &rdma_claim));
}

static void test_page_state_matches_encoded_generation(void)
{
    uint32_t generation = 0x10007;
    uint64_t slot = cxl_hybrid_page_state_make_not_sent(generation);
    CXLHybridPageClaim claim = { 0 };

    g_assert_cmpuint(cxl_hybrid_page_state_generation(slot), ==, 7);
    g_assert_true(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_CXL, generation, &claim));
    g_assert_cmpuint(claim.generation, ==, 7);
    g_assert_true(cxl_hybrid_page_state_complete(
        &slot, &claim, CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, generation, CXL_HYBRID_PAGE_LOCATION_CXL));
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
                                       NULL, 0, NULL, 0, NULL, 0, 0, 12, 8);

    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible, NULL,
                                                     0, 0, 8));
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible, NULL,
                                                     UINT64_MAX, 2, 8));
    cxl_hybrid_control_mark_page_visible(&hdr, visible, BITS_PER_LONG + 1);
    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible,
                                                   BITS_PER_LONG + 1, 8));
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible, NULL,
                                                     BITS_PER_LONG, 2, 8));
}

static void test_region_owned_bitmap_respects_generation(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024, NULL, 0,
                                       NULL, 0, owned, 8, 64 * 1024, 12, 11);

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

    cxl_hybrid_control_reset_run_state(&hdr, NULL, 0, NULL, 0, NULL, 0,
                                       owned, BITS_PER_LONG + 1,
                                       64 * 1024, 12, 9);

    cxl_hybrid_control_mark_region_owned(&hdr, owned, BITS_PER_LONG + 1);
    g_assert_false(cxl_hybrid_control_region_owned(&hdr, owned,
                                                   BITS_PER_LONG + 1, 9));
}

static void test_region_visibility_requires_all_pages(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024, NULL, 0,
                                       NULL, 0, owned, 8, 64 * 1024, 12, 4);

    cxl_hybrid_control_mark_page_visible(&hdr, visible, 10);
    cxl_hybrid_control_mark_page_visible(&hdr, visible, 11);
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible, NULL,
                                                     10, 3, 4));
    cxl_hybrid_control_mark_page_visible(&hdr, visible, 12);
    g_assert_true(cxl_hybrid_control_region_visible(&hdr, visible, NULL,
                                                    10, 3, 4));
}

static void test_region_visible_bitmap_marks_region_visible(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible, 512, 4));
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible,
                                                     visible_regions, 512,
                                                     128, 4));

    cxl_hybrid_control_mark_region_visible(&hdr, visible_regions, 4);

    g_assert_false(cxl_hybrid_control_page_visible(&hdr, visible, 512, 4));
    g_assert_true(cxl_hybrid_control_region_visible(&hdr, visible,
                                                    visible_regions, 512,
                                                    128, 4));
    g_assert_false(cxl_hybrid_control_region_visible(&hdr, visible,
                                                     visible_regions, 640,
                                                     128, 4));
}

static void test_region_bit_visible_synthesizes_from_page_bitmap(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);
    bitmap_set(visible, 512, 128);

    g_assert_true(cxl_hybrid_control_region_visible(&hdr, visible,
                                                    visible_regions, 512,
                                                    128, 4));
    g_assert_false(cxl_hybrid_control_region_bit_visible(&hdr,
                                                        visible_regions,
                                                        512, 128, 4));
    g_assert_true(cxl_hybrid_control_region_visible_or_synthesize(
                      &hdr, visible, visible_regions, 512, 128, 4));
    g_assert_true(test_bit(4, visible_regions));

    bitmap_clear(visible_regions, 0, 8);
    g_assert_false(cxl_hybrid_control_region_visible_or_synthesize(
                       &hdr, visible, visible_regions, 512, 128, 3));
    g_assert_false(test_bit(4, visible_regions));

    bitmap_clear(visible, 600, 1);

    g_assert_false(cxl_hybrid_control_region_bit_visible(&hdr,
                                                        visible_regions,
                                                        512, 128, 4));
    g_assert_false(cxl_hybrid_control_region_visible_or_synthesize(
                       &hdr, visible, visible_regions, 512, 128, 4));
}

static void test_region_span_valid_requires_complete_region(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };
    uint64_t region_index = UINT64_MAX;

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    g_assert_true(cxl_hybrid_control_region_span_valid(&hdr, 512, 128));
    g_assert_true(cxl_hybrid_control_region_span_index(&hdr, 512, 128,
                                                       &region_index));
    g_assert_cmpuint(region_index, ==, 4);
    g_assert_false(cxl_hybrid_control_region_span_valid(&hdr, 513, 127));
    g_assert_false(cxl_hybrid_control_region_span_index(&hdr, 513, 127,
                                                        &region_index));
    g_assert_false(cxl_hybrid_control_region_span_valid(&hdr, 512, 64));
    g_assert_false(cxl_hybrid_control_region_span_valid(&hdr, 1024, 128));
}

static void test_set_page_visible_rejects_stale_generation(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, NULL, 512, 3, CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_false(test_bit(512, visible));
    cxl_hybrid_control_mark_region_visible_generation(&hdr, visible_regions,
                                                      4, 3);
    g_assert_false(test_bit(4, visible_regions));

    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, NULL, 512, 4, CXL_HYBRID_PAGE_LOCATION_CXL);
    cxl_hybrid_control_mark_region_visible_generation(&hdr, visible_regions,
                                                      4, 4);
    g_assert_true(test_bit(512, visible));
    g_assert_true(test_bit(4, visible_regions));
}

static void test_mark_page_visible_sets_page_state_cxl(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[8];

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, 4);

    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 3, 4,
        CXL_HYBRID_PAGE_LOCATION_CXL);

    g_assert_true(test_bit(3, visible));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[3]), ==,
                     CXL_HYBRID_PAGE_STATE_PUBLISHED);
    g_assert_cmpuint(cxl_hybrid_page_state_location(page_state[3]), ==,
                     CXL_HYBRID_PAGE_LOCATION_CXL);
}

static void test_set_pages_visible_marks_range_once_generation_matches(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    cxl_hybrid_control_mark_pages_visible_generation(
        &hdr, visible, NULL, 100, 200, 3, CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_false(test_bit(100, visible));
    g_assert_false(test_bit(299, visible));

    cxl_hybrid_control_mark_pages_visible_generation(
        &hdr, visible, NULL, 100, 200, 4, CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_false(test_bit(99, visible));
    g_assert_true(test_bit(100, visible));
    g_assert_true(test_bit(299, visible));
    g_assert_false(test_bit(300, visible));

    cxl_hybrid_control_mark_pages_visible_generation(
        &hdr, visible, NULL, 900, 200, 4, CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_false(test_bit(900, visible));
}

static void test_mark_region_visible_for_complete_span_generation(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    g_assert_false(cxl_hybrid_control_mark_region_visible_for_span_generation(
                       &hdr, visible_regions, 513, 127, 4));
    g_assert_false(test_bit(4, visible_regions));
    g_assert_false(cxl_hybrid_control_mark_region_visible_for_span_generation(
                       &hdr, visible_regions, 512, 128, 3));
    g_assert_false(test_bit(4, visible_regions));

    g_assert_true(cxl_hybrid_control_mark_region_visible_for_span_generation(
                      &hdr, visible_regions, 512, 128, 4));
    g_assert_true(test_bit(4, visible_regions));
}

static void test_mark_visible_region_span_requires_complete_span(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    g_assert_false(cxl_hybrid_control_mark_visible_region_span_generation(
                       &hdr, visible, visible_regions, NULL, 512, 64, 4,
                       CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_false(test_bit(512, visible));
    g_assert_false(test_bit(575, visible));
    g_assert_false(test_bit(4, visible_regions));

    g_assert_true(cxl_hybrid_control_mark_visible_region_span_generation(
                      &hdr, visible, visible_regions, NULL, 512, 128, 4,
                      CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_false(test_bit(511, visible));
    g_assert_true(test_bit(512, visible));
    g_assert_true(test_bit(639, visible));
    g_assert_false(test_bit(640, visible));
    g_assert_true(test_bit(4, visible_regions));
}

static void test_mark_visible_region_span_rejects_stale_generation(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    g_assert_false(cxl_hybrid_control_mark_visible_region_span_generation(
                       &hdr, visible, visible_regions, NULL, 512, 128, 3,
                       CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_false(test_bit(512, visible));
    g_assert_false(test_bit(639, visible));
    g_assert_false(test_bit(4, visible_regions));
}

static void test_partial_remap_span_marks_pages_without_region_bit(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    cxl_hybrid_control_mark_pages_visible_generation(
        &hdr, visible, NULL, 512, 64, 4, CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_true(test_bit(512, visible));
    g_assert_true(test_bit(575, visible));
    g_assert_false(test_bit(4, visible_regions));
    g_assert_false(cxl_hybrid_control_region_visible_or_synthesize(
                       &hdr, visible, visible_regions, 512, 128, 4));

    cxl_hybrid_control_mark_pages_visible_generation(
        &hdr, visible, NULL, 576, 64, 4, CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_true(cxl_hybrid_control_region_visible_or_synthesize(
                      &hdr, visible, visible_regions, 512, 128, 4));
    g_assert_true(test_bit(4, visible_regions));
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

static void test_fault_generation_uses_stable_source_run(void)
{
    g_assert_cmpuint(cxl_hybrid_select_fault_publish_generation(
                         false, 0, false, 0, 5), ==, 5);

    g_assert_cmpuint(cxl_hybrid_select_fault_publish_generation(
                         false, 0, true, 2, 3), ==, 2);

    g_assert_cmpuint(cxl_hybrid_select_fault_publish_generation(
                         true, 7, true, 2, 3), ==, 7);
}

static void test_switch_policy_enters_brake_before_coverage_switch(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BULK,
        .brake_enabled = true,
        .max_iters_trigger = true,
        .remap_coverage_threshold = 80,
        .remap_coverage = 0,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==, CXL_HYBRID_SWITCH_ACTION_ENTER_BRAKE);
    g_assert_cmpint(decision.reason, ==, CXL_MIGRATION_SWITCH_REASON_NONE);
}

static void test_switch_policy_waits_for_remap_coverage_in_brake(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .max_iters_trigger = true,
        .gain_trigger = true,
        .remap_coverage_threshold = 80,
        .remap_coverage = 79,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==, CXL_HYBRID_SWITCH_ACTION_NONE);
    g_assert_cmpint(decision.reason, ==, CXL_MIGRATION_SWITCH_REASON_NONE);
}

static void test_switch_policy_uses_remap_coverage_in_brake(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .max_iters_trigger = true,
        .remap_coverage_threshold = 80,
        .remap_coverage = 80,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==,
                    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY);
    g_assert_cmpint(decision.reason, ==,
                    CXL_MIGRATION_SWITCH_REASON_REMAP_COVERAGE);
}

static void test_switch_policy_waits_for_remap_coverage_before_precopy_complete(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .completion_ready = true,
        .staged_pages = 100,
        .remap_coverage_threshold = 80,
        .remap_coverage = 79,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==, CXL_HYBRID_SWITCH_ACTION_NONE);
    g_assert_cmpint(decision.reason, ==, CXL_MIGRATION_SWITCH_REASON_NONE);
}

static void test_switch_policy_uses_precopy_complete_without_coverage_gate(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .completion_ready = true,
        .staged_pages = 100,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==,
                    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY);
    g_assert_cmpint(decision.reason, ==,
                    CXL_MIGRATION_SWITCH_REASON_PRECOPY_COMPLETE);
}

static void test_switch_policy_uses_remap_coverage_before_precopy_complete(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .completion_ready = true,
        .staged_pages = 100,
        .remap_coverage_threshold = 80,
        .remap_coverage = 80,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==,
                    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY);
    g_assert_cmpint(decision.reason, ==,
                    CXL_MIGRATION_SWITCH_REASON_REMAP_COVERAGE);
}

static void test_switch_policy_enters_brake_for_bulk_complete_with_coverage(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BULK,
        .brake_enabled = true,
        .completion_ready = true,
        .max_iters_trigger = true,
        .staged_pages = 100,
        .remap_coverage_threshold = 80,
        .remap_coverage = 0,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==, CXL_HYBRID_SWITCH_ACTION_ENTER_BRAKE);
    g_assert_cmpint(decision.reason, ==, CXL_MIGRATION_SWITCH_REASON_NONE);
}

static void test_switch_policy_uses_precopy_complete_from_bulk_without_coverage(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BULK,
        .brake_enabled = true,
        .completion_ready = true,
        .max_iters_trigger = true,
        .staged_pages = 100,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==,
                    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY);
    g_assert_cmpint(decision.reason, ==,
                    CXL_MIGRATION_SWITCH_REASON_PRECOPY_COMPLETE);
}

static void test_switch_policy_ignores_precopy_complete_without_staged_pages(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .completion_ready = true,
        .staged_pages = 0,
        .remap_coverage_threshold = 80,
        .remap_coverage = 0,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==, CXL_HYBRID_SWITCH_ACTION_NONE);
    g_assert_cmpint(decision.reason, ==, CXL_MIGRATION_SWITCH_REASON_NONE);
}

static void test_switch_policy_keeps_remaining_and_time_cap_fallbacks(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .remaining_trigger = true,
        .max_iters_trigger = true,
        .remap_coverage_threshold = 80,
        .remap_coverage = 0,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==,
                    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY);
    g_assert_cmpint(decision.reason, ==,
                    CXL_MIGRATION_SWITCH_REASON_REMAINING_SMALL);

    input.remaining_trigger = false;
    input.time_cap_trigger = true;
    decision = cxl_hybrid_switch_decide(&input);
    g_assert_cmpint(decision.action, ==,
                    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY);
    g_assert_cmpint(decision.reason, ==,
                    CXL_MIGRATION_SWITCH_REASON_PRECOPY_TIME_CAP);
}

static void test_switch_policy_preserves_legacy_brake_without_coverage(void)
{
    CXLHybridSwitchPolicyInput input = {
        .phase = CXL_HYBRID_PHASE_PRECOPY_BRAKE,
        .brake_enabled = true,
        .max_iters_trigger = true,
    };
    CXLHybridSwitchDecision decision = cxl_hybrid_switch_decide(&input);

    g_assert_cmpint(decision.action, ==,
                    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY);
    g_assert_cmpint(decision.reason, ==,
                    CXL_MIGRATION_SWITCH_REASON_MAX_ITERS);
}

static void test_source_remap_coverage_uses_staged_pages_denominator(void)
{
    g_assert_cmpuint(cxl_hybrid_calculate_source_remap_coverage(0, 10), ==, 0);
    g_assert_cmpuint(cxl_hybrid_calculate_source_remap_coverage(100, 0), ==, 0);
    g_assert_cmpuint(cxl_hybrid_calculate_source_remap_coverage(100, 79), ==,
                     79);
    g_assert_cmpuint(cxl_hybrid_calculate_source_remap_coverage(100, 100), ==,
                     100);
    g_assert_cmpuint(cxl_hybrid_calculate_source_remap_coverage(100, 120), ==,
                     100);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cxl-hybrid-control/align-mapping-bytes-rounds-up-to-device-align",
                    test_align_mapping_bytes_rounds_up_to_device_align);
    g_test_add_func("/cxl-hybrid-control/header-reset-clears-visible-bitmap",
                    test_header_reset_clears_visible_bitmap);
    g_test_add_func("/cxl-hybrid-control/page-state-bytes",
                    test_page_state_bitmap_bytes_match_total_pages);
    g_test_add_func("/cxl-hybrid-control/header-reset-page-state",
                    test_header_reset_initializes_page_state_words);
    g_test_add_func("/cxl-hybrid-control/header-abort-generation",
                    test_header_abort_generation_only_matches_expected);
    g_test_add_func("/cxl-hybrid-control/header-abort-generation-wraps",
                    test_header_abort_generation_wraps);
    g_test_add_func("/cxl-hybrid-control/header-source-write-count-balances",
                    test_header_source_write_count_balances);
    g_test_add_func("/cxl-hybrid-control/header-fault-pressure-tracks-pending-and-active-requests",
                    test_header_fault_pressure_tracks_pending_and_active_requests);
    g_test_add_func("/cxl-hybrid-control/page-state-claim-complete-cxl",
                    test_page_state_claim_and_complete_cxl);
    g_test_add_func("/cxl-hybrid-control/page-state-dirty-stales-rdma",
                    test_page_state_dirty_makes_rdma_completion_stale);
    g_test_add_func("/cxl-hybrid-control/page-state-rejects-double-claim",
                    test_page_state_rejects_double_claim);
    g_test_add_func("/cxl-hybrid-control/page-state-matches-encoded-generation",
                    test_page_state_matches_encoded_generation);
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
    g_test_add_func("/cxl-hybrid-control/region-visible-bitmap-marks-region-visible",
                    test_region_visible_bitmap_marks_region_visible);
    g_test_add_func("/cxl-hybrid-control/region-bit-visible-synthesizes-from-page-bitmap",
                    test_region_bit_visible_synthesizes_from_page_bitmap);
    g_test_add_func("/cxl-hybrid-control/region-span-valid-requires-complete-region",
                    test_region_span_valid_requires_complete_region);
    g_test_add_func("/cxl-hybrid-control/set-page-visible-rejects-stale-generation",
                    test_set_page_visible_rejects_stale_generation);
    g_test_add_func("/cxl-hybrid-control/page-visible-mirrors-page-state",
                    test_mark_page_visible_sets_page_state_cxl);
    g_test_add_func("/cxl-hybrid-control/set-pages-visible-marks-range-once-generation-matches",
                    test_set_pages_visible_marks_range_once_generation_matches);
    g_test_add_func("/cxl-hybrid-control/mark-region-visible-for-complete-span-generation",
                    test_mark_region_visible_for_complete_span_generation);
    g_test_add_func("/cxl-hybrid-control/mark-visible-region-span-complete",
                    test_mark_visible_region_span_requires_complete_span);
    g_test_add_func("/cxl-hybrid-control/mark-visible-region-span-rejects-stale-generation",
                    test_mark_visible_region_span_rejects_stale_generation);
    g_test_add_func("/cxl-hybrid-control/partial-remap-span-marks-pages-without-region-bit",
                    test_partial_remap_span_marks_pages_without_region_bit);
    g_test_add_func("/cxl-hybrid-control/region-resolution-rejects-hole",
                    test_region_resolution_rejects_hole);
    g_test_add_func("/cxl-hybrid-control/fault-generation-uses-stable-source-run",
                    test_fault_generation_uses_stable_source_run);
    g_test_add_func("/cxl-hybrid-control/switch-policy-enters-brake-before-coverage-switch",
                    test_switch_policy_enters_brake_before_coverage_switch);
    g_test_add_func("/cxl-hybrid-control/switch-policy-waits-for-remap-coverage-in-brake",
                    test_switch_policy_waits_for_remap_coverage_in_brake);
    g_test_add_func("/cxl-hybrid-control/switch-policy-uses-remap-coverage-in-brake",
                    test_switch_policy_uses_remap_coverage_in_brake);
    g_test_add_func("/cxl-hybrid-control/switch-policy-waits-for-coverage-before-precopy-complete",
                    test_switch_policy_waits_for_remap_coverage_before_precopy_complete);
    g_test_add_func("/cxl-hybrid-control/switch-policy-uses-precopy-complete-without-coverage-gate",
                    test_switch_policy_uses_precopy_complete_without_coverage_gate);
    g_test_add_func("/cxl-hybrid-control/switch-policy-uses-coverage-before-precopy-complete",
                    test_switch_policy_uses_remap_coverage_before_precopy_complete);
    g_test_add_func("/cxl-hybrid-control/switch-policy-enters-brake-for-bulk-complete-with-coverage",
                    test_switch_policy_enters_brake_for_bulk_complete_with_coverage);
    g_test_add_func("/cxl-hybrid-control/switch-policy-uses-precopy-complete-from-bulk-without-coverage",
                    test_switch_policy_uses_precopy_complete_from_bulk_without_coverage);
    g_test_add_func("/cxl-hybrid-control/switch-policy-ignores-precopy-complete-without-staged-pages",
                    test_switch_policy_ignores_precopy_complete_without_staged_pages);
    g_test_add_func("/cxl-hybrid-control/switch-policy-keeps-remaining-and-time-cap-fallbacks",
                    test_switch_policy_keeps_remaining_and_time_cap_fallbacks);
    g_test_add_func("/cxl-hybrid-control/switch-policy-preserves-legacy-brake-without-coverage",
                    test_switch_policy_preserves_legacy_brake_without_coverage);
    g_test_add_func("/cxl-hybrid-control/source-remap-coverage-uses-staged-pages-denominator",
                    test_source_remap_coverage_uses_staged_pages_denominator);
    return g_test_run();
}
