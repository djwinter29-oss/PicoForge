/**
 * @file
 * @brief Transport-neutral board, boot configuration, storage control and radio commands.
 */
#ifndef PF_CLI_DEVICE_H
#define PF_CLI_DEVICE_H

#include <stdbool.h>

/** Largest `storage` argument string handled synchronously, matching the 128-byte CLI command buffer. */
#define PF_CLI_DEVICE_ARGS_SIZE 128u

/** `storage` group help: status and volume maintenance run on the storage worker. */
#define PF_CLI_STORAGE_HELP \
    "get [status | usb | benchmark]\r\n" \
    "\tset usb <on | off>\r\n" \
    "\tformat --confirm (destroys files)\r\n" \
    "\tmount (maintenance)\r\n" \
    "\tunmount (maintenance)\r\n" \
    "\tbenchmark <MiB: 1..1024 | cancel>\r\n" \
    "\trecover --confirm (fault recovery, not clean eject)"

/**
 * @brief Register device command groups for the current CLI session on the USB owner task.
 * @param request_reboot Schedules a deferred reset after queued CLI output is flushed; must not be NULL.
 * @return True when every binding was registered.
 */
bool pf_cli_device_register(void (*request_reboot)(void));

/**
 * @brief Run a synchronous `storage` verb on the CLI owner task.
 * @param args Raw arguments after `storage`; NULL is treated as empty.
 * @return False for status or volume maintenance, which the caller must queue to the storage worker; true when
 *         this function printed the reply, including usage errors.
 */
bool pf_cli_device_storage(const char *args);

#endif
