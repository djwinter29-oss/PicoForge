#include "self_test.h"
#include "sd_card_block.h"

#include <string.h>

static uint32_t test_buffer[PF_STORAGE_SELF_TEST_BUFFER_SIZE / sizeof(uint32_t)];

static uint32_t pattern_word(uint32_t offset)
{
    return (offset / 4u) ^ 0xa5c391f7u;
}

/* Offsets and sizes are word multiples; the little-endian bytes of each word are the on-card pattern. */
static void fill_pattern(uint32_t *words, uint32_t offset, uint32_t size)
{
    for (uint32_t index = 0u; index < size / 4u; ++index) {
        words[index] = pattern_word(offset + index * 4u);
    }
}

static uint32_t count_mismatched_bytes(const uint32_t *words, uint32_t offset, uint32_t size)
{
    uint32_t count = 0u;

    for (uint32_t index = 0u; index < size / 4u; ++index) {
        uint32_t difference = words[index] ^ pattern_word(offset + index * 4u);
        for (; difference != 0u; difference >>= 8u) {
            count += (difference & 0xffu) != 0u;
        }
    }
    return count;
}

static void filesystem_error(pf_self_test_status_t *status, FRESULT result)
{
    status->error = result == FR_EXIST ? PF_SELF_TEST_ERROR_EXISTS : PF_SELF_TEST_ERROR_FILESYSTEM;
    status->filesystem_result = result;
}

