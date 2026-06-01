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
#define CXL_HYBRID_TEST_PAGE_SIZE 4096ULL

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

static void test_staging_shared_map_reads_external_cxl_offset_without_slot(void)
{
    g_autofree char *path = NULL;
    Error *err = NULL;
    int fd;
    int ret;
    uint8_t in[4096];
    uint8_t out[4096];
    off_t external_offset = 2 * MiB;

    fd = g_file_open_tmp("test-cxl-hybrid-external-read-XXXXXX",
                         &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 8 * MiB), ==, 0);

    ret = cxl_hybrid_dst_staging_init_fixed_fd(fd, 2 * MiB, 4 * MiB,
                                               8 * MiB, true, &err);
    if (err) {
        error_free_or_abort(&err);
    }
    g_assert_cmpint(ret, ==, 0);

    memset(in, 0xa5, sizeof(in));
    memset(out, 0, sizeof(out));
    g_assert_cmpint(pwrite(fd, in, sizeof(in), external_offset), ==,
                    sizeof(in));

    ret = cxl_hybrid_dst_staging_read_external(external_offset, out,
                                               sizeof(out), &err);
    if (err) {
        error_free_or_abort(&err);
    }
    g_assert_cmpint(ret, ==, 0);
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));

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

static void test_page_state_snapshot_counts_current_generation(void)
{
    uint32_t generation = 9;
    uint64_t page_state[8];
    unsigned long visible[1] = { 0 };
    CXLHybridPageClaim claim = { 0 };
    CXLHybridPageStateSnapshot snapshot = { 0 };

    page_state[0] = cxl_hybrid_page_state_make_not_sent(generation);
    page_state[1] = cxl_hybrid_page_state_make_dirty(generation, 1);
    page_state[2] = cxl_hybrid_page_state_make_dirty(generation, 2);
    g_assert_true(cxl_hybrid_page_state_claim_for_cxl(&page_state[2],
                                                      generation, &claim));
    page_state[3] = cxl_hybrid_page_state_make_dirty(generation, 3);
    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&page_state[3],
                                                       generation, &claim));
    page_state[4] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 4);
    page_state[5] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL, 5);
    page_state[6] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_ZERO, 6);
    page_state[7] = cxl_hybrid_page_state_make_dirty(generation + 1, 7);

    set_bit(4, visible);
    set_bit(5, visible);

    cxl_hybrid_page_state_snapshot(page_state, visible,
                                   G_N_ELEMENTS(page_state), generation,
                                   &snapshot);

    g_assert_cmpuint(snapshot.total_pages, ==, G_N_ELEMENTS(page_state));
    g_assert_cmpuint(snapshot.generation_mismatch, ==, 1);
    g_assert_cmpuint(snapshot.not_sent, ==, 1);
    g_assert_cmpuint(snapshot.dirty, ==, 1);
    g_assert_cmpuint(snapshot.in_flight, ==, 2);
    g_assert_cmpuint(snapshot.in_flight_cxl, ==, 1);
    g_assert_cmpuint(snapshot.in_flight_rdma, ==, 1);
    g_assert_cmpuint(snapshot.published, ==, 3);
    g_assert_cmpuint(snapshot.published_cxl, ==, 1);
    g_assert_cmpuint(snapshot.published_dst_local, ==, 1);
    g_assert_cmpuint(snapshot.published_zero, ==, 1);
    g_assert_cmpuint(snapshot.visible, ==, 2);
    g_assert_cmpuint(snapshot.published_invisible, ==, 1);
    g_assert_cmpuint(snapshot.other, ==, 0);
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

static void test_cxl_page_claim_complete_publishes_cxl(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(4, 88);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_cxl(&slot, 4, &claim));
    g_assert_cmpuint(claim.owner, ==, CXL_HYBRID_PAGE_OWNER_CXL);
    g_assert_true(cxl_hybrid_page_state_complete_cxl(&slot, &claim));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 4, CXL_HYBRID_PAGE_LOCATION_CXL));
}

