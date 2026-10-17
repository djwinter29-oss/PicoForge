#ifndef PF_BOARD_RESOURCES_H
#define PF_BOARD_RESOURCES_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/storage/storage_transport.h"

#define PF_BOARD_PIN_UNASSIGNED UINT32_MAX

/** Board-owned GPIO and peripheral resource assignments. */
typedef struct {
    uint32_t gpio_count;
    bool led_enabled;
    uint32_t led_gpio;
    bool led_active_high;
    bool monitor_uart_enabled;
    uint32_t monitor_uart_index;
    uint32_t monitor_uart_tx_gpio;
    uint32_t monitor_uart_rx_gpio;
    uint32_t monitor_uart_baud_rate;
    bool sd_card_enabled;
    pf_storage_sdio_pins_t native_pins; /**< Native SD signal allocation. */
    uint32_t pio_index;
    uint32_t clock_hz;
    uint32_t spi_index;
    pf_storage_spi_pins_t spi_pins; /**< Shared SD/NOR SPI signal allocation. */
    uint32_t spi_clock_hz;
    pf_storage_qspi_pins_t qspi_pins; /**< Quad serial signal allocation. */
    uint32_t serial_clock_hz; /**< Initial external serial-device clock. */
    uint32_t serial_timeout_us; /**< SPI per-byte FIFO deadline. */
    uint32_t serial_wp_gpio; /**< Inactive-high SPI-only write protect. */
    uint32_t serial_hold_gpio; /**< Inactive-high SPI-only hold. */
    bool card_detect_enabled;
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
    bool card_detect_use_pull;
    bool card_detect_pull_up;
} pf_board_resources_t;

/** Return the immutable pin profile selected for this board build. */
const pf_board_resources_t *pf_board_resources_get(void);

/** Validate board pin/peripheral assignments without accessing hardware. */
bool pf_board_resources_validate(const pf_board_resources_t *topology);

#endif