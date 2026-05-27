/*
 * Pure helpers for CXL hybrid fault region geometry.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/atomic.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "migration/cxl.h"
#include "trace.h"

bool cxl_hybrid_fault_resolve_mode_emits_burst(
    CXLHybridFaultResolveMode mode)
{
    return mode == CXL_HYBRID_FAULT_RESOLVE_MODE_COPY;
}

uint64_t cxl_hybrid_choose_fault_region_granule(uint64_t align,
                                                uint64_t configured,
                                                uint64_t total_ram)
{
    uint64_t granule;

    if (!align || !is_power_of_2(align)) {
        return 0;
    }

    granule = configured ? configured : CXL_FAULT_REGION_GRANULE_DEFAULT;
    granule = MAX(align, granule);
    granule = ROUND_UP(granule, align);
    if (total_ram > 0) {
        granule = MIN(granule, ROUND_UP(total_ram, align));
    }

    return granule;
}

uint64_t cxl_hybrid_choose_source_remap_granule(uint64_t min_align,
                                                uint64_t configured,
                                                uint64_t total_ram)
{
    uint64_t granule;

    if (!min_align || !is_power_of_2(min_align)) {
        return 0;
    }

    granule = configured ? configured : CXL_REMAP_GRANULE_DEFAULT;
    granule = MAX(min_align, granule);
    granule = ROUND_UP(granule, min_align);
    if (total_ram > 0) {
        granule = MIN(granule, ROUND_UP(total_ram, min_align));
    }

    return granule;
}

uint8_t cxl_hybrid_calculate_source_remap_coverage(uint64_t staged_pages,
                                                   uint64_t remapped_pages)
{
    if (!staged_pages) {
        return 0;
    }

    if (remapped_pages >= staged_pages) {
        return 100;
    }

    return (remapped_pages * 100) / staged_pages;
}

CXLHybridSwitchDecision cxl_hybrid_switch_decide(
    const CXLHybridSwitchPolicyInput *input)
{
    CXLHybridSwitchDecision decision = {
        .action = CXL_HYBRID_SWITCH_ACTION_NONE,
        .reason = CXL_MIGRATION_SWITCH_REASON_NONE,
    };
    bool remap_coverage_trigger;
    bool brake_coverage_gate;
    bool bulk_coverage_gate;

    if (!input) {
        return decision;
    }

    remap_coverage_trigger = input->remap_coverage_threshold &&
        input->remap_coverage >= input->remap_coverage_threshold;
    brake_coverage_gate = input->brake_enabled &&
        input->remap_coverage_threshold &&
        input->phase == CXL_HYBRID_PHASE_PRECOPY_BRAKE;
    bulk_coverage_gate = input->brake_enabled &&
        input->remap_coverage_threshold &&
        input->staged_pages &&
        input->phase == CXL_HYBRID_PHASE_PRECOPY_BULK &&
        (input->completion_ready || input->remaining_trigger ||
         input->time_cap_trigger || input->dirty_trigger ||
         input->gain_trigger || input->max_iters_trigger);

    if (bulk_coverage_gate) {
        decision.action = CXL_HYBRID_SWITCH_ACTION_ENTER_BRAKE;
        return decision;
    }

    if (input->completion_ready &&
        input->staged_pages &&
        (input->phase == CXL_HYBRID_PHASE_PRECOPY_BULK ||
         (input->phase == CXL_HYBRID_PHASE_PRECOPY_BRAKE &&
          !brake_coverage_gate))) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_PRECOPY_COMPLETE;
    } else if (input->phase == CXL_HYBRID_PHASE_PRECOPY_BULK &&
        input->brake_enabled &&
        !input->remaining_trigger && !input->time_cap_trigger &&
        (input->dirty_trigger || input->gain_trigger ||
         input->max_iters_trigger)) {
        decision.action = CXL_HYBRID_SWITCH_ACTION_ENTER_BRAKE;
        return decision;
    }

    if (decision.reason != CXL_MIGRATION_SWITCH_REASON_NONE) {
        /* Use the completion decision selected before brake entry. */
    } else if (input->remaining_trigger) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_REMAINING_SMALL;
    } else if (input->time_cap_trigger) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_PRECOPY_TIME_CAP;
    } else if (remap_coverage_trigger &&
               input->phase == CXL_HYBRID_PHASE_PRECOPY_BRAKE) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_REMAP_COVERAGE;
    } else if (input->dirty_trigger && !input->brake_enabled) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_DIRTY_RATE_HIGH;
    } else if (input->max_iters_trigger && !brake_coverage_gate) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_MAX_ITERS;
    } else if (input->gain_trigger && !brake_coverage_gate) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_GAIN_COLLAPSED;
    }

    if (decision.reason == CXL_MIGRATION_SWITCH_REASON_NONE) {
        return decision;
    }

    if (input->brake_enabled &&
        input->phase == CXL_HYBRID_PHASE_PRECOPY_BRAKE &&
        decision.reason != CXL_MIGRATION_SWITCH_REASON_REMAINING_SMALL &&
        decision.reason != CXL_MIGRATION_SWITCH_REASON_PRECOPY_TIME_CAP &&
        decision.reason != CXL_MIGRATION_SWITCH_REASON_REMAP_COVERAGE &&
        decision.reason != CXL_MIGRATION_SWITCH_REASON_PRECOPY_COMPLETE &&
        decision.reason != CXL_MIGRATION_SWITCH_REASON_MAX_ITERS &&
        decision.reason != CXL_MIGRATION_SWITCH_REASON_GAIN_COLLAPSED) {
        decision.reason = CXL_MIGRATION_SWITCH_REASON_NONE;
        return decision;
    }

    decision.action = CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY;
    return decision;
}

