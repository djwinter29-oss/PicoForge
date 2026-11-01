/**
 * @file
 * @brief CLI `storage` and `fs` command execution over the platform storage service.
 */
#ifndef PF_CLI_STORAGE_H
#define PF_CLI_STORAGE_H

#include <stdbool.h>
#include "app/storage/storage_worker.h"

/** `storage` group help. */
#define PF_CLI_STORAGE_HELP \
    "get [status | usb | benchmark]\r\n" \
    "\tset usb <on | off>\r\n" \
    "\tformat --confirm (destroys files)\r\n" \
    "\tmount (maintenance)\r\n" \
    "\tunmount (maintenance)\r\n" \
    "\tbenchmark <MiB: 1..1024 | cancel>\r\n" \
    "\trecover --confirm (fault recovery, not clean eject)\r\n" \
    "\tQueued worker results follow; USB: get usb, benchmark: get benchmark"

/** Largest `storage` argument string handled synchronously. */
#define PF_CLI_STORAGE_ARGS_SIZE 128u

/** Submit a command to the asynchronous storage worker. */
typedef bool (*pf_cli_worker_submit_fn_t)(const char *family, const char *arguments, void *context);

/**
 * @brief Register the `storage` group for the current CLI session.
 * @param submit Asynchronous worker submission callback; must not be NULL.
 * @param context Borrowed callback context, valid until the CLI session ends.
 * @return True when the binding was registered.
 */
bool pf_cli_storage_register(pf_cli_worker_submit_fn_t submit, void *context);

/**
 * @brief Execute one storage/fs command; registered with pf_storage_worker_init() and run on the worker task.
 *
 * The caller must prevent MSC ownership transitions until this returns. Formatting requires exact --confirm and an
 * unmounted, Pico-owned volume. File writes never overwrite existing files. Paths must be absolute on volume 0.
 * Mutations finish and close their files even if the reply callback stops accepting replies. A listing or `cat`
 * stops when the callback returns false, but the final status line is still sent.
 * Interrupted read-only output attempts a notice before final status; FatFs success does not prove delivery.
 *
 * @param host_owned True while USB owns the medium; file commands and volume maintenance are then refused.
 * @param family Command family, "storage" or "fs"; borrowed for this call.
 * @param arguments Space-separated arguments after the family; borrowed for this call.
 * @param reply Output callback; must not be NULL.
 * @param context Borrowed callback context.
 */
void pf_cli_storage_execute(bool host_owned, const char *family, const char *arguments,
                            pf_storage_worker_reply_fn_t reply, void *context);

#endif
