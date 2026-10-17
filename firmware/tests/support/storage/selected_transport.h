#ifndef STORAGE_TEST_SELECTED_TRANSPORT_H
#define STORAGE_TEST_SELECTED_TRANSPORT_H
#include "config/config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#include "drivers/sd_card/sd_spi.h"
#define backend_init pf_sd_spi_init
#define backend_config_t pf_sd_config_t
#define backend_stage pf_sd_spi_get_init_stage
#define backend_stage_t pf_sd_spi_init_stage_t
#define backend_ready pf_sd_spi_is_initialized
#define backend_blocks pf_sd_spi_block_count
#define backend_read pf_sd_spi_read_blocks
#define backend_write pf_sd_spi_write_blocks
#define backend_sync pf_sd_spi_sync
#define backend_info pf_sd_spi_get_info
#define TEST_TRANSPORT PF_STORAGE_TRANSPORT_SPI
#define OTHER_TRANSPORT PF_STORAGE_TRANSPORT_NATIVE_SD
#else
#include "drivers/sd_card/sd_native.h"
#define backend_init pf_sd_native_init
#define backend_config_t pf_sd_config_t
#define backend_stage pf_sd_native_get_init_stage
#define backend_stage_t pf_sd_native_init_stage_t
#define backend_ready pf_sd_native_is_initialized
#define backend_blocks pf_sd_native_block_count
#define backend_read pf_sd_native_read_blocks
#define backend_write pf_sd_native_write_blocks
#define backend_sync pf_sd_native_sync
#define backend_info pf_sd_native_get_info
#define TEST_TRANSPORT PF_STORAGE_TRANSPORT_NATIVE_SD
#define OTHER_TRANSPORT PF_STORAGE_TRANSPORT_SPI
#endif
#endif
