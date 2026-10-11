#ifndef PF_SDIO_PIO_PIO_H
#define PF_SDIO_PIO_PIO_H

#include <stdint.h>

#include "hardware/pio.h"
#include "sdio_block.h"

/** Board-provided SD memory-card connection settings for the native PIO host. */
typedef struct {
    uint32_t command_gpio;
    uint32_t clock_gpio;
    uint32_t data_gpio[4];    /**< GPIO assignment for logical DAT0, DAT1, DAT2, DAT3. */
    PIO pio;                 /**< Must be pio0 or pio1; the backend reserves this PIO instruction memory. */
    uint32_t clock_hz;       /**< 0 selects clk_sys / 12; otherwise 400 kHz through 25 MHz. */
    uint32_t dma_irq;        /**< Reserved; current block transfers poll DMA synchronously. */
    bool use_card_detect;
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
    bool card_detect_use_pull;
    bool card_detect_pull_up;
} pf_sdio_pio_config_t;

/**
 * Initialize the one configured SD memory card.
 *
 * The configuration is retained for the lifetime of the firmware and cannot be
 * changed after first use. Repeating the same call retries card initialization.
 */
pf_sdio_pio_result_t pf_sdio_pio_init(const pf_sdio_pio_config_t *config);

#endif