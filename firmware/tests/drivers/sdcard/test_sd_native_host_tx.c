#include "unity.h"
void pf_sd_lock(void) {}
void pf_sd_unlock(void) {}
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"
#include "sdio_pio.h"

static pf_sdio_pio_t test_wire;

#include <stdint.h>
#include <string.h>

#include "drivers/sdcard/sdio/sdio_pio.c"
#include "drivers/sdcard/sd_link.c"
#include "drivers/sdcard/sdio/sd_sdio_host.c"
#include "drivers/sdcard/sd_protocol.c"
#include "sdio_sdk_stubs.c"

_Static_assert(SDIO_DATA_NIBBLE_COUNT == 512u * 2u + 16u &&
               SDIO_DATA_WORD_COUNT * 8u == SDIO_DATA_NIBBLE_COUNT,
               "DMA data words cover payload and CRC framing exactly");

pf_sd_native_host_t *pf_test_sdio_observed_host;
bool pf_test_sdio_expect_streams_cleared;
bool pf_test_sdio_streams_were_cleared;
bool pf_test_sdio_can_add_program;
unsigned pf_test_sdio_removed_programs;

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
}

void tearDown(void) {}

static pf_sd_native_host_t make_host(pf_test_sdio_pio_t *pio)
{
    test_wire = (pf_sdio_pio_t){
        .pio = pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .dma_claimed = true,
        .command_program_offset = 1u,
        .data_rx_program_offset = 2u,
        .data_tx_program_offset = 3u,
        .clock_program_offset = 4u,
        .clock_div_ticks = 3u * 256u,
        .current_clock_hz = 25000000u,
        .command_program_loaded = true,
        .data_rx_program_loaded = true,
    };
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire), .bus_width = 4u };
    return host;
}

void test_host_init_binds_callbacks_once_and_preserves_them_until_deinit(void)
{
    test_wire = (pf_sdio_pio_t){
        .config = {.pio = pio0, .pins = {.clock_gpio = 0u, .command_gpio = 1u,
                                       .data_gpio = {2u, 3u, 4u, 5u}}},
    };
    pf_sd_config_t config = {.transport = pf_sdio_pio_bus(&test_wire)};
    pf_sd_native_host_t host = {0};
    TEST_ASSERT_TRUE(pf_sd_native_host_init(&host, &config));
    TEST_ASSERT_EQUAL_PTR(&host, test_wire.hook_context);
    TEST_ASSERT_TRUE(test_wire.error_hook == transport_error);
    TEST_ASSERT_TRUE(test_wire.idle_hook == pf_sd_idle);
    TEST_ASSERT_EQUAL_UINT8(1u, host.bus_width);
    TEST_ASSERT_FALSE(pf_sd_native_host_init(&host, &config));
    TEST_ASSERT_TRUE(pf_sd_native_host_set_clock(&host, 25000000u));
    uint8_t response[6];
    TEST_ASSERT_TRUE(pf_sd_native_host_command_bytes(&host, 13u, 0u, 48u, response, sizeof(response)));
    TEST_ASSERT_EQUAL_UINT(2u, host.last_response_word_count);
    TEST_ASSERT_EQUAL_HEX32(((uint32_t)response[0] << 24u) | ((uint32_t)response[1] << 16u) |
                          ((uint32_t)response[2] << 8u) | response[3], host.last_response_words[0]);
    TEST_ASSERT_EQUAL_HEX32(((uint32_t)response[4] << 8u) | response[5], host.last_response_words[1]);
    pf_test_sdio_command_fail = 13u;
    pf_test_sdio_time_step = 100001u;
    TEST_ASSERT_FALSE(pf_sd_native_host_command_bytes(&host, 13u, 0u, 48u, response, sizeof(response)));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_PIO_TIMEOUT, host.last_error);
    TEST_ASSERT_EQUAL_UINT32(1u, host.errors.pio_timeouts);
    pf_sd_native_host_abort(&host);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_PIO_TIMEOUT, host.last_error);
    TEST_ASSERT_EQUAL_PTR(&host, test_wire.hook_context);
    pf_sd_native_host_deinit(&host);
    TEST_ASSERT_NULL(test_wire.hook_context);
    TEST_ASSERT_NULL(test_wire.error_hook);
}

