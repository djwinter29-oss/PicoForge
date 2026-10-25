/** @file
 * @brief SPI card-protocol diagnostic declarations; not the shared backend API.
 *
 * Diagnostics belong to the singleton SPI backend. Callers hold the SD reservation or provide equivalent
 * serialization with all backend operations; these getters do not lock.
 */
#ifndef PF_SD_SPI_INTERNAL_H
#define PF_SD_SPI_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/** Initialization stages; numeric values are retained for diagnostic consumers. */
typedef enum {
    PF_SD_SPI_INIT_STAGE_NONE = 0, /**< No initialization failure. */
    PF_SD_SPI_INIT_STAGE_IDLE_CLOCKS, /**< Released-CS startup clocks. */
    PF_SD_SPI_INIT_STAGE_GO_IDLE, /**< CMD0 entry into SPI idle mode. */
    PF_SD_SPI_INIT_STAGE_IF_CONDITION, /**< CMD8 voltage and card-version classification. */
    PF_SD_SPI_INIT_STAGE_OPERATING_CONDITION, /**< CMD55/ACMD41 power-up negotiation. */
    PF_SD_SPI_INIT_STAGE_READ_OCR, /**< CMD58 voltage and addressing-mode validation. */
    PF_SD_SPI_INIT_STAGE_BLOCK_LENGTH, /**< CMD16 sector-size selection for byte-addressed cards. */
    PF_SD_SPI_INIT_STAGE_CSD, /**< CMD9 card-specific data reception. */
    PF_SD_SPI_INIT_STAGE_CID, /**< CMD10 identification reception and CSD capacity decoding. */
    PF_SD_SPI_INIT_STAGE_TRANSFER_CLOCK, /**< Transfer-clock selection and actual-rate validation. */
    PF_SD_SPI_INIT_STAGE_SWITCH_FUNCTION, /**< CMD6 capability check or timing switch failed. */
    PF_SD_SPI_INIT_STAGE_CRC_ENABLE, /**< CMD59 enabling card CRC checking failed. */
} pf_sd_spi_init_stage_t;

/** Return the stage that failed in the most recent card-init attempt. */
pf_sd_spi_init_stage_t pf_sd_spi_get_init_stage(void);
/** Copy the latest captured R1 and its command; capture resets at card-initialization entry.
 * Runtime commands also update this diagnostic. NULL outputs are ignored.
 * @param command Receives the command associated with the captured response.
 * @param response Receives the retained R1; only meaningful when received is true.
 * @param received Receives true when a response has been captured since reset.
 */
void pf_sd_spi_get_last_init_response(uint8_t *command, uint8_t *response,
                                      bool *received);

#endif