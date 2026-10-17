#include "board_resources.h"

#include "pico/stdlib.h"

/** Reference connector signals shared by the mutually exclusive storage modes. */
enum {
    STORAGE_CLK = 2u, /**< Common clock output. */
    STORAGE_IO0 = 3u, /**< CMD or MOSI/IO0. */
    STORAGE_IO1 = 4u, /**< DAT0 or MISO/IO1. */
    STORAGE_IO2 = 5u, /**< DAT1 or IO2/WP. */
    STORAGE_IO3 = 6u, /**< DAT2 or IO3/HOLD. */
    STORAGE_CS = 7u,  /**< DAT3 or serial CS. */
};

/* Fixed profile for standard Pico and Pico 2 boards. */
static const pf_board_resources_t board_resources = {
    .gpio_count = NUM_BANK0_GPIOS,
    .led_enabled = true,
    .led_gpio = 25u,
    .led_active_high = true,
    /* Monitor output is separate from USB CDC. */
    .monitor_uart_enabled = true,
    .monitor_uart_index = 0u,
    .monitor_uart_tx_gpio = 0u,
    .monitor_uart_rx_gpio = 1u,
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
    .qspi_pins = {
        .clock_gpio = STORAGE_CLK, .cs_gpio = STORAGE_CS,
        .data_gpio = { STORAGE_IO0, STORAGE_IO1, STORAGE_IO2, STORAGE_IO3 },
    },
    .serial_clock_hz = 1000000u,
    .serial_timeout_us = 100000u,
    .serial_wp_gpio = STORAGE_IO2,
    .serial_hold_gpio = STORAGE_IO3,
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
