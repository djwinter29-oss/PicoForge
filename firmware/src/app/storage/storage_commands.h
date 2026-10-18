/** @file
 * @brief Application storage command execution; mounting selects the configured media backend.
 */
#ifndef PF_STORAGE_COMMANDS_H
#define PF_STORAGE_COMMANDS_H

#include <stdbool.h>

/** Maximum command argument storage, including tokenizer terminators. */
#define PF_STORAGE_COMMAND_ARGS_SIZE 128u
/** Maximum reply storage, including the terminator. */
#define PF_STORAGE_REPLY_SIZE 256u

/** Emit a line synchronously. Return false to cancel further read-only output. */
typedef bool (*pf_storage_reply_t)(const char *line, void *context);

/** Execute one sd/fs command from the sole storage-owner task.
 * Caller must prevent MSC ownership transitions until this returns. Strings
 * and callback context are borrowed for this call. Mutations finish and close
 * their files even if the output callback stops accepting replies. Formatting
 * requires exact --confirm and an unmounted, Pico-owned volume. File writes
 * never overwrite existing files. Paths must be absolute on volume 0.
 */
void pf_storage_command_execute(bool host_owned, const char *family, const char *arguments,
                               pf_storage_reply_t reply, void *context);

#endif