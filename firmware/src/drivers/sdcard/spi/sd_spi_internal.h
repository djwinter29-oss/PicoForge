/** @file
 * @brief SPI card-protocol diagnostic declarations; not the shared backend API.
 */
#ifndef PF_SD_SPI_INTERNAL_H
#define PF_SD_SPI_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "sd_backend.h"

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
    PF_SD_SPI_INIT_STAGE_SWITCH_FUNCTION, /**< CMD6 capability check or timing switch failed. */
    PF_SD_SPI_INIT_STAGE_CRC_ENABLE, /**< CMD59 enabling card CRC checking failed. */
} pf_sd_spi_init_stage_t;

/** Return the stage that failed in the most recent card-init attempt. */
pf_sd_spi_init_stage_t pf_sd_spi_get_init_stage(void);
void pf_sd_spi_get_last_init_response(uint8_t *command, uint8_t *response,
                                      bool *received);

#endif