bool cxl_hybrid_source_remap_region_clean(const unsigned long *migrated_bmap,
                                          size_t migrated_first_page,
                                          const unsigned long *dirty_bmap,
                                          size_t dirty_first_page,
                                          size_t npages)
{
    size_t page;

    if (!migrated_bmap || !dirty_bmap || !npages) {
        return false;
    }

    for (page = 0; page < npages; page++) {
        if (!test_bit(migrated_first_page + page, migrated_bmap)) {
            return false;
        }
    }

    return bitmap_count_one_with_offset(dirty_bmap, dirty_first_page,
                                        npages) == 0;
}

bool cxl_hybrid_warm_page_eligible_for_push(
    const unsigned long *migrated_bmap,
    const unsigned long *warm_sent_bmap,
    const unsigned long *dst_sent_bmap,
    const unsigned long *cxl_visible_bmap,
    size_t page_idx)
{
    return migrated_bmap &&
           test_bit(page_idx, migrated_bmap) &&
           (!warm_sent_bmap || !test_bit(page_idx, warm_sent_bmap)) &&
           (!dst_sent_bmap || !test_bit(page_idx, dst_sent_bmap)) &&
           (!cxl_visible_bmap || !test_bit(page_idx, cxl_visible_bmap));
}

bool cxl_hybrid_clean_remap_budget_allows(uint64_t budget_bytes,
                                          uint64_t used_bytes,
                                          uint64_t region_len)
{
    uint64_t effective_budget;

    if (!region_len) {
        return false;
    }

    if (budget_bytes == 0) {
        return used_bytes == 0;
    }

    effective_budget = MAX(budget_bytes, region_len);
    return used_bytes <= effective_budget &&
           region_len <= effective_budget - used_bytes;
}

bool cxl_hybrid_clean_remap_should_throttle(uint64_t throttle_us,
                                            bool copied_region)
{
    return throttle_us > 0 && copied_region;
}

bool cxl_hybrid_clean_remap_region_is_candidate(bool epoch_seen,
                                                bool dirty_now,
                                                bool already_remapped,
                                                bool in_flight)
{
    return epoch_seen && !dirty_now && !already_remapped && !in_flight;
}

bool cxl_hybrid_clean_remap_debug_scan_only(const char *mode)
{
    return g_strcmp0(mode, "scan-only") == 0;
}

bool cxl_hybrid_clean_remap_debug_copy_only(const char *mode)
{
    return g_strcmp0(mode, "copy-only") == 0;
}

bool cxl_hybrid_clean_remap_debug_read_only(const char *mode)
{
    return g_strcmp0(mode, "read-only") == 0;
}

bool cxl_hybrid_clean_remap_debug_write_only(const char *mode)
{
    return g_strcmp0(mode, "write-only") == 0;
}

bool cxl_hybrid_clean_remap_debug_defer_remap(const char *mode)
{
    return g_strcmp0(mode, "defer-remap") == 0;
}

bool cxl_hybrid_clean_remap_prefault_valid(CXLCleanRemapPrefaultMode mode)
{
    return mode >= 0 && mode < CXL_CLEAN_REMAP_PREFAULT_MODE__MAX;
}

bool cxl_hybrid_clean_remap_prefault_enabled(CXLCleanRemapPrefaultMode mode)
{
    return mode == CXL_CLEAN_REMAP_PREFAULT_MODE_MADVISE ||
           mode == CXL_CLEAN_REMAP_PREFAULT_MODE_TOUCH;
}

