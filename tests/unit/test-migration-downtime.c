/*
 * Unit tests for migration downtime accounting helpers.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/migration.h"

static void test_zero_downtime_is_preserved(void)
{
    MigrationState state = { 0 };

    migration_downtime_reset(&state);
    migration_record_downtime(&state, 0);
    migration_record_downtime(&state, 42);

    g_assert_cmpint(state.downtime, ==, 0);
    g_assert_true(state.downtime_set);
}

static void test_reset_allows_new_measurement(void)
{
    MigrationState state = { 0 };

    migration_downtime_reset(&state);
    migration_record_downtime(&state, 11);
    migration_downtime_reset(&state);
    migration_record_downtime(&state, 7);

    g_assert_cmpint(state.downtime, ==, 7);
    g_assert_true(state.downtime_set);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/migration/downtime/zero-preserved",
                    test_zero_downtime_is_preserved);
    g_test_add_func("/migration/downtime/reset-allows-new-measurement",
                    test_reset_allows_new_measurement);
    return g_test_run();
}
