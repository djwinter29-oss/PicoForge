#ifndef PF_SD_SPI_H
#define PF_SD_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "sd.h"

typedef enum {
    PF_SD_SPI_INIT_STAGE_NONE = 0,
    PF_SD_SPI_INIT_STAGE_IDLE_CLOCKS,
    PF_SD_SPI_INIT_STAGE_GO_IDLE,
    PF_SD_SPI_INIT_STAGE_IF_CONDITION,
    PF_SD_SPI_INIT_STAGE_OPERATING_CONDITION,
    PF_SD_SPI_INIT_STAGE_READ_OCR,
    PF_SD_SPI_INIT_STAGE_BLOCK_LENGTH,
    PF_SD_SPI_INIT_STAGE_CSD,
    PF_SD_SPI_INIT_STAGE_CID,
    PF_SD_SPI_INIT_STAGE_TRANSFER_CLOCK,
} pf_sd_spi_init_stage_t;

/**
 * Initialize the configured card. The first successful SPI setup locks this
 * singleton to the supplied configuration; repeating it retries card init.
 */
pf_sd_result_t pf_sd_spi_init(const pf_sd_config_t *config);

/** Return the stage that failed in the most recent card-init attempt. */
pf_sd_spi_init_stage_t pf_sd_spi_get_init_stage(void);
void pf_sd_spi_get_last_init_response(uint8_t *command, uint8_t *response,
                                      bool *received);

/** Return true while the initialized card remains present. */
bool pf_sd_spi_is_initialized(void);

/** Return the card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sd_spi_block_count(void);

/** Read sectors into a caller-owned buffer of any alignment. */
pf_sd_result_t pf_sd_spi_read_blocks(uint32_t first_block, void *buffer,
                                         uint32_t block_count);

/** Write sectors from a caller-owned buffer of any alignment. */
pf_sd_result_t pf_sd_spi_write_blocks(uint32_t first_block, const void *buffer,
                                          uint32_t block_count);

/** Wait for the card to finish its current write operation. */
pf_sd_result_t pf_sd_spi_sync(void);

void pf_sd_spi_get_info(pf_sd_info_t *info);

#endif