uint64_t cxl_hybrid_mapped_ram_pages_offset_alignment(
    uint64_t file_align,
    uint64_t dax_align,
    uint64_t remap_granule,
    bool use_region_remap)
{
    uint64_t align = MAX(file_align, dax_align);

    if (use_region_remap) {
        align = MAX(align, remap_granule);
    }

    return align;
}

bool cxl_hybrid_mapped_ram_layout_next(uint64_t *offsetp,
                                       uint64_t used_length,
                                       uint64_t pages_align,
                                       uint64_t target_page_size,
                                       uint64_t *pages_offsetp,
                                       uint64_t *pages_lenp)
{
    uint64_t num_pages;
    uint64_t bitmap_size;
    uint64_t pages_offset;

    if (!offsetp || !pages_offsetp || !pages_lenp || !pages_align ||
        !target_page_size || !is_power_of_2(pages_align) ||
        !is_power_of_2(target_page_size) ||
        used_length % target_page_size) {
        return false;
    }

    num_pages = used_length / target_page_size;
    bitmap_size = (uint64_t)BITS_TO_LONGS(num_pages) * sizeof(unsigned long);

    if (*offsetp > UINT64_MAX - bitmap_size) {
        return false;
    }
    pages_offset = ROUND_UP(*offsetp + bitmap_size, pages_align);
    if (pages_offset > UINT64_MAX - used_length) {
        return false;
    }

    *pages_offsetp = pages_offset;
    *pages_lenp = used_length;
    *offsetp = pages_offset + used_length;
    return true;
}

static bool cxl_hybrid_dst_region_index_valid(
    const CXLHybridDstRegionState *state,
    uint64_t region_index)
{
    return state && state->remapped_bmap && state->copy_owned_bmap &&
           state->remapping_bmap && region_index < state->total_regions;
}

static bool cxl_hybrid_rdma_sidecar_region_index_valid(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    return state && state->candidate_bmap && state->inflight_bmap &&
           state->ready_bmap && state->stale_bmap &&
           state->cxl_published_bmap && state->invalidated_bmap &&
           state->republished_bmap && state->committed_bmap &&
           region_index < state->total_regions;
}

static uint64_t cxl_hybrid_rdma_sidecar_region_bytes(
    const CXLHybridRDMASidecarState *state)
{
    return state->pages_per_region * 4096;
}

static CXLHybridRDMASidecarStats cxl_hybrid_rdma_sidecar_stats;
static CXLHybridRDMASidecarState cxl_hybrid_rdma_sidecar_state;

void cxl_hybrid_rdma_sidecar_state_init_for_test(
    CXLHybridRDMASidecarState *state,
    uint64_t total_regions,
    uint64_t pages_per_region)
{
    if (!state) {
        return;
    }

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(state);
    state->total_regions = total_regions;
    state->pages_per_region = pages_per_region;
    if (total_regions) {
        state->candidate_bmap = bitmap_new(total_regions);
        state->inflight_bmap = bitmap_new(total_regions);
        state->ready_bmap = bitmap_new(total_regions);
        state->stale_bmap = bitmap_new(total_regions);
        state->cxl_published_bmap = bitmap_new(total_regions);
        state->invalidated_bmap = bitmap_new(total_regions);
        state->republished_bmap = bitmap_new(total_regions);
        state->committed_bmap = bitmap_new(total_regions);
    }
}

void cxl_hybrid_rdma_sidecar_state_destroy_for_test(
    CXLHybridRDMASidecarState *state)
{
    if (!state) {
        return;
    }

    g_free(state->ready_bmap);
    g_free(state->candidate_bmap);
    g_free(state->inflight_bmap);
    g_free(state->stale_bmap);
    g_free(state->cxl_published_bmap);
    g_free(state->invalidated_bmap);
    g_free(state->republished_bmap);
    g_free(state->committed_bmap);
    memset(state, 0, sizeof(*state));
}

bool cxl_hybrid_rdma_sidecar_region_is_rdma_owned(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return test_bit(region_index, state->inflight_bmap);
}

bool cxl_hybrid_rdma_sidecar_try_start_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index) ||
        test_bit(region_index, state->ready_bmap) ||
        test_bit(region_index, state->inflight_bmap) ||
        test_bit(region_index, state->committed_bmap) ||
        test_bit(region_index, state->invalidated_bmap) ||
        test_bit(region_index, state->stale_bmap)) {
        return false;
    }

    if (state->max_accepted_regions &&
        state->accepted_regions >= state->max_accepted_regions) {
        cxl_hybrid_account_rdma_sidecar_budget_skip();
        return false;
    }

    set_bit(region_index, state->candidate_bmap);
    set_bit(region_index, state->inflight_bmap);
    state->accepted_regions++;
    return true;
}

bool cxl_hybrid_rdma_sidecar_try_own_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    return cxl_hybrid_rdma_sidecar_try_start_region(state, region_index);
}

