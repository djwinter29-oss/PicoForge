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
} pf_sd_card_result_t;

#endif