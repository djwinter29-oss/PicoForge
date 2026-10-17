#ifndef STORAGE_TEST_SELECTED_TRANSPORT_H
#define STORAGE_TEST_SELECTED_TRANSPORT_H
#include "config/config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#include "drivers/sd_card/sd_card_spi.h"
#define backend_init pf_sd_spi_init
#define backend_config_t pf_sd_spi_config_t
#define backend_stage pf_sd_spi_get_init_stage
#define backend_stage_t pf_sd_spi_init_stage_t
#define backend_ready pf_sd_spi_is_initialized
#define backend_blocks pf_sd_spi_block_count
#define backend_read pf_sd_spi_read_blocks
#define backend_write pf_sd_spi_write_blocks
#define backend_sync pf_sd_spi_sync
#define backend_info pf_sd_spi_get_info
#define TEST_TRANSPORT PF_SD_CARD_TRANSPORT_SPI
#define OTHER_TRANSPORT PF_SD_CARD_TRANSPORT_SDIO_PIO
#else
#include "drivers/sd_card/sd_card_native.h"
#define backend_init pf_sdio_pio_init
#define backend_config_t pf_sdio_pio_config_t
#define backend_stage pf_sdio_pio_get_init_stage
#define backend_stage_t pf_sdio_pio_init_stage_t
#define backend_ready pf_sdio_pio_is_initialized
#define backend_blocks pf_sdio_pio_block_count
#define backend_read pf_sdio_pio_read_blocks
#define backend_write pf_sdio_pio_write_blocks
#define backend_sync pf_sdio_pio_sync
#define backend_info pf_sdio_pio_get_info
#define TEST_TRANSPORT PF_SD_CARD_TRANSPORT_SDIO_PIO
#define OTHER_TRANSPORT PF_SD_CARD_TRANSPORT_SPI
#endif
#endif
