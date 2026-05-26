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
                                                   bool final_ram_flushed)
{
    return hybrid_mode &&
           state == MIGRATION_STATUS_POSTCOPY_ACTIVE &&
           cxl_postcopy_warm &&
           final_ram_flushed;
}

MigrationPostcopyCXLRAMStreamWriteAction
migration_postcopy_cxl_ram_stream_write_action(bool destination_owned,
                                               bool source_remapped,
                                               bool page_visible)
{
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
