/** @file
 * @brief SDIO-PIO socket assignment, selected by CMake.
 */
#include "drivers/sdcard/sdio/sdio_pio_pins.h"

const pf_sdio_pio_board_pins_t pf_sdio_pio_board_pins = {
    .pins = {
        .clock_gpio = 2u,
        .command_gpio = 3u,
        .data_gpio = {4u, 5u, 6u, 7u},
    },
    .pio_index = 0u,
    .card_detect_gpio = 8u,
    .card_detect_active_high = true,
};