static void test_cxl_descriptor_completion_skips_stale_page(void)
{
    uint64_t page_state[3];
    CXLHybridPageClaim claims[3];
    uint32_t generation = 11;
    uint32_t completed = 0;
    uint32_t stale = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i);
        g_assert_true(cxl_hybrid_page_state_claim_for_cxl(
            &page_state[i], generation, &claims[i]));
    }
    cxl_hybrid_page_state_mark_dirty(&page_state[1], generation, 44);

    cxl_hybrid_cxl_descriptor_complete_pages_for_test(
        page_state, G_N_ELEMENTS(page_state), 0, claims, 3,
        &completed, &stale);
    g_assert_cmpuint(completed, ==, 2);
    g_assert_cmpuint(stale, ==, 1);
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[0], generation, CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[1]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
}

static void test_complete_cxl_page_visible_publishes_matching_claim(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    CXLHybridPageClaim claim = { 0 };
    uint32_t generation = 7;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    page_state[2] = cxl_hybrid_page_state_make_dirty(generation, 1);
    g_assert_true(cxl_hybrid_page_state_claim_for_cxl(&page_state[2],
                                                      generation, &claim));

    g_assert_true(cxl_hybrid_control_complete_cxl_page_visible_generation(
        &hdr, visible, page_state, 2, generation, &claim));

    g_assert_true(test_bit(2, visible));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[2], generation, CXL_HYBRID_PAGE_LOCATION_CXL));
}

static void test_complete_cxl_page_visible_skips_stale_page(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    CXLHybridPageClaim claim = { 0 };
    uint32_t generation = 8;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    page_state[2] = cxl_hybrid_page_state_make_dirty(generation, 1);
    g_assert_true(cxl_hybrid_page_state_claim_for_cxl(&page_state[2],
                                                      generation, &claim));
    cxl_hybrid_page_state_mark_dirty(&page_state[2], generation, 2);

    g_assert_false(cxl_hybrid_control_complete_cxl_page_visible_generation(
        &hdr, visible, page_state, 2, generation, &claim));

    g_assert_false(test_bit(2, visible));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[2]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_dirty_seq(page_state[2]), ==, 2);
}

static void test_complete_rdma_page_visible_publishes_dst_local(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    CXLHybridPageClaim claim = { 0 };
    uint32_t generation = 9;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    page_state[1] = cxl_hybrid_page_state_make_dirty(generation, 1);
    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&page_state[1],
                                                       generation, &claim));

    g_assert_true(cxl_hybrid_control_complete_rdma_page_visible_generation(
        &hdr, visible, page_state, 1, generation, &claim));

    g_assert_true(test_bit(1, visible));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[1], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
}

static void test_page_location_reports_visible_dst_local(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    CXLHybridPageLocation location = CXL_HYBRID_PAGE_LOCATION_NONE;
    uint32_t generation = 10;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 2, generation,
        CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);

    g_assert_true(cxl_hybrid_control_page_location(
        &hdr, visible, page_state, 2, generation, &location));
    g_assert_cmpuint(location, ==, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
    g_assert_false(cxl_hybrid_control_page_location(
        &hdr, visible, page_state, 2, generation + 1, &location));
}

