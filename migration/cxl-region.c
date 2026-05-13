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
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "migration/cxl.h"

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

    granule = configured ? configured : CXL_REMAP_GRANULE_DEFAULT;
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
