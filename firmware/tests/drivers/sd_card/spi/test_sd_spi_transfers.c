#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/spi")
#define PF_SPI_PRODUCTION_TEST
#include "spi_production_fixture.h"
#include "sd_card_protocol.h"
#include "sd_spi_protocol.h"

spi_inst_t test_spi;
bool spi_test_present;
static uint8_t queue[64];
static unsigned position, length, writes, stops, exchanges, reads;
static bool busy, selected, fail_trailing, short_payload;
static int short_size, short_exchange;
static uint32_t now, clock_step;
static uint8_t payload[512];
static unsigned crc_calls;

uint32_t time_us_32(void) { now += clock_step; return now; }
static uint32_t profile_clock(void) { return now; }
static void idle(void) { now += 25000u; }
static uint16_t spi_fixture_crc16(const uint8_t *data, size_t size)
{
    ++crc_calls;
    return pf_sd_card_crc16(data, size);
}
void gpio_put(unsigned pin, bool high) { (void)pin; selected = !high; }
int spi_write_blocking(spi_inst_t *spi, const uint8_t *data, size_t size)
{
    (void)spi; (void)data;
    ++writes;
    return (int)size == short_size ? 0 : (int)size;
}
int spi_read_blocking(spi_inst_t *spi, uint8_t value, uint8_t *data, size_t size)
{
    (void)spi; (void)value;
    ++reads;
    memcpy(data, payload, size);
    now += 100u;
    return short_payload ? (int)size - 1 : (int)size;
}
int spi_write_read_blocking(spi_inst_t *spi, const uint8_t *data, uint8_t *response, size_t size)
{
    (void)spi;
    TEST_ASSERT_EQUAL_UINT(1, size);
    now += 10u;
    if ((int)exchanges++ == short_exchange || (!selected && fail_trailing)) return 0;
    if (*data == SD_DATA_STOP_MULTIPLE) ++stops;
    *response = position < length ? queue[position++] : busy ? 0 : 0xff;
    return 1;
}

static void reset(uint8_t response)
{
    card_initialized = card_block_addressed = true;
    card_sector_count = 128;
    active_config = (pf_sd_spi_config_t){ .spi = &test_spi };
    position = 0; length = 5;
    queue[0] = 0xff; queue[1] = 0; queue[2] = queue[3] = 0xff; queue[4] = response;
    busy = selected = fail_trailing = short_payload = false;
    spi_test_present = true;
    short_size = short_exchange = -1;
    writes = stops = exchanges = reads = now = 0;
    crc_calls = 0;
    clock_step = 25000u;
    memset(payload, 0, sizeof(payload));
    pf_sd_card_set_profiling_enabled(false);
    pf_sd_card_reset_profile();
    pf_sd_card_profile_set_clock(profile_clock);
    pf_sd_card_set_idle_hook(idle);
}

void setUp(void) { reset(5); }
void tearDown(void)
{
    pf_sd_card_set_profiling_enabled(false);
    pf_sd_card_profile_set_clock(NULL);
    pf_sd_card_set_idle_hook(NULL);
}

void test_spi_writes_accept_success_and_explicit_rejection_without_replay(void)
{
    uint8_t data[1024] = {0};
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_spi_write_blocks(0, data, 1));
    TEST_ASSERT_TRUE(card_initialized && !selected);
    for (unsigned count = 1; count <= 2; ++count) {
        for (unsigned i = 0; i < 2; ++i) {
            reset(i ? 0x0d : 0x0b);
            TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_TRUE(card_initialized && !selected);
            TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_spi_sync());
            TEST_ASSERT_EQUAL_UINT(count == 2, stops);
        }
    }
}

