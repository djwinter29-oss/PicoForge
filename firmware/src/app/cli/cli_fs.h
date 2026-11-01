/**
 * @file
 * @brief Filesystem CLI command registration and worker execution.
 */
#ifndef PF_CLI_FS_H
#define PF_CLI_FS_H

#include <stdint.h>
#include "app/cli/cli_storage.h"

/** `fs` group help. */
#define PF_CLI_FS_HELP \
    "ls [path]\r\n" \
    "\tstat <path>\r\n" \
    "\tcat <path> [limit]\r\n" \
    "\tdf\r\n" \
    "\tmkdir <path>\r\n" \
    "\twrite <path> <text>\r\n" \
    "\tappend <path> <text>\r\n" \
    "\tmv <source> <destination>\r\n" \
    "\trm <path> --confirm"

/**
 * @brief Register the `fs` group; commands are submitted to the storage worker.
 * @param submit Asynchronous worker submission callback; must not be NULL.
 * @param context Borrowed callback context, valid until the CLI session ends.
 * @return True when the binding was registered.
 */
bool pf_cli_fs_register(pf_cli_worker_submit_fn_t submit, void *context);

/**
 * @brief Execute a parsed filesystem command on the storage worker.
 * @param operation Command word.
 * @param count Number of parsed tokens, including the operation.
 * @param first First command argument, if present.
 * @param second Second command argument, if present.
 * @param reply Output callback; must not be NULL.
 * @param context Borrowed callback context.
 */
void pf_cli_fs_execute(const char *operation, uint16_t count, const char *first, const char *second,
                       pf_storage_worker_reply_fn_t reply, void *context);

#endif
