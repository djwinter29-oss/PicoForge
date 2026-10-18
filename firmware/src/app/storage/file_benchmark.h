/** @file
 * @brief Serialized file verification and throughput measurement on an existing FatFs mount.
 */
#ifndef PF_FILE_BENCHMARK_H
#define PF_FILE_BENCHMARK_H

#include <stdint.h>
#include "app/storage/fatfs_volume.h"

/** Reserved file replaced by each accepted benchmark. */
#define PF_FILE_BENCHMARK_PATH "0:/PFTEST.BIN"
/** Maximum requested benchmark size in MiB. */
#define PF_FILE_BENCHMARK_MAX_MIB 128u
/** Static aligned transfer buffer size in bytes. */
#define PF_FILE_BENCHMARK_BUFFER_SIZE 32768u

#ifndef PF_STORAGE_SELF_TEST_PREALLOCATE
/** Existing build option controlling the default allocation policy. */
#define PF_STORAGE_SELF_TEST_PREALLOCATE 0
#endif

/** Select contiguous preallocation for subsequent tests; default is build-configured/off.
 * Call only from the serialized storage owner while no test is running.
 * Preallocation is excluded from write timing; allocation failure aborts with cleanup.
 */
void pf_file_benchmark_set_preallocation_enabled(bool enabled);

/** Benchmark lifecycle; explicit numeric values preserve HID Report 6 encoding. */
typedef enum {
    PF_FILE_BENCHMARK_IDLE = 0,      /**< No accepted run since startup. */
    PF_FILE_BENCHMARK_QUEUED = 1,    /**< Waiting for worker execution. */
    PF_FILE_BENCHMARK_PREPARING = 2, /**< Checking the existing mount and preparing the file; wire label is mounting. */
    PF_FILE_BENCHMARK_WRITING = 3,   /**< Generating and writing the complete pattern. */
    PF_FILE_BENCHMARK_READING = 4,   /**< Reading and verifying the complete pattern. */
    PF_FILE_BENCHMARK_CLEANUP = 5,   /**< Closing/removing the benchmark file. */
    PF_FILE_BENCHMARK_PASSED = 6,    /**< Full verification and cleanup succeeded. */
    PF_FILE_BENCHMARK_FAILED = 7,    /**< I/O, verification or cleanup failed. */
    PF_FILE_BENCHMARK_CANCELLED = 8, /**< Cancellation completed without another error. */
} pf_file_benchmark_state_t;

/** Failure/admission reason; reserved values remain assigned for HID compatibility. */
typedef enum {
    PF_FILE_BENCHMARK_ERROR_NONE = 0,        /**< No error. */
    PF_FILE_BENCHMARK_ERROR_BUSY = 1,        /**< Another operation or cleanup holds admission. */
    PF_FILE_BENCHMARK_ERROR_USB_OWNER = 2,   /**< USB owns the media. */
    PF_FILE_BENCHMARK_ERROR_MOUNTED = 3,     /**< Reserved legacy rejection; not emitted. */
    PF_FILE_BENCHMARK_ERROR_SIZE = 4,        /**< Requested size is outside supported bounds. */
    PF_FILE_BENCHMARK_ERROR_MEDIA = 5,       /**< Selected media is unavailable; wire label is card. */
    PF_FILE_BENCHMARK_ERROR_SPACE = 6,       /**< Insufficient free filesystem space. */
    PF_FILE_BENCHMARK_ERROR_EXISTS = 7,      /**< File creation encountered an existing entry. */
    PF_FILE_BENCHMARK_ERROR_FILESYSTEM = 8,  /**< FatFs operation or short transfer failed. */
    PF_FILE_BENCHMARK_ERROR_VERIFY = 9,      /**< Readback differed from the generated pattern. */
    PF_FILE_BENCHMARK_ERROR_CLEANUP = 10,    /**< Benchmark file cleanup failed. */
    PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED = 11, /**< An existing mount is required. */
} pf_file_benchmark_error_t;

/** Cleanup could not remove the benchmark file. */
#define PF_FILE_BENCHMARK_FILE_REMAINS 1u
/** The caller's filesystem was mounted when execution began. */
#define PF_FILE_BENCHMARK_VOLUME_MOUNTED 2u

/** Coherent worker-published snapshot; remains available after completion. */
typedef struct {
    pf_file_benchmark_state_t state; /**< Current or terminal lifecycle stage. */
    uint32_t target_bytes; /**< Requested file size in bytes. */
    uint32_t written_bytes; /**< Bytes accepted by FatFs writes. */
    uint32_t read_bytes; /**< Bytes read and verified. */
    uint32_t write_elapsed_ms; /**< Write, final sync and close elapsed time. */
    uint32_t read_elapsed_ms; /**< Read, verification and close elapsed time. */
    uint32_t mismatch_count; /**< Number of differing bytes detected before stopping. */
    uint8_t flags; /**< FILE_REMAINS and VOLUME_MOUNTED bits. */
    pf_file_benchmark_error_t error; /**< Accepted run's failure reason. */
    FRESULT filesystem_result; /**< FatFs error associated with the run. */
    pf_file_benchmark_error_t request_error; /**< Most recent start/cancel rejection, independent of run result. */
} pf_file_benchmark_status_t;

/** Monotonic millisecond clock; wraparound is supported for bounded tests. */
typedef uint32_t (*pf_file_benchmark_clock_t)(void *context);

/** Publish a snapshot synchronously; false requests cancellation.
 * Cleanup is always attempted, regardless of callback return during cleanup.
 */
typedef bool (*pf_file_benchmark_update_t)(const pf_file_benchmark_status_t *status, void *context);

/** Write and verify a 1..128 MiB dedicated file from the sole storage task.
 * Requires a mounted volume and firmware ownership (enforced by caller).
 * Deletes an existing PFTEST.BIN before checking space and creating a fresh file.
 * Refuses directories and stops on inspection/deletion errors. Owns one static 32 KiB
 * buffer, so calls must be serialized. Timing covers pattern generation plus
 * write/sync/close, and read/verification/close; mount and cleanup are excluded.
 * The test's own file is removed; the existing volume remains mounted.
 * Status flags identify failed deletion/cleanup. No other paths are deleted.
 */
void pf_file_benchmark_run(uint32_t size_mib,
                           pf_file_benchmark_clock_t clock_ms, pf_file_benchmark_update_t update,
                           void *context);

#endif