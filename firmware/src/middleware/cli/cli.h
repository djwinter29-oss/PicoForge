#ifndef PF_CLI_H
#define PF_CLI_H

#include <stdbool.h>
#include "embedded_cli.h"

/** Output callback, invoked synchronously from pf_cli_process(). */
typedef void (*pf_cli_write_char_t)(char character, void *context);

/** Initialize or reset the singleton CLI using static storage. No heap is used.
 * The callback and context must remain valid until the next initialization.
 * All CLI calls must run in the same task; initialization discards session history.
 */
bool pf_cli_init(pf_cli_write_char_t write_char, void *context);

/** Queue one input byte. Call pf_cli_process() regularly to avoid RX overflow. */
void pf_cli_receive_char(char character);

/** Process queued input and synchronously emit output through the callback. */
void pf_cli_process(void);

/** Register a binding for this session from the CLI-owner task.
 * Name, help, and context are borrowed and must remain valid for the session.
 */
bool pf_cli_add_binding(CliCommandBinding binding);

/** Print a bounded line from the CLI-owner task, preserving current input. */
void pf_cli_print(const char *line);

#endif