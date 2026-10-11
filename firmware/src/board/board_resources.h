#ifndef PF_BOARD_RESOURCES_H
#define PF_BOARD_RESOURCES_H

#include <stdbool.h>
#include <stdint.h>

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
    uint32_t command_gpio;
    uint32_t clock_gpio;
    uint32_t data_gpio[4];
    uint32_t pio_index;
    uint32_t clock_hz;
    uint32_t spi_index;
    uint32_t spi_clock_gpio;
    uint32_t spi_mosi_gpio;
    uint32_t spi_miso_gpio;
    uint32_t spi_chip_select_gpio;
    uint32_t spi_clock_hz;
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