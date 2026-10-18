#include "unity.h"
#include <string.h>
#include "selected_transport.h"

#define DRIVER_SOURCE "drivers/sd_card/sd.c"
#define PORT_SOURCE "drivers/sd_card/sd_port_baremetal.c"
#include DRIVER_SOURCE
#define MEDIA_SOURCE "app/storage/sd_media.c"
#include MEDIA_SOURCE
#include PORT_SOURCE
bool pf_fatfs_volume_is_mounted(void) { return false; }
static pf_sd_result_t initialize_media(const pf_sd_config_t *config)
{
    configured = true;
    media_config = *config;
    return pf_sd_media_initialize();
}

static bool initialized;
static unsigned reads, writes, syncs;
static uint8_t media[1024];
pf_sd_result_t backend_init(const backend_config_t *config)
{ (void)config; initialized = true; return PF_SD_OK; }
bool backend_ready(void) { return initialized; }
uint32_t backend_blocks(void) { return initialized ? 2u : 0u; }
backend_stage_t backend_stage(void) { return (backend_stage_t)0; }
void backend_info(pf_sd_info_t *info)
{ *info = (pf_sd_info_t){ .initialized = initialized }; }
#if PF_CONFIG_SD_CARD_TRANSPORT != PF_CONFIG_SD_CARD_TRANSPORT_SPI
void pf_sd_native_confirm_card_removed(void) { initialized = false; }
#endif
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
void pf_sd_get_profile(pf_sd_profile_t *profile)
{ *profile = (pf_sd_profile_t){0}; }

static void check_baremetal(void)
{
    TEST_ASSERT_TRUE(pf_sd_port_init(NULL));
    TEST_ASSERT_TRUE(pf_sd_try_lock());
    pf_sd_lock();
    TEST_ASSERT_TRUE(pf_sd_try_lock());
    pf_sd_unlock(); pf_sd_unlock(); pf_sd_unlock();
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_init(NULL));
    TEST_ASSERT_EQUAL(PF_SD_NOT_CONFIGURED, pf_sd_media_read_blocks(0, NULL, 0));
    pf_sd_config_t config = { .transport.mode = TEST_TRANSPORT };
    TEST_ASSERT_TRUE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL(PF_SD_OK, initialize_media(&config));
    uint8_t input[1024], output[1024];
    memset(input, 0x5a, sizeof(input));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_write_blocks(0, input, 2));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_sync());
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_read_blocks(0, output, 2));
    TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof(input));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_acquire_usb());
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_write_blocks(0, input, 2));
    TEST_ASSERT_EQUAL_UINT(1, writes);
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_usb_write_blocks(0, input, 2));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_usb_read_blocks(0, output, 2));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_usb_sync());
    TEST_ASSERT_EQUAL_UINT(2, reads);
    TEST_ASSERT_EQUAL_UINT(2, writes);
    TEST_ASSERT_EQUAL_UINT(2, syncs);
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_release_usb());
    TEST_ASSERT_TRUE(pf_sd_media_take_reinit_request());
    pf_sd_info_t info;
    pf_sd_get_info(&info);
    TEST_ASSERT_TRUE(info.initialized);
    TEST_ASSERT_EQUAL_UINT32(2, pf_sd_block_count());
}
