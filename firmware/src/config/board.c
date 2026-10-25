/** @file
 * @brief Build-selected board assignments; each object is declared extern in its driver header.
 */
#include "drivers/led.h"
#include "drivers/monitor_uart.h"
#if PF_BOARD_SD_SPI
#include "drivers/sdcard/spi/spi_pins.h"
#else
#include "drivers/sdcard/sdio/sdio_pio_pins.h"
#endif

#include "pico/stdlib.h"

#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0u
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0u
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1u
#endif
#ifndef PICO_DEFAULT_LED_PIN_INVERTED
#define PICO_DEFAULT_LED_PIN_INVERTED 0
#endif

#ifdef PICO_DEFAULT_LED_PIN
#define PF_BOARD_LED_GPIO PICO_DEFAULT_LED_PIN
#else
#define PF_BOARD_LED_GPIO PF_LED_GPIO_NONE
#endif

const pf_led_board_t pf_led_board = {
    .gpio = PF_BOARD_LED_GPIO,
    .active_high = !PICO_DEFAULT_LED_PIN_INVERTED,
};

const pf_monitor_uart_board_t pf_monitor_uart_board = {
    .uart_index = PICO_DEFAULT_UART,
    .tx_gpio = PICO_DEFAULT_UART_TX_PIN,
    .rx_gpio = PICO_DEFAULT_UART_RX_PIN,
    .baud_rate = 115200u,
};

/* The storage connector supports either transport; the build selects which one the SD driver uses. */

#if PF_BOARD_SD_SPI
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

#else
const pf_sdio_pio_board_pins_t pf_sdio_pio_board_pins = {
    .pins = {
        .clock_gpio = 2u,
        .command_gpio = 3u,
        .data_gpio = {4u, 5u, 6u, 7u},
    },
    .pio_index = 0u,
    /* GP8 DET; the selected breakout provides its bias. */
    .card_detect_gpio = 8u,
    .card_detect_active_high = true,
};
#endif
