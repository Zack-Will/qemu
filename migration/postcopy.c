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

MigrationPostcopyCXLRAMStreamWriteAction
migration_postcopy_cxl_ram_stream_write_action(bool destination_owned,
                                               bool source_remapped,
                                               bool page_visible)
{
    if (source_remapped) {
        return MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE;
    }

    if (destination_owned) {
        return page_visible ?
            MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE :
            MIGRATION_POSTCOPY_CXL_RAM_STREAM_ERROR;
    }

    return MIGRATION_POSTCOPY_CXL_RAM_STREAM_ALLOW;
}
