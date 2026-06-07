/*
 * QEMU migration capabilities
 *
 * Copyright (c) 2012-2023 Red Hat Inc
 *
 * Authors:
 *   Orit Wasserman <owasserm@redhat.com>
 *   Juan Quintela <quintela@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_MIGRATION_OPTIONS_H
#define QEMU_MIGRATION_OPTIONS_H

#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/client-options.h"

/* migration properties */

extern const Property migration_properties[];
extern const size_t migration_properties_count;

/* capabilities */

bool migrate_auto_converge(void);
bool migrate_colo(void);
bool migrate_dirty_bitmaps(void);
bool migrate_events(void);
bool migrate_mapped_ram(void);
bool migrate_ignore_shared(void);
bool migrate_late_block_activate(void);
bool migrate_multifd(void);
bool migrate_pause_before_switchover(void);
bool migrate_postcopy_blocktime(void);
bool migrate_postcopy_preempt(void);
bool migrate_cxl_hybrid(void);
bool migrate_rdma_pin_all(void);
bool migrate_release_ram(void);
bool migrate_return_path(void);
bool migrate_validate_uuid(void);
bool migrate_xbzrle(void);
bool migrate_zero_copy_send(void);

/*
 * pseudo capabilities
 *
 * These are functions that are used in a similar way to capabilities
 * check, but they are not a capability.
 */

bool migrate_multifd_flush_after_each_section(void);
bool migrate_postcopy(void);
bool migrate_rdma(void);
bool migrate_tls(void);

/* capabilities helpers */

bool migrate_rdma_caps_check(bool *caps, Error **errp);
bool migrate_caps_check(bool *old_caps, bool *new_caps, Error **errp);
bool migrate_can_snapshot(Error **errp);

/* parameters */

const BitmapMigrationNodeAliasList *migrate_block_bitmap_mapping(void);
bool migrate_has_block_bitmap_mapping(void);

uint32_t migrate_checkpoint_delay(void);
uint8_t migrate_cpu_throttle_increment(void);
uint8_t migrate_cpu_throttle_initial(void);
bool migrate_cpu_throttle_tailslow(void);
bool migrate_direct_io(void);
uint64_t migrate_downtime_limit(void);
uint8_t migrate_max_cpu_throttle(void);
uint64_t migrate_max_bandwidth(void);
uint64_t migrate_avail_switchover_bandwidth(void);
uint64_t migrate_max_postcopy_bandwidth(void);
int migrate_multifd_channels(void);
MultiFDCompression migrate_multifd_compression(void);
int migrate_multifd_zlib_level(void);
int migrate_multifd_qatzip_level(void);
int migrate_multifd_zstd_level(void);
uint8_t migrate_throttle_trigger_threshold(void);
const char *migrate_tls_authz(void);
const char *migrate_tls_creds(void);
const char *migrate_tls_hostname(void);
uint64_t migrate_xbzrle_cache_size(void);
ZeroPageDetection migrate_zero_page_detection(void);
const char *migrate_cxl_path(void);
bool migrate_cxl_path_enabled(void);
uint64_t migrate_cxl_switch_dirty_threshold(void);
uint32_t migrate_cxl_switch_max_iters(void);
uint64_t migrate_cxl_switch_max_precopy_ms(void);
uint64_t migrate_cxl_switch_min_remaining(void);
uint64_t migrate_cxl_switch_gain_floor(void);
uint8_t migrate_cxl_switch_remap_coverage(void);
bool migrate_cxl_brake_enable(void);
uint64_t migrate_cxl_brake_remap_granule(void);
uint64_t migrate_cxl_backing_rate(void);
bool migrate_cxl_clean_remap_enable(void);
uint64_t migrate_cxl_clean_remap_copy_budget(void);
uint64_t migrate_cxl_clean_remap_throttle_us(void);
CXLCleanRemapPrefaultMode migrate_cxl_clean_remap_prefault_mode(void);
uint64_t migrate_cxl_prefetch_rate(void);
uint64_t migrate_cxl_prefetch_heat_window_ms(void);
uint32_t migrate_cxl_prefetch_batch_pages(void);
uint64_t migrate_cxl_dst_cache_size(void);
bool migrate_cxl_shared_backing(void);
bool migrate_cxl_shared_bitmap(void);
bool migrate_cxl_rdma_sidecar(void);
const MigrationAddress *migrate_cxl_rdma_sidecar_address(void);
uint32_t migrate_cxl_rdma_sidecar_max_inflight_regions(void);
uint64_t migrate_cxl_rdma_sidecar_region_bytes(void);
bool migrate_cxl_rdma_sidecar_postcopy_dirty(void);
uint64_t migrate_cxl_rdma_sidecar_postcopy_dirty_min_bytes(void);
uint64_t migrate_cxl_rdma_cxl_priority_threshold_bytes(void);
CXLHybridFaultResolveMode migrate_cxl_fault_resolve_mode(void);
bool migrate_cxl_fault_resolve_copy(void);
bool migrate_cxl_fault_resolve_region_remap(void);
bool migrate_cxl_fault_resolve_region_remap_fallback_copy(void);
bool migrate_cxl_fault_resolve_uses_region(void);

/* parameters helpers */

bool migrate_params_check(MigrationParameters *params, Error **errp);
void migrate_params_init(MigrationParameters *params);
void migrate_tls_opts_free(MigrationParameters *params);
#endif
