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
#define PF_BOARD_LED_ENABLED true
#define PF_BOARD_LED_GPIO PICO_DEFAULT_LED_PIN
#else
#define PF_BOARD_LED_ENABLED false
#define PF_BOARD_LED_GPIO PF_BOARD_PIN_UNASSIGNED
#endif

/** Reference connector signals shared by the mutually exclusive storage modes. */
enum {
    STORAGE_CLK = 2u, /**< Common clock output. */
    STORAGE_IO0 = 3u, /**< CMD or MOSI. */
    STORAGE_IO1 = 4u, /**< DAT0 or MISO. */
    STORAGE_IO2 = 5u, /**< DAT1. */
    STORAGE_IO3 = 6u, /**< DAT2. */
    STORAGE_CS = 7u,  /**< DAT3 or serial CS. */
};

/* Shared storage wiring for Pico-family reference boards. */
static const pf_board_resources_t board_resources = {
    .led_enabled = PF_BOARD_LED_ENABLED,
    .led_gpio = PF_BOARD_LED_GPIO,
    .led_active_high = !PICO_DEFAULT_LED_PIN_INVERTED,
    /* Monitor output is separate from USB CDC. */
    .monitor_uart_enabled = true,
    .monitor_uart_index = PICO_DEFAULT_UART,
    .monitor_uart_tx_gpio = PICO_DEFAULT_UART_TX_PIN,
    .monitor_uart_rx_gpio = PICO_DEFAULT_UART_RX_PIN,
    .monitor_uart_baud_rate = 115200u,
    .sd_card_enabled = true,
    .native_pins.command_gpio = STORAGE_IO0,
    .native_pins.clock_gpio = STORAGE_CLK,
    /* Logical DAT0-DAT3 now follow consecutive GPIO order. */
    .native_pins.data_gpio = { STORAGE_IO1, STORAGE_IO2, STORAGE_IO3, STORAGE_CS },
    .pio_index = 0u,
    /* Fixed transfer target; no automatic probing or lower-clock retries. */
    .clock_hz = 25000000u,
    .spi_index = 0u,
    .spi_pins.clock_gpio = STORAGE_CLK,
    .spi_pins.mosi_gpio = STORAGE_IO0,
    .spi_pins.miso_gpio = STORAGE_IO1,
    .spi_pins.cs_gpio = STORAGE_CS,
    .spi_clock_hz = 50000000u,
    /* The selected breakout provides the DET bias; do not add an MCU pull. */
    .card_detect_enabled = true,
    .card_detect_gpio = 8u,
    .card_detect_active_high = true,
    .card_detect_use_pull = false,
    .card_detect_pull_up = false,
};

const pf_board_resources_t *pf_board_resources_get(void)
{
    return &board_resources;
}
