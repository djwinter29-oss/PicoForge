/** @file
 * @brief SPI socket assignment, selected by CMake.
 */
#include "drivers/sdcard/spi/spi_pins.h"

const pf_spi_board_pins_t pf_spi_board_pins = {
    .pins = {
        .clock_gpio = 2u,
        .mosi_gpio = 3u,
        .miso_gpio = 4u,
        .cs_gpio = 7u,
    },
    .spi_index = 0u,
    .card_detect_gpio = 8u,
    .card_detect_active_high = true,
};
