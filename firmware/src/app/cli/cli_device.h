/**
 * @file
 * @brief Transport-neutral board, boot configuration, MSC, benchmark and radio commands.
 */
#ifndef PF_CLI_DEVICE_H
#define PF_CLI_DEVICE_H

#include <stdbool.h>

/**
 * @brief Register device command groups for the current CLI session on the USB owner task.
 * @param request_reboot Schedules a deferred reset after queued CLI output is flushed; must not be NULL.
 * @return True when every binding was registered.
 */
bool pf_cli_device_register(void (*request_reboot)(void));

#endif
