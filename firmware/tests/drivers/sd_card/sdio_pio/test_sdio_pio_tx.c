#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"

#include <stdint.h>
#include <string.h>

#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio_host.c"
#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c"
#include "../../../../src/drivers/sd_card/sd_card_protocol.c"
#include "sdio_sdk_stubs.c"

pf_sdio_pio_host_t *pf_test_sdio_observed_host;
bool pf_test_sdio_expect_streams_cleared;
bool pf_test_sdio_streams_were_cleared;
bool pf_test_sdio_can_add_program;
unsigned pf_test_sdio_removed_programs;

static uint32_t profile_now;

static uint32_t test_profile_clock(void)
{
    profile_now += 17u;
    return profile_now;
}

void setUp(void)
{
    pf_test_sdio_observed_host = NULL;
    pf_test_sdio_expect_streams_cleared = false;
    pf_test_sdio_streams_were_cleared = false;
    pf_test_sdio_can_add_program = true;
    pf_test_sdio_removed_programs = 0u;
    pf_test_sdio_data_irq = true;
    pf_test_sdio_dma_busy = false;
    pf_test_sdio_dma_busy_delay = 0u;
    pf_test_sdio_data_irq_delay = 0u;
    pf_test_sdio_data_irq_queries = 0u;
    pf_test_sdio_dma_busy_queries = 0u;
    pf_test_sdio_corrupt_command_crc = false;
    pf_test_sdio_command_fail = 0u;
    pf_test_sdio_command_error = 0u;
    pf_test_sdio_corrupt_command = 0u;
    pf_test_sdio_data_fifo_words = 1u;
    pf_test_sdio_data_response = 0x05u;
    pf_test_sdio_time_step = 1u;
    pf_test_sdio_command_count = 0u;
    pf_test_sdio_last_command = 0u;
    pf_test_sdio_dma_word_count = 0u;
    memset(&pf_test_sdio_last_dma_config, 0, sizeof(pf_test_sdio_last_dma_config));
    pf_sd_card_set_profiling_enabled(true);
    pf_sd_card_profile_set_clock(test_profile_clock);
    pf_sd_card_reset_profile();
    profile_now = 0u;
}

void tearDown(void)
{
    pf_sd_card_profile_set_clock(NULL);
    pf_sd_card_set_profiling_enabled(false);
}

static pf_sdio_pio_host_t make_host(pf_test_sdio_pio_t *pio)
{
    pf_sdio_pio_host_t host = {
        .pio = pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .command_program_offset = 1u,
        .data_rx_program_offset = 2u,
        .data_tx_program_offset = 3u,
        .clock_program_offset = 4u,
        .clock_div_ticks = 3u * 256u,
        .current_clock_hz = 25000000u,
        .command_program_loaded = true,
        .data_rx_program_loaded = true
    };
    return host;
}

static void check_data_profile(unsigned calls, uint32_t elapsed_us)
{
    pf_sd_card_profile_t profile;
    pf_sd_card_get_profile(&profile);
    TEST_ASSERT_EQUAL_UINT(calls, profile.calls[PF_SD_CARD_PROFILE_DATA]);
    TEST_ASSERT_EQUAL_UINT32(elapsed_us, profile.elapsed_us[PF_SD_CARD_PROFILE_DATA]);
}

void test_cmd24_uses_full_source_dma_path_and_checks_data_completion(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint8_t block[PF_SD_CARD_BLOCK_SIZE];
    pf_sd_card_profile_t profile;

    for (unsigned i = 0u; i < sizeof(block); ++i) {
        block[i] = (uint8_t)(i * 73u);
    }

    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_sector(&host, 4u, true, block));
    TEST_ASSERT_EQUAL_UINT8(24u, pf_test_sdio_last_command);
    TEST_ASSERT_EQUAL_UINT(PF_SDIO_PIO_TRANSFER_WORDS, pf_test_sdio_dma_word_count);
    TEST_ASSERT_EQUAL_UINT(DMA_SIZE_32, pf_test_sdio_last_dma_config.size);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.swap);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.read_inc);
    TEST_ASSERT_FALSE(pf_test_sdio_last_dma_config.write_inc);
    check_data_profile(1u, 17u);

    pf_test_sdio_data_response = 0x0bu;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 4u, true, block));
    TEST_ASSERT_FALSE(host.mode_restore_failed);

    pf_test_sdio_data_response = 0x05u;
    pf_test_sdio_data_irq = false;
    pf_test_sdio_dma_busy = true;
    pf_test_sdio_time_step = 500001u;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 4u, true, block));
    TEST_ASSERT_FALSE(host.mode_restore_failed);
    pf_sd_card_get_profile(&profile);
    TEST_ASSERT_TRUE(profile.events[PF_SD_CARD_PROFILE_TIMEOUT] > 0u);
}

