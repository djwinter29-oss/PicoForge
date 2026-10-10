#ifndef PICOFORGE_SDIO_PIO_H
#define PICOFORGE_SDIO_PIO_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

#define PICOFORGE_SDIO_BLOCK_SIZE 512u
#define PICOFORGE_SDIO_MAX_BLOCKS_PER_TRANSFER 256u

typedef enum {
    PICOFORGE_SDIO_OK = 0,
    PICOFORGE_SDIO_INVALID_ARGUMENT,
    PICOFORGE_SDIO_CONFIG_LOCKED,
    PICOFORGE_SDIO_NOT_CONFIGURED,
    PICOFORGE_SDIO_CARD_INIT_FAILED,
    PICOFORGE_SDIO_NOT_READY,
    PICOFORGE_SDIO_IO_ERROR,
} picoforge_sdio_result_t;

/** Board-provided SD memory-card connection settings for the native PIO host. */
typedef struct {
    uint32_t command_gpio;
    uint32_t data0_gpio;
    PIO pio;                 /**< Must be pio0 or pio1; the backend reserves this PIO instruction memory. */
    uint32_t clock_hz;       /**< 0 selects clk_sys / 12; otherwise 400 kHz through 25 MHz. */
    uint32_t dma_irq;        /**< Reserved; current block transfers poll DMA synchronously. */
    bool use_card_detect;
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
    bool card_detect_use_pull;
    bool card_detect_pull_up;
} picoforge_sdio_config_t;

/**
 * Initialize the one configured SD memory card.
 *
 * The configuration is retained for the lifetime of the firmware and cannot be
 * changed after first use. Repeating the same call retries card initialization.
 */
picoforge_sdio_result_t picoforge_sdio_init(const picoforge_sdio_config_t *config);

/** Return true after the SD card has initialized successfully. */
bool picoforge_sdio_is_initialized(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t picoforge_sdio_block_count(void);

/**
 * Read 512-byte sectors into a caller-owned, 4-byte-aligned buffer.
 *
 * Serialize calls through one storage-owner task; do not call concurrently with
 * initialization or another transfer.
 */
picoforge_sdio_result_t picoforge_sdio_read_blocks(uint32_t first_block, void *buffer, uint32_t block_count);

/**
 * Write 512-byte sectors from a caller-owned, 4-byte-aligned buffer.
 *
 * Serialize calls through one storage-owner task; do not call concurrently with
 * initialization or another transfer.
 */
picoforge_sdio_result_t picoforge_sdio_write_blocks(uint32_t first_block, const void *buffer, uint32_t block_count);

/** Complete any pending write; current block writes are synchronous. */
picoforge_sdio_result_t picoforge_sdio_sync(void);

#endif