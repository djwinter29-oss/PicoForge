#include "unity.h"
#include "self_test.h"
#include "Mockff.h"
#include "Mockstorage.h"

#include <string.h>

static pf_self_test_status_t final_status;
static uint32_t timer;
static uint32_t write_offset;
static uint32_t read_offset;
static bool corrupt;
static bool cancel_write;
static bool short_write;
static FATFS filesystem;
static DWORD available_clusters;
static FRESULT sync_result;
static FRESULT close_result;
static FRESULT unlink_result;
static unsigned close_calls;
static unsigned unlink_calls;
static bool expect_preallocation;

static uint32_t clock_ms(void *context)
{
    (void)context;
    timer += 10u;
    return timer;
}

static bool update(const pf_self_test_status_t *status, void *context)
{
    (void)context;
    final_status = *status;
    return !(cancel_write && (status->state == PF_SELF_TEST_WRITING) && (status->written_bytes > 0u));
}

static FRESULT get_free(const TCHAR *path, DWORD *count, FATFS **volume, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:", path);
    *count = available_clusters;
    *volume = &filesystem;
    return FR_OK;
}

static FRESULT open_file(FIL *file, const TCHAR *path, BYTE mode, int calls)
{
    (void)file;
    TEST_ASSERT_EQUAL_STRING(PF_STORAGE_SELF_TEST_PATH, path);
    TEST_ASSERT_EQUAL_UINT8(calls == 0 ? FA_WRITE | FA_CREATE_NEW : FA_READ, mode);
    return FR_OK;
}

static FRESULT open_existing_file(FIL *file, const TCHAR *path, BYTE mode, int calls)
{
    TEST_ASSERT_EQUAL_INT(0, calls);
    (void)open_file(file, path, mode, calls);
    return FR_EXIST;
}

static uint8_t pattern(uint32_t offset)
{
    uint32_t word = (offset / 4u) ^ 0xa5c391f7u;
    return (uint8_t)(word >> ((offset % 4u) * 8u));
}

static FRESULT write_file(FIL *file, const void *data, UINT size, UINT *written, int calls)
{
    (void)file;
    (void)calls;
    const uint8_t *bytes = data;
    TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SELF_TEST_BUFFER_SIZE, size);
    for (UINT index = 0u; index < size; ++index) {
        TEST_ASSERT_EQUAL_UINT8(pattern(write_offset + index), bytes[index]);
    }
    *written = short_write ? size / 2u : size;
    write_offset += *written;
    return FR_OK;
}

static FRESULT read_file(FIL *file, void *data, UINT size, UINT *received, int calls)
{
    (void)file;
    (void)calls;
    uint8_t *bytes = data;
    for (UINT index = 0u; index < size; ++index) {
        bytes[index] = pattern(read_offset + index);
    }
    if (corrupt) {
        bytes[7] ^= 1u;
    }
    *received = size;
    read_offset += size;
    return FR_OK;
}

static FRESULT close_file(FIL *file, int calls)
{
    (void)file;
    (void)calls;
    ++close_calls;
    return close_result;
}

static FRESULT unlink_file(const TCHAR *path, int calls)
{
    TEST_ASSERT_EQUAL_STRING(PF_STORAGE_SELF_TEST_PATH, path);
    TEST_ASSERT_EQUAL_INT(0, calls);
    ++unlink_calls;
    TEST_ASSERT_EQUAL(PF_SELF_TEST_CLEANUP, final_status.state);
    TEST_ASSERT_GREATER_THAN_UINT(0u, close_calls);
    return unlink_result;
}

static void prepare(void)
{
    pf_storage_is_mounted_ExpectAndReturn(true);
    f_getfree_StubWithCallback(get_free);
    f_open_StubWithCallback(open_file);
    if (expect_preallocation) {
        f_expand_ExpectAnyArgsAndReturn(FR_OK);
    }
    f_write_StubWithCallback(write_file);
    f_read_StubWithCallback(read_file);
    f_sync_ExpectAnyArgsAndReturn(sync_result);
    f_close_StubWithCallback(close_file);
    if (close_result == FR_OK) {
        f_unlink_StubWithCallback(unlink_file);
    }
}