static void test_visible_published_pages_need_install_until_received(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    CXLHybridPageLocation location = CXL_HYBRID_PAGE_LOCATION_NONE;
    uint32_t generation = 13;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 0, generation,
        CXL_HYBRID_PAGE_LOCATION_CXL);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 1, generation,
        CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 2, generation,
        CXL_HYBRID_PAGE_LOCATION_ZERO);

    g_assert_true(cxl_hybrid_control_page_requires_destination_install(
        &hdr, visible, page_state, 0, generation, false, &location));
    g_assert_cmpuint(location, ==, CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_true(cxl_hybrid_control_page_requires_destination_install(
        &hdr, visible, page_state, 1, generation, false, &location));
    g_assert_cmpuint(location, ==, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
    g_assert_false(cxl_hybrid_control_page_requires_destination_install(
        &hdr, visible, page_state, 0, generation, true, &location));
    g_assert_false(cxl_hybrid_control_page_requires_destination_install(
        &hdr, visible, page_state, 2, generation, false, &location));
    g_assert_false(cxl_hybrid_control_page_requires_destination_install(
        &hdr, visible, page_state, 3, generation, false, &location));
}

static void test_cxl_owned_pages_need_postcopy_discard(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[6];
    CXLHybridPageClaim cxl_claim = { 0 };
    CXLHybridPageClaim rdma_claim = { 0 };
    uint32_t generation = 14;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 0, generation,
        CXL_HYBRID_PAGE_LOCATION_CXL);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 1, generation,
        CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 2, generation,
        CXL_HYBRID_PAGE_LOCATION_ZERO);
    g_assert_true(cxl_hybrid_page_state_claim_for_cxl(&page_state[3],
                                                      generation, &cxl_claim));
    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&page_state[4],
                                                       generation, &rdma_claim));

    g_assert_true(cxl_hybrid_control_page_requires_postcopy_discard(
        &hdr, visible, page_state, 0, generation));
    g_assert_false(cxl_hybrid_control_page_requires_postcopy_discard(
        &hdr, visible, page_state, 1, generation));
    g_assert_false(cxl_hybrid_control_page_requires_postcopy_discard(
        &hdr, visible, page_state, 2, generation));
    g_assert_true(cxl_hybrid_control_page_requires_postcopy_discard(
        &hdr, visible, page_state, 3, generation));
    g_assert_false(cxl_hybrid_control_page_requires_postcopy_discard(
        &hdr, visible, page_state, 4, generation));
    g_assert_false(cxl_hybrid_control_page_requires_postcopy_discard(
        &hdr, visible, page_state, 5, generation));
    g_assert_false(cxl_hybrid_control_page_requires_postcopy_discard(
        &hdr, visible, page_state, 0, generation + 1));
}

static void test_fault_place_eexist_requires_received_repair(void)
{
    g_assert_true(cxl_hybrid_fault_place_result_satisfied(0, false));
    g_assert_true(cxl_hybrid_fault_place_result_satisfied(-EEXIST, true));
    g_assert_false(cxl_hybrid_fault_place_result_satisfied(-EEXIST, false));
    g_assert_false(cxl_hybrid_fault_place_result_satisfied(-EIO, true));
}

static void test_mark_page_dirty_clears_visible_and_stales_published(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    uint32_t generation = 11;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 2, generation,
        CXL_HYBRID_PAGE_LOCATION_CXL);

    cxl_hybrid_control_mark_page_dirty_generation(
        &hdr, visible, page_state, 2, generation, 77);

    g_assert_false(test_bit(2, visible));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[2]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_dirty_seq(page_state[2]), ==, 77);
}

static void test_mark_page_dirty_rejects_generation_mismatch(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    uint32_t generation = 12;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 1, generation,
        CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);

    cxl_hybrid_control_mark_page_dirty_generation(
        &hdr, visible, page_state, 1, generation + 1, 88);

    g_assert_true(test_bit(1, visible));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[1], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
}

static void test_mark_dirty_pages_stales_only_dirty_dst_local_pages(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    unsigned long dirty[1] = { 0 };
    uint64_t page_state[8];
    uint32_t generation = 13;
    uint64_t marked;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 2, generation,
        CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 3, generation,
        CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
    set_bit(8, dirty);

    marked = cxl_hybrid_control_mark_dirty_pages_generation(
        &hdr, visible, page_state, dirty, 8, 2, 2, generation, 91);

    g_assert_cmpuint(marked, ==, 1);
    g_assert_false(test_bit(2, visible));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[2]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_dirty_seq(page_state[2]), ==, 91);
    g_assert_true(test_bit(3, visible));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[3], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
}

static void test_rdma_page_claim_complete_publishes_dst_local(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_not_sent(5);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&slot, 5, &claim));
    g_assert_cmpuint(claim.owner, ==, CXL_HYBRID_PAGE_OWNER_RDMA);
    g_assert_true(cxl_hybrid_page_state_complete_rdma(&slot, &claim));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 5, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
}

