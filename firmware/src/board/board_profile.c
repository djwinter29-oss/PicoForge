#include "board_resources.h"

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
#define PF_BOARD_LED_GPIO PF_BOARD_PIN_UNASSIGNED
#endif

/* Build-selected storage wiring for Pico-family reference boards. */
static const pf_board_resources_t board_resources = {
    .led_gpio = PF_BOARD_LED_GPIO,
    .led_active_high = !PICO_DEFAULT_LED_PIN_INVERTED,
    /* Monitor output is separate from USB CDC. */
    .monitor_uart_index = PICO_DEFAULT_UART,
    .monitor_uart_tx_gpio = PICO_DEFAULT_UART_TX_PIN,
    .monitor_uart_rx_gpio = PICO_DEFAULT_UART_RX_PIN,
    .monitor_uart_baud_rate = 115200u,
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    /* GP2 CLK, GP3 CMD, and GP4-GP7 DAT0-DAT3. */
    .sdio_pio_pins = {
        .clock_gpio = 2u,
        .command_gpio = 3u,
        .data_gpio = {4u, 5u, 6u, 7u},
    },
    .pio_index = 0u,
    /* Fixed transfer target; no automatic probing or lower-clock retries. */
    .sd_clock_hz = 25000000u,
#else
    /* GP2 SCK, GP3 MOSI, GP4 MISO, GP7 CS. */
    .spi_pins = {
        .clock_gpio = 2u,
        .mosi_gpio = 3u,
        .miso_gpio = 4u,
        .cs_gpio = 7u,
    },
    .spi_index = 0u,
    .sd_clock_hz = 50000000u,
#endif
    /* GP8 DET; the selected breakout provides its bias. */
    .card_detect_gpio = 8u,
    .card_detect_active_high = true,
};

const pf_board_resources_t *pf_board_resources_get(void)
{
    return &board_resources;
}
