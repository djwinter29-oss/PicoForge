#include "unity.h"
void pf_sd_lock(void) {}
void pf_sd_unlock(void) {}
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"
#include "sdio_pio.h"

static pf_sdio_pio_t test_wire;

#include <string.h>

#include "drivers/sdcard/sdio/sdio_pio.c"
#include "drivers/sdcard/sd_link.c"
#include "drivers/sdcard/sdio/sd_sdio_host.c"
#include "drivers/sdcard/sd_protocol.c"
#include "sdio_sdk_stubs.c"

pf_sd_native_host_t *pf_test_sdio_observed_host;
bool pf_test_sdio_expect_streams_cleared;
bool pf_test_sdio_streams_were_cleared;
bool pf_test_sdio_can_add_program;
unsigned pf_test_sdio_removed_programs;

static const uint8_t expected_scr[8] = {0x02u, 0xb5u, 0u, 0u, 0u, 0u, 0u, 0u};

static void make_scr_wire(void)
{
    uint16_t crc = pf_sd_crc16(expected_scr, sizeof(expected_scr));
    memset(pf_test_sdio_rx_wire, 0, sizeof(pf_test_sdio_rx_wire));
    for (unsigned bit_index = 0u; bit_index < 80u; ++bit_index) {
        unsigned bit = bit_index < 64u
            ? (expected_scr[bit_index / 8u] >> (7u - bit_index % 8u)) & 1u
            : (crc >> (79u - bit_index)) & 1u;
        pf_test_sdio_rx_wire[bit_index / 2u] |=
            (uint8_t)((0x0eu | bit) << ((bit_index & 1u) == 0u ? 4u : 0u));
    }
    pf_test_sdio_rx_wire_size = sizeof(pf_test_sdio_rx_wire);
}

void setUp(void)
{
    memset(&pf_test_sdio_dma_registers, 0, sizeof(pf_test_sdio_dma_registers));
    pf_test_sdio_rx_wire_size = 0u;
    pf_test_sdio_data_wait_ok = true;
    pf_test_sdio_command_fail = 0u;
    pf_test_sdio_command_error = 0u;
    pf_test_sdio_corrupt_command = 0u;
    pf_test_sdio_corrupt_command_crc = false;
    pf_test_sdio_pio_restart_calls = 0u;
    pf_test_sdio_command_count = 0u;
}

void tearDown(void) {}

static void set_failure(unsigned failure)
{
    switch (failure) {
    case 1u: pf_test_sdio_command_fail = 55u; break;
    case 2u:
        pf_test_sdio_corrupt_command_crc = true;
        pf_test_sdio_corrupt_command = 55u;
        break;
    case 3u: pf_test_sdio_command_fail = 51u; break;
    case 4u:
        pf_test_sdio_corrupt_command_crc = true;
        pf_test_sdio_corrupt_command = 51u;
        break;
    case 5u: pf_test_sdio_command_error = 55u; break;
    case 6u: pf_test_sdio_command_error = 51u; break;
    case 7u: pf_test_sdio_data_wait_ok = false; break;
    case 8u: pf_test_sdio_rx_wire[39] ^= 1u; break;
    default: break;
    }
}

void test_read_scr_returns_payload_and_reports_prefix_response_data_and_crc_failures(void)
{
    static const pf_sd_native_init_stage_t failure_stages[] = {
        PF_SD_NATIVE_INIT_STAGE_NONE,
        PF_SD_NATIVE_INIT_STAGE_SCR_PREFIX,
        PF_SD_NATIVE_INIT_STAGE_SCR_PREFIX,
        PF_SD_NATIVE_INIT_STAGE_SCR_RESPONSE,
        PF_SD_NATIVE_INIT_STAGE_SCR_RESPONSE,
        PF_SD_NATIVE_INIT_STAGE_SCR_PREFIX,
        PF_SD_NATIVE_INIT_STAGE_SCR_RESPONSE,
        PF_SD_NATIVE_INIT_STAGE_SCR_DATA,
        PF_SD_NATIVE_INIT_STAGE_SCR_CRC
    };
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 2u,
        .current_clock_hz = 25000000u,
        .command_program_loaded = true,
        .dma_claimed = true,
        .data_rx_program_loaded = true,
    };
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire) };
    TEST_ASSERT_TRUE(pf_sd_link_sdio_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t scr[8];

    for (unsigned failure = 0u; failure < sizeof(failure_stages) / sizeof(failure_stages[0]);
         ++failure) {
        memset(scr, 0, sizeof(scr));
        setUp();
        make_scr_wire();
        set_failure(failure);
        TEST_ASSERT_EQUAL(failure == 0u,
                          pf_sd_native_host_read_scr(&host, 0x12340000u, scr));
        TEST_ASSERT_EQUAL(failure_stages[failure], host.scr_stage);
        TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_pio_restart_calls);
        if (failure == 0u) {
            TEST_ASSERT_EQUAL_MEMORY(expected_scr, scr, sizeof(scr));
        }
        if (failure == 8u) {
            TEST_ASSERT_EQUAL_UINT32(1u, host.errors.data_crc_errors);
            TEST_ASSERT_EQUAL(PF_SD_DETAIL_DATA_CRC, host.last_error);
        }
        if (failure == 2u) {
            TEST_ASSERT_EQUAL_UINT32(1u, host.errors.command_crc_errors);
            TEST_ASSERT_EQUAL(PF_SD_DETAIL_RESPONSE_CRC, host.last_error);
        }
    }

    TEST_ASSERT_FALSE(pf_sd_native_host_read_scr(NULL, 0u, scr));
    TEST_ASSERT_FALSE(pf_sd_native_host_read_scr(&host, 0u, NULL));
    TEST_ASSERT_EQUAL(PF_SD_NATIVE_INIT_STAGE_SCR, host.scr_stage);
}
