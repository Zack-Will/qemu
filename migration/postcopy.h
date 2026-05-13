/*
 * Internal helpers for source-side postcopy decisions.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef QEMU_MIGRATION_POSTCOPY_H
#define QEMU_MIGRATION_POSTCOPY_H

#include "migration/migration.h"

typedef enum MigrationPostcopyCXLRAMStreamWriteAction {
    MIGRATION_POSTCOPY_CXL_RAM_STREAM_ALLOW,
    MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE,
    MIGRATION_POSTCOPY_CXL_RAM_STREAM_ERROR,
} MigrationPostcopyCXLRAMStreamWriteAction;

typedef struct MigrationPostcopyIncomingListenPlan {
    bool valid;
    bool prepare_discard;
    PostcopyState ready_state;
} MigrationPostcopyIncomingListenPlan;

bool migration_postcopy_device_should_wait_for_package_loaded(
    MigrationStatus state,
    bool hybrid_mode,
    bool package_loaded);
bool migration_postcopy_ram_stream_should_publish_cxl_visible(
    bool in_postcopy,
    bool hybrid_mode,
    bool mapped_ram,
    bool cxl_backing,
    bool data_saved);
bool migration_postcopy_cxl_source_completion_ready(bool hybrid_mode,
                                                   MigrationStatus state,
                                                   bool cxl_postcopy_warm,
                                                   bool final_ram_flushed);
MigrationPostcopyCXLRAMStreamWriteAction
migration_postcopy_cxl_ram_stream_write_action(bool destination_owned,
                                               bool source_remapped,
                                               bool page_visible);
MigrationPostcopyIncomingListenPlan
migration_postcopy_incoming_listen_plan(PostcopyState old_state);

#endif /* QEMU_MIGRATION_POSTCOPY_H */