void test_cmd24_uses_full_source_dma_path_and_checks_data_completion(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t block[PF_SD_BLOCK_SIZE];

    for (unsigned i = 0u; i < sizeof(block); ++i) {
        block[i] = (uint8_t)(i * 73u);
    }

    TEST_ASSERT_TRUE(pf_sd_native_host_write_sector(&host, 4u, block));
    TEST_ASSERT_EQUAL_UINT8(24u, pf_test_sdio_last_command);
    TEST_ASSERT_EQUAL_UINT(PF_SD_NATIVE_TRANSFER_WORDS, pf_test_sdio_dma_word_count);
    TEST_ASSERT_GREATER_THAN_UINT(0u, pf_test_sdio_tx_preload_queries);
    TEST_ASSERT_EQUAL_UINT(DMA_SIZE_32, pf_test_sdio_last_dma_config.size);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.swap);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.read_inc);
    TEST_ASSERT_FALSE(pf_test_sdio_last_dma_config.write_inc);

    pf_test_sdio_data_response = 0x0bu;
    TEST_ASSERT_FALSE(pf_sd_native_host_write_sector(&host, 4u, block));
    TEST_ASSERT_FALSE(test_wire.mode_restore_failed);

    pf_test_sdio_data_response = 0x05u;
    pf_test_sdio_data_irq = false;
    pf_test_sdio_dma_busy = true;
    pf_test_sdio_time_step = 500001u;
    TEST_ASSERT_FALSE(pf_sd_native_host_write_sector(&host, 4u, block));
    TEST_ASSERT_FALSE(test_wire.mode_restore_failed);
    TEST_ASSERT_EQUAL_UINT(1u, pf_test_sdio_dma_abort_count);
    TEST_ASSERT_EQUAL_UINT(1u, host.errors.dma_timeouts);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_DMA_TIMEOUT, host.last_error);
}

void test_cmd25_batches_and_continues_without_reissuing_command(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t blocks[4u * PF_SD_BLOCK_SIZE] = {0};
    unsigned command_count;

    TEST_ASSERT_TRUE(pf_sd_native_host_write_blocks(&host, 4u, blocks, 3u));
    TEST_ASSERT_EQUAL_UINT8(25u, pf_test_sdio_last_command);
    TEST_ASSERT_TRUE(host.write_stream_open);
    TEST_ASSERT_EQUAL_UINT32(7u, host.write_stream_next_sector);

    command_count = pf_test_sdio_command_count;
    TEST_ASSERT_TRUE(pf_sd_native_host_write_stream_continues(&host, 7u));
    TEST_ASSERT_TRUE(pf_sd_native_host_write_blocks(&host, 7u, blocks, 1u));
    TEST_ASSERT_EQUAL_UINT(command_count, pf_test_sdio_command_count);
    TEST_ASSERT_EQUAL_UINT32(8u, host.write_stream_next_sector);
}

void test_cmd24_waits_for_both_dma_and_pio_completion_independently(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t block[PF_SD_BLOCK_SIZE] = {0};

    pf_test_sdio_dma_busy_delay = 1u;
    TEST_ASSERT_TRUE(pf_sd_native_host_write_sector(&host, 2u, block));
    TEST_ASSERT_TRUE(pf_test_sdio_dma_busy_queries >= 2u);

    pf_test_sdio_data_irq_queries = 0u;
    pf_test_sdio_dma_busy_queries = 0u;
    pf_test_sdio_data_irq_delay = 1u;
    TEST_ASSERT_TRUE(pf_sd_native_host_write_sector(&host, 3u, block));
    TEST_ASSERT_TRUE(pf_test_sdio_data_irq_queries >= 2u);
    TEST_ASSERT_TRUE(pf_test_sdio_dma_busy_queries > 0u);
}

void test_cmd24_treats_a_pio_tx_fifo_stall_as_fatal(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t block[PF_SD_BLOCK_SIZE] = {0};

    pf_test_sdio_force_tx_stall = true;
    TEST_ASSERT_FALSE(pf_sd_native_host_write_sector(&host, 2u, block));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_TX_STALL, host.last_error);
    TEST_ASSERT_EQUAL_UINT32(1u, host.errors.tx_stalls);
    TEST_ASSERT_FALSE(test_wire.mode_restore_failed);
    pf_test_sdio_force_tx_stall = false;
}

