#ifndef PF_BOARD_RESOURCES_H
#define PF_BOARD_RESOURCES_H

#include <stdbool.h>
#include <stdint.h>
#include "config/config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
#include "drivers/storage/sdio_pio/sdio_pio_pins.h"
#else
#include "drivers/storage/spi/spi_pins.h"
#endif

#define PF_BOARD_PIN_UNASSIGNED UINT32_MAX

/** Immutable board GPIO and peripheral assignments selected at build time. */
typedef struct {
    uint32_t led_gpio;
    bool led_active_high;
    uint32_t monitor_uart_index;
    uint32_t monitor_uart_tx_gpio;
    uint32_t monitor_uart_rx_gpio;
    uint32_t monitor_uart_baud_rate;
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    pf_sdio_pio_pins_t sdio_pio_pins; /**< Selected SDIO-PIO signal allocation. */
    uint32_t pio_index; /**< Selected PIO block. */
#else
    pf_spi_pins_t spi_pins; /**< Selected SPI signal allocation. */
    uint32_t spi_index; /**< Selected SPI controller. */
#endif
    uint32_t sd_clock_hz; /**< Selected transport ceiling; above 25 MHz requires card High-Speed support. */
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
} pf_board_resources_t;

/** Return the immutable pin profile selected for this board build. */
const pf_board_resources_t *pf_board_resources_get(void);

#endif