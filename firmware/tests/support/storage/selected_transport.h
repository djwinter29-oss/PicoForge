#ifndef STORAGE_TEST_SELECTED_TRANSPORT_H
#define STORAGE_TEST_SELECTED_TRANSPORT_H
#include "drivers/sdcard/sd_backend.h"

#define backend_init pf_sd_backend_init
#define backend_config_t pf_sd_config_t
#define backend_ready pf_sd_backend_is_initialized
#define backend_blocks pf_sd_backend_block_count
#define backend_read pf_sd_backend_read_blocks
#define backend_write pf_sd_backend_write_blocks
#define backend_sync pf_sd_backend_sync
#define backend_info pf_sd_backend_get_info

#if PF_TEST_SDCARD_TRANSPORT == 1
#define TEST_TRANSPORT PF_SD_LINK_SPI
#define OTHER_TRANSPORT PF_SD_LINK_NATIVE_SD
#else
#define TEST_TRANSPORT PF_SD_LINK_NATIVE_SD
#define OTHER_TRANSPORT PF_SD_LINK_SPI
#endif

pf_sd_link_mode_t pf_sd_backend_mode(void) { return TEST_TRANSPORT; }
#endif
