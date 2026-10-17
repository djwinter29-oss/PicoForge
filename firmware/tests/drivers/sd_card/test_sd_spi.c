#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/spi")
#define PF_SPI_PRODUCTION_TEST
#include "spi_production_fixture.h"
#include "sd_protocol.h"
#include "bus.h"
#include "serial.h"
#include "spi.h"

static pf_spi_t physical_bus;
static bool test_present(const void *context) { (void)context; return spi_test_present; }

spi_inst_t test_spi;
bool spi_test_present;
static uint8_t queue[64];
static unsigned position, length, writes, stops, exchanges, reads;
static bool busy, selected, fail_trailing, short_payload;
static int short_size, short_exchange;
static uint32_t now, clock_step;
static uint8_t payload[512];
static unsigned crc_calls;
static unsigned commands[16], command_count, selects, deselects, payload_writes;
static uint32_t arguments[16];
static const uint8_t *expected_write;
static uint8_t *expected_read;
static unsigned fail_payload;

uint32_t time_us_32(void) { now += clock_step; return now; }
static uint32_t profile_clock(void) { return now; }
static void idle(void) { now += 25000u; }
static uint16_t spi_fixture_crc16(const uint8_t *data, size_t size)
{
    ++crc_calls;
    return pf_sd_crc16(data, size);
}
void gpio_put(unsigned pin, bool high)
{
    (void)pin;
    if (high) ++deselects; else ++selects;
    selected = !high;
}
int spi_write_blocking(spi_inst_t *spi, const uint8_t *data, size_t size)
{
    (void)spi;
    TEST_ASSERT_TRUE(selected);
    if (size == 6) {
        TEST_ASSERT_LESS_THAN_UINT(16, command_count);
        commands[command_count] = data[0] & 63u;
        arguments[command_count++] = ((uint32_t)data[1] << 24) |
            ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 8) | data[4];
    }
    if (size == 512) {
        if (expected_write != NULL)
            TEST_ASSERT_EQUAL_PTR(expected_write + payload_writes * 512, data);
        if (payload_writes++ == fail_payload) return 0;
    }
    ++writes;
    return (int)size == short_size ? 0 : (int)size;
}
int spi_read_blocking(spi_inst_t *spi, uint8_t value, uint8_t *data, size_t size)
{
    (void)spi; (void)value;
    TEST_ASSERT_TRUE(selected);
    if (expected_read != NULL)
        TEST_ASSERT_EQUAL_PTR(expected_read + reads * 512, data);
    if (reads++ == fail_payload) return 0;
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
    pf_spi_deinit(&physical_bus);
    physical_bus = (pf_spi_t){
        .instance = &test_spi, .pins.clock_gpio = 2u, .pins.mosi_gpio = 3u,
        .pins.miso_gpio = 0u, .pins.cs_gpio = 1u, .clock_hz = 25000000u,
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&physical_bus));
    card_initialized = card_block_addressed = true;
    card_sector_count = 128;
    configuration_set = false;
    last_init_response_received = false;
    last_init_command = last_init_response = 0;
    active_config = (pf_sd_config_t){ .transport = pf_spi_bus(&physical_bus) };
    position = 0; length = 5;
    queue[0] = 0xff; queue[1] = 0; queue[2] = queue[3] = 0xff; queue[4] = response;
    busy = selected = fail_trailing = short_payload = false;
    spi_test_present = true;
    short_size = short_exchange = -1;
    writes = stops = exchanges = reads = now = 0;
    crc_calls = 0;
    command_count = selects = deselects = payload_writes = 0;
    expected_write = NULL; expected_read = NULL; fail_payload = UINT32_MAX;
    clock_step = 25000u;
    memset(payload, 0, sizeof(payload));
    pf_sd_set_profiling_enabled(false);
    pf_sd_reset_profile();
    pf_sd_profile_set_clock(profile_clock);
    pf_sd_set_idle_hook(idle);
}

void setUp(void) { reset(5); }
void tearDown(void)
{
    pf_sd_set_profiling_enabled(false);
    pf_sd_profile_set_clock(NULL);
    pf_sd_set_idle_hook(NULL);
}

static unsigned selection_attempts;

