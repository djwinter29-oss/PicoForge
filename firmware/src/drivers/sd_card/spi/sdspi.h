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
    uint32_t clock_hz;
    bool use_card_detect;
    uint32_t card_detect_gpio;
    bool card_detect_active_high;
    bool card_detect_use_pull;
    bool card_detect_pull_up;
} pf_sdspi_config_t;

pf_sd_card_result_t pf_sdspi_init(const pf_sdspi_config_t *config);
bool pf_sdspi_is_initialized(void);
uint32_t pf_sdspi_block_count(void);
pf_sd_card_result_t pf_sdspi_read_blocks(uint32_t first_block, void *buffer,
                                         uint32_t block_count);
pf_sd_card_result_t pf_sdspi_write_blocks(uint32_t first_block, const void *buffer,
                                          uint32_t block_count);
pf_sd_card_result_t pf_sdspi_sync(void);

#endif