void test_fast_tx_uses_half_clock_divider_and_synchronized_sm_restart(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint32_t sm_mask = (1u << test_wire.command_sm) | (1u << test_wire.data_sm);

    test_wire.system_clock_hz = 250000000u;
    TEST_ASSERT_TRUE(pf_sdio_pio_set_clock(&test_wire, 50000000u));
    TEST_ASSERT_EQUAL_UINT32(50000000u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(640u, test_wire.clock_div_ticks);
    TEST_ASSERT_TRUE(pf_sdio_pio_tx_prepare(&test_wire));
    TEST_ASSERT_TRUE(test_wire.data_tx_fast);
    TEST_ASSERT_EQUAL_UINT16(1u, pf_test_sdio_config_divider_integer);
    TEST_ASSERT_EQUAL_UINT8(64u, pf_test_sdio_config_divider_fraction);

    uint32_t image[PF_SD_NATIVE_TRANSFER_WORDS] = {0};
    TEST_ASSERT_TRUE(pf_sdio_pio_tx_start(&test_wire, image,
                                                PF_SD_NATIVE_TRANSFER_WORDS, SDIO_DATA_NIBBLE_COUNT));
    TEST_ASSERT_EQUAL_UINT32(sm_mask, pf_test_sdio_sync_mask);
    TEST_ASSERT_EQUAL_UINT32(sm_mask, pf_test_sdio_clkdiv_restart_mask);
}

void test_clock_owner_rejects_command_during_standalone_or_tx_clock_ownership(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t response[6];

    test_wire.clock_owner = PF_SDIO_PIO_CLOCK_INITIALIZATION;
    TEST_ASSERT_FALSE(pf_sd_native_host_command_bytes(&host, 0u, 0u, 48u,
                                               response, sizeof(response)));
    TEST_ASSERT_FALSE(pf_sd_native_host_set_clock(&host, 25000000u));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_HOST_STATE, host.last_error);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);

    test_wire.clock_owner = PF_SDIO_PIO_CLOCK_DATA_TX;
    TEST_ASSERT_FALSE(pf_sd_native_host_command_bytes(&host, 13u, 0u, 48u,
                                               response, sizeof(response)));
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
}

void test_cmd24_command_and_program_failures_do_not_report_data_success(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t block[PF_SD_BLOCK_SIZE] = {0};

    pf_test_sdio_command_error = 24u;
    TEST_ASSERT_FALSE(pf_sd_native_host_write_sector(&host, 2u, block));

    pf_test_sdio_command_error = 0u;
    pf_test_sdio_command_fail = 24u;
    pf_test_sdio_time_step = 500001u;
    TEST_ASSERT_FALSE(pf_sd_native_host_write_sector(&host, 2u, block));
    TEST_ASSERT_GREATER_THAN_UINT(0u, host.errors.command_timeouts);

    pf_test_sdio_command_fail = 0u;
    pf_test_sdio_time_step = 1u;
    pf_test_sdio_can_add_program = false;
    TEST_ASSERT_FALSE(pf_sd_native_host_write_sector(&host, 2u, block));
    TEST_ASSERT_TRUE(test_wire.mode_restore_failed);
}

void test_cmd25_failed_data_keeps_stream_until_stop_recovery_succeeds(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = make_host(&pio);
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t blocks[2u * PF_SD_BLOCK_SIZE] = {0};

    pf_test_sdio_data_response = 0x0bu;
    pf_test_sdio_command_error = 12u;
    TEST_ASSERT_FALSE(pf_sd_native_host_write_blocks(&host, 5u, blocks, 2u));
    TEST_ASSERT_TRUE(host.write_stream_open);
    TEST_ASSERT_TRUE(host.write_stream_next_sector == 5u);

    pf_test_sdio_command_error = 0u;
    TEST_ASSERT_TRUE(pf_sd_native_host_recover(&host));
    TEST_ASSERT_FALSE(host.write_stream_open);
}