void setUp(void)
{
    pf_storage_self_test_set_preallocation_enabled(false);
    memset(&final_status, 0, sizeof(final_status));
    memset(&filesystem, 0, sizeof(filesystem));
    filesystem.csize = 8u;
    timer = write_offset = read_offset = 0u;
    corrupt = cancel_write = short_write = false;
    expect_preallocation = false;
    available_clusters = 100000u;
    sync_result = close_result = unlink_result = FR_OK;
    close_calls = unlink_calls = 0u;
}
void tearDown(void) {}

void test_self_test_writes_and_verifies_every_byte_then_cleans_up(void)
{
    prepare();
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_PASSED, final_status.state);
    TEST_ASSERT_EQUAL_UINT32(1048576u, final_status.written_bytes);
    TEST_ASSERT_EQUAL_UINT32(1048576u, final_status.read_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.mismatch_count);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, final_status.write_elapsed_ms);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, final_status.read_elapsed_ms);
    TEST_ASSERT_EQUAL_UINT(2u, close_calls);
    TEST_ASSERT_EQUAL_UINT(1u, unlink_calls);
}

void test_preallocated_self_test_keeps_full_verification_and_cleanup(void)
{
    pf_storage_self_test_set_preallocation_enabled(true);
    expect_preallocation = true;
    prepare();
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_PASSED, final_status.state);
    TEST_ASSERT_EQUAL_UINT32(1048576u, final_status.written_bytes);
    TEST_ASSERT_EQUAL_UINT32(1048576u, final_status.read_bytes);
    TEST_ASSERT_EQUAL_UINT(2u, close_calls);
    TEST_ASSERT_EQUAL_UINT(1u, unlink_calls);
}

void test_preallocation_failure_closes_and_removes_only_the_new_file(void)
{
    pf_storage_self_test_set_preallocation_enabled(true);
    pf_storage_is_mounted_ExpectAndReturn(true);
    f_getfree_StubWithCallback(get_free);
    f_open_StubWithCallback(open_file);
    f_expand_ExpectAnyArgsAndReturn(FR_DENIED);
    f_close_StubWithCallback(close_file);
    f_unlink_ExpectAnyArgsAndReturn(FR_OK);
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(FR_DENIED, final_status.filesystem_result);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.written_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.write_elapsed_ms);
    TEST_ASSERT_EQUAL_UINT(1u, close_calls);
}

void test_self_test_detects_corrupt_readback(void)
{
    prepare();
    corrupt = true;
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_VERIFY, final_status.error);
    TEST_ASSERT_EQUAL_UINT32(1u, final_status.mismatch_count);
}

void test_self_test_cancel_finishes_sync_close_and_cleanup(void)
{
    prepare();
    cancel_write = true;
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_CANCELLED, final_status.state);
    TEST_ASSERT_EQUAL_UINT32(PF_STORAGE_SELF_TEST_BUFFER_SIZE, final_status.written_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.read_bytes);
    TEST_ASSERT_EQUAL_UINT(1u, close_calls);
}

void test_self_test_rejects_invalid_size(void)
{
    pf_storage_self_test_run(NULL, 0u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_SIZE, final_status.error);
}

void test_self_test_reuses_existing_mount_and_leaves_it_mounted(void)
{
    prepare();
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_PASSED, final_status.state);
    TEST_ASSERT_EQUAL_UINT32(1048576u, final_status.written_bytes);
    TEST_ASSERT_EQUAL_UINT32(1048576u, final_status.read_bytes);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}

void test_self_test_preserves_existing_mount_after_verification_failure(void)
{
    prepare();
    corrupt = true;
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_VERIFY, final_status.error);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}

void test_self_test_preserves_existing_mount_after_cancellation(void)
{
    prepare();
    cancel_write = true;
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_CANCELLED, final_status.state);
    TEST_ASSERT_EQUAL_UINT(1u, close_calls);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}

