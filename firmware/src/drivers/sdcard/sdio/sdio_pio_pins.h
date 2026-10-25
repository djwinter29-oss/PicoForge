#ifndef PF_SDIO_PIO_PINS_H
#define PF_SDIO_PIO_PINS_H

#include <stdbool.h>
#include <stdint.h>

/** Board-provided SDIO-PIO signal allocation; DAT0-DAT3 are in logical order. UINT8_MAX is unassigned and invalid. */
typedef struct {
    uint8_t clock_gpio;   /**< Clock output. */
    uint8_t command_gpio; /**< Bidirectional command signal. */
    uint8_t data_gpio[4]; /**< DAT0-DAT3; ascending, consecutive GPIOs. */
} pf_sdio_pio_pins_t;

/** Complete SDIO-PIO socket assignment; card detection is managed by the SD board binding. */
typedef struct {
    pf_sdio_pio_pins_t pins; /**< Required SDIO bus signals. */
    uint8_t pio_index; /**< PIO block: 0 or 1. */
    uint8_t card_detect_gpio; /**< Detect input, or UINT8_MAX to assume presence. */
    bool card_detect_active_high; /**< Detect input polarity. */
} pf_sdio_pio_board_pins_t;

/** Board assignment defined in config/board_sdio.c; used only by the SDIO-PIO SD binding. */
extern const pf_sdio_pio_board_pins_t pf_sdio_pio_board_pins;

#endif