bool cxl_hybrid_rdma_sidecar_try_own_cxl_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    return cxl_hybrid_rdma_sidecar_note_cxl_publish(state, region_index);
}

bool cxl_hybrid_rdma_sidecar_region_is_cxl_owned(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return test_bit(region_index, state->cxl_published_bmap) ||
           test_bit(region_index, state->invalidated_bmap);
}

bool cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return !test_bit(region_index, state->committed_bmap);
}

void cxl_hybrid_rdma_sidecar_drop_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return;
    }

    clear_bit(region_index, state->inflight_bmap);
    clear_bit(region_index, state->candidate_bmap);
    clear_bit(region_index, state->ready_bmap);
}

bool cxl_hybrid_rdma_sidecar_complete_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index) ||
        !test_bit(region_index, state->inflight_bmap)) {
        return false;
    }

    clear_bit(region_index, state->inflight_bmap);
    if (test_bit(region_index, state->cxl_published_bmap) ||
        test_bit(region_index, state->invalidated_bmap) ||
        test_bit(region_index, state->committed_bmap)) {
        set_bit(region_index, state->stale_bmap);
        cxl_hybrid_account_rdma_sidecar_stale(
            region_index, cxl_hybrid_rdma_sidecar_region_bytes(state),
            test_bit(region_index, state->cxl_published_bmap));
        return true;
    }

    set_bit(region_index, state->ready_bmap);
    state->stats.rdma_ready_regions++;
    state->stats.rdma_ready_pages += state->pages_per_region;
    cxl_hybrid_account_rdma_ready(region_index, state->pages_per_region);
    return true;
}

bool cxl_hybrid_rdma_sidecar_mark_ready(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    return cxl_hybrid_rdma_sidecar_complete_region(state, region_index);
}

bool cxl_hybrid_rdma_sidecar_invalidate_ready(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index) ||
        !test_bit(region_index, state->ready_bmap)) {
        return false;
    }

    if (test_and_set_bit(region_index, state->invalidated_bmap)) {
        return false;
    }

    state->stats.rdma_invalidated_regions++;
    state->stats.rdma_ready_pages_lost += state->pages_per_region;
    clear_bit(region_index, state->ready_bmap);
    return true;
}

bool cxl_hybrid_rdma_sidecar_region_ready_current(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return test_bit(region_index, state->ready_bmap) &&
           !test_bit(region_index, state->invalidated_bmap);
}

bool cxl_hybrid_rdma_sidecar_commit_ready_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_ready_current(state, region_index)) {
        return false;
    }

    if (test_and_set_bit(region_index, state->committed_bmap)) {
        return false;
    }

    clear_bit(region_index, state->ready_bmap);
    return true;
}

bool cxl_hybrid_rdma_sidecar_region_committed(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return test_bit(region_index, state->committed_bmap);
}

bool cxl_hybrid_rdma_sidecar_region_invalidated(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return test_bit(region_index, state->invalidated_bmap);
}

bool cxl_hybrid_rdma_sidecar_region_stale(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return test_bit(region_index, state->stale_bmap);
}

bool cxl_hybrid_rdma_sidecar_region_inflight(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    return test_bit(region_index, state->inflight_bmap);
}

bool cxl_hybrid_rdma_sidecar_note_cxl_publish(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index)) {
        return false;
    }

    set_bit(region_index, state->cxl_published_bmap);
    if (test_bit(region_index, state->ready_bmap)) {
        cxl_hybrid_rdma_sidecar_invalidate_ready(state, region_index);
    }
    return true;
}

bool cxl_hybrid_rdma_sidecar_note_cxl_republish(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_region_index_valid(state, region_index) ||
        !test_bit(region_index, state->invalidated_bmap)) {
        return false;
    }

    if (test_and_set_bit(region_index, state->republished_bmap)) {
        return false;
    }

    state->stats.cxl_republish_regions_due_to_rdma_invalidate++;
    state->stats.cxl_republish_pages_due_to_rdma_invalidate +=
        state->pages_per_region;
    return true;
}

uint64_t cxl_hybrid_rdma_sidecar_invalidate_dirty_ready_regions(
    CXLHybridRDMASidecarState *state,
    const unsigned long *dirty_bmap,
    uint64_t dirty_pages)
{
    uint64_t invalidated = 0;
    unsigned long region_index;

    if (!state || !dirty_bmap || !state->ready_bmap ||
        !state->pages_per_region || !dirty_pages) {
        return 0;
    }

    region_index = find_first_bit(state->ready_bmap, state->total_regions);
    while (region_index < state->total_regions) {
        uint64_t first_page = (uint64_t)region_index * state->pages_per_region;
        uint64_t npages;

        if (first_page >= dirty_pages) {
            break;
        }

        npages = MIN(state->pages_per_region, dirty_pages - first_page);
        if (bitmap_count_one_with_offset(dirty_bmap, first_page, npages) &&
            cxl_hybrid_rdma_sidecar_invalidate_ready(state, region_index)) {
            invalidated++;
        }

        region_index = find_next_bit(state->ready_bmap, state->total_regions,
                                     region_index + 1);
    }

    return invalidated;
}

