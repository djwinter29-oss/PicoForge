#ifndef PF_SD_CARD_NATIVE_H
#define PF_SD_CARD_NATIVE_H

#include <stdint.h>
#include <stddef.h>

#include "hardware/pio.h"
#include "sd_card_block.h"
#include "../storage/storage_bus.h"

/** Fixed transfer target; integer PIO divider rounds down when not exactly representable. */
#define PF_SDIO_PIO_TRANSFER_CLOCK_HZ 25000000u
/** Compatibility name for the default transfer limit. */
#define PF_SDIO_PIO_DEFAULT_CLOCK_MAX_HZ PF_SDIO_PIO_TRANSFER_CLOCK_HZ

#ifndef PF_SDIO_PIO_HIGH_SPEED
#define PF_SDIO_PIO_HIGH_SPEED 0
#endif

/** Optional platform wait callback; completion flags remain owned by the host.
 * Called only during bounded data waits. Must use a bounded wait,
 * must not clear the PIO flag or access the card, and must tolerate boot before
 * a scheduler starts. Set before card access; NULL retains plain polling.
 */
typedef void (*pf_sdio_pio_wait_hook_t)(PIO pio, uint32_t completion_flag);
void pf_sdio_pio_set_wait_hook(pf_sdio_pio_wait_hook_t hook);

/** Board-provided SDHC/SDXC connection settings for the native four-bit PIO host. */
typedef struct {
    pf_storage_sdio_pins_t pins; /**< Physical signals shared with the native transport. */
    PIO pio;                 /**< Must be pio0 or pio1; the backend reserves this PIO instruction memory. */
    uint32_t clock_hz;       /**< Only 0 or 25000000 accepted; High Speed is a separate build opt-in. */
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
    PF_SDIO_PIO_INIT_STAGE_SCR,
    PF_SDIO_PIO_INIT_STAGE_BUS_WIDTH_PREFIX,
    PF_SDIO_PIO_INIT_STAGE_SELECT_STATUS,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_PREFIX,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_RESPONSE,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_PARSE,
    PF_SDIO_PIO_INIT_STAGE_OPERATING_READY,
    PF_SDIO_PIO_INIT_STAGE_SCR_PREFIX,
    PF_SDIO_PIO_INIT_STAGE_SCR_RESPONSE,
    PF_SDIO_PIO_INIT_STAGE_SCR_DATA,
    PF_SDIO_PIO_INIT_STAGE_SCR_CRC,
    PF_SDIO_PIO_INIT_STAGE_SCR_BUS_WIDTH,
} pf_sdio_pio_init_stage_t;

/**
 * Initialize the configured SDHC/SDXC card and CRC-check LBA 0 at each selected timing.
 *
 * The configuration is retained for the lifetime of the firmware and cannot be
 * changed after first use. Repeating the same call retries card initialization.
 * Legacy/SDSC cards are rejected. No user sectors are written. High Speed is
 * disabled unless PF_SDIO_PIO_HIGH_SPEED is enabled at build time; that path
 * permits one 50-to-25 MHz startup fallback, never runtime transfer replay.
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
/** Storage owner calls only after debounced removal; sends no card command. */
void pf_sdio_pio_confirm_card_removed(void);

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