#ifndef PF_STORAGE_COMMANDS_H
#define PF_STORAGE_COMMANDS_H

#include "middleware/storage/fatfs_volume.h"

#define PF_STORAGE_COMMAND_ARGS_SIZE 128u
#define PF_STORAGE_REPLY_SIZE 256u

/** Emit a line synchronously. Return false to cancel further read-only output. */
typedef bool (*pf_storage_reply_t)(const char *line, void *context);

/** Execute one sd/fs command from the sole storage-owner task.
 * Caller must prevent MSC ownership transitions until this returns. Strings
 * and callback context are borrowed for this call. Mutations finish and close
 * their files even if the output callback stops accepting replies. No format
 * or overwrite operation is provided. Paths must be absolute on volume 0.
 */
void pf_storage_command_execute(const pf_sd_config_t *config, bool host_owned,
                                const char *family, const char *arguments,
                                pf_storage_reply_t reply, void *context);

#endif