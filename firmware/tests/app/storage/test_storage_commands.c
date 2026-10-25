#include "unity.h"
#include "app/storage/storage_commands.h"
#include "embedded_cli.h"
#include "Mockff.h"
#include "Mockfatfs_volume.h"
#include "Mockstorage.h"

#include <string.h>

static char output[2048];
static FRESULT open_result;
static BYTE expected_mode;
static UINT written_size;
static FRESULT write_result;
static bool directory_entry;
static const pf_block_device_t device = {0};

static FRESULT open_file(FIL *file, const TCHAR *path, BYTE mode, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/a", path);
    TEST_ASSERT_EQUAL_HEX8(expected_mode, mode);
    memset(file, 0, sizeof(*file));
    file->obj.objsize = 100u;
    return open_result;
}

static FRESULT write_file(FIL *file, const void *buffer, UINT size, UINT *written, int calls)
{
    (void)file;
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("hello world", buffer);
    TEST_ASSERT_EQUAL_UINT(11u, size);
    *written = written_size;
    return write_result;
}

static FRESULT read_file(FIL *file, void *buffer, UINT size, UINT *received, int calls)
{
    (void)file;
    (void)calls;
    const unsigned char text[] = { 'a', '\n', 0x1bu, 'z' };
    TEST_ASSERT_EQUAL_UINT(4u, size);
    memcpy(buffer, text, sizeof(text));
    *received = sizeof(text);
    return FR_OK;
}

static FRESULT read_directory(DIR *directory, FILINFO *info, int calls)
{
    (void)directory;
    memset(info, 0, sizeof(*info));
    if (calls == 0) {
        memcpy(info->fname, "folder", 7u);
        info->fattrib = AM_DIR;
        info->fsize = 10u;
    }
    return FR_OK;
}

static FRESULT file_stat(const TCHAR *path, FILINFO *info, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/a", path);
    memset(info, 0, sizeof(*info));
    info->fattrib = directory_entry ? AM_DIR : AM_ARC;
    info->fsize = 42u;
    return FR_OK;
}

static FRESULT rename_file(const TCHAR *source, const TCHAR *target, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/a", source);
    TEST_ASSERT_EQUAL_STRING("0:/b", target);
    return FR_EXIST;
}

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

static FRESULT mkdir_denied(const TCHAR *path, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/new", path);
    return FR_DENIED;
}

void setUp(void)
{
    output[0] = '\0';
    open_result = FR_OK;
    expected_mode = FA_READ;
    written_size = 11u;
    write_result = FR_OK;
    directory_entry = false;
}
void tearDown(void) {}

void test_refuses_filesystem_access_while_usb_owns_card(void)
{
    pf_storage_command_execute(true, "fs", "write /a text", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Media owned by USB"));
}

void test_requires_mount_before_file_operations(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    run("ls");
    TEST_ASSERT_NOT_NULL(strstr(output, "not mounted"));
}

void test_rejects_unsafe_paths_and_unconfirmed_removal(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    run("stat /../a");
    TEST_ASSERT_NOT_NULL(strstr(output, "Invalid path"));
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    run("rm /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "requires fs rm"));
}

void test_reports_existing_file_without_overwriting(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_open_ExpectAnyArgsAndReturn(FR_EXIST);
    run("write /existing text");
    TEST_ASSERT_NOT_NULL(strstr(output, "EXIST"));
}

void test_rejects_invalid_cat_limits(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    run("cat /a 4097");
    TEST_ASSERT_NOT_NULL(strstr(output, "1..4096"));
}

void test_creates_directory_and_reports_fatfs_errors(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_mkdir_StubWithCallback(mkdir_denied);
    run("mkdir /new");
    TEST_ASSERT_NOT_NULL(strstr(output, "DENIED"));
}

void test_mount_and_unmount_report_success(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_OK);
    pf_storage_firmware_device_ExpectAndReturn(&device);
    pf_fatfs_volume_mount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_OK);
    pf_storage_command_execute(false, "fs", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Filesystem mounted"));
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    pf_fatfs_volume_unmount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_OK);
    run("unmount");
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
    pf_storage_command_execute(false, "fs", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "NO_FILESYSTEM"));
}

void test_mount_does_not_initialize_an_already_mounted_device(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    run("mount");
    TEST_ASSERT_NOT_NULL(strstr(output, "already mounted"));
}

void test_mount_rejects_missing_configuration_without_binding(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_NOT_READY);
    run("mount");
    TEST_ASSERT_NOT_NULL(strstr(output, "configuration unavailable"));
}

void test_mount_reports_card_initialization_failure_without_binding(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(false);
    pf_storage_initialize_ExpectAndReturn(PF_BLOCK_IO_ERROR);
    pf_storage_command_execute(false, "fs", "mount", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "storage initialization"));
}

void test_lists_directory_and_closes_handle(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_directory);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls");
    TEST_ASSERT_NOT_NULL(strstr(output, "d 10 folder"));
}

