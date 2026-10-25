#ifndef PF_BOARD_H
#define PF_BOARD_H

#include <stdbool.h>

#include "board_resources.h"
#include "sd.h"

/** Initialize board monitor UART and optional LED. */
bool pf_board_init(void);

/**
 * Build this profile's SD-card configuration for storage startup from the selected transport and active boot config.
 *
 * Returns false if the output is null or SD-card support is disabled.
 */
bool pf_board_get_sd_card_config(pf_sd_config_t *config);

#endif