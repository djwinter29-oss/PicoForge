// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#include "unity.h"
TEST_SOURCE_FILE("embedded_cli_adapter.c")
#include "app/cli/cli.h"
#include "app/cli/cli_fs.h"
#include "app/cli/cli_storage.h"
#include "app/cli/cli_fatfs_result.h"
#include "app/cli/cli_internal.h"
#include "app/usb/usb_msc.h"
#include "embedded_cli.h"
#include "Mockff.h"
#include "Mockfatfs_volume.h"
#include "Mockstorage.h"
#include "Mockusb_msc.h"
#include <stdio.h>
#include <string.h>

#include "cli/storage_fixture.inc"

void setUp(void) { storage_set_up(); }
void tearDown(void) { storage_tear_down(); }

void test_refuses_filesystem_access_while_usb_owns_card(void)
{
    pf_cli_storage_execute(true, "fs", "write /a text", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Media owned by USB"));
}

void test_empty_storage_arguments_report_usage_without_a_missing_token(void)
{
    pf_cli_storage_execute(false, "storage", "", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "ERR Usage: storage get"));
}

void test_requires_mount_before_file_operations(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_cli_storage_execute(false, "fs", "ls", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "not mounted"));
}

void test_mount_and_unmount_report_success(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_storage_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_mount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_OK);
    pf_cli_storage_execute(false, "storage", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Filesystem mounted"));
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    pf_fatfs_volume_unmount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_OK);
    pf_cli_storage_execute(false, "storage", "unmount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "handoff is now allowed"));
}

void test_mount_reports_fatfs_failure(void)
{
    FRESULT result = FR_NO_FILESYSTEM;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_storage_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_mount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_FILESYSTEM_ERROR);
    pf_fatfs_volume_mount_ReturnThruPtr_filesystem_result(&result);
    pf_cli_storage_execute(false, "storage", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "NO_FILESYSTEM"));
}

void test_mount_does_not_initialize_an_already_mounted_device(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    pf_cli_storage_execute(false, "storage", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "already mounted"));
}

void test_mount_rejects_missing_configuration_without_binding(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_NOT_READY);
    pf_cli_storage_execute(false, "storage", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "configuration unavailable"));
}

void test_mount_reports_card_initialization_failure_without_binding(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_IO_ERROR);
    pf_cli_storage_execute(false, "storage", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "storage initialization"));
}

