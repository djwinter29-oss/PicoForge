/** @file
 * @brief Sole FatFs worker, command admission and application media lifecycle policy.
 */
#ifndef PF_STORAGE_WORKER_H
#define PF_STORAGE_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#ifndef PF_ENABLE_STORAGE_BENCHMARK
/** Include optional benchmark APIs only when enabled by the build. */
#define PF_ENABLE_STORAGE_BENCHMARK 0
#endif
#if PF_ENABLE_STORAGE_BENCHMARK
#include "file_benchmark.h"
#endif

/** Maximum command argument storage, including tokenizer terminators. */
#define PF_STORAGE_WORKER_ARGS_SIZE 128u
/** Maximum reply line storage, including the terminator. */
#define PF_STORAGE_WORKER_REPLY_SIZE 256u

/** Emit one reply line synchronously. Return false to cancel further read-only output. */
typedef bool (*pf_storage_worker_reply_fn_t)(const char *line, void *context);

/** Command executor run on the worker task; strings and @p context are borrowed for the call. */
typedef void (*pf_storage_worker_execute_fn_t)(bool host_owned, const char *family, const char *arguments,
                                               pf_storage_worker_reply_fn_t reply, void *context);

/** One queued command output line or completion marker. */
typedef struct {
    uint32_t session; /**< Originating CDC session; stale output must be discarded. */
    bool done; /**< Completion marker; draining it releases command admission. */
    char line[PF_STORAGE_WORKER_REPLY_SIZE]; /**< NUL-terminated output; unused for completion. */
} pf_storage_worker_reply_t;

/**
 * @brief Create the statically allocated sole FatFs worker before the scheduler starts.
 * @param execute Runs each submitted command on the worker; must not be NULL.
 * @return True when the queues and task exist; false for a NULL executor, repeated init or failed board setup.
 * @note SD configuration and execution-port setup come from board support. Missing SD configuration permits status
 *       commands but prevents mounting.
 */
bool pf_storage_worker_init(pf_storage_worker_execute_fn_t execute);

/**
 * @brief Submit one command from the USB task.
 * @param family Command family; copied, must fit the request family field.
 * @param arguments Command arguments; copied, must fit PF_STORAGE_WORKER_ARGS_SIZE.
 * @param host_owned Current MSC ownership snapshot supplied by the caller.
 * @param session Current CDC session ID.
 * @return False when busy, uninitialized or the input is invalid or oversized.
 * @note USB task only. The USB task also coordinates owner transitions, so the ownership snapshot cannot race another
 *       caller. Admission stays busy until the completion reply is drained.
 */
bool pf_storage_worker_submit(const char *family, const char *arguments, bool host_owned, uint32_t session);

/**
 * @brief Report whether work or undrained replies are outstanding.
 * @return True until all work and replies are drained.
 * @note USB task only.
 */
bool pf_storage_worker_busy(void);

/**
 * @brief Report debounced media removal confirmed by the serialized worker.
 * @return True only after removal is confirmed; false while the media service is running.
 * @note USB task only.
 */
bool pf_storage_worker_media_absent(void);

/**
 * @brief Queue explicit fault recovery from the USB task.
 * @return False when busy, unconfigured or unavailable.
 * @note The caller reserves media until USB detaches and firmware ownership is restored. Reinitializes only, never
 *       mounts. Initialization failure is retained in backend diagnostics/logging; telemetry reports readiness.
 *       Automatic mounting is blocked until a successful explicit fs mount.
 */
bool pf_storage_worker_recover_media(void);

/**
 * @brief Queue one reinitialization/mount attempt after a confirmed forced USB detach.
 * @return False when busy, unconfigured or unavailable.
 * @note The caller holds the media reservation until MSC disconnects and firmware ownership is restored. Poll busy
 *       then the mounted snapshot for completion. Failure blocks automatic retries and is logged; no formatting or
 *       repair is performed.
 */
bool pf_storage_worker_return_from_usb(void);

/**
 * @brief Queue a request to unregister FatFs on the sole executor.
 * @return False when busy or unavailable.
 * @note USB task only; requires idle admission. Completion clears busy without a CLI reply. Success leaves automount
 *       paused for USB acquisition; failure logs FatFs details and retains the mount. Poll busy and then the atomic
 *       mount snapshot before changing ownership. No formatting is performed.
 */
bool pf_storage_worker_unmount_for_usb(void);

/**
 * @brief Poll one reply without blocking.
 * @param reply Receives the reply.
 * @return True when a reply was returned; a completion reply releases busy.
 * @note USB task only. Drain replies even for cancelled sessions so admission is released.
 */
bool pf_storage_worker_poll(pf_storage_worker_reply_t *reply);

/**
 * @brief Cancel read-only output for a disconnected session.
 * @param session Session to cancel; ignored unless it owns the active command.
 * @note USB task only. Accepted mutations still finish safely; all replies must still be drained.
 */
void pf_storage_worker_cancel(uint32_t session);

#if PF_ENABLE_STORAGE_BENCHMARK
/**
 * @brief Start a verified file benchmark.
 * @param size_mib Requested size in MiB.
 * @param host_owned Current MSC ownership snapshot supplied by the caller.
 * @return True when queued; false records the rejection in the status request_error.
 * @note USB task only. Requires firmware ownership, initialized media and no pending storage work. The caller
 *       coordinates all USB ownership transitions. Admission remains busy through cleanup; USB polling observes
 *       completion and releases it.
 */
bool pf_storage_worker_benchmark_start(uint32_t size_mib, bool host_owned);

/**
 * @brief Request benchmark cancellation; cleanup still runs before admission is released.
 * @return True when accepted. False when no benchmark is running, admission is still pending or cleanup has begun;
 *         request_error then reports busy.
 * @note USB task only.
 */
bool pf_storage_worker_benchmark_cancel(void);

/**
 * @brief Copy a coherent benchmark snapshot; the result persists after completion.
 * @param status Receives the snapshot; ignored when NULL.
 * @note USB task only.
 */
void pf_storage_worker_benchmark_status(pf_file_benchmark_status_t *status);

/**
 * @brief Poll the asynchronous benchmark admission result.
 * @param accepted Receives whether the file/space preflight succeeded.
 * @param error Receives the specific preflight rejection.
 * @return True when a result was available; false while preflight is pending. The result is consumed.
 */
bool pf_storage_worker_benchmark_admission_result(bool *accepted, pf_file_benchmark_error_t *error);
#endif

#endif