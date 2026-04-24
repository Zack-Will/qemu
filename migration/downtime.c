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

void migration_stop_to_start_reset(MigrationState *s)
{
    s->stop_to_start_time = 0;
    s->stop_to_start_time_set = false;
}

void migration_record_stop_to_start(MigrationState *s, int64_t stop_to_start_ms)
{
    if (s->stop_to_start_time_set) {
        return;
    }

    s->stop_to_start_time = stop_to_start_ms;
    s->stop_to_start_time_set = true;
}
