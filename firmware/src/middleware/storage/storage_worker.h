#ifndef PF_STORAGE_WORKER_H
#define PF_STORAGE_WORKER_H

#include <stdint.h>
#include "middleware/storage/commands.h"

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

/** Poll one reply without blocking from the USB task; completion releases busy. */
bool pf_storage_worker_poll(pf_storage_worker_reply_t *reply);

/** Cancel read-only output for a disconnected session from the USB task.
 * Accepted mutations still finish safely; all replies must still be drained.
 */
void pf_storage_worker_cancel(uint32_t session);

#endif