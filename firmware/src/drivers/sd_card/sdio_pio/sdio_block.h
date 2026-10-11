#ifndef PF_SDIO_PIO_BLOCK_H
#define PF_SDIO_PIO_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "../sd_card_block.h"

typedef enum {
    PF_SDIO_PIO_OK = 0,
    PF_SDIO_PIO_INVALID_ARGUMENT,
    PF_SDIO_PIO_CONFIG_LOCKED,
    PF_SDIO_PIO_NOT_CONFIGURED,
    PF_SDIO_PIO_CARD_INIT_FAILED,
    PF_SDIO_PIO_NOT_READY,
    PF_SDIO_PIO_IO_ERROR,
} pf_sdio_pio_result_t;

/** Return true after the SD card has initialized successfully. */
bool pf_sdio_pio_is_initialized(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sdio_pio_block_count(void);

/** Read 512-byte sectors into a caller-owned, 4-byte-aligned buffer. */
pf_sdio_pio_result_t pf_sdio_pio_read_blocks(uint32_t first_block,
                                                    void *buffer,
                                                    uint32_t block_count);

/** Write 512-byte sectors from a caller-owned, 4-byte-aligned buffer. */
pf_sdio_pio_result_t pf_sdio_pio_write_blocks(uint32_t first_block,
                                                     const void *buffer,
                                                     uint32_t block_count);

/** Complete any pending write; current block writes are synchronous. */
pf_sdio_pio_result_t pf_sdio_pio_sync(void);

#endif