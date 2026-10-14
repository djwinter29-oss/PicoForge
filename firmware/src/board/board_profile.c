#include "board_resources.h"

#include "pico/stdlib.h"

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
    .command_gpio = 3u,
    .clock_gpio = 2u,
    /* Logical DAT0-DAT3 order; lane remapping handles physical GPIO order. */
    .data_gpio = { 4u, 7u, 6u, 5u },
    .pio_index = 0u,
    .clock_hz = 25000000u, /* 25 MHz qualification build; CPU remains at 150 MHz. */
    .spi_index = 0u,
    .spi_clock_gpio = 2u,
    .spi_mosi_gpio = 3u,
    .spi_miso_gpio = 4u,
    .spi_chip_select_gpio = 5u,
    .spi_clock_hz = 0u,
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