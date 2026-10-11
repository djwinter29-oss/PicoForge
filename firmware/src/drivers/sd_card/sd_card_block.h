#ifndef PF_SD_CARD_BLOCK_H
#define PF_SD_CARD_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define PF_SD_CARD_BLOCK_SIZE 512u
#define PF_SD_CARD_MAX_BLOCKS_PER_TRANSFER 256u

typedef enum {
    PF_SD_CARD_OK = 0,
    PF_SD_CARD_INVALID_ARGUMENT,
    PF_SD_CARD_CONFIG_LOCKED,
    PF_SD_CARD_NOT_CONFIGURED,
    PF_SD_CARD_INIT_FAILED,
    PF_SD_CARD_NOT_READY,
    PF_SD_CARD_IO_ERROR,
    PF_SD_CARD_UNSUPPORTED_TRANSPORT,
} pf_sd_card_result_t;

/** Return whether the selected card transport has initialized successfully. */
bool pf_sd_card_is_initialized(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sd_card_block_count(void);

/** Read 512-byte sectors into a caller-owned buffer. */
pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count);

/** Write 512-byte sectors from a caller-owned buffer. */
pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block, const void *buffer,
                                            uint32_t block_count);

/** Complete any pending write. */
pf_sd_card_result_t pf_sd_card_sync(void);

#endif