static void test_page_state_drop_claim_restores_dirty(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(6, 42);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&slot, 6, &claim));
    g_assert_true(cxl_hybrid_page_state_drop_claim(&slot, &claim));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_dirty_seq(slot), ==, 42);
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

static void test_remap_span_grows_over_contiguous_cxl_pages(void)
{
    uint64_t page_state[8];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 13;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_published(
            generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    }
    page_state[1] = cxl_hybrid_page_state_make_dirty(generation, 2);
    page_state[6] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL, 0);

    g_assert_true(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 4, generation, 0, &span));
    g_assert_cmpuint(span.first_page, ==, 2);
    g_assert_cmpuint(span.nr_pages, ==, 4);
}

static void test_remap_span_rejects_non_cxl_fault_page(void)
{
    uint64_t page_state[4];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 3;

    page_state[0] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    page_state[1] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL, 0);
    page_state[2] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    page_state[3] = cxl_hybrid_page_state_make_not_sent(generation);

    g_assert_false(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 1, generation, 0, &span));
}

static void test_remap_span_honors_scan_cap(void)
{
    uint64_t page_state[16];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 9;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_published(
            generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    }

    g_assert_true(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 8, generation, 5, &span));
    g_assert_cmpuint(span.first_page, ==, 6);
    g_assert_cmpuint(span.nr_pages, ==, 5);
}

static void test_remap_span_reuses_unused_left_cap(void)
{
    uint64_t page_state[8];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 11;

    page_state[0] = cxl_hybrid_page_state_make_dirty(generation, 1);
    for (size_t i = 1; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_published(
            generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    }

    g_assert_true(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 2, generation, 5, &span));
    g_assert_cmpuint(span.first_page, ==, 1);
    g_assert_cmpuint(span.nr_pages, ==, 5);
}

static void test_remap_span_reuses_unused_right_cap(void)
{
    uint64_t page_state[8];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 12;

    for (size_t i = 0; i < 7; i++) {
        page_state[i] = cxl_hybrid_page_state_make_published(
            generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    }
    page_state[7] = cxl_hybrid_page_state_make_dirty(generation, 1);

    g_assert_true(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 5, generation, 5, &span));
    g_assert_cmpuint(span.first_page, ==, 2);
    g_assert_cmpuint(span.nr_pages, ==, 5);
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

static void test_region_bit_synthesis_preserves_page_locations(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };
    uint64_t page_state[1024];
    uint32_t generation = 4;

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       visible_regions, 8,
                                       owned, 8, 512 * 1024, 12,
                                       generation);
    cxl_hybrid_control_mark_pages_visible_generation(
        &hdr, visible, page_state, 512, 64, generation,
        CXL_HYBRID_PAGE_LOCATION_CXL);
    cxl_hybrid_control_mark_pages_visible_generation(
        &hdr, visible, page_state, 576, 64, generation,
        CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);

    g_assert_true(cxl_hybrid_control_region_visible_or_synthesize(
                      &hdr, visible, visible_regions, 512, 128,
                      generation));
    g_assert_true(test_bit(4, visible_regions));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[512], generation, CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[576], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[639], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
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

static void test_fault_region_plans_one_demand_and_prefetch_remainder(void)
{
    CXLHybridFaultRegionPlan plan = { 0 };

    g_assert_true(cxl_hybrid_fault_region_plan(100, 8, 103, &plan));
    g_assert_cmpuint(plan.demand_page, ==, 103);
    g_assert_cmpuint(plan.prefetch_first_page, ==, 100);
    g_assert_cmpuint(plan.prefetch_nr_pages, ==, 8);
    g_assert_cmpuint(plan.prefetch_skip_page, ==, 103);
}

static void test_fault_region_plan_rejects_out_of_span_demand(void)
{
    CXLHybridFaultRegionPlan plan = { 0 };

    g_assert_false(cxl_hybrid_fault_region_plan(100, 8, 108, &plan));
}

