#ifndef PICOFORGE_SDIO_BLOCK_H
#define PICOFORGE_SDIO_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

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

/** Return true after the SD card has initialized successfully. */
bool picoforge_sdio_is_initialized(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t picoforge_sdio_block_count(void);

/** Read 512-byte sectors into a caller-owned, 4-byte-aligned buffer. */
picoforge_sdio_result_t picoforge_sdio_read_blocks(uint32_t first_block,
                                                    void *buffer,
                                                    uint32_t block_count);

/** Write 512-byte sectors from a caller-owned, 4-byte-aligned buffer. */
picoforge_sdio_result_t picoforge_sdio_write_blocks(uint32_t first_block,
                                                     const void *buffer,
                                                     uint32_t block_count);

/** Complete any pending write; current block writes are synchronous. */
picoforge_sdio_result_t picoforge_sdio_sync(void);

#endif