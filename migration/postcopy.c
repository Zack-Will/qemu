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
