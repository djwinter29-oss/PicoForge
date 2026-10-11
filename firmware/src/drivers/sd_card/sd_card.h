#ifndef PF_SD_CARD_H
#define PF_SD_CARD_H

#include "sd_card_block.h"
#include "sdio_pio/sdio_pio.h"
#include "spi/sdspi.h"

typedef enum {
    PF_SD_CARD_TRANSPORT_SDIO_PIO = 0,
    PF_SD_CARD_TRANSPORT_SPI,
} pf_sd_card_transport_t;

typedef struct {
    pf_sd_card_transport_t transport;
    union {
        pf_sdio_pio_config_t sdio_pio;
        pf_sdspi_config_t spi;
    } bus;
} pf_sd_card_config_t;

pf_sd_card_result_t pf_sd_card_init(const pf_sd_card_config_t *config);
bool pf_sd_card_is_initialized(void);
uint32_t pf_sd_card_block_count(void);
pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count);
pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block, const void *buffer,
                                            uint32_t block_count);
pf_sd_card_result_t pf_sd_card_sync(void);

#endif