/*
 * Internal helpers for source-side postcopy decisions.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/postcopy.h"

bool migration_postcopy_device_should_wait_for_package_loaded(
    MigrationStatus state,
    bool hybrid_mode,
    bool package_loaded)
{
    return hybrid_mode &&
           state == MIGRATION_STATUS_POSTCOPY_DEVICE &&
           !package_loaded;
}

bool migration_postcopy_device_can_pipeline_before_package_loaded(
    MigrationStatus state,
    bool hybrid_mode,
    bool package_loaded,
    uint64_t pending_size,
    bool pipeline_started)
{
    return hybrid_mode &&
           state == MIGRATION_STATUS_POSTCOPY_DEVICE &&
           !package_loaded &&
           pending_size > 0 &&
           !pipeline_started;
}

bool migration_postcopy_ram_stream_should_publish_cxl_visible(
    bool in_postcopy,
    bool hybrid_mode,
    bool mapped_ram,
    bool cxl_backing,
    bool data_saved)
{
    return in_postcopy && hybrid_mode && mapped_ram && cxl_backing &&
           data_saved;
}

bool migration_postcopy_cxl_source_completion_ready(bool hybrid_mode,
                                                   MigrationStatus state,
                                                   bool cxl_postcopy_warm,
                                                   bool final_ram_flushed,
                                                   bool cxl_source_drained)
{
    return hybrid_mode &&
           state == MIGRATION_STATUS_POSTCOPY_ACTIVE &&
           cxl_postcopy_warm &&
           final_ram_flushed &&
           cxl_source_drained;
}

MigrationPostcopyCXLRAMStreamWriteAction
migration_postcopy_cxl_ram_stream_write_action(bool destination_owned,
                                               bool source_remapped,
                                               bool page_visible)
{
    if (page_visible && !destination_owned && !source_remapped) {
        return MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_ALREADY_VISIBLE;
    }

    if (source_remapped) {
        return page_visible ?
            MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_ALREADY_VISIBLE :
            MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE;
    }

    if (destination_owned) {
        return page_visible ?
            MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE :
            MIGRATION_POSTCOPY_CXL_RAM_STREAM_ERROR;
    }

    return MIGRATION_POSTCOPY_CXL_RAM_STREAM_ALLOW;
}

MigrationPostcopyCXLHybridFaultAction
migration_postcopy_cxl_hybrid_fault_action(bool hybrid_mode,
                                           bool cxl_fault_supported)
{
    return hybrid_mode && cxl_fault_supported ?
        MIGRATION_POSTCOPY_CXL_HYBRID_FAULT_HANDLE_CXL :
        MIGRATION_POSTCOPY_CXL_HYBRID_FAULT_FALLBACK_RAM;
}

MigrationPostcopyIncomingListenPlan
migration_postcopy_incoming_listen_plan(PostcopyState old_state)
{
    MigrationPostcopyIncomingListenPlan plan = {
        .valid = old_state == POSTCOPY_INCOMING_ADVISE ||
                 old_state == POSTCOPY_INCOMING_DISCARD,
        .prepare_discard = old_state == POSTCOPY_INCOMING_ADVISE,
        .ready_state = POSTCOPY_INCOMING_LISTENING,
    };

    return plan;
}

bool migration_postcopy_cxl_should_drain_source_remaps(
    bool hybrid_mode,
    CXLHybridPhase phase,
    bool clean_remap_enabled)
{
    return hybrid_mode &&
           phase == CXL_HYBRID_PHASE_PRECOPY_BRAKE;
}

bool migration_postcopy_cxl_should_drain_rdma_before_precopy_complete(
    bool hybrid_mode,
    bool rdma_sidecar_enabled,
    MigrationStatus state)
{
    return hybrid_mode &&
           rdma_sidecar_enabled &&
           state == MIGRATION_STATUS_ACTIVE;
}

bool migration_postcopy_cxl_should_try_dirty_rdma_before_ram_stream(
    bool in_postcopy,
    bool hybrid_mode,
    CXLHybridPhase phase,
    bool postcopy_dirty_rdma_enabled,
    bool dirty_rdma_candidate)
{
    return in_postcopy &&
           hybrid_mode &&
           phase == CXL_HYBRID_PHASE_POSTCOPY_WARM &&
           postcopy_dirty_rdma_enabled &&
           dirty_rdma_candidate;
}

unsigned long migration_postcopy_cxl_dirty_rdma_next_ram_stream_page(
    unsigned long page,
    uint32_t claimed_pages)
{
    if (!claimed_pages) {
        return page;
    }
    if (claimed_pages > ULONG_MAX - page) {
        return ULONG_MAX;
    }
    return page + claimed_pages;
}

bool migration_postcopy_uffd_copy_result_satisfied(int ret,
                                                   bool allow_existing)
{
    return ret == 0 || (allow_existing && ret == -EEXIST);
}

bool migration_postcopy_cleanup_unregister_result_satisfied(int ret,
                                                            bool hybrid_cxl)
{
    return ret == 0 || (hybrid_cxl && ret == -EINVAL);
}

bool migration_postcopy_timeline_marker_skip_guest_write(
    bool incoming_postcopy,
    CXLGuestTimelineEvent event)
{
    return incoming_postcopy && event == CXL_GUEST_TIMELINE_EVENT_DST_STARTED;
}
