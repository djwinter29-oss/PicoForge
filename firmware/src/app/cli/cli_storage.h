/** @file
 * @brief CLI `storage` and `fs` command execution over the storage middleware.
 */
#ifndef PF_CLI_STORAGE_H
#define PF_CLI_STORAGE_H

#include <stdbool.h>
#include "app/storage/storage_worker.h"

/** Execute one storage/fs command; registered with pf_storage_worker_init() and run on the worker task.
 * Caller must prevent MSC ownership transitions until this returns. Strings
 * and callback context are borrowed for this call. Mutations finish and close
 * their files even if the output callback stops accepting replies. Formatting
 * requires exact --confirm and an unmounted, Pico-owned volume. File writes
 * never overwrite existing files. Paths must be absolute on volume 0.
 */
void pf_cli_storage_execute(bool host_owned, const char *family, const char *arguments,
                            pf_storage_worker_reply_fn_t reply, void *context);

#endif