static pf_storage_bus_result_t reject_selection(void *context, bool select)
{
    TEST_ASSERT_EQUAL_PTR(&physical_bus, context);
    TEST_ASSERT_TRUE(select);
    ++selection_attempts;
    return PF_STORAGE_BUS_IO_ERROR;
}

void test_spi_selection_failure_stops_every_command_and_transfer_without_cleanup_or_replay(void)
{
    uint8_t data[1024] = {0};
    uint8_t response;
    for (unsigned operation = 0u; operation < 8u; ++operation) {
        reset(0);
        pf_storage_bus_ops_t ops = *active_config.transport.ops;
        pf_storage_serial_ops_t serial_ops = *ops.serial;
        serial_ops.select = reject_selection;
        ops.serial = &serial_ops;
        active_config.transport.ops = &ops;
        selection_attempts = 0u;
        switch (operation) {
        case 0u: TEST_ASSERT_FALSE(send_command(0u, 0u, &response, NULL, 0u)); break;
        case 1u: TEST_ASSERT_FALSE(read_register(SD_CMD_SEND_CSD, data)); break;
        case 2u: TEST_ASSERT_FALSE(read_register(SD_CMD_SEND_CID, data)); break;
        case 3u: TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_read_blocks(0u, data, 1u)); break;
        case 4u: TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0u, data, 1u)); break;
        case 5u: TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_read_blocks(0u, data, 2u)); break;
        case 6u: TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0u, data, 2u)); break;
        case 7u: TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_sync()); break;
        }
        TEST_ASSERT_FALSE(card_initialized);
        TEST_ASSERT_EQUAL_UINT(1u, selection_attempts);
        TEST_ASSERT_EQUAL_UINT(0u, selects + deselects + exchanges + writes + reads);
        TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_spi_sync());
        TEST_ASSERT_EQUAL_UINT(1u, selection_attempts);
    }
}

void test_spi_application_prefix_accepts_only_ready_or_idle(void)
{
    for (unsigned value = 0u; value < 256u; ++value) {
        reset(0);
        queue[1] = (uint8_t)value;
        length = 5u;
        queue[3] = 0xffu;
        queue[4] = value <= 1u ? 0u : 0xffu;
        uint8_t response;
        TEST_ASSERT_EQUAL(value <= 1u, send_application_command(41u, 0u, &response));
        TEST_ASSERT_EQUAL_UINT(value <= 1u ? 2u : 1u, command_count);
    }
}

void test_spi_write_response_accepts_only_the_accepted_token(void)
{
    for (unsigned value = 0u; value < 256u; ++value) {
        reset(0);
        selected = true;
        clock_step = 25000u;
        queue[0] = (uint8_t)value;
        length = 1u;
        uint8_t response;
        TEST_ASSERT_EQUAL((value & 0x1fu) == 0x05u, receive_write_response(&response));
        TEST_ASSERT_EQUAL_UINT(value == 0xffu ? 2u : 1u, exchanges);
    }
}

void test_spi_writes_accept_success_and_explicit_rejection_without_replay(void)
{
    uint8_t data[1024] = {0};
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_spi_write_blocks(0, data, 1));
    TEST_ASSERT_TRUE(card_initialized && !selected);
    for (unsigned count = 1; count <= 2; ++count) {
        for (unsigned i = 0; i < 2; ++i) {
            reset(i ? 0x0d : 0x0b);
            TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_TRUE(card_initialized && !selected);
            TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_spi_sync());
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
            TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_FALSE(card_initialized || selected);
            unsigned before = writes;
            TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_spi_sync());
            TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_spi_write_blocks(0, data, 1));
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
            TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_FALSE(card_initialized || selected);
            TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_spi_sync());
        }
        for (int size = 1; size <= 6; size += 5) {
            reset(5); short_size = size;
            TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
            TEST_ASSERT_FALSE(card_initialized);
        }
        reset(5); length = 1;
        TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
        TEST_ASSERT_FALSE(card_initialized);
    }
}