void cxl_hybrid_rdma_sidecar_get_stats(
    const CXLHybridRDMASidecarState *state,
    CXLHybridRDMASidecarStats *stats)
{
    if (!stats) {
        return;
    }

    *stats = state ? state->stats : (CXLHybridRDMASidecarStats) { 0 };
}

void cxl_hybrid_reset_rdma_sidecar_stats(void)
{
    memset(&cxl_hybrid_rdma_sidecar_stats, 0,
           sizeof(cxl_hybrid_rdma_sidecar_stats));
}

void cxl_hybrid_reset_rdma_sidecar_stats_for_test(void)
{
    cxl_hybrid_reset_rdma_sidecar_stats();
}

void cxl_hybrid_account_rdma_ready(uint64_t region_index, uint64_t pages)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.rdma_ready_regions);
    qatomic_add(&cxl_hybrid_rdma_sidecar_stats.rdma_ready_pages, pages);
    trace_cxl_hybrid_rdma_ready(region_index, pages);
}

void cxl_hybrid_account_rdma_invalidate(uint64_t region_index, uint64_t pages)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.rdma_invalidated_regions);
    qatomic_add(&cxl_hybrid_rdma_sidecar_stats.rdma_ready_pages_lost, pages);
    trace_cxl_hybrid_rdma_invalidate(region_index, pages);
}

void cxl_hybrid_account_rdma_cxl_republish(uint64_t region_index,
                                           uint64_t pages)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.
                cxl_republish_regions_due_to_rdma_invalidate);
    qatomic_add(&cxl_hybrid_rdma_sidecar_stats.
                cxl_republish_pages_due_to_rdma_invalidate, pages);
    trace_cxl_hybrid_rdma_cxl_republish(region_index, pages);
}

void cxl_hybrid_account_rdma_sidecar_connect(uint64_t time_ns)
{
    qatomic_add(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_connect_time_ns, time_ns);
    trace_cxl_rdma_sidecar_connect_complete(time_ns);
}

void cxl_hybrid_account_rdma_sidecar_registered(uint64_t bytes)
{
    qatomic_add(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_registered_bytes, bytes);
    trace_cxl_rdma_sidecar_register(bytes);
}

void cxl_hybrid_account_rdma_sidecar_posted(uint64_t region_index,
                                            uint64_t bytes)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_posted_regions);
    qatomic_add(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_posted_bytes, bytes);
    trace_cxl_rdma_sidecar_schedule(region_index, bytes);
}

void cxl_hybrid_account_rdma_sidecar_completed(uint64_t region_index,
                                               uint64_t bytes)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_completed_regions);
    qatomic_add(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_completed_bytes, bytes);
    trace_cxl_rdma_sidecar_complete(region_index, bytes);
}

void cxl_hybrid_account_rdma_sidecar_stale(uint64_t region_index,
                                           uint64_t bytes,
                                           bool cxl_race_lost)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.rdma_sidecar_stale_regions);
    if (cxl_race_lost) {
        qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.
                    rdma_sidecar_cxl_race_lost_regions);
    }
    trace_cxl_rdma_sidecar_stale(region_index, bytes, cxl_race_lost);
}

void cxl_hybrid_account_rdma_sidecar_failed(uint64_t region_index)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.rdma_sidecar_failed_regions);
    qatomic_set(&cxl_hybrid_rdma_sidecar_stats.rdma_sidecar_failed, true);
    trace_cxl_rdma_sidecar_failed(region_index);
}

void cxl_hybrid_account_rdma_sidecar_no_candidate(void)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_no_candidate_events);
    trace_cxl_rdma_sidecar_no_candidate();
}

void cxl_hybrid_account_rdma_sidecar_budget_skip(void)
{
    qatomic_inc(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_budget_skip_events);
    trace_cxl_rdma_sidecar_budget_skip();
}

void cxl_hybrid_set_rdma_sidecar_budget_stats(uint32_t max_inflight,
                                              uint8_t max_cover_percent)
{
    qatomic_set(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_max_inflight_regions, max_inflight);
    qatomic_set(&cxl_hybrid_rdma_sidecar_stats.
                rdma_sidecar_max_cover_percent, max_cover_percent);
}

