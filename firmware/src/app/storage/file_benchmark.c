/** @file Application file-verification benchmark and cleanup. */
#include "file_benchmark.h"

#include <string.h>

static uint32_t test_buffer[PF_FILE_BENCHMARK_BUFFER_SIZE / sizeof(uint32_t)];
static bool preallocate = PF_STORAGE_SELF_TEST_PREALLOCATE != 0;

void pf_file_benchmark_set_preallocation_enabled(bool enabled)
{
    preallocate = enabled;
}

static uint32_t pattern_word(uint32_t offset)
{
    return (offset / 4u) ^ 0xa5c391f7u;
}

/** @brief Fill word-aligned file chunks; little-endian word bytes define the persisted pattern. */
static void fill_pattern(uint32_t *words, uint32_t offset, uint32_t size)
{
    for (uint32_t index = 0u; index < size / 4u; ++index) {
        words[index] = pattern_word(offset + index * 4u);
    }
}

/** @brief Count differing bytes in one complete word-aligned verification chunk. */
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

static void filesystem_error(pf_file_benchmark_status_t *status, FRESULT result)
{
    if (result == FR_EXIST) {
        status->error = PF_FILE_BENCHMARK_ERROR_EXISTS;
    } else if (result == FR_NOT_READY) {
        status->error = PF_FILE_BENCHMARK_ERROR_MEDIA;
    } else {
        status->error = PF_FILE_BENCHMARK_ERROR_FILESYSTEM;
    }
    status->filesystem_result = status->error == PF_FILE_BENCHMARK_ERROR_FILESYSTEM ? result : FR_OK;
}

static pf_file_benchmark_error_t preflight_mounted_volume(uint32_t size_mib, FRESULT *filesystem_result)
{
    FILINFO existing;
    FRESULT result = f_stat(PF_FILE_BENCHMARK_PATH, &existing);
    if (result == FR_OK) {
        return PF_FILE_BENCHMARK_ERROR_EXISTS;
    }
    if (result != FR_NO_FILE) {
        if (result == FR_NOT_READY) {
            return PF_FILE_BENCHMARK_ERROR_MEDIA;
        }
        if (filesystem_result != NULL) {
            *filesystem_result = result;
        }
        return PF_FILE_BENCHMARK_ERROR_FILESYSTEM;
    }

    DWORD free_clusters;
    FATFS *filesystem = NULL;
    result = f_getfree(PF_FATFS_VOLUME_PATH, &free_clusters, &filesystem);
    if ((result != FR_OK) || (filesystem == NULL)) {
        if (result == FR_NOT_READY) {
            return PF_FILE_BENCHMARK_ERROR_MEDIA;
        }
        if (filesystem_result != NULL) {
            *filesystem_result = result != FR_OK ? result : FR_INT_ERR;
        }
        return PF_FILE_BENCHMARK_ERROR_FILESYSTEM;
    }
    uint64_t free_bytes = (uint64_t)free_clusters * filesystem->csize * FF_MAX_SS;
    uint64_t target_bytes = (uint64_t)size_mib * 1024u * 1024u;
    if (free_bytes < target_bytes + PF_FILE_BENCHMARK_BUFFER_SIZE) {
        return PF_FILE_BENCHMARK_ERROR_SPACE;
    }
    return PF_FILE_BENCHMARK_ERROR_NONE;
}

pf_file_benchmark_error_t pf_file_benchmark_preflight(uint32_t size_mib, FRESULT *filesystem_result)
{
    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if ((size_mib == 0u) || (size_mib > PF_FILE_BENCHMARK_MAX_MIB)) {
        return PF_FILE_BENCHMARK_ERROR_SIZE;
    }
    if (!pf_fatfs_volume_is_mounted()) {
        return PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED;
    }
    return preflight_mounted_volume(size_mib, filesystem_result);
}

