#ifndef PF_CLI_H
#define PF_CLI_H

#include <stdbool.h>

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

#endif