#ifndef PF_SPI_PINS_H
#define PF_SPI_PINS_H

#include <stdint.h>

/** Board-provided mode-0 SPI signal allocation; chip select is software-controlled. */
typedef struct {
    uint32_t clock_gpio; /**< Serial clock output. */
    uint32_t mosi_gpio;  /**< Controller transmit signal. */
    uint32_t miso_gpio;  /**< Controller receive signal. */
    uint32_t cs_gpio;    /**< Active-low device select. */
} pf_spi_pins_t;

#endif
