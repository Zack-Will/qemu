/*
 * Unit tests for postcopy source-side helper decisions.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/postcopy.h"

static void test_hybrid_postcopy_device_waits_until_package_loaded(void)
{
    g_assert_true(migration_postcopy_device_should_wait_for_package_loaded(
        MIGRATION_STATUS_POSTCOPY_DEVICE, true, false));
}

static void test_hybrid_postcopy_device_stops_waiting_after_package_loaded(void)
{
    g_assert_false(migration_postcopy_device_should_wait_for_package_loaded(
        MIGRATION_STATUS_POSTCOPY_DEVICE, true, true));
}

static void test_native_postcopy_device_does_not_wait_for_package_loaded(void)
{
    g_assert_false(migration_postcopy_device_should_wait_for_package_loaded(
        MIGRATION_STATUS_POSTCOPY_DEVICE, false, false));
}

static void test_hybrid_postcopy_active_does_not_wait_for_package_loaded(void)
{
    g_assert_false(migration_postcopy_device_should_wait_for_package_loaded(
        MIGRATION_STATUS_POSTCOPY_ACTIVE, true, false));
}

static void test_hybrid_mapped_cxl_ram_stream_publishes_after_data_save(void)
{
    g_assert_true(migration_postcopy_ram_stream_should_publish_cxl_visible(
        true, true, true, true, true));
}

static void test_ram_stream_does_not_publish_cxl_visible_before_data_save(void)
{
    g_assert_false(migration_postcopy_ram_stream_should_publish_cxl_visible(
        true, true, true, true, false));
}

static void test_native_ram_stream_does_not_publish_cxl_visible(void)
{
    g_assert_false(migration_postcopy_ram_stream_should_publish_cxl_visible(
        true, false, false, false, true));
}

static void test_source_remapped_ram_stream_skips_backing_write(void)
{
    g_assert_cmpint(migration_postcopy_cxl_ram_stream_write_action(
                        false, true, false), ==,
                    MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE);
}

static void test_destination_owned_visible_ram_stream_skips_backing_write(void)
{
    g_assert_cmpint(migration_postcopy_cxl_ram_stream_write_action(
                        true, false, true), ==,
                    MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE);
}

static void test_destination_owned_invisible_ram_stream_errors(void)
{
    g_assert_cmpint(migration_postcopy_cxl_ram_stream_write_action(
                        true, false, false), ==,
                    MIGRATION_POSTCOPY_CXL_RAM_STREAM_ERROR);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/migration/postcopy/hybrid-device-waits-until-package-loaded",
                    test_hybrid_postcopy_device_waits_until_package_loaded);
    g_test_add_func("/migration/postcopy/hybrid-device-stops-waiting-after-package-loaded",
                    test_hybrid_postcopy_device_stops_waiting_after_package_loaded);
    g_test_add_func("/migration/postcopy/native-device-does-not-wait-for-package-loaded",
                    test_native_postcopy_device_does_not_wait_for_package_loaded);
    g_test_add_func("/migration/postcopy/hybrid-active-does-not-wait-for-package-loaded",
                    test_hybrid_postcopy_active_does_not_wait_for_package_loaded);
    g_test_add_func("/migration/postcopy/hybrid-mapped-cxl-ram-stream-publishes-after-data-save",
                    test_hybrid_mapped_cxl_ram_stream_publishes_after_data_save);
    g_test_add_func("/migration/postcopy/ram-stream-does-not-publish-cxl-visible-before-data-save",
                    test_ram_stream_does_not_publish_cxl_visible_before_data_save);
    g_test_add_func("/migration/postcopy/native-ram-stream-does-not-publish-cxl-visible",
                    test_native_ram_stream_does_not_publish_cxl_visible);
    g_test_add_func("/migration/postcopy/source-remapped-ram-stream-skips-backing-write",
                    test_source_remapped_ram_stream_skips_backing_write);
    g_test_add_func("/migration/postcopy/destination-owned-visible-ram-stream-skips-backing-write",
                    test_destination_owned_visible_ram_stream_skips_backing_write);
    g_test_add_func("/migration/postcopy/destination-owned-invisible-ram-stream-errors",
                    test_destination_owned_invisible_ram_stream_errors);
    return g_test_run();
}
