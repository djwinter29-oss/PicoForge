#ifndef PF_BOARD_H
#define PF_BOARD_H

#include <stdbool.h>

#include "board_resources.h"
#include "sdio_pio.h"

/** Apply required board pin assignments, including monitor UART and optional LED. */
bool pf_board_init(void);

/**
 * Copy this board's SD-card configuration for storage startup.
 *
 * Returns false while SD wiring is unassigned or if the topology is invalid.
 */
bool pf_board_get_sdio_config(pf_sdio_config_t *config);

#endif