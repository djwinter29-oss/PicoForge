#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"

#include <stdint.h>
#include <string.h>

#include "../../../../src/drivers/storage/sdio_pio/storage_sdio_pio.c"
#include "../../../../src/drivers/storage/storage_bus.c"
#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio_host.c"
#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c"
#include "../../../../src/drivers/sd_card/sd_card_protocol.c"
#include "sdio_sdk_stubs.c"

_Static_assert(PF_SD_CARD_PROFILE_COMMAND == 0 &&
               PF_SD_CARD_PROFILE_DATA == 1 &&
               PF_SD_CARD_PROFILE_CRC == 2 &&
               PF_SD_CARD_PROFILE_BUSY == 3 &&
               PF_SD_CARD_PROFILE_STOP == 4 &&
               PF_SD_CARD_PROFILE_RECOVERY == 5 &&
               PF_SD_CARD_PROFILE_PHASE_COUNT == 6,
               "profile array indices remain ABI-stable");
_Static_assert(SDIO_DATA_NIBBLE_COUNT == 512u * 2u + 16u &&
               SDIO_DATA_WORD_COUNT * 8u == SDIO_DATA_NIBBLE_COUNT,
               "DMA data words cover payload and CRC framing exactly");

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
    pf_test_sdio_system_clock_hz = 125000000u;
    pf_test_sdio_removed_programs = 0u;
    pf_test_sdio_data_irq = true;
    pf_test_sdio_dma_busy = false;
    pf_test_sdio_dma_busy_delay = 0u;
    pf_test_sdio_data_irq_delay = 0u;
    pf_test_sdio_data_irq_queries = 0u;
    pf_test_sdio_dma_busy_queries = 0u;
    pf_test_sdio_dma_abort_count = 0u;
    pf_test_sdio_force_tx_stall = false;
    pf_test_sdio_config_divider_integer = 0u;
    pf_test_sdio_config_divider_fraction = 0u;
    pf_test_sdio_clkdiv_restart_mask = 0u;
    pf_test_sdio_sync_mask = 0u;
    pf_test_sdio_tx_preload_queries = 0u;
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
        .transport.pio = pio,
        .transport.command_sm = 0u,
        .transport.data_sm = 1u,
        .transport.dma_channel = 3u,
        .transport.dma_claimed = true,
        .transport.command_program_offset = 1u,
        .transport.data_rx_program_offset = 2u,
        .transport.data_tx_program_offset = 3u,
        .transport.clock_program_offset = 4u,
        .transport.clock_div_ticks = 3u * 256u,
        .transport.current_clock_hz = 25000000u,
        .bus_width = 4u,
        .transport.command_program_loaded = true,
        .transport.data_rx_program_loaded = true
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
    TEST_ASSERT_GREATER_THAN_UINT(0u, pf_test_sdio_tx_preload_queries);
    TEST_ASSERT_EQUAL_UINT(DMA_SIZE_32, pf_test_sdio_last_dma_config.size);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.swap);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.read_inc);
    TEST_ASSERT_FALSE(pf_test_sdio_last_dma_config.write_inc);
    check_data_profile(1u, 17u);

    pf_test_sdio_data_response = 0x0bu;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 4u, true, block));
    TEST_ASSERT_FALSE(host.transport.mode_restore_failed);

    pf_test_sdio_data_response = 0x05u;
    pf_test_sdio_data_irq = false;
    pf_test_sdio_dma_busy = true;
    pf_test_sdio_time_step = 500001u;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 4u, true, block));
    TEST_ASSERT_FALSE(host.transport.mode_restore_failed);
    TEST_ASSERT_EQUAL_UINT(1u, pf_test_sdio_dma_abort_count);
    TEST_ASSERT_EQUAL_UINT(1u, host.errors.dma_timeouts);
    TEST_ASSERT_EQUAL(PF_SD_CARD_DETAIL_DMA_TIMEOUT, host.last_error);
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

void test_cmd24_treats_a_pio_tx_fifo_stall_as_fatal(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint8_t block[PF_SD_CARD_BLOCK_SIZE] = {0};

    pf_test_sdio_force_tx_stall = true;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_write_sector(&host, 2u, true, block));
    TEST_ASSERT_EQUAL(PF_SD_CARD_DETAIL_TX_STALL, host.last_error);
    TEST_ASSERT_EQUAL_UINT32(1u, host.errors.tx_stalls);
    TEST_ASSERT_FALSE(host.transport.mode_restore_failed);
    pf_test_sdio_force_tx_stall = false;
}

void test_fast_tx_uses_half_clock_divider_and_synchronized_sm_restart(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint32_t sm_mask = (1u << host.transport.command_sm) | (1u << host.transport.data_sm);

    host.transport.current_clock_hz = 50000000u;
    host.transport.clock_div_ticks = 640u;
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_tx_prepare(&host.transport));
    TEST_ASSERT_TRUE(host.transport.data_tx_fast);
    TEST_ASSERT_EQUAL_UINT16(1u, pf_test_sdio_config_divider_integer);
    TEST_ASSERT_EQUAL_UINT8(64u, pf_test_sdio_config_divider_fraction);

    uint32_t image[PF_SDIO_PIO_TRANSFER_WORDS] = {0};
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_tx_start(&host.transport, image,
                                                PF_SDIO_PIO_TRANSFER_WORDS, SDIO_DATA_NIBBLE_COUNT));
    TEST_ASSERT_EQUAL_UINT32(sm_mask, pf_test_sdio_sync_mask);
    TEST_ASSERT_EQUAL_UINT32(sm_mask, pf_test_sdio_clkdiv_restart_mask);
}

void test_clock_owner_rejects_command_during_standalone_or_tx_clock_ownership(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint32_t response[2];

    host.transport.clock_owner = PF_SDIO_PIO_CLOCK_INITIALIZATION;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_command(&host, 0u, 0u, 48u,
                                               response, 2u));
    TEST_ASSERT_FALSE(pf_sdio_pio_host_set_clock(&host, 25000000u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_DETAIL_HOST_STATE, host.last_error);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);

    host.transport.clock_owner = PF_SDIO_PIO_CLOCK_DATA_TX;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_command(&host, 13u, 0u, 48u,
                                               response, 2u));
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
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
    TEST_ASSERT_TRUE(host.transport.mode_restore_failed);
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

void test_cmd24_profile_can_be_disabled_or_run_without_a_clock(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = make_host(&pio);
    uint8_t block[PF_SD_CARD_BLOCK_SIZE] = {0};
    pf_sd_card_profile_t profile;

    pf_sd_card_profile_set_clock(NULL);
    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_sector(&host, 4u, true, block));
    pf_sd_card_get_profile(&profile);
    TEST_ASSERT_EQUAL_UINT(0u, profile.calls[PF_SD_CARD_PROFILE_DATA]);

    pf_sd_card_profile_set_clock(test_profile_clock);
    pf_sd_card_set_profiling_enabled(false);
    TEST_ASSERT_TRUE(pf_sdio_pio_host_write_sector(&host, 4u, true, block));
    pf_sd_card_get_profile(&profile);
    TEST_ASSERT_EQUAL_UINT(0u, profile.calls[PF_SD_CARD_PROFILE_DATA]);
}
