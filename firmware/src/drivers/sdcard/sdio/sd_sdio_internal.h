/** @file
 * @brief Native SD protocol configuration and diagnostic declarations; not the shared backend API.
 */
#ifndef PF_SD_SDIO_INTERNAL_H
#define PF_SD_SDIO_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "sd_backend.h"

/** Default-Speed transfer target; the PIO divider rounds down when not exactly representable. */
#define PF_SD_NATIVE_TRANSFER_CLOCK_HZ 25000000u
/** Compatibility name for the default transfer limit. */
#define PF_SD_NATIVE_DEFAULT_CLOCK_MAX_HZ PF_SD_NATIVE_TRANSFER_CLOCK_HZ

#ifndef PF_SD_NATIVE_HIGH_SPEED
#define PF_SD_NATIVE_HIGH_SPEED 0
#endif

typedef enum {
    PF_SD_NATIVE_INIT_STAGE_NONE = 0,
    PF_SD_NATIVE_INIT_STAGE_IDLE,
    PF_SD_NATIVE_INIT_STAGE_IF_CONDITION,
    PF_SD_NATIVE_INIT_STAGE_OPERATING_CONDITION,
    PF_SD_NATIVE_INIT_STAGE_CID,
    PF_SD_NATIVE_INIT_STAGE_RCA,
    PF_SD_NATIVE_INIT_STAGE_CSD,
    PF_SD_NATIVE_INIT_STAGE_SELECT,
    PF_SD_NATIVE_INIT_STAGE_BUS_WIDTH,
    PF_SD_NATIVE_INIT_STAGE_BLOCK_LENGTH,
    PF_SD_NATIVE_INIT_STAGE_TRANSFER_CLOCK,
    PF_SD_NATIVE_INIT_STAGE_CMD8_RESPONSE,
    PF_SD_NATIVE_INIT_STAGE_CMD8_CLASSIFY,
    PF_SD_NATIVE_INIT_STAGE_CID_RESPONSE,
    PF_SD_NATIVE_INIT_STAGE_CID_VALIDATE,
    PF_SD_NATIVE_INIT_STAGE_SELECT_RESPONSE,
    PF_SD_NATIVE_INIT_STAGE_SELECT_READY,
    PF_SD_NATIVE_INIT_STAGE_SCR,
    PF_SD_NATIVE_INIT_STAGE_BUS_WIDTH_PREFIX,
    PF_SD_NATIVE_INIT_STAGE_SELECT_STATUS,
    PF_SD_NATIVE_INIT_STAGE_OPERATING_PREFIX,
    PF_SD_NATIVE_INIT_STAGE_OPERATING_RESPONSE,
    PF_SD_NATIVE_INIT_STAGE_OPERATING_PARSE,
    PF_SD_NATIVE_INIT_STAGE_OPERATING_READY,
    PF_SD_NATIVE_INIT_STAGE_SCR_PREFIX,
    PF_SD_NATIVE_INIT_STAGE_SCR_RESPONSE,
    PF_SD_NATIVE_INIT_STAGE_SCR_DATA,
    PF_SD_NATIVE_INIT_STAGE_SCR_CRC,
    PF_SD_NATIVE_INIT_STAGE_SCR_BUS_WIDTH,
} pf_sd_native_init_stage_t;

/** Return the stage that failed in the most recent card-init attempt. */
pf_sd_native_init_stage_t pf_sd_native_get_init_stage(void);

/** Copy the last six-byte short response captured during card initialization. */
void pf_sd_native_get_init_response(uint8_t response[6]);
void pf_sd_native_get_last_short_response(uint8_t *command, uint32_t *argument,
                                         uint32_t *response_argument);
void pf_sd_native_get_last_response_words(uint32_t words[5], size_t *word_count);

#endif