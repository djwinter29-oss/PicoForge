#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"

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
    pf_sdio_pio_host_t host = {
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .clock_program_offset = 2u,
        .data_tx_program_offset = 3u,
        .clock_program_loaded = true,
        .command_program_loaded = true,
        .data_rx_program_loaded = true,
        .data_tx_program_loaded = true,
        .write_stream_open = true,
        .read_stream_unresolved = true,
        .mode_restore_failed = true
    };

    pf_test_sdio_observed_host = &host;
    pf_test_sdio_expect_streams_cleared = true;
    pf_sdio_pio_host_forget_removed_card(&host);

    TEST_ASSERT_TRUE(pf_test_sdio_streams_were_cleared);
    TEST_ASSERT_FALSE(host.write_stream_open);
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_FALSE(host.mode_restore_failed);
    TEST_ASSERT_TRUE(host.command_program_loaded);
    TEST_ASSERT_TRUE(host.data_rx_program_loaded);
    TEST_ASSERT_FALSE(host.data_tx_program_loaded);
    TEST_ASSERT_FALSE(host.clock_program_loaded);
    TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_removed_programs);
}

void test_forget_removed_deinitializes_when_command_mode_cannot_be_restored(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = {
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .data_rx_program_offset = 4u,
        .data_rx_program_loaded = true,
        .write_stream_open = true,
        .read_stream_unresolved = true
    };

    pf_test_sdio_can_add_program = false;
    pf_sdio_pio_host_forget_removed_card(&host);

    TEST_ASSERT_NULL(host.pio);
    TEST_ASSERT_FALSE(host.write_stream_open);
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_FALSE(host.data_rx_program_loaded);
}

void test_forget_removed_ignores_null_and_uninitialized_hosts(void)
{
    pf_sdio_pio_host_t host = {0};

    pf_sdio_pio_host_forget_removed_card(NULL);
    pf_sdio_pio_host_forget_removed_card(&host);

    TEST_ASSERT_NULL(host.pio);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_removed_programs);
}
