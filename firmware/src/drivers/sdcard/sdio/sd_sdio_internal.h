/** @file
 * @brief Native SD protocol configuration and diagnostic declarations; not the shared backend API.
 */
#ifndef PF_SD_SDIO_INTERNAL_H
#define PF_SD_SDIO_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "sd.h"

/** Default-Speed transfer target; the PIO divider rounds down when not exactly representable. */
#define PF_SD_SDIO_TRANSFER_CLOCK_HZ 25000000u
/** Initialization diagnostic stages; retained numeric values include legacy coarse stages. */
typedef enum {
    PF_SD_SDIO_INIT_STAGE_NONE = 0, /**< No initialization failure. */
    PF_SD_SDIO_INIT_STAGE_IDLE, /**< CMD0 reset. */
    PF_SD_SDIO_INIT_STAGE_IF_CONDITION, /**< Legacy coarse CMD8 stage. */
    PF_SD_SDIO_INIT_STAGE_OPERATING_CONDITION, /**< ACMD41 setup. */
    PF_SD_SDIO_INIT_STAGE_CID, /**< Legacy coarse CID stage. */
    PF_SD_SDIO_INIT_STAGE_RCA, /**< CMD3 relative address assignment. */
    PF_SD_SDIO_INIT_STAGE_CSD, /**< CMD9 register and capacity validation. */
    PF_SD_SDIO_INIT_STAGE_SELECT, /**< Legacy coarse card selection stage. */
    PF_SD_SDIO_INIT_STAGE_BUS_WIDTH, /**< ACMD6 four-bit selection. */
    PF_SD_SDIO_INIT_STAGE_BLOCK_LENGTH, /**< Retained legacy block-length stage. */
    PF_SD_SDIO_INIT_STAGE_TRANSFER_CLOCK, /**< Transfer-clock setup and validation. */
    PF_SD_SDIO_INIT_STAGE_CMD8_RESPONSE, /**< CMD8 exchange and response validation. */
    PF_SD_SDIO_INIT_STAGE_CMD8_CLASSIFY, /**< CMD8 voltage and card-version classification. */
    PF_SD_SDIO_INIT_STAGE_CID_RESPONSE, /**< CMD2 long response exchange. */
    PF_SD_SDIO_INIT_STAGE_CID_VALIDATE, /**< CID framing and CRC validation. */
    PF_SD_SDIO_INIT_STAGE_SELECT_RESPONSE, /**< CMD7 response validation. */
    PF_SD_SDIO_INIT_STAGE_SELECT_READY, /**< DAT0 release after selection. */
    PF_SD_SDIO_INIT_STAGE_SCR, /**< SCR reception setup. */
    PF_SD_SDIO_INIT_STAGE_BUS_WIDTH_PREFIX, /**< CMD55 before ACMD6. */
    PF_SD_SDIO_INIT_STAGE_SELECT_STATUS, /**< CMD13 transfer-ready check. */
    PF_SD_SDIO_INIT_STAGE_OPERATING_PREFIX, /**< CMD55 before ACMD41. */
    PF_SD_SDIO_INIT_STAGE_OPERATING_RESPONSE, /**< ACMD41 response exchange. */
    PF_SD_SDIO_INIT_STAGE_OPERATING_PARSE, /**< R3/OCR decoding. */
    PF_SD_SDIO_INIT_STAGE_OPERATING_READY, /**< OCR power-up completion. */
    PF_SD_SDIO_INIT_STAGE_SCR_PREFIX, /**< CMD55 before ACMD51. */
    PF_SD_SDIO_INIT_STAGE_SCR_RESPONSE, /**< ACMD51 response validation. */
    PF_SD_SDIO_INIT_STAGE_SCR_DATA, /**< SCR data completion. */
    PF_SD_SDIO_INIT_STAGE_SCR_CRC, /**< SCR CRC validation. */
    PF_SD_SDIO_INIT_STAGE_SCR_BUS_WIDTH, /**< SCR format and four-bit capability validation. */
} pf_sd_sdio_init_stage_t;

/** @name Borrowed singleton diagnostics
 * Callers hold the SD reservation; these getters do not lock. NULL output pointers are ignored.
 * @{
 */
/** Return the stage that failed in the most recent card-init attempt. */
pf_sd_sdio_init_stage_t pf_sd_sdio_get_init_stage(void);

/** Copy the last six-byte short response captured during card initialization. */
void pf_sd_sdio_get_init_response(uint8_t response[6]);
/** Copy the most recently decoded short command response and its request. */
void pf_sd_sdio_get_last_short_response(uint8_t *command, uint32_t *argument,
                                         uint32_t *response_argument);
/** Copy all five retained response words and the number captured, including partial responses. */
void pf_sd_sdio_get_last_response_words(uint32_t words[5], size_t *word_count);
/** @} */

#endif