void test_self_test_rejects_unmounted_volume_without_mounting_or_file_io(void)
{
    pf_storage_is_mounted_ExpectAndReturn(false);
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_NOT_MOUNTED, final_status.error);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.written_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.read_bytes);
    TEST_ASSERT_EQUAL_UINT8(0u, final_status.flags);
}

void test_self_test_short_write_is_not_a_pass(void)
{
    prepare();
    short_write = true;
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(FR_DISK_ERR, final_status.filesystem_result);
}

void test_self_test_refuses_existing_file_without_unlink_or_other_file_io(void)
{
    pf_storage_is_mounted_ExpectAndReturn(true);
    f_getfree_StubWithCallback(get_free);
    f_open_StubWithCallback(open_existing_file);
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_EXISTS, final_status.error);
    TEST_ASSERT_EQUAL(FR_EXIST, final_status.filesystem_result);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.written_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.read_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.write_elapsed_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, final_status.read_elapsed_ms);
    TEST_ASSERT_EQUAL_UINT8(PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}

void test_self_test_open_failure_does_not_unlink_a_file(void)
{
    pf_storage_is_mounted_ExpectAndReturn(true);
    f_getfree_StubWithCallback(get_free);
    f_open_ExpectAnyArgsAndReturn(FR_DENIED);
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_FILESYSTEM, final_status.error);
    TEST_ASSERT_EQUAL(FR_DENIED, final_status.filesystem_result);
    TEST_ASSERT_EQUAL_UINT8(PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}

void test_self_test_insufficient_space_does_not_create_a_file(void)
{
    available_clusters = 0u;
    pf_storage_is_mounted_ExpectAndReturn(true);
    f_getfree_StubWithCallback(get_free);
    pf_storage_self_test_run(NULL, 32u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_SPACE, final_status.error);
}

void test_self_test_sync_failure_is_not_a_pass(void)
{
    sync_result = FR_DISK_ERR;
    prepare();
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(FR_DISK_ERR, final_status.filesystem_result);
    TEST_ASSERT_EQUAL_UINT(1u, close_calls);
}

void test_self_test_close_failure_preserves_file_for_recovery(void)
{
    close_result = FR_DISK_ERR;
    prepare();
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FILE_REMAINS | PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}

void test_self_test_reports_unlink_cleanup_failure(void)
{
    unlink_result = FR_DENIED;
    prepare();
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_CLEANUP, final_status.error);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FILE_REMAINS | PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}
void test_preallocation_and_close_failure_preserves_new_file(void)
{
    pf_storage_self_test_set_preallocation_enabled(true);
    pf_storage_is_mounted_ExpectAndReturn(true);
    f_getfree_StubWithCallback(get_free);
    f_open_StubWithCallback(open_file);
    f_expand_ExpectAnyArgsAndReturn(FR_DENIED);
    close_result = FR_DISK_ERR;
    f_close_StubWithCallback(close_file);
    /* No unlink is permitted when FatFs could still hold the file open. */
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(FR_DENIED, final_status.filesystem_result);
    TEST_ASSERT_EQUAL_UINT(1u, close_calls);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FILE_REMAINS | PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}

static FRESULT close_read_failure(FIL *file, int calls)
{
    (void)file;
    ++close_calls;
    return calls == 0 ? FR_OK : FR_DISK_ERR;
}

void test_read_close_failure_preserves_verified_file(void)
{
    prepare();
    f_close_StubWithCallback(close_read_failure);
    pf_storage_self_test_run(NULL, 1u, clock_ms, update, NULL);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FAILED, final_status.state);
    TEST_ASSERT_EQUAL(FR_DISK_ERR, final_status.filesystem_result);
    TEST_ASSERT_EQUAL_UINT32(1048576u, final_status.read_bytes);
    TEST_ASSERT_EQUAL_UINT(0u, unlink_calls);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_FILE_REMAINS | PF_SELF_TEST_VOLUME_MOUNTED, final_status.flags);
}