void test_spi_multiblock_stop_and_second_block_errors_preserve_completion_state(void)
{
    uint8_t data[1024] = {0};
    for (unsigned rejection = 0; rejection < 2; ++rejection) {
        reset(5); length = 9;
        queue[5] = queue[6] = queue[7] = 0xff; queue[8] = rejection ? 0x0b : 5;
        TEST_ASSERT_EQUAL(rejection ? PF_SD_IO_ERROR : PF_SD_OK,
                          pf_sd_spi_write_blocks(0, data, 2));
        TEST_ASSERT_EQUAL_UINT(1, stops);
        TEST_ASSERT_TRUE(card_initialized);
    }
    reset(0x0b); length = 6; queue[5] = 0xff; busy = true;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, 2));
    TEST_ASSERT_FALSE(card_initialized || selected);
    reset(5); length = 9;
    queue[5] = queue[6] = queue[7] = 0xff; queue[8] = 5; short_exchange = 10;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, 2));
    TEST_ASSERT_FALSE(card_initialized);
}

void test_spi_sync_failure_invalidates_and_blocks_later_io(void)
{
    short_exchange = 0;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_sync());
    TEST_ASSERT_FALSE(card_initialized);
    reset(5); length = 1; queue[0] = 0; busy = true;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_sync());
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_spi_sync());
}

void test_spi_trailing_exchange_failure_never_hides_errors_or_authorizes_fallback(void)
{
    uint8_t data[1024] = {0};
    for (unsigned count = 1; count <= 2; ++count) {
        reset(5); length = 9;
        queue[5] = queue[6] = queue[7] = 0xff; queue[8] = 5; fail_trailing = true;
        TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, count));
        TEST_ASSERT_FALSE(selected || card_initialized);
        unsigned before = writes;
        TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_spi_write_blocks(0, data, count));
        TEST_ASSERT_EQUAL_UINT(before, writes);
    }

    reset(5); queue[1] = SD_R1_ILLEGAL_COMMAND; fail_trailing = true;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_write_blocks(0, data, 2));
    TEST_ASSERT_EQUAL_UINT(1, writes);
    TEST_ASSERT_FALSE(card_initialized || selected);
    reset(5); fail_trailing = true;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_sync());
    TEST_ASSERT_FALSE(selected || card_initialized);
    for (unsigned count = 1; count <= 2; ++count) {
        reset(0); length = 10;
        queue[5] = queue[6] = queue[7] = 0xff; queue[8] = 0xfe; queue[9] = 0xff;
        fail_trailing = true;
        TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_spi_read_blocks(0, data, count));
        TEST_ASSERT_FALSE(selected || card_initialized);
        unsigned before = exchanges;
        TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_spi_read_blocks(0, data, count));
        TEST_ASSERT_EQUAL_UINT(before, exchanges);
    }
}

static void prepare_read(void)
{
    reset(0);
    selected = true;
    clock_step = 0;
    length = 3;
    queue[0] = 0xfe;
    queue[1] = queue[2] = 0;
    pf_sd_set_profiling_enabled(true);
}

void test_spi_read_data_profiles_token_payload_and_crc(void)
{
    uint8_t data[512];
    prepare_read();
    TEST_ASSERT_TRUE(read_data_block(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT(3, exchanges);
    TEST_ASSERT_EQUAL_UINT(1, reads);
    TEST_ASSERT_EQUAL_UINT(1, crc_calls);
    pf_sd_profile_t profile;
    pf_sd_get_profile(&profile);
    TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_PROFILE_DATA]);
    TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_PROFILE_CRC]);
    TEST_ASSERT_EQUAL_UINT(130, profile.elapsed_us[PF_SD_PROFILE_DATA]);
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
        pf_sd_profile_t profile;
        pf_sd_get_profile(&profile);
        TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_PROFILE_DATA]);
        TEST_ASSERT_EQUAL_UINT(0, profile.calls[PF_SD_PROFILE_CRC]);
        TEST_ASSERT_EQUAL_UINT(0, profile.events[PF_SD_PROFILE_TIMEOUT]);
    }
    prepare_read();
    bool found = false;
    for (unsigned value = 0; value <= UINT16_MAX; ++value) {
        payload[510] = (uint8_t)(value >> 8); payload[511] = (uint8_t)value;
        if (pf_sd_crc16(payload, sizeof(payload)) == 0xffffu) { found = true; break; }
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
        pf_sd_profile_t profile;
        pf_sd_get_profile(&profile);
        TEST_ASSERT_EQUAL_UINT(1, profile.calls[PF_SD_PROFILE_DATA]);
        TEST_ASSERT_EQUAL_UINT(0, crc_calls);
        TEST_ASSERT_EQUAL_UINT(failure == 0, profile.events[PF_SD_PROFILE_TIMEOUT]);
        TEST_ASSERT_EQUAL_UINT(failure == 2, reads);
        if (failure == 0) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT(SD_SPI_RESPONSE_TIMEOUT_US,
                                            profile.elapsed_us[PF_SD_PROFILE_DATA]);
        } else {
            TEST_ASSERT_EQUAL_UINT(1, exchanges);
        }
    }
}

