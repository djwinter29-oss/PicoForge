/** @file
 * @brief Sole FatFs worker, command admission and application media lifecycle policy.
 */
#ifndef PF_STORAGE_WORKER_H
#define PF_STORAGE_WORKER_H

#include <stdint.h>
#include "storage_commands.h"
#include "file_benchmark.h"

/** One queued command output line or completion marker. */
typedef struct {
    uint32_t session; /**< Originating CDC session; stale output must be discarded. */
    bool done; /**< Completion marker; draining it releases command admission. */
    char line[PF_STORAGE_REPLY_SIZE]; /**< NUL-terminated output; unused for completion. */
} pf_storage_worker_reply_t;

/** Create the statically allocated sole FatFs worker before the scheduler starts.
 * SD configuration and execution-port setup come from board support. Missing SD
 * configuration permits status commands but prevents mounting.
 */
bool pf_storage_worker_init(void);

/** Submit one command from the USB task; false means busy or invalid input.
 * Strings are copied. Caller must supply current MSC ownership and session ID.
 * The USB task also coordinates owner transitions, so this snapshot cannot race
 * another USB caller. Admission stays busy until the completion reply is drained.
 */
bool pf_storage_worker_submit(const char *family, const char *arguments,
                              bool host_owned, uint32_t session);

/** True until all work and replies are drained. Only call from the USB task. */
bool pf_storage_worker_busy(void);

/** True only after the serialized worker confirms debounced removal. USB-task query. */
bool pf_storage_worker_media_absent(void);

/** Queue explicit fault recovery from the USB task; caller reserves media until USB detaches
 * and firmware ownership is restored. Reinitializes only, never mounts.
 * Initialization failure is retained in backend diagnostics/logging; telemetry reports
 * readiness. Automatic mounting is blocked until a successful explicit fs mount.
 */
bool pf_storage_worker_recover_media(void);

/** @brief Queue one reinitialization/mount attempt after a confirmed forced USB detach.
 * Caller holds the media reservation until MSC disconnects and firmware ownership is restored.
 * Poll busy then the mounted snapshot for completion. Failure blocks automatic retries and is logged;
 * no formatting or repair is performed.
 */
bool pf_storage_worker_return_from_usb(void);

/** @brief Queue a USB-task request to unregister FatFs on its sole executor.
 * Requires idle admission; false means busy or unavailable. Completion clears busy without a CLI reply.
 * Success leaves automount paused for USB acquisition; failure logs FatFs details and retains the mount.
 * Poll busy and then the atomic mount snapshot before changing ownership. No formatting is performed.
 */
bool pf_storage_worker_unmount_for_usb(void);

/** Poll one reply without blocking from the USB task; completion releases busy. */
bool pf_storage_worker_poll(pf_storage_worker_reply_t *reply);

/** Cancel read-only output for a disconnected session from the USB task.
 * Accepted mutations still finish safely; all replies must still be drained.
 */
void pf_storage_worker_cancel(uint32_t session);

/** Start a verified file benchmark from the USB task. False records rejection.
 * Requires firmware ownership, initialized media, and no pending storage work.
 * Caller supplies current ownership and coordinates all USB ownership transitions.
 * Admission remains busy through cleanup; USB polling observes completion and releases it.
 */
bool pf_storage_worker_benchmark_start(uint32_t size_mib, bool host_owned);

/** Request cancellation from the USB task; cleanup still runs before admission is released. */
bool pf_storage_worker_benchmark_cancel(void);

/** Copy a coherent benchmark snapshot from the USB task; result persists. */
void pf_storage_worker_benchmark_status(pf_file_benchmark_status_t *status);

#endif