static void test_fault_region_completed_visibility_uses_demand_page(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };
    CXLHybridFaultRequestRecord record = {
        .page_index = 512,
        .demand_page = 531,
        .generation = 4,
        .flags = CXL_HYBRID_FAULT_REQUEST_F_REGION,
        .nr_pages = 128,
    };

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, NULL, record.demand_page, 4,
        CXL_HYBRID_PAGE_LOCATION_CXL);

    g_assert_true(cxl_hybrid_fault_request_demand_visible(&hdr, visible,
                                                          &record));
    g_assert_false(cxl_hybrid_control_region_visible_or_synthesize(
                       &hdr, visible, visible_regions, record.page_index,
                       record.nr_pages, record.generation));
}

static void test_fault_region_completed_status_accepts_demand_page(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };
    CXLHybridFaultRequestRecord record = {
        .page_index = 512,
        .demand_page = 531,
        .generation = 4,
        .flags = CXL_HYBRID_FAULT_REQUEST_F_REGION,
        .nr_pages = 128,
    };
    Error *err = NULL;
    int ret;

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);
    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, NULL, 531, 4, CXL_HYBRID_PAGE_LOCATION_CXL);

    ret = cxl_hybrid_fault_request_completed_status(&hdr, visible, &record,
                                                    &err);

    if (err) {
        error_free_or_abort(&err);
    }
    g_assert_cmpint(ret, ==, 0);
    g_assert_false(cxl_hybrid_control_region_visible_or_synthesize(
                       &hdr, visible, visible_regions, record.page_index,
                       record.nr_pages, record.generation));
}

