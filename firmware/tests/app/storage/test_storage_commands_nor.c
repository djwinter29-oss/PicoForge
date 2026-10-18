#include "unity.h"
#include "storage_commands.h"
#include "embedded_cli.h"
#include "Mockff.h"
#include "Mockfatfs_volume.h"
#include "Mocknor_media.h"

#include <string.h>

static char output[512];
static const pf_block_device_t device = {0};

static bool reply(const char *line, void *context)
{
    (void)context;
    TEST_ASSERT_LESS_THAN(sizeof(output), strlen(output) + strlen(line) + 2u);
    strcat(output, line);
    strcat(output, "\n");
    return true;
}

static void run(const char *arguments)
{
    pf_storage_command_execute(false, "fs", arguments, reply, NULL);
}

void setUp(void) { output[0] = '\0'; }
void tearDown(void) {}

void test_format_requires_exact_confirmation_and_firmware_ownership(void)
{
    run("format");
    TEST_ASSERT_NOT_NULL(strstr(output, "Usage: fs format --confirm"));
    run("format --confirm extra");
    pf_storage_command_execute(true, "fs", "format --confirm", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Media owned by USB"));
}

void test_format_refuses_mounted_volume(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    run("format --confirm");
    TEST_ASSERT_NOT_NULL(strstr(output, "format refused"));
}

void test_format_uses_logical_nor_device_then_mounts(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_nor_media_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_nor_media_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_format_ExpectAndReturn(&device, FR_OK);
    pf_fatfs_volume_mount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_OK);
    run("format --confirm");
    TEST_ASSERT_NOT_NULL(strstr(output, "Filesystem formatted"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Filesystem mounted"));
}

void test_failed_format_reports_error_without_mounting(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_nor_media_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_nor_media_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_format_ExpectAndReturn(&device, FR_DISK_ERR);
    run("format --confirm");
    TEST_ASSERT_NOT_NULL(strstr(output, "partially changed"));
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_nor_mount_initializes_and_binds_firmware_descriptor(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_nor_media_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_nor_media_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_mount_ExpectAndReturn(&device, NULL, PF_FATFS_VOLUME_OK);
    pf_fatfs_volume_mount_IgnoreArg_filesystem_result();
    run("mount");
    TEST_ASSERT_NOT_NULL(strstr(output, "Filesystem mounted."));
}

void test_nor_mount_initialization_failure_does_not_bind(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_nor_media_initialize_ExpectAndReturn(PF_BLOCK_IO_ERROR);
    run("mount");
    TEST_ASSERT_NOT_NULL(strstr(output, "LevelX recovery failed"));
}

void test_nor_mount_preserves_filesystem_failure_reason(void)
{
    FRESULT reason = FR_NO_FILESYSTEM;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_nor_media_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_nor_media_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_mount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_FILESYSTEM_ERROR);
    pf_fatfs_volume_mount_ReturnThruPtr_filesystem_result(&reason);
    run("mount");
    TEST_ASSERT_NOT_NULL(strstr(output, "NO_FILESYSTEM"));
}

void test_nor_already_mounted_does_not_reinitialize(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    run("mount");
    TEST_ASSERT_NOT_NULL(strstr(output, "already mounted"));
}

void test_nor_sd_diagnostics_are_rejected_without_sd_dependencies(void)
{
    pf_storage_command_execute(false, "sd", "info", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "SD diagnostics unavailable"));
}

void test_nor_empty_sd_command_and_unsupported_diagnostic_actions_are_rejected(void)
{
    const char *commands[] = {"", "diagnostics clear", "profile"};
    for (unsigned index = 0u; index < 3u; ++index) {
        output[0] = '\0';
        pf_storage_command_execute(false, "sd", commands[index], reply, NULL);
        TEST_ASSERT_NOT_NULL(strstr(output, "SD diagnostics unavailable"));
    }
}

void test_nor_status_and_diagnostics_expose_retained_failure_and_current_state(void)
{
    pf_nor_media_diagnostics_t diagnostics = {
        .sequence = 7u, .result = PF_BLOCK_IO_ERROR, .raw_result = 3u,
        .levelx_result = 1u, .system_error = 9u, .ready = true, .usb_owned = false,
        .operation = PF_NOR_MEDIA_OPERATION_WRITE, .failure_usb_owned = true,
    };
    const char *commands[] = {"status", "diagnostics", "diagnostics status"};
    for (unsigned index = 0u; index < 3u; ++index) {
        output[0] = '\0';
        pf_nor_media_get_diagnostics_ExpectAnyArgs();
        pf_nor_media_get_diagnostics_ReturnThruPtr_diagnostics(&diagnostics);
        pf_storage_command_execute(false, "sd", commands[index], reply, NULL);
        TEST_ASSERT_NOT_NULL(strstr(output, "backend=nor owner=Pico initialized=1 sequence=7"));
        TEST_ASSERT_NOT_NULL(strstr(output, "raw=3 LevelX=1 system=9"));
        TEST_ASSERT_NOT_NULL(strstr(output, "failure_operation=6 failure_owner=USB LevelX_block_error=synthetic"));
    }
}
