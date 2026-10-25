#ifndef PF_SDIO_PIO_PINS_H
#define PF_SDIO_PIO_PINS_H

#include <stdint.h>

/** Board-provided SDIO-PIO signal allocation; DAT0-DAT3 are in logical order. */
typedef struct {
    uint32_t clock_gpio;   /**< Clock output. */
    uint32_t command_gpio; /**< Bidirectional command signal. */
    uint32_t data_gpio[4]; /**< DAT0-DAT3; ascending, consecutive GPIOs. */
} pf_sdio_pio_pins_t;

#endif
