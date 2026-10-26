/**
 * @file
 * @brief CLI `storage` and `fs` command execution over the platform storage service.
 */
#ifndef PF_CLI_STORAGE_H
#define PF_CLI_STORAGE_H

#include <stdbool.h>
#include "app/storage/storage_worker.h"

/**
 * @brief Execute one storage/fs command; registered with pf_storage_worker_init() and run on the worker task.
 *
 * The caller must prevent MSC ownership transitions until this returns. Formatting requires exact --confirm and an
 * unmounted, Pico-owned volume. File writes never overwrite existing files. Paths must be absolute on volume 0.
 * Mutations finish and close their files even if the reply callback stops accepting replies. A listing or `cat`
 * stops when the callback returns false, but the final status line is still sent.
 *
 * @param host_owned True while USB owns the medium; all fs commands are then refused.
 * @param family Command family, "storage" or "fs"; borrowed for this call.
 * @param arguments Space-separated arguments after the family; borrowed for this call.
 * @param reply Output callback; must not be NULL.
 * @param context Borrowed callback context.
 */
void pf_cli_storage_execute(bool host_owned, const char *family, const char *arguments,
                            pf_storage_worker_reply_fn_t reply, void *context);

#endif
