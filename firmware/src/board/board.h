#ifndef PF_BOARD_H
#define PF_BOARD_H

#include <stdbool.h>

#include "board_topology.h"
#include "sdio_pio.h"

typedef struct {
	uint32_t gpio;
	bool active_high;
} pf_board_led_config_t;

/** Return the immutable topology selected for this firmware build. */
const pf_board_topology_t *pf_board_get_topology(void);

/** Copy the board LED GPIO and polarity when this board provides one. */
bool pf_board_get_led_config(pf_board_led_config_t *config);

/**
 * Copy this board's SD-card configuration for storage startup.
 *
 * Returns false while SD wiring is unassigned or if the topology is invalid.
 */
bool pf_board_get_sdio_config(pf_sdio_config_t *config);

#endif