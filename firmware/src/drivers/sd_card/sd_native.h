#ifndef PF_SD_NATIVE_H
#define PF_SD_NATIVE_H

#include <stdint.h>
#include <stddef.h>

#include "sd.h"

/** Fixed transfer target; integer PIO divider rounds down when not exactly representable. */
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

/**
 * Initialize the configured SDHC/SDXC card and CRC-check LBA 0 at each selected timing.
 *
 * The configuration is retained for the lifetime of the firmware and cannot be
 * changed after first use. Repeating the same call retries card initialization.
 * Legacy/SDSC cards are rejected. No user sectors are written. High Speed is
 * disabled unless PF_SD_NATIVE_HIGH_SPEED is enabled at build time; that path
 * requests up to 50 MHz (the transport clamps to its ceiling and skips High Speed
 * when that is not above 25 MHz) and permits one fallback to 25 MHz at startup,
 * never runtime transfer replay.
 */
pf_sd_result_t pf_sd_native_init(const pf_sd_config_t *config);

/** Return the stage that failed in the most recent card-init attempt. */
pf_sd_native_init_stage_t pf_sd_native_get_init_stage(void);

/** Copy the last six-byte short response captured during card initialization. */
void pf_sd_native_get_init_response(uint8_t response[6]);
void pf_sd_native_get_last_short_response(uint8_t *command, uint32_t *argument,
                                         uint32_t *response_argument);
void pf_sd_native_get_last_response_words(uint32_t words[5], size_t *word_count);

/** Return whether the card has initialized successfully. */
bool pf_sd_native_is_initialized(void);
/** Storage owner calls only after debounced removal; sends no card command. */
void pf_sd_native_confirm_card_removed(void);

/** Return the initialized card's number of 512-byte sectors, or zero. */
uint32_t pf_sd_native_block_count(void);

/** Read sectors into a caller-owned buffer of any alignment. */
pf_sd_result_t pf_sd_native_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count);

/** Write sectors from a caller-owned buffer of any alignment. */
pf_sd_result_t pf_sd_native_write_blocks(uint32_t first_block, const void *buffer,
                                            uint32_t block_count);

/** Complete pending writes; current block writes are synchronous. */
pf_sd_result_t pf_sd_native_sync(void);

void pf_sd_native_get_info(pf_sd_info_t *info);

#endif