void test_spi_cmd12_discards_exactly_one_stuff_byte(void)
{
    uint8_t response = 0xff;
    selected = true; position = 0; length = 2;
    queue[0] = 0; queue[1] = SD_R1_ILLEGAL_COMMAND;
    TEST_ASSERT_TRUE(send_command_selected(12, 0, &response));
    TEST_ASSERT_EQUAL_UINT(SD_R1_ILLEGAL_COMMAND, response);
    TEST_ASSERT_EQUAL_UINT(2, exchanges);
    TEST_ASSERT_TRUE(last_init_response_received);
}

static void prepare_multiblock_read(void)
{
    reset(0); length = 15;
    const uint8_t wire[] = {0xff, 0, 0xfe, 0, 0, 0xfe, 0, 0, 0xfe, 0, 0,
                            0xff, 0, 0xff, 0xff};
    memcpy(queue, wire, sizeof(wire));
}

void test_spi_multiblock_reads_use_one_select_and_stop_with_sequential_buffers(void)
{
    uint8_t data[1536];
    prepare_multiblock_read(); expected_read = data;
    TEST_ASSERT_EQUAL(PF_SD_OK, transfer_blocks(5, data, NULL, 3));
    TEST_ASSERT_EQUAL_UINT(3, reads);
    TEST_ASSERT_EQUAL_UINT(1, selects);
    TEST_ASSERT_EQUAL_UINT(1, deselects);
    TEST_ASSERT_EQUAL_UINT(2, command_count);
    TEST_ASSERT_EQUAL_UINT(18, commands[0]);
    TEST_ASSERT_EQUAL_UINT(5, arguments[0]);
    TEST_ASSERT_EQUAL_UINT(12, commands[1]);
}

void test_spi_multiblock_writes_use_one_select_stop_token_and_sequential_buffers(void)
{
    uint8_t data[1536] = {0};
    reset(5); length = 17; memset(queue, 0xff, length);
    queue[1] = 0; queue[4] = queue[8] = queue[12] = 5;
    expected_write = data;
    TEST_ASSERT_EQUAL(PF_SD_OK, transfer_blocks(5, NULL, data, 3));
    TEST_ASSERT_EQUAL_UINT(3, payload_writes);
    TEST_ASSERT_EQUAL_UINT(1, stops);
    TEST_ASSERT_EQUAL_UINT(1, selects);
    TEST_ASSERT_EQUAL_UINT(1, deselects);
    TEST_ASSERT_EQUAL_UINT(1, command_count);
    TEST_ASSERT_EQUAL_UINT(25, commands[0]);
    TEST_ASSERT_EQUAL_UINT(5, arguments[0]);
}

void test_spi_only_explicit_illegal_multiblock_command_allows_single_block_fallback(void)
{
    uint8_t data[1536];
    reset(0);
    const uint8_t wire[] = {0xff, 4, 0xff,
        0xff, 0, 0xfe, 0, 0, 0xff,
        0xff, 0, 0xfe, 0, 0, 0xff,
        0xff, 0, 0xfe, 0, 0, 0xff};
    length = sizeof(wire); memcpy(queue, wire, length); expected_read = data;
    TEST_ASSERT_EQUAL(PF_SD_OK, transfer_blocks(5, data, NULL, 3));
    TEST_ASSERT_EQUAL_UINT(4, command_count);
    for (unsigned i = 1; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT(17, commands[i]);
        TEST_ASSERT_EQUAL_UINT(4 + i, arguments[i]);
    }
    reset(0); queue[1] = 8;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_blocks(5, NULL, data, 3));
    TEST_ASSERT_EQUAL_UINT(1, command_count);
    TEST_ASSERT_EQUAL_UINT(0, payload_writes);
    TEST_ASSERT_FALSE(selected);
}