void test_volume_maintenance_is_no_longer_a_filesystem_command(void)
{
    pf_cli_storage_execute(false, "fs", "mount", reply, NULL);
    pf_cli_storage_execute(false, "fs", "unmount", reply, NULL);
    pf_cli_storage_execute(false, "fs", "format --confirm", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Volume maintenance moved to storage"));
}

void test_storage_maintenance_rejects_usb_ownership_and_invalid_arguments(void)
{
    pf_cli_storage_execute(true, "storage", "format --confirm", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Media owned by USB"));
    pf_cli_storage_execute(true, "storage", "mount", reply, NULL);
    pf_cli_storage_execute(true, "storage", "unmount", reply, NULL);
    pf_cli_storage_execute(false, "storage", "format", reply, NULL);
    pf_cli_storage_execute(false, "storage", "format --confirm extra", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Usage: storage format --confirm"));
    pf_cli_storage_execute(false, "storage", "mount extra", reply, NULL);
    pf_cli_storage_execute(false, "storage", "unmount extra", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Invalid arguments"));
}

void test_storage_format_refuses_a_mounted_volume(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    pf_cli_storage_execute(false, "storage", "format --confirm", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Unmount first with storage unmount; format refused"));
}

void test_storage_format_formats_and_mounts_the_existing_device(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_storage_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_format_ExpectAndReturn(&device, FR_OK);
    pf_fatfs_volume_mount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_OK);
    pf_cli_storage_execute(false, "storage", "format --confirm", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Filesystem formatted; mounting"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Filesystem mounted"));
}

void test_storage_format_failure_is_reported_without_mounting(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_storage_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_format_ExpectAndReturn(&device, FR_DISK_ERR);
    pf_cli_storage_execute(false, "storage", "format --confirm", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Format failed; volume contents may be partially changed"));
    TEST_ASSERT_NULL(strstr(output, "Filesystem mounted"));
}

void test_status_is_available_during_host_ownership(void)
{
    pf_storage_status_t status = {
        .medium = PF_STORAGE_MEDIUM_SDCARD, .bus = PF_STORAGE_BUS_SDIO, .state = PF_STORAGE_STATE_USB,
        .presence_known = true, .bus_clock_hz = 12500000u, .sector_count = 30875648u,
        .last_failure = PF_STORAGE_OPERATION_WRITE, .last_failure_result = PF_BLOCK_IO_ERROR,
    };
    pf_storage_get_status_ExpectAnyArgs();
    pf_storage_get_status_ReturnThruPtr_status(&status);
    pf_cli_storage_execute(true, "storage", "get status", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "state=usb owner=USB medium=sd bus=sdio present=yes"));
    TEST_ASSERT_NOT_NULL(strstr(output, "bytes=15808331776"));
    TEST_ASSERT_NOT_NULL(strstr(output, "last_error=write:"));
}

void test_removed_sd_family_reports_usage(void)
{
    pf_cli_storage_execute(false, "sd", "info", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Usage: storage get [status]"));
}

void test_rejects_long_arguments_without_truncation(void)
{
    char arguments[129];
    memset(arguments, 'a', sizeof(arguments) - 1u);
    arguments[sizeof(arguments) - 1u] = '\0';
    pf_cli_storage_execute(false, "fs", arguments, reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Arguments too long"));
}

void test_out_of_range_status_values_print_unknown(void)
{
    pf_storage_status_t status = { .state = (pf_storage_state_t)99, .last_failure = (pf_storage_operation_t)99 };

    pf_storage_get_status_ExpectAnyArgs();
    pf_storage_get_status_ReturnThruPtr_status(&status);
    pf_cli_storage_execute(false, "storage", "get", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "state=unknown"));
    TEST_ASSERT_NOT_NULL(strstr(output, "last_error=unknown:"));
}

void test_storage_and_fs_bindings_queue_commands_and_report_admission(void)
{
    send_cli("help\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, " * storage\r\n\tget [status | usb | benchmark]"));
    TEST_ASSERT_NOT_NULL(strstr(cli_output, " * fs\r\n\tls [path]"));

    send_cli("storage get status\r");
    TEST_ASSERT_EQUAL_UINT(1u, queued_commands);
    TEST_ASSERT_EQUAL_STRING("storage", queued_family);
    TEST_ASSERT_EQUAL_STRING("get status", queued_arguments);
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "QUEUED Storage command"));

    send_cli("fs ls /data\r");
    TEST_ASSERT_EQUAL_UINT(2u, queued_commands);
    TEST_ASSERT_EQUAL_STRING("fs", queued_family);
    TEST_ASSERT_EQUAL_STRING("ls /data", queued_arguments);

    queue_ok = false;
    send_cli("fs ls /data\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR Storage busy"));
}

void test_usb_ownership_reports_admission_and_refusal_without_claiming_completion(void)
{
    send_cli("storage set usb on\r");
    TEST_ASSERT_EQUAL_UINT(1u, msc_requests);
    TEST_ASSERT_TRUE(msc_enabled);
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "QUEUED USB ownership request"));

    worker_busy = true;
    send_cli("storage set usb off\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR storage busy"));
    TEST_ASSERT_EQUAL_UINT(1u, msc_requests);

    worker_busy = false;
    msc_request_ok = false;
    send_cli("storage set usb off\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR USB request refused"));
    TEST_ASSERT_EQUAL_UINT(2u, msc_requests);
    TEST_ASSERT_TRUE(msc_enabled);
}

