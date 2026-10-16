#include "unity.h"
#include <string.h>
#include "selected_transport.h"

#define FACADE_SOURCE "../../../src/drivers/sd_card/sd_card.c"
#define PORT_SOURCE "../../../src/drivers/sd_card/ports/baremetal/sd_card_port.c"
#include FACADE_SOURCE
#include PORT_SOURCE

static bool initialized;
static unsigned reads, writes, syncs;
static uint8_t media[1024];
pf_sd_card_result_t backend_init(const backend_config_t *config)
{ (void)config; initialized = true; return PF_SD_CARD_OK; }
bool backend_ready(void) { return initialized; }
uint32_t backend_blocks(void) { return initialized ? 2u : 0u; }
backend_stage_t backend_stage(void) { return (backend_stage_t)0; }
void backend_info(pf_sd_card_info_t *info)
{ *info = (pf_sd_card_info_t){ .initialized = initialized }; }
#if PF_CONFIG_SD_CARD_TRANSPORT != PF_CONFIG_SD_CARD_TRANSPORT_SPI
void pf_sdio_pio_confirm_card_removed(void) { initialized = false; }
#endif
pf_sd_card_result_t backend_read(uint32_t first, void *buffer, uint32_t count)
{
    TEST_ASSERT_TRUE(initialized);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_NOT_NULL(buffer);
    ++reads;
    memcpy(buffer, media, sizeof(media));
    return PF_SD_CARD_OK;
}
pf_sd_card_result_t backend_write(uint32_t first, const void *buffer, uint32_t count)
{
    TEST_ASSERT_TRUE(initialized);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_NOT_NULL(buffer);
    ++writes;
    memcpy(media, buffer, sizeof(media));
    return PF_SD_CARD_OK;
}
pf_sd_card_result_t backend_sync(void) { ++syncs; return PF_SD_CARD_OK; }
void pf_sd_card_get_profile(pf_sd_card_profile_t *profile)
{ *profile = (pf_sd_card_profile_t){0}; }

static void check_baremetal(void)
{
    pf_sd_card_port_init(NULL);
    TEST_ASSERT_TRUE(pf_sd_card_try_lock());
    pf_sd_card_lock();
    TEST_ASSERT_TRUE(pf_sd_card_try_lock());
    pf_sd_card_unlock(); pf_sd_card_unlock(); pf_sd_card_unlock();
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT, pf_sd_card_init(NULL));
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_CONFIGURED, pf_sd_card_read_blocks(0, NULL, 0));
    pf_sd_card_config_t config = { .transport = TEST_TRANSPORT };
    pf_sd_card_port_init(&config);
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_init(&config));
    uint8_t input[1024], output[1024];
    memset(input, 0x5a, sizeof(input));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_write_blocks(0, input, 2));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_sync());
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_read_blocks(0, output, 2));
    TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof(input));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_acquire());
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_card_write_blocks(0, input, 2));
    TEST_ASSERT_EQUAL_UINT(1, writes);
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_write_blocks(0, input, 2));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_read_blocks(0, output, 2));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_sync());
    TEST_ASSERT_EQUAL_UINT(2, reads);
    TEST_ASSERT_EQUAL_UINT(2, writes);
    TEST_ASSERT_EQUAL_UINT(2, syncs);
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_release());
    TEST_ASSERT_TRUE(pf_sd_card_take_reinit_request());
    pf_sd_card_info_t info;
    pf_sd_card_get_info(&info);
    TEST_ASSERT_TRUE(info.initialized);
    TEST_ASSERT_EQUAL_UINT32(2, pf_sd_card_block_count());
}
