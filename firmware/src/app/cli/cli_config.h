/**
 * @file
 * @brief Boot configuration CLI command registration.
 */
#ifndef PF_CLI_CONFIG_H
#define PF_CLI_CONFIG_H

#include <stdbool.h>

/**
 * @brief Register the `config` command group for the current CLI session.
 * @param request_reboot Schedules a deferred reset after queued CLI output is flushed; must not be NULL.
 * @return True when the binding was registered.
 */
bool pf_cli_config_register(void (*request_reboot)(void));

#endif