static void test_fault_region_completed_status_rejects_missing_demand_page(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[BITS_TO_LONGS(1024)] = { 0 };
    unsigned long visible_regions[BITS_TO_LONGS(8)] = { 0 };
    unsigned long owned[BITS_TO_LONGS(8)] = { 0 };
    CXLHybridFaultRequestRecord record = {
        .page_index = 512,
        .demand_page = 531,
        .generation = 4,
        .flags = CXL_HYBRID_FAULT_REQUEST_F_REGION,
        .nr_pages = 128,
    };
    Error *err = NULL;
    int ret;

    cxl_hybrid_control_reset_run_state(&hdr, visible, 1024,
                                       NULL, 0, visible_regions, 8,
                                       owned, 8, 512 * 1024, 12, 4);

    ret = cxl_hybrid_fault_request_completed_status(&hdr, visible, &record,
                                                    &err);

    g_assert_cmpint(ret, ==, -ENOENT);
    g_assert_nonnull(err);
    error_free(err);
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

static void test_cxl_transfer_queue_drains_high_before_low(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor out = { 0 };

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_LOW,
        &(CXLHybridPageDescriptor) { .page_index = 10 });
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) { .page_index = 20 });

    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 20);
    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 10);
    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_cxl_transfer_queue_duplicate_descriptor_is_allowed(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor out = { 0 };

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) { .page_index = 7 });
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) { .page_index = 7 });

    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 7);
    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 7);
    g_assert_false(cxl_hybrid_transfer_queue_pop(&queue, &out));
    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_cxl_transfer_queue_ignores_negative_class(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor out = { 0 };

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, (CXLHybridTransferClass)-1,
        &(CXLHybridPageDescriptor) { .page_index = 99 });

    g_assert_false(cxl_hybrid_transfer_queue_pop(&queue, &out));
    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_transfer_queue_pop_cxl_ignores_rdma(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor rdma = { .page_index = 10, .nr_pages = 2 };
    CXLHybridPageDescriptor cxl = { .page_index = 20, .nr_pages = 1 };
    CXLHybridPageDescriptor out = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_RDMA_BULK,
                                   &rdma);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_LOW,
                                   &cxl);

    g_assert_true(cxl_hybrid_transfer_queue_pop_cxl(&queue, &out, &klass));
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_CXL_LOW);
    g_assert_cmpuint(out.page_index, ==, 20);
    g_assert_cmpuint(cxl_hybrid_transfer_queue_depth(
                         &queue, CXL_HYBRID_TRANSFER_RDMA_BULK), ==, 1);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_transfer_queue_pop_rdma_ignores_cxl(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor cxl = { .page_index = 7, .nr_pages = 1 };
    CXLHybridPageDescriptor rdma = { .page_index = 8, .nr_pages = 4 };
    CXLHybridPageDescriptor out = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
                                   &cxl);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_RDMA_PREFETCH,
                                   &rdma);

    g_assert_true(cxl_hybrid_transfer_queue_pop_rdma(&queue, &out, &klass));
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_RDMA_PREFETCH);
    g_assert_cmpuint(out.page_index, ==, 8);
    g_assert_cmpuint(cxl_hybrid_transfer_queue_depth(
                         &queue, CXL_HYBRID_TRANSFER_CXL_HIGH), ==, 1);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_transfer_queue_pop_cxl_batch_merges_adjacent_lane_pages(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor out[4] = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;
    uint32_t count;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    for (uint32_t i = 0; i < 3; i++) {
        cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_LOW,
            &(CXLHybridPageDescriptor) {
                .block_offset = i * CXL_HYBRID_TEST_PAGE_SIZE,
                .page_index = 20 + i,
                .cxl_offset = (100 + i) * CXL_HYBRID_TEST_PAGE_SIZE,
                .generation = 7,
                .nr_pages = 1,
                .has_claim = true,
            });
    }

    count = cxl_hybrid_transfer_queue_pop_cxl_batch(
        &queue, out, G_N_ELEMENTS(out), &klass);
    g_assert_cmpuint(count, ==, 3);
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_CXL_LOW);
    g_assert_cmpuint(out[0].page_index, ==, 20);
    g_assert_cmpuint(out[2].page_index, ==, 22);
    g_assert_cmpuint(cxl_hybrid_transfer_queue_depth(
                         &queue, CXL_HYBRID_TRANSFER_CXL_LOW), ==, 0);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_transfer_queue_pop_cxl_batch_stops_before_gap(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor out[4] = { 0 };
    CXLHybridPageDescriptor next = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;
    uint32_t count;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) {
            .block_offset = 10 * CXL_HYBRID_TEST_PAGE_SIZE,
            .page_index = 10,
            .cxl_offset = 110 * CXL_HYBRID_TEST_PAGE_SIZE,
            .generation = 3,
            .nr_pages = 1,
            .has_claim = true,
        });
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) {
            .block_offset = 12 * CXL_HYBRID_TEST_PAGE_SIZE,
            .page_index = 12,
            .cxl_offset = 112 * CXL_HYBRID_TEST_PAGE_SIZE,
            .generation = 3,
            .nr_pages = 1,
            .has_claim = true,
        });

    count = cxl_hybrid_transfer_queue_pop_cxl_batch(
        &queue, out, G_N_ELEMENTS(out), &klass);
    g_assert_cmpuint(count, ==, 1);
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_CXL_HIGH);
    g_assert_cmpuint(out[0].page_index, ==, 10);
    g_assert_cmpuint(cxl_hybrid_transfer_queue_depth(
                         &queue, CXL_HYBRID_TRANSFER_CXL_HIGH), ==, 1);

    g_assert_true(cxl_hybrid_transfer_queue_pop_cxl(&queue, &next, &klass));
    g_assert_cmpuint(next.page_index, ==, 12);
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_CXL_HIGH);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_transfer_queue_push_batch_preserves_cxl_order(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor in[3] = {
        {
            .block_offset = 30 * CXL_HYBRID_TEST_PAGE_SIZE,
            .page_index = 30,
            .cxl_offset = 130 * CXL_HYBRID_TEST_PAGE_SIZE,
            .generation = 11,
            .nr_pages = 1,
            .has_claim = true,
        },
        {
            .block_offset = 31 * CXL_HYBRID_TEST_PAGE_SIZE,
            .page_index = 31,
            .cxl_offset = 131 * CXL_HYBRID_TEST_PAGE_SIZE,
            .generation = 11,
            .nr_pages = 1,
            .has_claim = true,
        },
        {
            .block_offset = 32 * CXL_HYBRID_TEST_PAGE_SIZE,
            .page_index = 32,
            .cxl_offset = 132 * CXL_HYBRID_TEST_PAGE_SIZE,
            .generation = 11,
            .nr_pages = 1,
            .has_claim = true,
        },
    };
    CXLHybridPageDescriptor out[3] = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;
    uint32_t count;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    g_assert_cmpuint(cxl_hybrid_transfer_queue_push_batch(
                         &queue, CXL_HYBRID_TRANSFER_CXL_LOW, in,
                         G_N_ELEMENTS(in)), ==, 3);

    count = cxl_hybrid_transfer_queue_pop_cxl_batch(
        &queue, out, G_N_ELEMENTS(out), &klass);
    g_assert_cmpuint(count, ==, 3);
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_CXL_LOW);
    g_assert_cmpuint(out[0].page_index, ==, 30);
    g_assert_cmpuint(out[1].page_index, ==, 31);
    g_assert_cmpuint(out[2].page_index, ==, 32);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_transfer_queue_preserves_cxl_claim(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor in = {
        .page_index = 42,
        .nr_pages = 1,
        .generation = 5,
        .has_claim = true,
        .claim = {
            .observed = 0x1234,
            .generation = 5,
            .dirty_seq = 9,
            .owner = CXL_HYBRID_PAGE_OWNER_CXL,
        },
    };
    CXLHybridPageDescriptor out = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_LOW,
                                   &in);

    g_assert_true(cxl_hybrid_transfer_queue_pop_cxl(&queue, &out, &klass));
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_CXL_LOW);
    g_assert_true(out.has_claim);
    g_assert_cmpuint(out.page_index, ==, 42);
    g_assert_cmpuint(out.claim.owner, ==, CXL_HYBRID_PAGE_OWNER_CXL);
    g_assert_cmphex(out.claim.observed, ==, 0x1234);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
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
    g_test_add_func("/cxl-hybrid-control/page-state-snapshot-counts-current-generation",
                    test_page_state_snapshot_counts_current_generation);
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
    g_test_add_func("/cxl-hybrid-control/page-state-cxl-claim-complete-wrapper",
                    test_cxl_page_claim_complete_publishes_cxl);
    g_test_add_func("/cxl-hybrid-control/cxl-descriptor-completion-skips-stale",
                    test_cxl_descriptor_completion_skips_stale_page);
    g_test_add_func("/cxl-hybrid-control/complete-cxl-page-visible-publishes-matching-claim",
                    test_complete_cxl_page_visible_publishes_matching_claim);
    g_test_add_func("/cxl-hybrid-control/complete-cxl-page-visible-skips-stale-page",
                    test_complete_cxl_page_visible_skips_stale_page);
    g_test_add_func("/cxl-hybrid-control/complete-rdma-page-visible-publishes-dst-local",
                    test_complete_rdma_page_visible_publishes_dst_local);
    g_test_add_func("/cxl-hybrid-control/page-location-reports-visible-dst-local",
                    test_page_location_reports_visible_dst_local);
    g_test_add_func("/cxl-hybrid-control/visible-published-pages-need-install-until-received",
                    test_visible_published_pages_need_install_until_received);
    g_test_add_func("/cxl-hybrid-control/fault-place-eexist-requires-received-repair",
                    test_fault_place_eexist_requires_received_repair);
    g_test_add_func("/cxl-hybrid-control/mark-page-dirty-clears-visible-and-stales-published",
                    test_mark_page_dirty_clears_visible_and_stales_published);
    g_test_add_func("/cxl-hybrid-control/mark-page-dirty-rejects-generation-mismatch",
                    test_mark_page_dirty_rejects_generation_mismatch);
    g_test_add_func("/cxl-hybrid-control/mark-dirty-pages-stales-only-dirty-dst-local-pages",
                    test_mark_dirty_pages_stales_only_dirty_dst_local_pages);
    g_test_add_func("/cxl-hybrid-control/page-state-rdma-claim-complete-wrapper",
                    test_rdma_page_claim_complete_publishes_dst_local);
    g_test_add_func("/cxl-hybrid-control/page-state-drop-claim-restores-dirty",
                    test_page_state_drop_claim_restores_dirty);
    g_test_add_func("/cxl-hybrid-control/page-state-dirty-stales-rdma",
                    test_page_state_dirty_makes_rdma_completion_stale);
    g_test_add_func("/cxl-hybrid-control/page-state-rejects-double-claim",
                    test_page_state_rejects_double_claim);
    g_test_add_func("/cxl-hybrid-control/page-state-matches-encoded-generation",
                    test_page_state_matches_encoded_generation);
    g_test_add_func("/cxl-hybrid-control/cxl-owned-pages-need-postcopy-discard",
                    test_cxl_owned_pages_need_postcopy_discard);
    g_test_add_func("/cxl-hybrid-control/remap-span-grows-contiguous-cxl",
                    test_remap_span_grows_over_contiguous_cxl_pages);
    g_test_add_func("/cxl-hybrid-control/remap-span-rejects-non-cxl-fault-page",
                    test_remap_span_rejects_non_cxl_fault_page);
    g_test_add_func("/cxl-hybrid-control/remap-span-honors-scan-cap",
                    test_remap_span_honors_scan_cap);
    g_test_add_func("/cxl-hybrid-control/remap-span-reuses-unused-left-cap",
                    test_remap_span_reuses_unused_left_cap);
    g_test_add_func("/cxl-hybrid-control/remap-span-reuses-unused-right-cap",
                    test_remap_span_reuses_unused_right_cap);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-supports-fixed-extent-io",
                    test_staging_shared_map_supports_fixed_extent_io);
    g_test_add_func("/cxl-hybrid-control/staging-shared-map-reads-external-cxl-offset-without-slot",
                    test_staging_shared_map_reads_external_cxl_offset_without_slot);
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
    g_test_add_func("/cxl-hybrid-control/region-bit-synthesis-preserves-page-locations",
                    test_region_bit_synthesis_preserves_page_locations);
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
    g_test_add_func("/cxl-hybrid-control/fault-region-plan-demand-plus-prefetch",
                    test_fault_region_plans_one_demand_and_prefetch_remainder);
    g_test_add_func("/cxl-hybrid-control/fault-region-plan-rejects-out-of-span",
                    test_fault_region_plan_rejects_out_of_span_demand);
    g_test_add_func("/cxl-hybrid-control/fault-region-completed-visibility-uses-demand-page",
                    test_fault_region_completed_visibility_uses_demand_page);
    g_test_add_func("/cxl-hybrid-control/fault-region-completed-status-accepts-demand-page",
                    test_fault_region_completed_status_accepts_demand_page);
    g_test_add_func("/cxl-hybrid-control/fault-region-completed-status-rejects-missing-demand-page",
                    test_fault_region_completed_status_rejects_missing_demand_page);
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
    g_test_add_func("/cxl-hybrid-control/transfer-queue-high-before-low",
                    test_cxl_transfer_queue_drains_high_before_low);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-allows-duplicates",
                    test_cxl_transfer_queue_duplicate_descriptor_is_allowed);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-ignores-negative-class",
                    test_cxl_transfer_queue_ignores_negative_class);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-pop-cxl-ignores-rdma",
                    test_transfer_queue_pop_cxl_ignores_rdma);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-pop-rdma-ignores-cxl",
                    test_transfer_queue_pop_rdma_ignores_cxl);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-pop-cxl-batch-merges-adjacent-lane-pages",
                    test_transfer_queue_pop_cxl_batch_merges_adjacent_lane_pages);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-pop-cxl-batch-stops-before-gap",
                    test_transfer_queue_pop_cxl_batch_stops_before_gap);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-push-batch-preserves-cxl-order",
                    test_transfer_queue_push_batch_preserves_cxl_order);
    g_test_add_func("/cxl-hybrid-control/transfer-queue-preserves-cxl-claim",
                    test_transfer_queue_preserves_cxl_claim);
    return g_test_run();
}
