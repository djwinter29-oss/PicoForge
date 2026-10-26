/** @file Application file-verification benchmark and cleanup. */
#include "file_benchmark.h"

#include <string.h>

/* ponytail: static 32 KiB chunk (always resident); RP2040/RP2350 are little-endian so words are written as-is. */
static uint32_t test_buffer[PF_FILE_BENCHMARK_BUFFER_SIZE / sizeof(uint32_t)];
/* Whole-MiB targets must be whole chunks: the write and read loops transfer full buffers only. */
_Static_assert((1024u * 1024u) % PF_FILE_BENCHMARK_BUFFER_SIZE == 0u, "MiB target must be whole chunks");
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

/**
 * @brief Map a FatFs result to the benchmark error shared by preflight and execution.
 * @param result Failing FatFs result.
 * @param detail Receives the FatFs result for FILESYSTEM errors, otherwise FR_OK; may be NULL.
 */
static pf_file_benchmark_error_t map_fresult(FRESULT result, FRESULT *detail)
{
    pf_file_benchmark_error_t error = PF_FILE_BENCHMARK_ERROR_FILESYSTEM;

    if (result == FR_EXIST) {
        error = PF_FILE_BENCHMARK_ERROR_EXISTS;
    } else if (result == FR_NOT_READY) {
        error = PF_FILE_BENCHMARK_ERROR_MEDIA;
    }
    if (detail != NULL) {
        *detail = error == PF_FILE_BENCHMARK_ERROR_FILESYSTEM ? result : FR_OK;
    }
    return error;
}

/** @brief Record a FatFs failure in a run status. */
static void filesystem_error(pf_file_benchmark_status_t *status, FRESULT result)
{
    status->error = map_fresult(result, &status->filesystem_result);
}

/* Unlike map_fresult(), this leaves *filesystem_result untouched for non-FILESYSTEM errors: preflight callers
 * pre-set it to FR_OK. */
/** @brief Preflight a FatFs failure; null @p filesystem_result discards the detail. */
static pf_file_benchmark_error_t preflight_error(FRESULT result, FRESULT *filesystem_result)
{
    FRESULT detail;
    pf_file_benchmark_error_t error = map_fresult(result, &detail);

    if ((filesystem_result != NULL) && (error == PF_FILE_BENCHMARK_ERROR_FILESYSTEM)) {
        *filesystem_result = detail;
    }
    return error;
}