void test_status_is_queued_and_recovery_requires_confirmation_and_admission(void)
{
    send_cli("storage get status\r");
    TEST_ASSERT_EQUAL_UINT(1u, queued_commands);
    TEST_ASSERT_EQUAL_STRING("storage", queued_family);
    TEST_ASSERT_EQUAL_STRING("get status", queued_arguments);

    send_cli("storage recover\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR storage "));
    TEST_ASSERT_EQUAL_UINT(0u, recovery_requests);

    send_cli("storage recover --confirm\r");
    TEST_ASSERT_EQUAL_UINT(1u, recovery_requests);
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "QUEUED fault recovery"));
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "not a clean eject"));

    recovery_ok = false;
    send_cli("storage recover --confirm\r");
    TEST_ASSERT_EQUAL_UINT(2u, recovery_requests);
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR fault recovery request refused"));
}

void test_storage_maintenance_queues_only_valid_commands(void)
{
    send_cli("storage mount\r");
    TEST_ASSERT_EQUAL_STRING("mount", queued_arguments);
    send_cli("storage unmount\r");
    TEST_ASSERT_EQUAL_STRING("unmount", queued_arguments);
    TEST_ASSERT_EQUAL_UINT(2u, queued_commands);

    send_cli("storage format\r");
    send_cli("storage format --confirm extra\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR storage "));
    TEST_ASSERT_EQUAL_UINT(2u, queued_commands);

    send_cli("storage format --confirm\r");
    TEST_ASSERT_EQUAL_UINT(3u, queued_commands);
    TEST_ASSERT_EQUAL_STRING("format --confirm", queued_arguments);
}

void test_benchmark_size_boundaries_ownership_and_cancellation(void)
{
    send_cli("storage benchmark 0\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "1..1024 MiB"));
    TEST_ASSERT_EQUAL_UINT(0u, benchmark_size);

    send_cli("storage benchmark 1\r");
    TEST_ASSERT_EQUAL_UINT(1u, benchmark_size);
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "QUEUED benchmark request"));

    send_cli("storage benchmark 1024\r");
    TEST_ASSERT_EQUAL_UINT(1024u, benchmark_size);
    send_cli("storage benchmark 1025\r");
    TEST_ASSERT_EQUAL_UINT(1024u, benchmark_size);
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "1..1024 MiB"));

    msc_enabled = true;
    send_cli("storage benchmark 4\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "USB owns storage"));
    TEST_ASSERT_EQUAL_UINT(1024u, benchmark_size);

    msc_enabled = false;
    send_cli("storage benchmark cancel\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "QUEUED benchmark cancellation"));
    benchmark_cancel_ok = false;
    send_cli("storage benchmark cancel\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR no cancellable run"));
}

void test_benchmark_status_includes_failure_cleanup_and_request_details(void)
{
    benchmark_status = (pf_file_benchmark_status_t){
        .state = PF_FILE_BENCHMARK_FAILED,
        .target_bytes = 4096u,
        .written_bytes = 2048u,
        .read_bytes = 1024u,
        .mismatch_count = 3u,
        .write_elapsed_ms = 25u,
        .read_elapsed_ms = 12u,
        .error = PF_FILE_BENCHMARK_ERROR_CLEANUP,
        .filesystem_result = FR_DISK_ERR,
        .request_error = PF_FILE_BENCHMARK_ERROR_BUSY,
        .flags = PF_FILE_BENCHMARK_FILE_REMAINS | PF_FILE_BENCHMARK_VOLUME_MOUNTED,
    };
    send_cli("storage get benchmark\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "benchmark=failed target=4096 written=2048 read=1024 mismatches=3"));
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "write_ms=25 read_ms=12 error=9 filesystem_result=1 request_error=1 flags=3"));

    benchmark_ok = false;
    send_cli("storage benchmark 8\r");
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "ERR benchmark request refused"));
    TEST_ASSERT_NOT_NULL(strstr(cli_output, "benchmark=failed target=4096"));
}
