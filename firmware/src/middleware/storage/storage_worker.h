#ifndef PF_STORAGE_WORKER_H
#define PF_STORAGE_WORKER_H

#include <stdint.h>
#include "middleware/storage/commands.h"
#include "self_test.h"

typedef struct {
    uint32_t session;
    bool done;
    char line[PF_STORAGE_REPLY_SIZE];
} pf_storage_worker_reply_t;

/** Create the statically allocated sole FatFs worker before the scheduler starts.
 * Configuration is copied; NULL allows status commands but prevents mounting.
 */
bool pf_storage_worker_init(const pf_sd_card_config_t *config);

/** Submit one command from the USB task; false means busy or invalid input.
 * Strings are copied. Caller must supply current MSC ownership and session ID.
 */
bool pf_storage_worker_submit(const char *family, const char *arguments,
                              bool host_owned, uint32_t session);

/** True until all work and replies are drained. Only call from the USB task. */
bool pf_storage_worker_busy(void);

/** True only after the serialized worker confirms debounced removal. */
bool pf_storage_worker_media_absent(void);

/** Queue explicit fault recovery; caller reserves the card until USB detaches
 * and firmware ownership is restored. Reinitializes only, never mounts.
 * Initialization failure is retained in card diagnostics; telemetry reports
 * readiness. Automatic mounting is blocked until a successful explicit fs mount.
 */
bool pf_storage_worker_recover_card(void);

/** Poll one reply without blocking from the USB task; completion releases busy. */
bool pf_storage_worker_poll(pf_storage_worker_reply_t *reply);

/** Cancel read-only output for a disconnected session from the USB task.
 * Accepted mutations still finish safely; all replies must still be drained.
 */
void pf_storage_worker_cancel(uint32_t session);

/** Start a verified file benchmark from the USB task. False records rejection.
 * Requires firmware ownership, initialized media, and no pending storage work.
 */
bool pf_storage_worker_self_test_start(uint32_t size_mib, bool host_owned);

/** Request cancellation from USB; cleanup still runs before ownership release. */
bool pf_storage_worker_self_test_cancel(void);

/** Copy a coherent benchmark snapshot from the USB task; result persists. */
void pf_storage_worker_self_test_status(pf_self_test_status_t *status);

#endif