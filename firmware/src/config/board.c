/** @file
 * @brief Build-selected board assignments; each object is declared extern in its driver header.
 */
#include "config/config.h"
#include "drivers/led.h"
#include "drivers/monitor_uart.h"
#include "drivers/sdcard/sd_board.h"
#if PF_CONFIG_SDCARD_TRANSPORT == PF_CONFIG_SDCARD_TRANSPORT_SDIO_PIO
#include "drivers/sdcard/sdio_pio/sdio_pio_pins.h"
#else
#include "drivers/sdcard/spi/spi_pins.h"
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

#if PF_CONFIG_SDCARD_TRANSPORT == PF_CONFIG_SDCARD_TRANSPORT_SDIO_PIO
/* GP2 CLK, GP3 CMD, and GP4-GP7 DAT0-DAT3. */
const pf_sdio_pio_board_t pf_sdio_pio_board = {
    .pins = {
        .clock_gpio = 2u,
        .command_gpio = 3u,
        .data_gpio = {4u, 5u, 6u, 7u},
    },
    .pio_index = 0u,
};
#else
/* GP2 SCK, GP3 MOSI, GP4 MISO, GP7 CS. */
const pf_spi_board_t pf_spi_board = {
    .pins = {
        .clock_gpio = 2u,
        .mosi_gpio = 3u,
        .miso_gpio = 4u,
        .cs_gpio = 7u,
    },
    .spi_index = 0u,
};
#endif

const pf_sd_board_t pf_sd_board = {
#if PF_CONFIG_SDCARD_TRANSPORT == PF_CONFIG_SDCARD_TRANSPORT_SDIO_PIO
    /* Fixed transfer target; no automatic probing or lower-clock retries. */
    .clock_hz = 25000000u,
#else
    .clock_hz = 50000000u,
#endif
    /* GP8 DET; the selected breakout provides its bias. */
    .card_detect_gpio = 8u,
    .card_detect_active_high = true,
};