void test_spi_uncertain_write_failures_invalidate_card(void)
{
    uint8_t data[1024] = {0};
    for (unsigned count = 1; count <= 2; ++count) {
        const uint8_t responses[] = {5, 0x09, 0x0b, 0xff, 5};
        for (unsigned failure = 0; failure < sizeof(responses); ++failure) {
            reset(responses[failure]);
            busy = failure == 0 || failure == 2;
            if (failure == 4) short_size = 512;
            TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_FALSE(card_initialized || selected);
            unsigned before = writes;
            TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_spi_sync());
            TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_spi_write_blocks(0, data, 1));
            TEST_ASSERT_EQUAL_UINT(before, writes);
        }
    }
}

void test_spi_short_command_crc_response_and_busy_exchanges_are_not_replayed(void)
{
    uint8_t data[1024] = {0};
    for (unsigned count = 1; count <= 2; ++count) {
        for (int failure = 0; failure <= 5; ++failure) {
            reset(5); short_exchange = failure;
            TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_FALSE(card_initialized || selected);
            TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_spi_sync());
        }
        for (int size = 1; size <= 6; size += 5) {
            reset(5); short_size = size;
            TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_FALSE(card_initialized);
        }
        reset(5); length = 1;
        TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
        TEST_ASSERT_FALSE(card_initialized);
    }
}

void test_spi_multiblock_stop_and_second_block_errors_preserve_completion_state(void)
{
    uint8_t data[1024] = {0};
    for (unsigned rejection = 0; rejection < 2; ++rejection) {
        reset(5); length = 9;
        queue[5] = queue[6] = queue[7] = 0xff; queue[8] = rejection ? 0x0b : 5;
        TEST_ASSERT_EQUAL(rejection ? PF_SD_CARD_IO_ERROR : PF_SD_CARD_OK,
                          pf_sd_spi_write_blocks(0, data, 2));
        TEST_ASSERT_EQUAL_UINT(1, stops);
        TEST_ASSERT_TRUE(card_initialized);
    }
    reset(0x0b); length = 6; queue[5] = 0xff; busy = true;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, 2));
    TEST_ASSERT_FALSE(card_initialized || selected);
    reset(5); length = 9;
    queue[5] = queue[6] = queue[7] = 0xff; queue[8] = 5; short_exchange = 10;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, 2));
    TEST_ASSERT_FALSE(card_initialized);
}

void test_spi_sync_failure_invalidates_and_blocks_later_io(void)
{
    short_exchange = 0;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_sync());
    TEST_ASSERT_FALSE(card_initialized);
    reset(5); length = 1; queue[0] = 0; busy = true;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_sync());
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_spi_sync());
}

