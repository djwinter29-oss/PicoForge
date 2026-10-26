/**
 * @file
 * @brief Application CLI session interface over Embedded CLI.
 */
#ifndef PF_CLI_H
#define PF_CLI_H

#include <stdbool.h>
#include "embedded_cli.h"

/** Output callback, invoked synchronously from pf_cli_process(). */
typedef void (*pf_cli_write_char_t)(char character, void *context);

/**
 * @brief Initialize or reset the singleton CLI using static storage. No heap is used.
 * @param write_char Output callback; must not be NULL and must remain valid until the next initialization.
 * @param context Borrowed callback context.
 * @return True when the CLI and its built-in commands are ready; false leaves the CLI inactive.
 * @note All CLI calls must run in the same task; initialization discards session history.
 */
bool pf_cli_init(pf_cli_write_char_t write_char, void *context);

/**
 * @brief Queue one input byte. Call pf_cli_process() regularly to avoid RX overflow.
 * @param character Received byte.
 */
void pf_cli_receive_char(char character);

/** @brief Process queued input and synchronously emit output through the callback. */
void pf_cli_process(void);

/**
 * @brief Register a binding for this session from the CLI-owner task.
 * @param binding Name, help, and context are borrowed and must remain valid for the session.
 * @return False when the CLI is inactive or its binding table is full.
 */
bool pf_cli_add_binding(CliCommandBinding binding);

/**
 * @brief Print a bounded line from the CLI-owner task, preserving current input.
 * @param line NUL-terminated text; ignored when NULL.
 */
void pf_cli_print(const char *line);

#endif