void test_cmd25_batches_and_continues_without_reissuing_command(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint8_t blocks[4u * PF_SD_CARD_BLOCK_SIZE] = {0};
    unsigned command_count;
    pf_sd_card_profile_t profile;

    pf_sd_card_reset_profile();
    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_blocks(&host, 4u, true, blocks, 3u));
    TEST_ASSERT_EQUAL_UINT8(25u, pf_test_sdio_last_command);
    TEST_ASSERT_TRUE(host.write_stream_open);
    TEST_ASSERT_EQUAL_UINT32(7u, host.write_stream_next_sector);
    pf_sd_card_get_profile(&profile);
    TEST_ASSERT_EQUAL_UINT(3u, profile.calls[PF_SD_CARD_PROFILE_DATA]);
    TEST_ASSERT_EQUAL_UINT32(119u, profile.elapsed_us[PF_SD_CARD_PROFILE_DATA]);
    TEST_ASSERT_EQUAL_UINT(3u, profile.calls[PF_SD_CARD_PROFILE_CRC]);
    TEST_ASSERT_EQUAL_UINT32(51u, profile.elapsed_us[PF_SD_CARD_PROFILE_CRC]);

    command_count = pf_test_sdio_command_count;
    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_stream_continues(&host, 7u));
    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_blocks(&host, 7u, true, blocks, 1u));
    TEST_ASSERT_EQUAL_UINT(command_count, pf_test_sdio_command_count);
    TEST_ASSERT_EQUAL_UINT32(8u, host.write_stream_next_sector);
}

void test_cmd24_waits_for_both_dma_and_pio_completion_independently(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint8_t block[PF_SD_CARD_BLOCK_SIZE] = {0};

    pf_test_sdio_dma_busy_delay = 1u;
    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_sector(&host, 2u, true, block));
    TEST_ASSERT_TRUE(pf_test_sdio_dma_busy_queries >= 2u);

    pf_test_sdio_data_irq_queries = 0u;
    pf_test_sdio_dma_busy_queries = 0u;
    pf_test_sdio_data_irq_delay = 1u;
    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_sector(&host, 3u, true, block));
    TEST_ASSERT_TRUE(pf_test_sdio_data_irq_queries >= 2u);
    TEST_ASSERT_TRUE(pf_test_sdio_dma_busy_queries > 0u);
}

void test_cmd24_command_and_program_failures_do_not_report_data_success(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint8_t block[PF_SD_CARD_BLOCK_SIZE] = {0};
    pf_sd_card_profile_t profile;

    pf_test_sdio_command_error = 24u;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 2u, true, block));
    check_data_profile(0u, 0u);

    pf_test_sdio_command_error = 0u;
    pf_test_sdio_command_fail = 24u;
    pf_test_sdio_time_step = 500001u;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 2u, true, block));
    pf_sd_card_get_profile(&profile);
    TEST_ASSERT_EQUAL_UINT(0u, profile.calls[PF_SD_CARD_PROFILE_DATA]);
    TEST_ASSERT_TRUE(profile.events[PF_SD_CARD_PROFILE_TIMEOUT] > 0u);

    pf_test_sdio_command_fail = 0u;
    pf_test_sdio_time_step = 1u;
    pf_test_sdio_can_add_program = false;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 2u, true, block));
    TEST_ASSERT_TRUE(host.mode_restore_failed);
}

void test_cmd25_failed_data_keeps_stream_until_stop_recovery_succeeds(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint8_t blocks[2u * PF_SD_CARD_BLOCK_SIZE] = {0};

    pf_test_sdio_data_response = 0x0bu;
    pf_test_sdio_command_error = 12u;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_blocks(&host, 5u, true, blocks, 2u));
    TEST_ASSERT_TRUE(host.write_stream_open);
    TEST_ASSERT_TRUE(host.write_stream_next_sector == 5u);

    pf_test_sdio_command_error = 0u;
    TEST_ASSERT_TRUE(pf_sdio_pio_host_recover(&host));
    TEST_ASSERT_FALSE(host.write_stream_open);
}
