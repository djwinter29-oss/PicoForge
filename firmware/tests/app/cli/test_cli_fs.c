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

void test_rejects_unsafe_paths_and_unconfirmed_removal(void)
{
    run("stat /../a");
    TEST_ASSERT_NOT_NULL(strstr(output, "Invalid path"));
    run("rm /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "requires fs rm"));
}

void test_reports_existing_file_without_overwriting(void)
{
    f_open_ExpectAnyArgsAndReturn(FR_EXIST);
    run("write /existing text");
    TEST_ASSERT_NOT_NULL(strstr(output, "EXIST"));
}

void test_rejects_invalid_cat_limits(void)
{
    run("cat /a 4097");
    TEST_ASSERT_NOT_NULL(strstr(output, "1..4096"));
}

void test_creates_directory_and_reports_fatfs_errors(void)
{
    f_mkdir_StubWithCallback(mkdir_denied);
    run("mkdir /new");
    TEST_ASSERT_NOT_NULL(strstr(output, "DENIED"));
}

void test_lists_directory_and_closes_handle(void)
{
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_directory);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls");
    TEST_ASSERT_NOT_NULL(strstr(output, "d 10 folder"));
}

void test_directory_failure_still_closes_handle(void)
{
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_ExpectAnyArgsAndReturn(FR_DISK_ERR);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_stat_reports_size_and_attributes(void)
{
    f_stat_StubWithCallback(file_stat);
    run("stat /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "size=42 attributes=0x20"));
}

void test_cat_bounds_read_and_escapes_terminal_controls(void)
{
    f_open_StubWithCallback(open_file);
    f_read_StubWithCallback(read_file);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("cat /a 4");
    TEST_ASSERT_NOT_NULL(strstr(output, "a\\x0a\\x1bz"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Output limit reached"));
}

void test_cat_closes_file_after_read_failure(void)
{
    f_open_StubWithCallback(open_file);
    f_read_ExpectAnyArgsAndReturn(FR_DISK_ERR);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("cat /a");
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_write_uses_create_new_then_syncs_and_closes(void)
{
    expected_mode = FA_WRITE | FA_CREATE_NEW;
    f_open_StubWithCallback(open_file);
    f_write_StubWithCallback(write_file);
    f_sync_ExpectAnyArgsAndReturn(FR_OK);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("write /a \"hello world\"");
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}

void test_append_uses_append_mode_and_reports_sync_failure(void)
{
    expected_mode = FA_WRITE | FA_OPEN_APPEND;
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
    f_open_StubWithCallback(open_file);
    f_write_StubWithCallback(write_file);
    f_sync_ExpectAnyArgsAndReturn(FR_OK);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("write /a \"hello world\"");
    TEST_ASSERT_NOT_NULL(strstr(output, "DISK_ERR"));
}

void test_move_preserves_existing_destination(void)
{
    f_rename_StubWithCallback(rename_file);
    run("mv /a /b");
    TEST_ASSERT_NOT_NULL(strstr(output, "EXIST"));
}

void test_remove_requires_confirmation_and_rejects_directories(void)
{
    directory_entry = true;
    f_stat_StubWithCallback(file_stat);
    run("rm /a --confirm");
    TEST_ASSERT_NOT_NULL(strstr(output, "Refusing directory removal"));
}

void test_confirmed_file_removal_calls_unlink(void)
{
    f_stat_StubWithCallback(file_stat);
    f_unlink_ExpectAnyArgsAndReturn(FR_OK);
    run("rm /a --confirm");
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}

void test_df_reports_64_bit_capacity(void)
{
    DWORD clusters = 1000u;
    FATFS filesystem = { .n_fatent = 2000002u, .csize = 32u };
    FATFS *pointer = &filesystem;
    f_getfree_ExpectAnyArgsAndReturn(FR_OK);
    f_getfree_ReturnThruPtr_nclst(&clusters);
    f_getfree_ReturnThruPtr_fatfs(&pointer);
    run("df");
    TEST_ASSERT_NOT_NULL(strstr(output, "total=32768000000 bytes free=16384000 bytes"));
}

void test_interrupted_listing_reports_incomplete_delivery_before_fatfs_status(void)
{
    unsigned calls = 0u;
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_directory);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    pf_cli_fs_execute("ls", 1u, NULL, NULL, interrupt_first_reply, &calls);
    TEST_ASSERT_NOT_NULL(strstr(output, "Listing output interrupted"));
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}

void test_interrupted_cat_reports_incomplete_delivery_and_closes_file(void)
{
    unsigned calls = 0u;
    expected_mode = FA_READ;
    f_open_StubWithCallback(open_file);
    f_read_StubWithCallback(read_file);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    pf_cli_fs_execute("cat", 3u, "/a", "4", interrupt_first_reply, &calls);
    TEST_ASSERT_NOT_NULL(strstr(output, "Cat output interrupted"));
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}

void test_cancelled_listing_still_closes_directory(void)
{
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_directory);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    pf_cli_fs_execute("ls", 1u, NULL, NULL, disconnected_reply, NULL);
}

void test_disconnected_output_does_not_skip_write_sync_or_close(void)
{
    expected_mode = FA_WRITE | FA_CREATE_NEW;
    f_open_StubWithCallback(open_file);
    f_write_StubWithCallback(write_file);
    f_sync_ExpectAnyArgsAndReturn(FR_OK);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    pf_cli_fs_execute("write", 3u, "/a", "hello world", disconnected_reply, NULL);
}

void test_argument_count_boundaries_are_rejected(void)
{
    const char *const invalid[] = { "stat", "stat /a x", "mkdir", "cat", "cat /a 1 x", "write /a", "write /a b c",
                                    "append /a", "mv /a", "mv /a /b /c", "rm /a", "ls /a /b" };

    for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        output[0] = '\0';
        run(invalid[index]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Invalid arguments"), invalid[index]);
    }
}

void test_cat_of_all_control_bytes_fits_one_reply_line(void)
{
    f_open_StubWithCallback(open_file);
    f_read_StubWithCallback(read_full_chunk);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("cat /a 48");
    TEST_ASSERT_NOT_NULL(strstr(output, "\\x01\\x01"));
    TEST_ASSERT_EQUAL_UINT(48u * 4u + 1u, (unsigned)(strchr(output, '\n') - output) + 1u);
}

void test_move_rejects_an_invalid_destination_path(void)
{
    run("mv /a /b/../c");
    TEST_ASSERT_NOT_NULL(strstr(output, "Invalid path"));
}

void test_cat_limit_accepts_4096_and_rejects_4097(void)
{
    run("cat /a 4097");
    TEST_ASSERT_NOT_NULL(strstr(output, "1..4096"));
    output[0] = '\0';
    f_open_StubWithCallback(open_file);
    f_read_ExpectAnyArgsAndReturn(FR_DISK_ERR);
    f_close_ExpectAnyArgsAndReturn(FR_OK);
    run("cat /a 4096");
    TEST_ASSERT_NULL(strstr(output, "1..4096"));
}

void test_ls_without_a_path_lists_the_volume_root(void)
{
    f_opendir_StubWithCallback(open_directory_root);
    f_readdir_StubWithCallback(read_directory);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls");
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}

void test_long_listing_name_is_marked_and_not_split_inside_utf8(void)
{
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_long_directory_name);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls");
    TEST_ASSERT_NOT_NULL(strstr(output, " [name truncated]"));
    TEST_ASSERT_NOT_NULL(strstr(output, "use host file tools"));
    TEST_ASSERT_NULL(strchr(output, (char)0xe2));
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}

void test_listing_name_at_display_limit_is_not_marked(void)
{
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_exact_directory_name);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls");
    TEST_ASSERT_NULL(strstr(output, "[name truncated]"));
    TEST_ASSERT_NOT_NULL(strstr(output, "use host file tools"));
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}

void test_short_unicode_listing_name_is_preserved_but_not_actionable_by_cli(void)
{
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_unicode_directory_name);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls");
    TEST_ASSERT_NOT_NULL(strstr(output, "\xe2\x82\xac.txt"));
    TEST_ASSERT_NOT_NULL(strstr(output, "use host file tools"));
    TEST_ASSERT_NULL(strstr(output, "[name truncated]"));
}

void test_listing_path_limit_accounts_for_parent_directory(void)
{
    char arguments[105] = "ls /";
    memset(arguments + 4u, 'a', 94u);
    arguments[98] = '\0';
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_directory);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run(arguments);
    TEST_ASSERT_NOT_NULL(strstr(output, "use host file tools"));
}

void test_listing_summarizes_unsupported_paths_once_for_many_entries(void)
{
    f_opendir_ExpectAnyArgsAndReturn(FR_OK);
    f_readdir_StubWithCallback(read_multiple_unsupported_names);
    f_closedir_ExpectAnyArgsAndReturn(FR_OK);
    run("ls");
    TEST_ASSERT_EQUAL_UINT(1u, occurrences(output, "WARN Some listed paths"));
    TEST_ASSERT_NOT_NULL(strstr(output, "OK FatFs: OK"));
}