void pf_storage_self_test_run(const pf_sd_card_config_t *config, uint32_t size_mib,
                              pf_self_test_clock_t clock_ms, pf_self_test_update_t update,
                              void *context)
{
    pf_self_test_status_t status = { .state = PF_SELF_TEST_MOUNTING };
    FIL file;
    bool mounted = false;
    bool file_created = false;
    bool file_open = false;
    bool cancelled = false;
    bool close_failed = false;
    uint32_t phase_start = 0u;
    unsigned char *buffer = (unsigned char *)test_buffer;
    FRESULT result = FR_OK;

    if ((clock_ms == NULL) || (update == NULL)) {
        return;
    }
    if ((size_mib == 0u) || (size_mib > PF_STORAGE_SELF_TEST_MAX_MIB)) {
        status.error = PF_SELF_TEST_ERROR_SIZE;
        goto finish;
    }
    status.target_bytes = size_mib * 1024u * 1024u;
    if (pf_storage_is_mounted()) {
        status.error = PF_SELF_TEST_ERROR_MOUNTED;
        goto finish;
    }
    if (!update(&status, context)) {
        cancelled = true;
        goto finish;
    }
    pf_storage_result_t mount_result = pf_storage_mount(config, &result);
    if (mount_result != PF_STORAGE_OK) {
        status.error = mount_result == PF_STORAGE_FILESYSTEM_ERROR
            ? PF_SELF_TEST_ERROR_FILESYSTEM : PF_SELF_TEST_ERROR_CARD;
        status.filesystem_result = result;
        goto finish;
    }
    mounted = true;
    DWORD free_clusters;
    FATFS *filesystem;
    result = f_getfree(PF_STORAGE_VOLUME, &free_clusters, &filesystem);
    if (result != FR_OK) {
        filesystem_error(&status, result);
        goto cleanup;
    }
    uint64_t free_bytes = (uint64_t)free_clusters * filesystem->csize * PF_SD_CARD_BLOCK_SIZE;
    if (free_bytes < (uint64_t)status.target_bytes + PF_STORAGE_SELF_TEST_BUFFER_SIZE) {
        status.error = PF_SELF_TEST_ERROR_SPACE;
        goto cleanup;
    }
    if (!update(&status, context)) {
        cancelled = true;
        goto cleanup;
    }
    result = f_open(&file, PF_STORAGE_SELF_TEST_PATH, FA_WRITE | FA_CREATE_NEW);
    if (result != FR_OK) {
        filesystem_error(&status, result);
        goto cleanup;
    }
    file_created = file_open = true;
    status.state = PF_SELF_TEST_WRITING;
    phase_start = clock_ms(context);
    while (status.written_bytes < status.target_bytes) {
        if (!update(&status, context)) {
            cancelled = true;
            break;
        }
        fill_pattern(test_buffer, status.written_bytes, PF_STORAGE_SELF_TEST_BUFFER_SIZE);
        UINT written = 0u;
        result = f_write(&file, buffer, sizeof(test_buffer), &written);
        status.written_bytes += written;
        status.write_elapsed_ms = clock_ms(context) - phase_start;
        if ((result != FR_OK) || (written != sizeof(test_buffer))) {
            filesystem_error(&status, result != FR_OK ? result : FR_DISK_ERR);
            break;
        }
    }
    result = f_sync(&file);
    if ((result != FR_OK) && (status.error == PF_SELF_TEST_ERROR_NONE)) {
        filesystem_error(&status, result);
    }
    result = f_close(&file);
    file_open = false;
    close_failed = result != FR_OK;
    status.write_elapsed_ms = clock_ms(context) - phase_start;
    if (close_failed && (status.error == PF_SELF_TEST_ERROR_NONE)) {
        filesystem_error(&status, result);
    }
    if (cancelled || (status.error != PF_SELF_TEST_ERROR_NONE)) {
        goto cleanup;
    }
    result = f_open(&file, PF_STORAGE_SELF_TEST_PATH, FA_READ);
    if (result != FR_OK) {
        filesystem_error(&status, result);
        goto cleanup;
    }
    file_open = true;
    status.state = PF_SELF_TEST_READING;
    phase_start = clock_ms(context);
    while (status.read_bytes < status.target_bytes) {
        if (!update(&status, context)) {
            cancelled = true;
            break;
        }
        UINT received = 0u;
        result = f_read(&file, buffer, sizeof(test_buffer), &received);
        if ((result != FR_OK) || (received != sizeof(test_buffer))) {
            filesystem_error(&status, result != FR_OK ? result : FR_DISK_ERR);
            break;
        }
        status.mismatch_count += count_mismatched_bytes(test_buffer, status.read_bytes, received);
        status.read_bytes += received;
        status.read_elapsed_ms = clock_ms(context) - phase_start;
        if (status.mismatch_count != 0u) {
            status.error = PF_SELF_TEST_ERROR_VERIFY;
            break;
        }
    }
    result = f_close(&file);
    file_open = false;
    close_failed = result != FR_OK;
    status.read_elapsed_ms = clock_ms(context) - phase_start;
    if (close_failed && (status.error == PF_SELF_TEST_ERROR_NONE)) {
        filesystem_error(&status, result);
    }

cleanup:
    status.state = PF_SELF_TEST_CLEANUP;
    (void)update(&status, context);
    if (file_open) {
        result = f_close(&file);
        close_failed = result != FR_OK;
    }
    if (file_created) {
        result = close_failed ? FR_LOCKED : f_unlink(PF_STORAGE_SELF_TEST_PATH);
        if (result != FR_OK) {
            status.flags |= PF_SELF_TEST_FILE_REMAINS;
            if (status.error == PF_SELF_TEST_ERROR_NONE) {
                status.error = PF_SELF_TEST_ERROR_CLEANUP;
                status.filesystem_result = result;
            }
        }
    }
    if (mounted && (pf_storage_unmount(&result) != PF_STORAGE_OK)) {
        status.flags |= PF_SELF_TEST_VOLUME_MOUNTED;
        if (status.error == PF_SELF_TEST_ERROR_NONE) {
            status.error = PF_SELF_TEST_ERROR_CLEANUP;
            status.filesystem_result = result;
        }
    }
finish:
    status.state = status.error != PF_SELF_TEST_ERROR_NONE ? PF_SELF_TEST_FAILED
        : cancelled ? PF_SELF_TEST_CANCELLED : PF_SELF_TEST_PASSED;
    (void)update(&status, context);
}