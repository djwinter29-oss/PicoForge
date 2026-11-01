/**
 * @file
 * @brief Private helpers shared by application CLI command groups.
 */
#include "cli_internal.h"

#include <stdio.h>

/** @brief Reply with the FatFs result name and number; always replies once. */
void pf_cli_report_result(FRESULT result, pf_storage_worker_reply_fn_t reply, void *context)
{
    static const char *const names[] = {
        "OK", "DISK_ERR", "INT_ERR", "NOT_READY", "NO_FILE", "NO_PATH",
        "INVALID_NAME", "DENIED", "EXIST", "INVALID_OBJECT", "WRITE_PROTECTED",
        "INVALID_DRIVE", "NOT_ENABLED", "NO_FILESYSTEM", "MKFS_ABORTED",
        "TIMEOUT", "LOCKED", "NOT_ENOUGH_CORE", "TOO_MANY_OPEN_FILES",
        "INVALID_PARAMETER"
    };
    char line[80];
    unsigned code = (unsigned)result;

    /* ponytail: names track FatFs 0.15 FRESULT; newer values print UNKNOWN until this table grows. */
    (void)snprintf(line, sizeof(line), "%s FatFs: %s (%u)",
                   result == FR_OK ? "OK" : "ERR",
                   code < sizeof(names) / sizeof(names[0]) ? names[code] : "UNKNOWN", code);
    (void)reply(line, context);
}
