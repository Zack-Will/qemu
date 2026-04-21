/*
 * Pure helpers for migration downtime accounting.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/migration.h"

void migration_downtime_reset(MigrationState *s)
{
    s->downtime = 0;
    s->downtime_set = false;
}

void migration_record_downtime(MigrationState *s, int64_t downtime_ms)
{
    if (s->downtime_set) {
        return;
    }

    s->downtime = downtime_ms;
    s->downtime_set = true;
}
