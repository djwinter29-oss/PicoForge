#include "unity.h"
#include <string.h>
#include "selected_transport.h"

#define DRIVER_SOURCE "drivers/sdcard/sd.c"
#define PORT_SOURCE "drivers/sdcard/sd_port_baremetal.c"
#include DRIVER_SOURCE
#define MEDIA_SOURCE "platform/storage/storage.c"
#include MEDIA_SOURCE
#include PORT_SOURCE
bool pf_fatfs_volume_is_mounted(void) { return false; }
static pf_sd_result_t bind_and_initialize(const pf_sd_config_t *config)
{
    configured = true;
    media_config = *config;
    return initialize_media();
}
pf_sd_link_mode_t pf_sd_board_pins(uint8_t bus_gpios[6], uint8_t *detect_gpio)
{ (void)bus_gpios; (void)detect_gpio; return PF_SD_LINK_NATIVE_SD; }

static bool initialized;
static pf_sd_result_t initialization_result = PF_SD_OK;
static unsigned reads, writes, syncs;
static uint8_t media[1024];
pf_sd_result_t backend_init(const backend_config_t *config)
{ (void)config; initialized = initialization_result == PF_SD_OK; return initialization_result; }
bool backend_ready(void) { return initialized; }
uint32_t backend_blocks(void) { return initialized ? 2u : 0u; }
void backend_info(pf_sd_info_t *info)
{ *info = (pf_sd_info_t){ .initialized = initialized }; }
void pf_sd_backend_confirm_removed(void) { initialized = false; }
pf_sd_result_t backend_read(uint32_t first, void *buffer, uint32_t count)
{
    TEST_ASSERT_TRUE(initialized);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_NOT_NULL(buffer);
    ++reads;
    memcpy(buffer, media, sizeof(media));
    return PF_SD_OK;
}
pf_sd_result_t backend_write(uint32_t first, const void *buffer, uint32_t count)
{
    TEST_ASSERT_TRUE(initialized);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_NOT_NULL(buffer);
    ++writes;
    memcpy(media, buffer, sizeof(media));
    return PF_SD_OK;
}
pf_sd_result_t backend_sync(void) { ++syncs; return PF_SD_OK; }

static void check_baremetal(void)
{
    TEST_ASSERT_TRUE(pf_sd_port_init(NULL));
    TEST_ASSERT_TRUE(pf_sd_try_lock());
    pf_sd_lock();
    TEST_ASSERT_TRUE(pf_sd_try_lock());
    pf_sd_unlock(); pf_sd_unlock(); pf_sd_unlock();
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_init(NULL));
    TEST_ASSERT_FALSE(backend_configured);
    TEST_ASSERT_EQUAL(PF_SD_NOT_CONFIGURED, read_blocks_for_owner(false, 0, NULL, 0));
    pf_sd_config_t config = { .transport.mode = TEST_TRANSPORT };
    TEST_ASSERT_TRUE(pf_sd_port_init(&config));
    initialization_result = PF_SD_INIT_FAILED;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, bind_and_initialize(&config));
    TEST_ASSERT_TRUE(backend_configured);
    TEST_ASSERT_FALSE(pf_sd_is_initialized());
    TEST_ASSERT_EQUAL_UINT32(0, pf_sd_block_count());
    initialization_result = PF_SD_OK;
    TEST_ASSERT_EQUAL(PF_SD_OK, bind_and_initialize(&config));
    uint8_t input[1024], output[1024];
    memset(input, 0x5a, sizeof(input));
    TEST_ASSERT_EQUAL(PF_SD_OK, write_blocks_for_owner(false, 0, input, 2));
    TEST_ASSERT_EQUAL(PF_SD_OK, sync_for_owner(false));
    TEST_ASSERT_EQUAL(PF_SD_OK, read_blocks_for_owner(false, 0, output, 2));
    TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof(input));
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_acquire());
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, write_blocks_for_owner(false, 0, input, 2));
    TEST_ASSERT_EQUAL_UINT(1, writes);
    TEST_ASSERT_EQUAL(PF_SD_OK, write_blocks_for_owner(true, 0, input, 2));
    TEST_ASSERT_EQUAL(PF_SD_OK, read_blocks_for_owner(true, 0, output, 2));
    TEST_ASSERT_EQUAL(PF_SD_OK, sync_for_owner(true));
    TEST_ASSERT_EQUAL_UINT(2, reads);
    TEST_ASSERT_EQUAL_UINT(2, writes);
    TEST_ASSERT_EQUAL_UINT(2, syncs);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_release());
    TEST_ASSERT_TRUE(reinit_after_usb_owner);
    pf_sd_info_t info;
    pf_sd_get_info(&info);
    TEST_ASSERT_TRUE(info.initialized);
    TEST_ASSERT_EQUAL_UINT32(2, pf_sd_block_count());
}
