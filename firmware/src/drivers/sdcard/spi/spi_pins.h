#ifndef PF_SPI_PINS_H
#define PF_SPI_PINS_H

#include <stdbool.h>
#include <stdint.h>

/** Board-provided mode-0 SPI signal allocation; CS is software-controlled. UINT8_MAX is unassigned and invalid. */
typedef struct {
    uint8_t clock_gpio; /**< Serial clock output. */
    uint8_t mosi_gpio;  /**< Controller transmit signal. */
    uint8_t miso_gpio;  /**< Controller receive signal. */
    uint8_t cs_gpio;    /**< Active-low device select. */
} pf_spi_pins_t;

/** Complete SPI socket assignment; card detection is managed by the SD board binding, not the SPI engine. */
typedef struct {
    pf_spi_pins_t pins; /**< Required SPI bus signals. */
    uint8_t spi_index; /**< SPI controller: 0 or 1. */
    uint8_t card_detect_gpio; /**< Detect input, or UINT8_MAX to assume presence. */
    bool card_detect_active_high; /**< Detect input polarity. */
} pf_spi_board_pins_t;

/** Board assignment defined in config/board_spi.c; used only by the SPI SD binding. */
extern const pf_spi_board_pins_t pf_spi_board_pins;

#endif
