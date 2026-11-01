/**
 * @file
 * @brief Private helpers shared by application CLI command groups.
 */
#ifndef PF_CLI_INTERNAL_H
#define PF_CLI_INTERNAL_H

#include "app/cli/cli.h"
#include "app/storage/storage_worker.h"
#include "app/usb/usb_msc.h"
#include "ff.h"

/**
 * @brief Dispatch the configuration binding while preserving raw assignment whitespace.
 * @param instance Embedded CLI instance.
 * @param args Mutable command arguments.
 * @param context Borrowed binding context.
 */
void pf_cli_config_command(EmbeddedCli *instance, char *args, void *context);

/** @brief Refuse restart while storage activity or host ownership makes it unsafe. */
static inline bool pf_cli_restart_allowed(void)
{
    if (pf_storage_worker_busy() || pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
        pf_cli_print("ERR storage busy or USB-owned; safely eject and disable MSC before save/reboot");
        return false;
    }
    return true;
}

/**
 * @brief Reply with a named FatFs result and numeric code.
 * @param result FatFs result.
 * @param reply Output callback; must not be NULL.
 * @param context Borrowed callback context.
 */
void pf_cli_report_result(FRESULT result, pf_storage_worker_reply_fn_t reply, void *context);

#endif
