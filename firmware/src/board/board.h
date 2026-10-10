#ifndef PICOFORGE_BOARD_H
#define PICOFORGE_BOARD_H

#include <stdbool.h>

#include "board_topology.h"
#include "sdio_pio.h"

typedef struct {
	uint32_t gpio;
	bool active_high;
} picoforge_board_led_config_t;

/** Return the immutable topology selected for this firmware build. */
const picoforge_board_topology_t *picoforge_board_get_topology(void);

/** Copy the board LED GPIO and polarity when this board provides one. */
bool picoforge_board_get_led_config(picoforge_board_led_config_t *config);

/**
 * Copy this board's SD-card configuration for storage startup.
 *
 * Returns false while SD wiring is unassigned or if the topology is invalid.
 */
bool picoforge_board_get_sdio_config(picoforge_sdio_config_t *config);

#endif