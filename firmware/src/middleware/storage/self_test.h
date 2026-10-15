#ifndef PF_STORAGE_SELF_TEST_H
#define PF_STORAGE_SELF_TEST_H

#include <stdint.h>
#include "storage.h"

#define PF_STORAGE_SELF_TEST_PATH "0:/PFTEST.BIN"
#define PF_STORAGE_SELF_TEST_MAX_MIB 128u
#define PF_STORAGE_SELF_TEST_BUFFER_SIZE 32768u

#ifndef PF_STORAGE_SELF_TEST_PREALLOCATE
#define PF_STORAGE_SELF_TEST_PREALLOCATE 0
#endif

/** Select contiguous preallocation for subsequent tests; default is build-configured/off.
 * Call only from the serialized storage owner while no test is running.
 * Preallocation is excluded from write timing; allocation failure aborts with cleanup.
 */
void pf_storage_self_test_set_preallocation_enabled(bool enabled);

typedef enum {
    PF_SELF_TEST_IDLE = 0,
    PF_SELF_TEST_QUEUED,
    PF_SELF_TEST_MOUNTING,
    PF_SELF_TEST_WRITING,
    PF_SELF_TEST_READING,
    PF_SELF_TEST_CLEANUP,
    PF_SELF_TEST_PASSED,
    PF_SELF_TEST_FAILED,
    PF_SELF_TEST_CANCELLED,
} pf_self_test_state_t;

typedef enum {
    PF_SELF_TEST_ERROR_NONE = 0,
    PF_SELF_TEST_ERROR_BUSY,
    PF_SELF_TEST_ERROR_USB_OWNER,
    PF_SELF_TEST_ERROR_MOUNTED,
    PF_SELF_TEST_ERROR_SIZE,
    PF_SELF_TEST_ERROR_CARD,
    PF_SELF_TEST_ERROR_SPACE,
    PF_SELF_TEST_ERROR_EXISTS,
    PF_SELF_TEST_ERROR_FILESYSTEM,
    PF_SELF_TEST_ERROR_VERIFY,
    PF_SELF_TEST_ERROR_CLEANUP,
    PF_SELF_TEST_ERROR_NOT_MOUNTED,
} pf_self_test_error_t;

#define PF_SELF_TEST_FILE_REMAINS 1u
#define PF_SELF_TEST_VOLUME_MOUNTED 2u

typedef struct {
    pf_self_test_state_t state;
    uint32_t target_bytes;
    uint32_t written_bytes;
    uint32_t read_bytes;
    uint32_t write_elapsed_ms;
    uint32_t read_elapsed_ms;
    uint32_t mismatch_count;
    uint8_t flags;
    pf_self_test_error_t error;
    FRESULT filesystem_result;
    pf_self_test_error_t request_error;
} pf_self_test_status_t;

/** Monotonic millisecond clock; wraparound is supported for bounded tests. */
typedef uint32_t (*pf_self_test_clock_t)(void *context);

/** Publish a snapshot synchronously; false requests cancellation.
 * Cleanup is always attempted, regardless of callback return during cleanup.
 */
typedef bool (*pf_self_test_update_t)(const pf_self_test_status_t *status, void *context);

/** Write and verify a 1..128 MiB dedicated file from the sole storage task.
 * Requires a mounted volume and firmware ownership (enforced by caller).
 * Never overwrites PFTEST.BIN if it already exists. Owns one static 32 KiB
 * buffer, so calls must be serialized. Timing covers pattern generation plus
 * write/sync/close, and read/verification/close; mount and cleanup are excluded.
 * The test's own file is removed; the existing volume remains mounted.
 * Status flags identify failed cleanup; existing files are never deleted.
 */
void pf_storage_self_test_run(uint32_t size_mib,
                              pf_self_test_clock_t clock_ms, pf_self_test_update_t update,
                              void *context);

#endif