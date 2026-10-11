#ifndef PF_SDIO_BLOCK_H
#define PF_SDIO_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define PF_SDIO_BLOCK_SIZE 512u
#define PF_SDIO_MAX_BLOCKS_PER_TRANSFER 256u

typedef enum {
    PF_SDIO_OK = 0,
    PF_SDIO_INVALID_ARGUMENT,
    PF_SDIO_CONFIG_LOCKED,
    PF_SDIO_NOT_CONFIGURED,
    PF_SDIO_CARD_INIT_FAILED,
    PF_SDIO_NOT_READY,
    PF_SDIO_IO_ERROR,
} pf_sdio_result_t;

/** Return true after the SD card has initialized successfully. */
bool pf_sdio_is_initialized(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sdio_block_count(void);

/** Read 512-byte sectors into a caller-owned, 4-byte-aligned buffer. */
pf_sdio_result_t pf_sdio_read_blocks(uint32_t first_block,
                                                    void *buffer,
                                                    uint32_t block_count);

/** Write 512-byte sectors from a caller-owned, 4-byte-aligned buffer. */
pf_sdio_result_t pf_sdio_write_blocks(uint32_t first_block,
                                                     const void *buffer,
                                                     uint32_t block_count);

/** Complete any pending write; current block writes are synchronous. */
pf_sdio_result_t pf_sdio_sync(void);

#endif