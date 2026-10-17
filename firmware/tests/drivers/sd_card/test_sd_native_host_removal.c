#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"

#include <string.h>

#include "../../../src/drivers/sd_card/sd_native.c"
#include "../../../src/drivers/storage/sdio_pio/storage_sdio_pio.c"
#include "../../../src/drivers/storage/storage_bus.c"
#include "../../../src/drivers/sd_card/sd_native_host.c"
#include "../../../src/drivers/sd_card/sd_native_protocol.c"
#include "../../../src/drivers/sd_card/sd_protocol.c"
#include "sdio_sdk_stubs.c"

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
    pf_test_sdio_removed_programs = 0u;
}

void tearDown(void) {}

void test_forget_removed_clears_stream_state_before_restoring_mode(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = {
        .transport.pio = &pio,
        .transport.command_sm = 0u,
        .transport.data_sm = 1u,
        .transport.clock_program_offset = 2u,
        .transport.data_tx_program_offset = 3u,
        .transport.clock_program_loaded = true,
        .transport.command_program_loaded = true,
        .transport.data_rx_program_loaded = true,
        .transport.data_tx_program_loaded = true,
        .write_stream_open = true,
        .read_stream_unresolved = true,
        .transport.mode_restore_failed = true
    };

    pf_test_sdio_observed_host = &host;
    pf_test_sdio_expect_streams_cleared = true;
    pf_sd_native_host_forget_removed_card(&host);

    TEST_ASSERT_TRUE(pf_test_sdio_streams_were_cleared);
    TEST_ASSERT_FALSE(host.write_stream_open);
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_FALSE(host.transport.mode_restore_failed);
    TEST_ASSERT_TRUE(host.transport.command_program_loaded);
    TEST_ASSERT_TRUE(host.transport.data_rx_program_loaded);
    TEST_ASSERT_FALSE(host.transport.data_tx_program_loaded);
    TEST_ASSERT_FALSE(host.transport.clock_program_loaded);
    TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_removed_programs);
}

void test_forget_removed_deinitializes_when_command_mode_cannot_be_restored(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sd_native_host_t host = {
        .transport.pio = &pio,
        .transport.command_sm = 0u,
        .transport.data_sm = 1u,
        .transport.data_rx_program_offset = 4u,
        .transport.data_rx_program_loaded = true,
        .write_stream_open = true,
        .read_stream_unresolved = true
    };

    pf_test_sdio_can_add_program = false;
    pf_sd_native_host_forget_removed_card(&host);

    TEST_ASSERT_NULL(host.transport.pio);
    TEST_ASSERT_FALSE(host.write_stream_open);
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_FALSE(host.transport.data_rx_program_loaded);
}

void test_forget_removed_ignores_null_and_uninitialized_hosts(void)
{
    pf_sd_native_host_t host = {0};

    pf_sd_native_host_forget_removed_card(NULL);
    pf_sd_native_host_forget_removed_card(&host);

    TEST_ASSERT_NULL(host.transport.pio);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_removed_programs);
}

void test_confirm_card_removed_invalidates_card_before_forgetting_host_streams(void)
{
    pf_test_sdio_pio_t pio = {0};
    sdio_pio_host = (pf_sd_native_host_t){
        .transport.pio = &pio,
        .transport.command_sm = 0u,
        .transport.data_sm = 1u,
        .transport.command_program_loaded = true,
        .transport.data_rx_program_loaded = true,
        .write_stream_open = true,
        .read_stream_unresolved = true
    };
    configuration_set = false;
    card_initialized = true;
    pf_test_sdio_observed_host = &sdio_pio_host;
    pf_test_sdio_expect_streams_cleared = true;

    pf_sd_native_confirm_card_removed();

    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_TRUE(pf_test_sdio_streams_were_cleared);
    TEST_ASSERT_FALSE(sdio_pio_host.write_stream_open);
    TEST_ASSERT_FALSE(sdio_pio_host.read_stream_unresolved);
}
