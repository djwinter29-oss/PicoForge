/** @file
 * @brief CLI `storage` and `fs` command execution over the storage middleware.
 */
#ifndef PF_CLI_STORAGE_H
#define PF_CLI_STORAGE_H

#include <stdbool.h>

/** Maximum command argument storage, including tokenizer terminators. */
#define PF_CLI_STORAGE_ARGS_SIZE 128u
/** Maximum reply storage, including the terminator. */
#define PF_CLI_STORAGE_REPLY_SIZE 256u

/** Emit a line synchronously. Return false to cancel further read-only output. */
typedef bool (*pf_cli_storage_reply_t)(const char *line, void *context);

/** Execute one storage/fs command from the sole storage-owner task.
 * Caller must prevent MSC ownership transitions until this returns. Strings
 * and callback context are borrowed for this call. Mutations finish and close
 * their files even if the output callback stops accepting replies. Formatting
 * requires exact --confirm and an unmounted, Pico-owned volume. File writes
 * never overwrite existing files. Paths must be absolute on volume 0.
 */
void pf_cli_storage_execute(bool host_owned, const char *family, const char *arguments,
                            pf_cli_storage_reply_t reply, void *context);

#endif