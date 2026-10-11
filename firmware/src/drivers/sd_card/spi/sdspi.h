#ifndef PF_SDSPI_H
#define PF_SDSPI_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/spi.h"
#include "sd_card_block.h"

/** Board-provided SPI pin assignments; chip select is GPIO-controlled for whole SD transactions. */
typedef struct {
    spi_inst_t *spi;
    uint32_t clock_gpio;
    uint32_t mosi_gpio;
    uint32_t miso_gpio;
    uint32_t chip_select_gpio;
    uint32_t clock_hz; /**< 0 selects 25 MHz after the 400 kHz initialization phase. */
    bool use_card_detect;
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
    bool card_detect_use_pull;
    bool card_detect_pull_up;
} pf_sdspi_config_t;

/**
 * Initialize the configured card. The first successful SPI setup locks this
 * singleton to the supplied configuration; repeating it retries card init.
 */
pf_sd_card_result_t pf_sdspi_init(const pf_sdspi_config_t *config);

/** Return true while the initialized card remains present. */
bool pf_sdspi_is_initialized(void);

/** Return the card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sdspi_block_count(void);

/** Read up to PF_SD_CARD_MAX_BLOCKS_PER_TRANSFER sectors into a caller buffer. */
pf_sd_card_result_t pf_sdspi_read_blocks(uint32_t first_block, void *buffer,
                                         uint32_t block_count);

/** Write up to PF_SD_CARD_MAX_BLOCKS_PER_TRANSFER sectors from a caller buffer. */
pf_sd_card_result_t pf_sdspi_write_blocks(uint32_t first_block, const void *buffer,
                                          uint32_t block_count);

/** Wait for the card to finish its current write operation. */
pf_sd_card_result_t pf_sdspi_sync(void);

#endif