static void run_benchmark(uint32_t size_mib, pf_file_benchmark_clock_t clock_ms,
                          pf_file_benchmark_update_t update, void *context, bool preflighted)
{
    pf_file_benchmark_status_t status = { .state = PF_FILE_BENCHMARK_PREPARING };
    FIL file;
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
    if ((size_mib == 0u) || (size_mib > PF_FILE_BENCHMARK_MAX_MIB)) {
        status.error = PF_FILE_BENCHMARK_ERROR_SIZE;
        goto finish;
    }
    status.target_bytes = size_mib * 1024u * 1024u;
    if (!preflighted && !pf_fatfs_volume_is_mounted()) {
        status.error = PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED;
        goto finish;
    }
    status.flags |= PF_FILE_BENCHMARK_VOLUME_MOUNTED;
    if (!update(&status, context)) {
        cancelled = true;
        goto finish;
    }
    if (!preflighted) {
        FRESULT preflight_result = FR_OK;
        pf_file_benchmark_error_t preflight_error = preflight_mounted_volume(size_mib, &preflight_result);
        if (preflight_error != PF_FILE_BENCHMARK_ERROR_NONE) {
            status.error = preflight_error;
            status.filesystem_result = preflight_result;
            goto finish;
        }
    }
    if (!update(&status, context)) {
        cancelled = true;
        goto cleanup;
    }
    result = f_open(&file, PF_FILE_BENCHMARK_PATH, FA_WRITE | FA_CREATE_NEW);
    if (result != FR_OK) {
        filesystem_error(&status, result);
        goto cleanup;
    }
    file_created = file_open = true;
    if (preallocate) {
        result = f_expand(&file, status.target_bytes, 1u);
        if (result != FR_OK) {
            filesystem_error(&status, result);
            goto cleanup;
        }
    }
    status.state = PF_FILE_BENCHMARK_WRITING;
    phase_start = clock_ms(context);
    while (status.written_bytes < status.target_bytes) {
        if (!update(&status, context)) {
            cancelled = true;
            break;
        }
        fill_pattern(test_buffer, status.written_bytes, PF_FILE_BENCHMARK_BUFFER_SIZE);
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
    if ((result != FR_OK) && (status.error == PF_FILE_BENCHMARK_ERROR_NONE)) {
        filesystem_error(&status, result);
    }
    result = f_close(&file);
    file_open = false;
    close_failed = result != FR_OK;
    status.write_elapsed_ms = clock_ms(context) - phase_start;
    if (close_failed && (status.error == PF_FILE_BENCHMARK_ERROR_NONE)) {
        filesystem_error(&status, result);
    }
    if (cancelled || (status.error != PF_FILE_BENCHMARK_ERROR_NONE)) {
        goto cleanup;
    }
    result = f_open(&file, PF_FILE_BENCHMARK_PATH, FA_READ);
    if (result != FR_OK) {
        filesystem_error(&status, result);
        goto cleanup;
    }
    file_open = true;
    status.state = PF_FILE_BENCHMARK_READING;
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
            status.error = PF_FILE_BENCHMARK_ERROR_VERIFY;
            break;
        }
    }
    result = f_close(&file);
    file_open = false;
    close_failed = result != FR_OK;
    status.read_elapsed_ms = clock_ms(context) - phase_start;
    if (close_failed && (status.error == PF_FILE_BENCHMARK_ERROR_NONE)) {
        filesystem_error(&status, result);
    }

cleanup:
    status.state = PF_FILE_BENCHMARK_CLEANUP;
    (void)update(&status, context);
    if (file_open) {
        result = f_close(&file);
        close_failed = result != FR_OK;
    }
    if (file_created) {
        result = close_failed ? FR_LOCKED : f_unlink(PF_FILE_BENCHMARK_PATH);
        if (result != FR_OK) {
            status.flags |= PF_FILE_BENCHMARK_FILE_REMAINS;
            if (status.error == PF_FILE_BENCHMARK_ERROR_NONE) {
                status.error = PF_FILE_BENCHMARK_ERROR_CLEANUP;
                status.filesystem_result = result;
            }
        }
    }
finish:
    status.state = status.error != PF_FILE_BENCHMARK_ERROR_NONE ? PF_FILE_BENCHMARK_FAILED
        : cancelled ? PF_FILE_BENCHMARK_CANCELLED : PF_FILE_BENCHMARK_PASSED;
    (void)update(&status, context);
}

void pf_file_benchmark_run(uint32_t size_mib,
                           pf_file_benchmark_clock_t clock_ms, pf_file_benchmark_update_t update,
                           void *context)
{
    run_benchmark(size_mib, clock_ms, update, context, false);
}

void pf_file_benchmark_run_after_preflight(uint32_t size_mib,
                                           pf_file_benchmark_clock_t clock_ms,
                                           pf_file_benchmark_update_t update, void *context)
{
    run_benchmark(size_mib, clock_ms, update, context, true);
}