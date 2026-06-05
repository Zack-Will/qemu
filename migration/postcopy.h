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
#include "migration/cxl.h"

typedef enum MigrationPostcopyCXLRAMStreamWriteAction {
    MIGRATION_POSTCOPY_CXL_RAM_STREAM_ALLOW,
    MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_ALREADY_VISIBLE,
    MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE,
    MIGRATION_POSTCOPY_CXL_RAM_STREAM_ERROR,
} MigrationPostcopyCXLRAMStreamWriteAction;

typedef enum MigrationPostcopyCXLHybridFaultAction {
    MIGRATION_POSTCOPY_CXL_HYBRID_FAULT_HANDLE_CXL,
    MIGRATION_POSTCOPY_CXL_HYBRID_FAULT_FALLBACK_RAM,
} MigrationPostcopyCXLHybridFaultAction;

typedef struct MigrationPostcopyIncomingListenPlan {
    bool valid;
    bool prepare_discard;
    PostcopyState ready_state;
} MigrationPostcopyIncomingListenPlan;

bool migration_postcopy_device_should_wait_for_package_loaded(
    MigrationStatus state,
    bool hybrid_mode,
    bool package_loaded);
bool migration_postcopy_device_can_pipeline_before_package_loaded(
    MigrationStatus state,
    bool hybrid_mode,
    bool package_loaded,
    uint64_t pending_size,
    bool pipeline_started);
bool migration_postcopy_ram_stream_should_publish_cxl_visible(
    bool in_postcopy,
    bool hybrid_mode,
    bool mapped_ram,
    bool cxl_backing,
    bool data_saved);
bool migration_postcopy_cxl_source_completion_ready(bool hybrid_mode,
                                                   MigrationStatus state,
                                                   bool cxl_postcopy_warm,
                                                   bool final_ram_flushed,
                                                   bool cxl_source_drained);
MigrationPostcopyCXLRAMStreamWriteAction
migration_postcopy_cxl_ram_stream_write_action(bool destination_owned,
                                               bool source_remapped,
                                               bool page_visible);
MigrationPostcopyCXLHybridFaultAction
migration_postcopy_cxl_hybrid_fault_action(bool hybrid_mode,
                                           bool cxl_fault_supported);
MigrationPostcopyIncomingListenPlan
migration_postcopy_incoming_listen_plan(PostcopyState old_state);
bool migration_postcopy_cxl_should_drain_source_remaps(
    bool hybrid_mode,
    CXLHybridPhase phase,
    bool clean_remap_enabled);
bool migration_postcopy_cxl_should_drain_rdma_before_precopy_complete(
    bool hybrid_mode,
    bool rdma_sidecar_enabled,
    MigrationStatus state);
bool migration_postcopy_cxl_should_try_dirty_rdma_before_ram_stream(
    bool in_postcopy,
    bool hybrid_mode,
    CXLHybridPhase phase,
    bool postcopy_dirty_rdma_enabled,
    bool dirty_rdma_candidate);
unsigned long migration_postcopy_cxl_dirty_rdma_next_ram_stream_page(
    unsigned long page,
    uint32_t claimed_pages);
bool migration_postcopy_uffd_copy_result_satisfied(int ret,
                                                   bool allow_existing);
bool migration_postcopy_cleanup_unregister_result_satisfied(int ret,
                                                            bool hybrid_cxl);
bool migration_postcopy_timeline_marker_skip_guest_write(
    bool incoming_postcopy,
    CXLGuestTimelineEvent event);

#endif /* QEMU_MIGRATION_POSTCOPY_H */
