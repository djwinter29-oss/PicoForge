#ifndef PF_SDIO_PIO_H
#define PF_SDIO_PIO_H

#include <stdint.h>
#include <stddef.h>

#include "hardware/pio.h"
#include "sd_card_block.h"

/** Upper bound of the default bus clock selected by a zero pf_sdio_pio_config_t::clock_hz. */
#define PF_SDIO_PIO_DEFAULT_CLOCK_MAX_HZ 12500000u

/** Board-provided SD memory-card connection settings for the native PIO host. */
typedef struct {
    uint32_t command_gpio;
    uint32_t clock_gpio;
    uint32_t data_gpio[4];    /**< GPIO assignment for logical DAT0, DAT1, DAT2, DAT3. */
    PIO pio;                 /**< Must be pio0 or pio1; the backend reserves this PIO instruction memory. */
    uint32_t clock_hz;       /**< 0 selects min(clk_sys / 12, 12.5 MHz); otherwise 400 kHz through 25 MHz. */
    bool use_card_detect;
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
    bool card_detect_use_pull;
    bool card_detect_pull_up;
} pf_sdio_pio_config_t;

typedef enum {
    PF_SDIO_PIO_INIT_STAGE_NONE = 0,
    PF_SDIO_PIO_INIT_STAGE_IDLE,
    PF_SDIO_PIO_INIT_STAGE_IF_CONDITION,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_CONDITION,
    PF_SDIO_PIO_INIT_STAGE_CID,
    PF_SDIO_PIO_INIT_STAGE_RCA,
    PF_SDIO_PIO_INIT_STAGE_CSD,
    PF_SDIO_PIO_INIT_STAGE_SELECT,
    PF_SDIO_PIO_INIT_STAGE_BUS_WIDTH,
    PF_SDIO_PIO_INIT_STAGE_BLOCK_LENGTH,
    PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK,
    PF_SDIO_PIO_INIT_STAGE_CMD8_RESPONSE,
    PF_SDIO_PIO_INIT_STAGE_CMD8_CLASSIFY,
    PF_SDIO_PIO_INIT_STAGE_CID_RESPONSE,
    PF_SDIO_PIO_INIT_STAGE_CID_VALIDATE,
    PF_SDIO_PIO_INIT_STAGE_SELECT_RESPONSE,
    PF_SDIO_PIO_INIT_STAGE_SELECT_READY,
    PF_SDIO_PIO_INIT_STAGE_BUS_WIDTH_PREFIX,
    PF_SDIO_PIO_INIT_STAGE_SELECT_STATUS,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_PREFIX,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_RESPONSE,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_PARSE,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_READY,
} pf_sdio_pio_init_stage_t;

/**
 * Initialize the one configured SD memory card.
 *
 * The configuration is retained for the lifetime of the firmware and cannot be
 * changed after first use. Repeating the same call retries card initialization.
 */
pf_sd_card_result_t pf_sdio_pio_init(const pf_sdio_pio_config_t *config);

/** Return the stage that failed in the most recent card-init attempt. */
pf_sdio_pio_init_stage_t pf_sdio_pio_get_init_stage(void);

/** Copy the last six-byte short response captured during card initialization. */
void pf_sdio_pio_get_init_response(uint8_t response[6]);
void pf_sdio_pio_get_last_short_response(uint8_t *command, uint32_t *argument,
                                         uint32_t *response_argument);
void pf_sdio_pio_get_last_response_words(uint32_t words[5], size_t *word_count);

/** Return whether the card has initialized successfully. */
bool pf_sdio_pio_is_initialized(void);

/** Return the initialized card's number of 512-byte sectors, or zero. */
uint32_t pf_sdio_pio_block_count(void);

/** Read sectors into a caller-owned buffer of any alignment. */
pf_sd_card_result_t pf_sdio_pio_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count);

/** Write sectors from a caller-owned buffer of any alignment. */
pf_sd_card_result_t pf_sdio_pio_write_blocks(uint32_t first_block, const void *buffer,
                                            uint32_t block_count);

/** Complete pending writes; current block writes are synchronous. */
pf_sd_card_result_t pf_sdio_pio_sync(void);

void pf_sdio_pio_get_info(pf_sd_card_info_t *info);

#endif