void cxl_hybrid_get_rdma_sidecar_stats(CXLHybridRDMASidecarStats *stats)
{
    if (!stats) {
        return;
    }

    stats->rdma_sidecar_connect_time_ns =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_connect_time_ns);
    stats->rdma_sidecar_registered_bytes =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_registered_bytes);
    stats->rdma_sidecar_posted_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_posted_regions);
    stats->rdma_sidecar_posted_bytes =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_posted_bytes);
    stats->rdma_sidecar_completed_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_completed_regions);
    stats->rdma_sidecar_completed_bytes =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_completed_bytes);
    stats->rdma_sidecar_stale_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_stale_regions);
    stats->rdma_sidecar_cxl_race_lost_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_cxl_race_lost_regions);
    stats->rdma_sidecar_failed_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_failed_regions);
    stats->rdma_sidecar_no_candidate_events =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_no_candidate_events);
    stats->rdma_sidecar_budget_skip_events =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_budget_skip_events);
    stats->rdma_sidecar_max_inflight_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_max_inflight_regions);
    stats->rdma_sidecar_max_cover_percent =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     rdma_sidecar_max_cover_percent);
    stats->rdma_sidecar_failed =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.rdma_sidecar_failed);
    stats->rdma_ready_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.rdma_ready_regions);
    stats->rdma_ready_pages =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.rdma_ready_pages);
    stats->rdma_invalidated_regions =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.rdma_invalidated_regions);
    stats->rdma_ready_pages_lost =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.rdma_ready_pages_lost);
    stats->cxl_republish_regions_due_to_rdma_invalidate =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     cxl_republish_regions_due_to_rdma_invalidate);
    stats->cxl_republish_pages_due_to_rdma_invalidate =
        qatomic_read(&cxl_hybrid_rdma_sidecar_stats.
                     cxl_republish_pages_due_to_rdma_invalidate);
}

void cxl_hybrid_rdma_sidecar_global_init(uint64_t total_regions,
                                         uint64_t pages_per_region)
{
    cxl_hybrid_rdma_sidecar_state_init_for_test(&cxl_hybrid_rdma_sidecar_state,
                                                total_regions,
                                                pages_per_region);
}

void cxl_hybrid_rdma_sidecar_global_destroy(void)
{
    cxl_hybrid_rdma_sidecar_state_destroy_for_test(
        &cxl_hybrid_rdma_sidecar_state);
}

