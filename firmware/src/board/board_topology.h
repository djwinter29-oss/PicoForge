#ifndef PICOFORGE_BOARD_TOPOLOGY_H
#define PICOFORGE_BOARD_TOPOLOGY_H

#include <stdbool.h>
#include <stdint.h>

#define PICOFORGE_BOARD_PIN_UNASSIGNED UINT32_MAX

/** Hardware-independent SD-card resource assignment for one board. */
typedef struct {
    uint32_t gpio_count;
    bool led_enabled;
    uint32_t led_gpio;
    bool led_active_high;
    bool sdio_enabled;
    uint32_t command_gpio;
    uint32_t data0_gpio;
    uint32_t pio_index;
    uint32_t clock_hz;
    bool card_detect_enabled;
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
    bool card_detect_use_pull;
    bool card_detect_pull_up;
} picoforge_board_topology_t;

/** Validate the board-owned SD pin and PIO assignment without accessing hardware. */
bool picoforge_board_topology_validate(const picoforge_board_topology_t *topology);

#endif