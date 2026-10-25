#ifndef PF_BOARD_H
#define PF_BOARD_H

#include <stdbool.h>

#include "board_resources.h"
#include "drivers/boot_config.h"
#include "sd.h"

/** Apply required board pin assignments, including monitor UART and optional LED. */
bool pf_board_init(void);

/**
 * Select the persisted SDIO or SPI bus-speed setting (only the field for the compiled transport is used).
 *
 * Returns false and keeps the defaults for invalid settings or after storage configuration has started, so call
 * it before the first pf_board_get_sd_card_config().
 */
bool pf_board_apply_boot_config(const pf_boot_config_t *config);

/**
 * Copy this board's SD-card configuration for storage startup.
 *
 * Returns false while SD wiring is unassigned or if the topology is invalid.
 */
bool pf_board_get_sd_card_config(pf_sd_config_t *config);

#endif