void test_spi_invalid_bounds_zero_count_absence_and_ready_timeout_never_transfer_data(void)
{
    uint8_t data[1536];
    TEST_ASSERT_EQUAL(PF_SD_OK, transfer_blocks(5, NULL, NULL, 0));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, transfer_blocks(5, NULL, NULL, 2));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, transfer_blocks(127, data, NULL, 2));
    TEST_ASSERT_EQUAL_UINT(0, selects);
    reset(0); length = 0; busy = true;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_blocks(5, NULL, data, 3));
    TEST_ASSERT_FALSE(card_initialized || selected);
    TEST_ASSERT_EQUAL_UINT(0, payload_writes);
    reset(0); active_config.transport.present = test_present;
     spi_test_present = false;
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, transfer_blocks(5, NULL, data, 3));
    TEST_ASSERT_FALSE(card_initialized || selected);
    TEST_ASSERT_EQUAL_UINT(0, payload_writes);
}

void test_spi_multiblock_read_payload_and_stop_failures_do_not_fall_back_or_replay(void)
{
    uint8_t data[1536];
    prepare_multiblock_read(); fail_payload = 1;
    /* The failed payload leaves its CRC bytes for the stop stuff/response. */
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_blocks(5, data, NULL, 3));
    TEST_ASSERT_EQUAL_UINT(2, reads);
    TEST_ASSERT_EQUAL_UINT(2, command_count);
    TEST_ASSERT_EQUAL_UINT(12, commands[1]);
    TEST_ASSERT_FALSE(selected);
    prepare_multiblock_read(); queue[12] = 1;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_blocks(5, data, NULL, 3));
    TEST_ASSERT_FALSE(card_initialized || selected);
}

void test_spi_multiblock_short_second_write_stops_without_single_block_replay(void)
{
    uint8_t data[1536] = {0};
    reset(5); length = 10; memset(queue, 0xff, length);
    queue[1] = 0; queue[4] = 5;
    fail_payload = 1;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_blocks(5, NULL, data, 3));
    TEST_ASSERT_EQUAL_UINT(2, payload_writes);
    TEST_ASSERT_EQUAL_UINT(1, command_count);
    TEST_ASSERT_EQUAL_UINT(25, commands[0]);
    TEST_ASSERT_EQUAL_UINT(1, stops);
    TEST_ASSERT_FALSE(selected);
}

void test_spi_single_block_command_failure_and_rejection_have_distinct_recovery(void)
{
    uint8_t data[512] = {0};
    for (unsigned write = 0; write < 2; ++write) {
        reset(write ? 5 : 0);
        if (!write) {
            length = 6; queue[2] = 0xfe; queue[3] = queue[4] = 0; queue[5] = 0xff;
        }
        TEST_ASSERT_EQUAL(PF_SD_OK, transfer_block(5, write ? NULL : data,
                                                      write ? data : NULL));
        TEST_ASSERT_TRUE(card_initialized && !selected);
        reset(0); short_size = 6;
        TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_block(5, write ? NULL : data,
                                                            write ? data : NULL));
        TEST_ASSERT_FALSE(card_initialized || selected);
        TEST_ASSERT_EQUAL_UINT(0, reads + payload_writes);
        reset(0); queue[1] = 4;
        TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_block(5, write ? NULL : data,
                                                            write ? data : NULL));
        TEST_ASSERT_TRUE(card_initialized && !selected);
        TEST_ASSERT_EQUAL_UINT(0, reads + payload_writes);
    }
    reset(0); queue[2] = 0x0b;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_block(5, data, NULL));
    TEST_ASSERT_FALSE(card_initialized || selected);
    reset(0); length = 0; busy = true;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, transfer_block(5, data, NULL));
    TEST_ASSERT_FALSE(card_initialized || selected);
    reset(0); active_config.transport.present = test_present;
     spi_test_present = false;
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, transfer_block(5, data, NULL));
    TEST_ASSERT_FALSE(card_initialized);
}