static pf_file_benchmark_error_t preflight_mounted_volume(uint32_t size_mib, FRESULT *filesystem_result)
{
    FILINFO existing;
    FRESULT result = f_stat(PF_FILE_BENCHMARK_PATH, &existing);
    if (result == FR_OK) {
        return PF_FILE_BENCHMARK_ERROR_EXISTS;
    }
    if (result != FR_NO_FILE) {
        return preflight_error(result, filesystem_result);
    }

    DWORD free_clusters;
    FATFS *filesystem = NULL;
    result = f_getfree(PF_FATFS_VOLUME_PATH, &free_clusters, &filesystem);
    if ((result != FR_OK) || (filesystem == NULL)) {
        return preflight_error(result != FR_OK ? result : FR_INT_ERR, filesystem_result);
    }
    uint64_t free_bytes = (uint64_t)free_clusters * filesystem->csize * FF_MAX_SS;
    uint64_t target_bytes = (uint64_t)PF_FILE_BENCHMARK_BYTES(size_mib);
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

/** Mutable state of one benchmark run, shared by its phases. */
typedef struct {
    pf_file_benchmark_status_t status; /**< Snapshot published through @c update. */
    FIL file; /**< Benchmark file handle; valid while @c file_open. */
    pf_file_benchmark_clock_t clock_ms; /**< Monotonic clock for phase timing. */
    pf_file_benchmark_update_t update; /**< Progress publisher and cancellation check. */
    void *context; /**< Callback context. */
    bool file_created; /**< The benchmark file was created and must be unlinked. */
    bool file_open; /**< The handle is open and must be closed on cleanup. */
    bool cancelled; /**< The update callback requested cancellation. */
    bool close_failed; /**< The last close failed, so the file must not be unlinked. */
} run_t;

/** @brief Close the benchmark file, recording a failure unless an earlier error is already set. */
static void close_file(run_t *run)
{
    FRESULT result = f_close(&run->file);

    run->file_open = false;
    run->close_failed = result != FR_OK;
    if (run->close_failed && (run->status.error == PF_FILE_BENCHMARK_ERROR_NONE)) {
        filesystem_error(&run->status, result);
    }
}

/** @brief Create the file (optionally preallocated); returns false with the error recorded on failure. */
static bool create_file(run_t *run)
{
    FRESULT result = f_open(&run->file, PF_FILE_BENCHMARK_PATH, FA_WRITE | FA_CREATE_NEW);

    if (result != FR_OK) {
        filesystem_error(&run->status, result);
        return false;
    }
    run->file_created = run->file_open = true;
    if (preallocate) {
        result = f_expand(&run->file, run->status.target_bytes, 1u);
        if (result != FR_OK) {
            filesystem_error(&run->status, result);
            return false;
        }
    }
    return true;
}

/** @brief Write the pattern, then sync and close; timing includes sync and close. */
static void write_phase(run_t *run)
{
    pf_file_benchmark_status_t *status = &run->status;
    uint32_t phase_start;
    FRESULT result;

    status->state = PF_FILE_BENCHMARK_WRITING;
    phase_start = run->clock_ms(run->context);
    while (status->written_bytes < status->target_bytes) {
        if (!run->update(status, run->context)) {
            run->cancelled = true;
            break;
        }
        fill_pattern(test_buffer, status->written_bytes, PF_FILE_BENCHMARK_BUFFER_SIZE);
        UINT written = 0u;
        result = f_write(&run->file, test_buffer, sizeof(test_buffer), &written);
        status->written_bytes += written;
        status->write_elapsed_ms = run->clock_ms(run->context) - phase_start;
        if ((result != FR_OK) || (written != sizeof(test_buffer))) {
            filesystem_error(status, result != FR_OK ? result : FR_DISK_ERR);
            break;
        }
    }
    result = f_sync(&run->file);
    if ((result != FR_OK) && (status->error == PF_FILE_BENCHMARK_ERROR_NONE)) {
        filesystem_error(status, result);
    }
    close_file(run);
    status->write_elapsed_ms = run->clock_ms(run->context) - phase_start;
}

/** @brief Reopen the file, read and verify the pattern, then close. */
static void read_phase(run_t *run)
{
    pf_file_benchmark_status_t *status = &run->status;
    FRESULT result = f_open(&run->file, PF_FILE_BENCHMARK_PATH, FA_READ);
    uint32_t phase_start;

    if (result != FR_OK) {
        filesystem_error(status, result);
        return;
    }
    run->file_open = true;
    status->state = PF_FILE_BENCHMARK_READING;
    phase_start = run->clock_ms(run->context);
    while (status->read_bytes < status->target_bytes) {
        if (!run->update(status, run->context)) {
            run->cancelled = true;
            break;
        }
        UINT received = 0u;
        result = f_read(&run->file, test_buffer, sizeof(test_buffer), &received);
        if ((result != FR_OK) || (received != sizeof(test_buffer))) {
            filesystem_error(status, result != FR_OK ? result : FR_DISK_ERR);
            break;
        }
        status->mismatch_count += count_mismatched_bytes(test_buffer, status->read_bytes, received);
        status->read_bytes += received;
        status->read_elapsed_ms = run->clock_ms(run->context) - phase_start;
        if (status->mismatch_count != 0u) {
            status->error = PF_FILE_BENCHMARK_ERROR_VERIFY;
            break;
        }
    }
    close_file(run);
    status->read_elapsed_ms = run->clock_ms(run->context) - phase_start;
}

/** @brief Close a still-open handle and remove the benchmark file; a failed close keeps the file and flags it. */
static void cleanup_file(run_t *run)
{
    pf_file_benchmark_status_t *status = &run->status;

    status->state = PF_FILE_BENCHMARK_CLEANUP;
    (void)run->update(status, run->context);
    if (run->file_open) {
        run->close_failed = f_close(&run->file) != FR_OK;
    }
    if (run->file_created) {
        FRESULT result = run->close_failed ? FR_LOCKED : f_unlink(PF_FILE_BENCHMARK_PATH);

        if (result != FR_OK) {
            status->flags |= PF_FILE_BENCHMARK_FILE_REMAINS;
            if (status->error == PF_FILE_BENCHMARK_ERROR_NONE) {
                status->error = PF_FILE_BENCHMARK_ERROR_CLEANUP;
                status->filesystem_result = result;
            }
        }
    }
}

/** @brief Run the phases; the file exists only after create_file() succeeds. */
static void run_phases(run_t *run)
{
    if (!run->update(&run->status, run->context)) {
        run->cancelled = true;
    } else if (create_file(run)) {
        write_phase(run);
        if (!run->cancelled && (run->status.error == PF_FILE_BENCHMARK_ERROR_NONE)) {
            read_phase(run);
        }
    }
    cleanup_file(run);
}

static void run_benchmark(uint32_t size_mib, pf_file_benchmark_clock_t clock_ms,
                          pf_file_benchmark_update_t update, void *context, bool preflighted)
{
    run_t run = { .status = { .state = PF_FILE_BENCHMARK_PREPARING }, .clock_ms = clock_ms, .update = update,
                  .context = context };

    if ((clock_ms == NULL) || (update == NULL)) {
        return;
    }
    if ((size_mib == 0u) || (size_mib > PF_FILE_BENCHMARK_MAX_MIB)) {
        run.status.error = PF_FILE_BENCHMARK_ERROR_SIZE;
    } else if (!preflighted && !pf_fatfs_volume_is_mounted()) {
        run.status.target_bytes = PF_FILE_BENCHMARK_BYTES(size_mib);
        run.status.error = PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED;
    } else {
        run.status.target_bytes = PF_FILE_BENCHMARK_BYTES(size_mib);
        run.status.flags |= PF_FILE_BENCHMARK_VOLUME_MOUNTED;
        if (!update(&run.status, context)) {
            run.cancelled = true;
        } else {
            FRESULT detail = FR_OK;
            pf_file_benchmark_error_t error = preflighted ? PF_FILE_BENCHMARK_ERROR_NONE
                                                          : preflight_mounted_volume(size_mib, &detail);
            if (error != PF_FILE_BENCHMARK_ERROR_NONE) {
                run.status.error = error;
                run.status.filesystem_result = detail;
            } else {
                run_phases(&run);
            }
        }
    }
    run.status.state = run.status.error != PF_FILE_BENCHMARK_ERROR_NONE ? PF_FILE_BENCHMARK_FAILED
        : run.cancelled ? PF_FILE_BENCHMARK_CANCELLED : PF_FILE_BENCHMARK_PASSED;
    (void)update(&run.status, context);
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