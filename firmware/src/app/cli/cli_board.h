/**
 * @file
 * @brief Board and radio CLI command registration.
 */
#ifndef PF_CLI_BOARD_H
#define PF_CLI_BOARD_H

#include <stdbool.h>

/**
 * @brief Register the `board` and `radio` command groups for the current CLI session.
 * @param request_reboot Schedules a deferred reset after queued CLI output is flushed; must not be NULL.
 * @return True when both bindings were registered.
 */
bool pf_cli_board_register(void (*request_reboot)(void));

#endif