bool cxl_hybrid_region_is_rdma_owned(uint64_t region_index)
{
    return cxl_hybrid_rdma_sidecar_region_is_rdma_owned(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}

bool cxl_hybrid_region_try_own_rdma(uint64_t region_index)
{
    return cxl_hybrid_rdma_sidecar_try_own_region(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}

bool cxl_hybrid_region_try_own_cxl(uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_state.total_regions) {
        return true;
    }

    return cxl_hybrid_rdma_sidecar_try_own_cxl_region(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}

bool cxl_hybrid_region_is_cxl_owned(uint64_t region_index)
{
    return cxl_hybrid_rdma_sidecar_region_is_cxl_owned(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}

bool cxl_hybrid_region_cxl_bulk_allowed(uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_state.total_regions) {
        return true;
    }

    return cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}

void cxl_hybrid_region_drop_rdma(uint64_t region_index)
{
    cxl_hybrid_rdma_sidecar_drop_region(&cxl_hybrid_rdma_sidecar_state,
                                        region_index);
}

void cxl_hybrid_mark_region_rdma_ready(uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_mark_ready(&cxl_hybrid_rdma_sidecar_state,
                                            region_index)) {
        return;
    }
}

void cxl_hybrid_invalidate_region_rdma_ready(uint64_t region_index)
{
    uint64_t pages;

    if (!cxl_hybrid_rdma_sidecar_invalidate_ready(
            &cxl_hybrid_rdma_sidecar_state, region_index)) {
        return;
    }

    pages = cxl_hybrid_rdma_sidecar_state.pages_per_region;
    cxl_hybrid_account_rdma_invalidate(region_index, pages);
}

bool cxl_hybrid_region_commit_rdma_ready(uint64_t region_index)
{
    return cxl_hybrid_rdma_sidecar_commit_ready_region(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}

bool cxl_hybrid_region_note_cxl_republish(uint64_t region_index)
{
    if (cxl_hybrid_rdma_sidecar_note_cxl_republish(
            &cxl_hybrid_rdma_sidecar_state, region_index)) {
        cxl_hybrid_account_rdma_cxl_republish(
            region_index, cxl_hybrid_rdma_sidecar_state.pages_per_region);
        return true;
    }

    if (cxl_hybrid_rdma_sidecar_region_invalidated(
            &cxl_hybrid_rdma_sidecar_state, region_index)) {
        return false;
    }

    return cxl_hybrid_rdma_sidecar_note_cxl_publish(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}

bool cxl_hybrid_rdma_brake_fallback_enabled(bool rdma_sidecar_enabled,
                                            bool brake_enabled)
{
    return rdma_sidecar_enabled && brake_enabled;
}

void cxl_hybrid_dst_region_state_init_for_test(CXLHybridDstRegionState *state,
                                               uint64_t total_regions)
{
    if (!state) {
        return;
    }

    cxl_hybrid_dst_region_state_destroy_for_test(state);
    qemu_mutex_init(&state->lock);
    qemu_cond_init(&state->cond);
    state->lock_ready = true;
    state->cond_ready = true;
    state->total_regions = total_regions;
    if (total_regions) {
        state->remapped_bmap = bitmap_new(total_regions);
        state->copy_owned_bmap = bitmap_new(total_regions);
        state->remapping_bmap = bitmap_new(total_regions);
    }
}

void cxl_hybrid_dst_region_state_destroy_for_test(
    CXLHybridDstRegionState *state)
{
    bool lock_ready;
    bool cond_ready;

    if (!state) {
        return;
    }

    lock_ready = state->lock_ready;
    cond_ready = state->cond_ready;
    if (lock_ready) {
        qemu_mutex_lock(&state->lock);
    }
    g_free(state->remapped_bmap);
    g_free(state->copy_owned_bmap);
    g_free(state->remapping_bmap);
    state->remapped_bmap = NULL;
    state->copy_owned_bmap = NULL;
    state->remapping_bmap = NULL;
    state->total_regions = 0;
    if (lock_ready) {
        state->lock_ready = false;
        state->cond_ready = false;
        qemu_mutex_unlock(&state->lock);
        qemu_mutex_destroy(&state->lock);
    }
    if (cond_ready) {
        qemu_cond_destroy(&state->cond);
    }
    memset(state, 0, sizeof(*state));
}

bool cxl_hybrid_dst_region_copy_owned(const CXLHybridDstRegionState *state,
                                      uint64_t region_index)
{
    CXLHybridDstRegionState *mutable_state = (CXLHybridDstRegionState *)state;
    bool owned;

    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return false;
    }

    qemu_mutex_lock(&mutable_state->lock);
    owned = test_bit(region_index, state->copy_owned_bmap);
    qemu_mutex_unlock(&mutable_state->lock);
    return owned;
}

bool cxl_hybrid_dst_region_remapped(const CXLHybridDstRegionState *state,
                                    uint64_t region_index)
{
    CXLHybridDstRegionState *mutable_state = (CXLHybridDstRegionState *)state;
    bool remapped;

    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return false;
    }

    qemu_mutex_lock(&mutable_state->lock);
    remapped = test_bit(region_index, state->remapped_bmap);
    qemu_mutex_unlock(&mutable_state->lock);
    return remapped;
}

bool cxl_hybrid_dst_region_remapping(const CXLHybridDstRegionState *state,
                                     uint64_t region_index)
{
    CXLHybridDstRegionState *mutable_state = (CXLHybridDstRegionState *)state;
    bool remapping;

    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return false;
    }

    qemu_mutex_lock(&mutable_state->lock);
    remapping = test_bit(region_index, state->remapping_bmap);
    qemu_mutex_unlock(&mutable_state->lock);
    return remapping;
}

void cxl_hybrid_dst_region_wait_not_remapping(
    CXLHybridDstRegionState *state,
    uint64_t region_index)
{
    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return;
    }

    qemu_mutex_lock(&state->lock);
    while (test_bit(region_index, state->remapping_bmap)) {
        qemu_cond_wait(&state->cond, &state->lock);
    }
    qemu_mutex_unlock(&state->lock);
}

void cxl_hybrid_dst_region_mark_copy_owned(CXLHybridDstRegionState *state,
                                           uint64_t region_index)
{
    cxl_hybrid_dst_region_try_mark_copy_owned(state, region_index);
}

bool cxl_hybrid_dst_region_try_mark_copy_owned(CXLHybridDstRegionState *state,
                                               uint64_t region_index)
{
    bool marked = false;

    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return false;
    }

    qemu_mutex_lock(&state->lock);
    if (!test_bit(region_index, state->copy_owned_bmap) &&
        !test_bit(region_index, state->remapped_bmap) &&
        !test_bit(region_index, state->remapping_bmap)) {
        set_bit(region_index, state->copy_owned_bmap);
        marked = true;
    }
    qemu_mutex_unlock(&state->lock);
    return marked;
}

