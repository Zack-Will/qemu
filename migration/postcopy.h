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

#endif /* QEMU_MIGRATION_POSTCOPY_H */