void test_directory_failure_still_closes_handle(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_ExpectAnyArgsAndReturn(FR_DISK_ERR);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_stat_reports_size_and_attributes(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_stat_StubWithCallback(file_stat);
    run("stat /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "size=42 attributes=0x20"));
}

void test_cat_bounds_read_and_escapes_terminal_controls(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_open_StubWithCallback(open_file);
    f_read_StubWithCallback(read_file);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("cat /a 4");
    TEST_ASSERT_NOT_NULL(strstr(output, "a\\x0a\\x1bz"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Output limit reached"));
}

void test_cat_closes_file_after_read_failure(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_open_StubWithCallback(open_file);
    f_read_ExpectAnyArgsAndReturn(FR_DISK_ERR);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("cat /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_write_uses_create_new_then_syncs_and_closes(void)
{
    expected_mode = FA_WRITE | FA_CREATE_NEW;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_open_StubWithCallback(open_file);
    f_write_StubWithCallback(write_file);
    f_sync_ExpectAnyArgsAndReturn(FR_OK);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("write /a \"hello world\"");
    TEST_ASSERT_NOT_NULL(strstr(output, "FatFs: OK"));
}

void test_append_uses_append_mode_and_reports_sync_failure(void)
{
    expected_mode = FA_WRITE | FA_OPEN_APPEND;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_open_StubWithCallback(open_file);
    f_write_StubWithCallback(write_file);
    f_sync_ExpectAnyArgsAndReturn(FR_DISK_ERR);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("append /a \"hello world\"");
    TEST_ASSERT_NOT_NULL(strstr(output, "partially changed file may remain"));
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_short_write_is_an_error_and_still_syncs_and_closes(void)
{
    expected_mode = FA_WRITE | FA_CREATE_NEW;
    written_size = 3u;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_open_StubWithCallback(open_file);
    f_write_StubWithCallback(write_file);
    f_sync_ExpectAnyArgsAndReturn(FR_OK);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("write /a \"hello world\"");
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_move_preserves_existing_destination(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_rename_StubWithCallback(rename_file);
    run("mv /a /b");
    TEST_ASSERT_NOT_NULL(strstr(output, "EXIST"));
}

void test_remove_requires_confirmation_and_rejects_directories(void)
{
    directory_entry = true;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_stat_StubWithCallback(file_stat);
    run("rm /a --confirm");
    TEST_ASSERT_NOT_NULL(strstr(output, "Refusing directory removal"));
}

void test_confirmed_file_removal_calls_unlink(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_stat_StubWithCallback(file_stat);
    f_unlink_ExpectAnyArgsAndReturn(FR_OK);
    run("rm /a --confirm");
    TEST_ASSERT_NOT_NULL(strstr(output, "FatFs: OK"));
}

void test_df_reports_64_bit_capacity(void)
{
    DWORD clusters = 1000u;
    FATFS filesystem = { .n_fatent = 2000002u, .csize = 32u };
    FATFS *pointer = &filesystem;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_getfree_ExpectAnyArgsAndReturn(FR_OK);
    f_getfree_ReturnThruPtr_nclst(&clusters);
    f_getfree_ReturnThruPtr_fatfs(&pointer);
    run("df");
    TEST_ASSERT_NOT_NULL(strstr(output, "total=32768000000 bytes free=16384000 bytes"));
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
    pf_storage_command_execute(true, "storage", "status", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "state=usb owner=USB medium=sd bus=sdio present=yes"));
    TEST_ASSERT_NOT_NULL(strstr(output, "bytes=15808331776"));
    TEST_ASSERT_NOT_NULL(strstr(output, "last_error=write:"));
}

void test_removed_sd_family_reports_usage(void)
{
    pf_storage_command_execute(false, "sd", "info", reply, NULL);
    TEST_ASSERT_NOT_NULL(strstr(output, "Usage: storage status"));
}

static bool disconnected_reply(const char *line, void *context)
{
    (void)line;
    (void)context;
    return false;
}

void test_cancelled_listing_still_closes_directory(void)
{
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_directory);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    pf_storage_command_execute(false, "fs", "ls", disconnected_reply, NULL);
}

void test_disconnected_output_does_not_skip_write_sync_or_close(void)
{
    expected_mode = FA_WRITE | FA_CREATE_NEW;
    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    f_open_StubWithCallback(open_file);
    f_write_StubWithCallback(write_file);
    f_sync_ExpectAnyArgsAndReturn(FR_OK);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    pf_storage_command_execute(false, "fs", "write /a \"hello world\"", disconnected_reply, NULL);
}

void test_rejects_long_arguments_without_truncation(void)
{
    char arguments[129];
    memset(arguments, 'a', sizeof(arguments) - 1u);
    arguments[sizeof(arguments) - 1u] = '\0';
    run(arguments);
    TEST_ASSERT_NOT_NULL(strstr(output, "Arguments too long"));
}