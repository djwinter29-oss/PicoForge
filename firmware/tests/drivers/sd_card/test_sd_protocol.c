#include "unity.h"
#include "sd_protocol.h"

void setUp(void)
{
    pf_sd_set_profiling_enabled(false);
    pf_sd_profile_set_clock(NULL);
    pf_sd_reset_profile();
}
void tearDown(void) {}

static uint32_t profile_time;

static uint32_t profile_clock(void)
{
    return profile_time;
}

void test_profile_is_optional_and_handles_clock_wrap(void)
{
    pf_sd_profile_t metrics;
    pf_sd_profile_set_clock(profile_clock);
    profile_time = 100u;
    pf_sd_profile_end(PF_SD_PROFILE_DATA, 0u);
    pf_sd_profile_event(PF_SD_PROFILE_TIMEOUT);
    pf_sd_get_profile(&metrics);
    TEST_ASSERT_EQUAL_UINT32(0u, metrics.calls[PF_SD_PROFILE_DATA]);
    TEST_ASSERT_EQUAL_UINT32(0u, metrics.events[PF_SD_PROFILE_TIMEOUT]);

    pf_sd_set_profiling_enabled(true);
    profile_time = UINT32_MAX - 4u;
    uint32_t start = pf_sd_profile_begin();
    profile_time = 5u;
    pf_sd_profile_end(PF_SD_PROFILE_DATA, start);
    pf_sd_profile_event(PF_SD_PROFILE_TIMEOUT);
    pf_sd_get_profile(&metrics);
    TEST_ASSERT_EQUAL_UINT64(10u, metrics.elapsed_us[PF_SD_PROFILE_DATA]);
    TEST_ASSERT_EQUAL_UINT32(1u, metrics.calls[PF_SD_PROFILE_DATA]);
    TEST_ASSERT_EQUAL_UINT32(1u, metrics.events[PF_SD_PROFILE_TIMEOUT]);

    pf_sd_profile_end(PF_SD_PROFILE_PHASE_COUNT, start);
    pf_sd_profile_end((pf_sd_profile_phase_t)-1, start);
    pf_sd_profile_event(PF_SD_PROFILE_EVENT_COUNT);
    pf_sd_profile_event((pf_sd_profile_event_t)-1);
    pf_sd_get_profile(NULL);
    pf_sd_reset_profile();
    pf_sd_get_profile(&metrics);
    TEST_ASSERT_EQUAL_UINT32(0u, metrics.calls[PF_SD_PROFILE_DATA]);
    start = pf_sd_profile_begin();
    profile_time = 8u;
    pf_sd_profile_end(PF_SD_PROFILE_DATA, start);
    pf_sd_get_profile(&metrics);
    TEST_ASSERT_EQUAL_UINT64(3u, metrics.elapsed_us[PF_SD_PROFILE_DATA]);
}

void test_shared_transfer_bounds_and_zero_count(void)
{
    uint8_t buffer[512];

    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_validate_transfer(0u, NULL, 0u, 0u));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_validate_transfer(10u, buffer, 300u, 310u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_validate_transfer(10u, buffer, 301u, 310u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_validate_transfer(0u, NULL, 1u, 1u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT,
                      pf_sd_validate_transfer(UINT32_MAX, buffer, 1u, UINT32_MAX));
}

void test_shared_transfer_byte_count_must_fit_size_t(void)
{
    uint8_t buffer[1];
    uint32_t largest_transfer = (SIZE_MAX / PF_SD_BLOCK_SIZE < UINT32_MAX)
        ? (uint32_t)(SIZE_MAX / PF_SD_BLOCK_SIZE) : UINT32_MAX;

    TEST_ASSERT_EQUAL(PF_SD_OK,
                      pf_sd_validate_transfer(0u, buffer, largest_transfer, UINT32_MAX));
    if (largest_transfer < UINT32_MAX) {
        TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT,
                          pf_sd_validate_transfer(0u, buffer, largest_transfer + 1u,
                                                       UINT32_MAX));
    }
}

void test_shared_serial_crc16_reference(void)
{
    const uint8_t bytes[] = "123456789";

    TEST_ASSERT_EQUAL_HEX16(0x31c3u, pf_sd_crc16(bytes, 9u));
    TEST_ASSERT_EQUAL_HEX16(0u, pf_sd_crc16(NULL, 9u));
}

void test_shared_crc16_matches_bitwise_reference_for_a_sector(void)
{
    uint8_t block[512];
    uint16_t expected = 0u;

    for (size_t index = 0u; index < sizeof(block); index++) {
        block[index] = (uint8_t)(index * 73u + 5u);
        expected ^= (uint16_t)block[index] << 8u;
        for (int bit = 0; bit < 8; bit++) {
            expected = (expected & 0x8000u) ? (uint16_t)((expected << 1u) ^ 0x1021u)
                                            : (uint16_t)(expected << 1u);
        }
    }
    TEST_ASSERT_EQUAL_HEX16(expected, pf_sd_crc16(block, sizeof(block)));
}

static uint32_t idle_calls;

static void count_idle(void)
{
    idle_calls++;
}

void test_idle_hook_runs_only_when_set(void)
{
    idle_calls = 0u;
    pf_sd_idle();
    TEST_ASSERT_EQUAL_UINT32(0u, idle_calls);
    pf_sd_set_idle_hook(count_idle);
    pf_sd_idle();
    TEST_ASSERT_EQUAL_UINT32(1u, idle_calls);
    pf_sd_set_idle_hook(NULL);
    pf_sd_idle();
    TEST_ASSERT_EQUAL_UINT32(1u, idle_calls);
}

void test_shared_command_reference(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = {0x40u, 0u, 0u, 0u, 0u, 0x95u};

    TEST_ASSERT_TRUE(pf_sd_build_command(0u, 0u, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 6u);
    TEST_ASSERT_FALSE(pf_sd_build_command(64u, 0u, frame));
    TEST_ASSERT_FALSE(pf_sd_build_command(0u, 0u, NULL));
}

void test_csd_transfer_speed_decodes_standard_units_and_multipliers(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t transfer_hz = 0u;

    csd[3] = 0x32u;
    TEST_ASSERT_TRUE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_EQUAL_UINT32(25000000u, transfer_hz);
    csd[3] = 0x5au;
    TEST_ASSERT_TRUE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_EQUAL_UINT32(50000000u, transfer_hz);
    csd[3] = 0x7bu;
    TEST_ASSERT_TRUE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_EQUAL_UINT32(800000000u, transfer_hz);
    csd[3] = 0xb2u;
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    csd[3] = 0x0fu;
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(NULL, &transfer_hz));
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(csd, NULL));
}

void test_scr_reports_four_bit_bus_support(void)
{
    uint8_t scr[8] = { 0u };

    TEST_ASSERT_FALSE(pf_sd_scr_supports_four_bit(scr));
    scr[1] = 0x04u;
    TEST_ASSERT_TRUE(pf_sd_scr_supports_four_bit(scr));
    TEST_ASSERT_FALSE(pf_sd_scr_supports_four_bit(NULL));
}