bool cxl_hybrid_dst_region_can_remap(const CXLHybridDstRegionState *state,
                                     uint64_t region_index)
{
    CXLHybridDstRegionState *mutable_state = (CXLHybridDstRegionState *)state;
    bool can_remap;

    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return false;
    }

    qemu_mutex_lock(&mutable_state->lock);
    can_remap = !test_bit(region_index, state->copy_owned_bmap) &&
                !test_bit(region_index, state->remapped_bmap) &&
                !test_bit(region_index, state->remapping_bmap);
    qemu_mutex_unlock(&mutable_state->lock);
    return can_remap;
}

bool cxl_hybrid_dst_region_try_begin_remap(CXLHybridDstRegionState *state,
                                           uint64_t region_index)
{
    bool reserved = false;

    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return false;
    }

    qemu_mutex_lock(&state->lock);
    if (!test_bit(region_index, state->copy_owned_bmap) &&
        !test_bit(region_index, state->remapped_bmap) &&
        !test_bit(region_index, state->remapping_bmap)) {
        set_bit(region_index, state->remapping_bmap);
        reserved = true;
    }
    qemu_mutex_unlock(&state->lock);
    return reserved;
}

void cxl_hybrid_dst_region_finish_remap(CXLHybridDstRegionState *state,
                                        uint64_t region_index,
                                        bool success)
{
    if (!cxl_hybrid_dst_region_index_valid(state, region_index)) {
        return;
    }

    qemu_mutex_lock(&state->lock);
    clear_bit(region_index, state->remapping_bmap);
    if (success) {
        set_bit(region_index, state->remapped_bmap);
    }
    qemu_cond_broadcast(&state->cond);
    qemu_mutex_unlock(&state->lock);
}

int cxl_hybrid_fault_region_compute(uint64_t block_global_base,
                                    uint64_t block_used_len,
                                    uint64_t block_cxl_pages_offset,
                                    uint64_t fault_block_offset,
                                    uint64_t granule,
                                    uint64_t target_page_size,
                                    CXLHybridFaultRegionGeometry *out,
                                    Error **errp)
{
    uint64_t fault_global;
    uint64_t region_global;
    uint64_t region_block_offset;
    uint64_t cxl_offset;
    uint64_t nr_pages;

    if (!out || !granule || !target_page_size ||
        !is_power_of_2(granule) ||
        !is_power_of_2(target_page_size) ||
        granule < target_page_size ||
        granule % target_page_size) {
        error_setg(errp, "invalid CXL hybrid fault region granule");
        return -EINVAL;
    }

    if (fault_block_offset >= block_used_len ||
        block_global_base > UINT64_MAX - fault_block_offset) {
        error_setg(errp, "CXL hybrid fault offset is outside the RAMBlock");
        return -EINVAL;
    }

    fault_global = block_global_base + fault_block_offset;
    region_global = QEMU_ALIGN_DOWN(fault_global, granule);
    if (region_global < block_global_base) {
        error_setg(errp, "CXL hybrid fault region crosses RAMBlock start");
        return -EINVAL;
    }

    region_block_offset = region_global - block_global_base;
    if (region_block_offset > block_used_len ||
        granule > block_used_len - region_block_offset) {
        error_setg(errp, "CXL hybrid fault region crosses RAMBlock end");
        return -EINVAL;
    }

    if (!QEMU_IS_ALIGNED(region_global, granule) ||
        !QEMU_IS_ALIGNED(region_block_offset, granule) ||
        !QEMU_IS_ALIGNED(block_cxl_pages_offset, granule)) {
        error_setg(errp, "CXL hybrid fault region is not DAX-granule aligned");
        return -EINVAL;
    }

    if (block_cxl_pages_offset > UINT64_MAX - region_block_offset) {
        error_setg(errp, "CXL hybrid fault region CXL offset overflows");
        return -EOVERFLOW;
    }

    cxl_offset = block_cxl_pages_offset + region_block_offset;
    if (!QEMU_IS_ALIGNED(cxl_offset, granule)) {
        error_setg(errp, "CXL hybrid fault region CXL offset is not aligned");
        return -EINVAL;
    }

    nr_pages = granule / target_page_size;
    if (nr_pages > UINT32_MAX) {
        error_setg(errp, "CXL hybrid fault region page count overflows");
        return -EOVERFLOW;
    }

    *out = (CXLHybridFaultRegionGeometry) {
        .global_offset = region_global,
        .block_offset = region_block_offset,
        .cxl_offset = cxl_offset,
        .region_len = granule,
        .first_page_index = region_global / target_page_size,
        .nr_pages = (uint32_t)nr_pages,
        .region_index = region_global / granule,
    };
    return 0;
}