void test_spi_trailing_exchange_failure_never_hides_errors_or_authorizes_fallback(void)
{
    uint8_t data[1024] = {0};
    for (unsigned count = 1; count <= 2; ++count) {
        reset(5); length = 9;
        queue[5] = queue[6] = queue[7] = 0xff; queue[8] = 5; fail_trailing = true;
        TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
        TEST_ASSERT_FALSE(selected || card_initialized);
        unsigned before = writes;
        TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_spi_write_blocks(0, data, count));
        TEST_ASSERT_EQUAL_UINT(before, writes);
    }

    static void prepare_read(void)
    {
        reset(0);
        selected = true;
        clock_step = 0;
        length = 3;
        queue[0] = 0xfe;
        queue[1] = queue[2] = 0;
        pf_sd_card_set_profiling_enabled(true);
    }

    void test_spi_read_data_profiles_token_payload_and_crc(void)
    {
        uint8_t data[512];
        prepare_read();
        TEST_ASSERT_TRUE(read_data_block(data, sizeof(data)));
        TEST_ASSERT_EQUAL_UINT(3, exchanges);
        TEST_ASSERT_EQUAL_UINT(1, reads);
        TEST_ASSERT_EQUAL_UINT(1, crc_calls);
        pf_sd_card_profile_t profile;
        pf_sd_card_get_profile(&profile);
        TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_CARD_PROFILE_DATA]);
        TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_CARD_PROFILE_CRC]);
        TEST_ASSERT_EQUAL_UINT(130, profile.elapsed_us[PF_SD_CARD_PROFILE_DATA]);
    }

    void test_spi_read_short_token_or_crc_tail_never_substitutes_success_bytes(void)
    {
        uint8_t data[512];
        for (unsigned failure = 0; failure < 3; ++failure) {
            prepare_read(); short_exchange = (int)failure;
            TEST_ASSERT_FALSE(read_data_block(data, sizeof(data)));
            TEST_ASSERT_FALSE(card_initialized);
            TEST_ASSERT_EQUAL_UINT(failure + 1, exchanges);
            TEST_ASSERT_EQUAL_UINT(failure != 0, reads);
            TEST_ASSERT_EQUAL_UINT(0, crc_calls);
            pf_sd_card_profile_t profile;
            pf_sd_card_get_profile(&profile);
            TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_CARD_PROFILE_DATA]);
            TEST_ASSERT_EQUAL_UINT(0, profile.calls[PF_SD_CARD_PROFILE_CRC]);
            TEST_ASSERT_EQUAL_UINT(0, profile.events[PF_SD_CARD_PROFILE_TIMEOUT]);
        }
        prepare_read();
        bool found = false;
        for (unsigned value = 0; value <= UINT16_MAX; ++value) {
            payload[510] = (uint8_t)(value >> 8); payload[511] = (uint8_t)value;
            if (pf_sd_card_crc16(payload, sizeof(payload)) == 0xffffu) { found = true; break; }
        }
        TEST_ASSERT_TRUE(found);
        queue[1] = queue[2] = 0xff; short_exchange = 2;
        TEST_ASSERT_FALSE(read_data_block(data, sizeof(data)));
        TEST_ASSERT_FALSE(card_initialized);
        TEST_ASSERT_EQUAL_UINT(0, crc_calls);
    }

    void test_spi_read_token_timeout_invalid_token_and_short_payload_are_reported(void)
    {
        uint8_t data[512];
        for (unsigned failure = 0; failure < 3; ++failure) {
            prepare_read();
            if (failure == 0) length = 0;
            if (failure == 1) queue[0] = 0x0b;
            if (failure == 2) short_payload = true;
            TEST_ASSERT_FALSE(read_data_block(data, sizeof(data)));
            pf_sd_card_profile_t profile;
            pf_sd_card_get_profile(&profile);
            TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_CARD_PROFILE_DATA]);
            TEST_ASSERT_EQUAL_UINT(0, crc_calls);
            TEST_ASSERT_EQUAL_UINT(failure == 0, profile.events[PF_SD_CARD_PROFILE_TIMEOUT]);
            TEST_ASSERT_EQUAL_UINT(failure == 2, reads);
            if (failure == 0) {
                TEST_ASSERT_GREATER_OR_EQUAL_UINT(SD_SPI_RESPONSE_TIMEOUT_US,
                                                profile.elapsed_us[PF_SD_CARD_PROFILE_DATA]);
            } else {
                TEST_ASSERT_EQUAL_UINT(1, exchanges);
            }
        }
    }
    reset(5); queue[1] = SD_R1_ILLEGAL_COMMAND; fail_trailing = true;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_write_blocks(0, data, 2));
    TEST_ASSERT_EQUAL_UINT(1, writes);
    TEST_ASSERT_FALSE(card_initialized || selected);
    reset(5); fail_trailing = true;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_sync());
    TEST_ASSERT_FALSE(selected || card_initialized);
    for (unsigned count = 1; count <= 2; ++count) {
        reset(0); length = 10;
        queue[5] = queue[6] = queue[7] = 0xff; queue[8] = 0xfe; queue[9] = 0xff;
        fail_trailing = true;
        TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sd_spi_read_blocks(0, data, count));
        TEST_ASSERT_FALSE(selected || card_initialized);
        unsigned before = exchanges;
        TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_spi_read_blocks(0, data, count));
        TEST_ASSERT_EQUAL_UINT(before, exchanges);
    }
}
