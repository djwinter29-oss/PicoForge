#ifndef PF_SDIO_PIO_PINS_H
#define PF_SDIO_PIO_PINS_H

#include <stdint.h>

/** Board-provided SDIO-PIO signal allocation; DAT0-DAT3 are in logical order. */
typedef struct {
    uint32_t clock_gpio;   /**< Clock output. */
    uint32_t command_gpio; /**< Bidirectional command signal. */
    uint32_t data_gpio[4]; /**< DAT0-DAT3; ascending, consecutive GPIOs. */
} pf_sdio_pio_pins_t;

/** Board-supplied SDIO-PIO assignment. */
typedef struct {
    pf_sdio_pio_pins_t pins; /**< Signal allocation. */
    uint32_t pio_index; /**< PIO block. */
} pf_sdio_pio_board_t;

/** Build-selected assignment, defined in config/board.c when SDIO-PIO is selected. */
extern const pf_sdio_pio_board_t pf_sdio_pio_board;

#endif
