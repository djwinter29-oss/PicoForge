#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"
#include "../../../src/drivers/storage/sdio_pio/sdio_pio.c"
#include "../../../src/drivers/storage/storage_transport.c"
#include "../../../src/drivers/storage/native.c"

static pf_sdio_pio_t test_wire;
static bool test_present(const void *context) { (void)context; return pf_test_sdio_gpio_level != 0u; }

#include <string.h>

#include "../../../src/drivers/sd_card/sd_native.c"
#include "../../../src/drivers/sd_card/sd_native_protocol.c"
#include "../../../src/drivers/sd_card/sd_protocol.c"
#include "sdio_sdk_stubs.c"

pf_sd_native_host_t *pf_test_sdio_observed_host;
bool pf_test_sdio_expect_streams_cleared;
bool pf_test_sdio_streams_were_cleared;
bool pf_test_sdio_can_add_program;
unsigned pf_test_sdio_removed_programs;

static unsigned recovery_calls;
static unsigned reads;
static unsigned sectors_read;
static uint32_t read_first[32];
static uint32_t read_sizes[32];
static unsigned single_writes;
static unsigned batch_writes;
static unsigned forget_calls;
static bool recovery_ok;
static bool read_ok;
static bool read_fail_unresolved;
static unsigned recover_failure_call;
static uint32_t clock_requests[4];
static unsigned clock_request_count;
static uint8_t csd_transfer_speed;
static unsigned card_init_calls;
static uint8_t fail_command;
static bool single_write_ok;
static bool batch_write_ok;
static unsigned abort_calls;
static unsigned validation_reads;
static unsigned switch_calls;
static bool high_speed_supported;
static bool high_speed_selected;
static bool fail_high_speed_read;
static bool fail_default_read;
static uint32_t r7_value;
static uint32_t ocr_value;
static uint32_t status_value;
static uint8_t csd_structure;
static bool four_bit_supported;
static uint8_t bad_crc_command;
static uint8_t bad_index_command;
static uint8_t commands[64];
static unsigned command_count;
static unsigned fail_validation_read;
static uint32_t r6_value;
static bool fail_switch_check;

void setUp(void)
{
    memset(&test_wire, 0, sizeof(test_wire));
    memset(&native_host, 0, sizeof(native_host));
    memset(&active_config, 0, sizeof(active_config));
    memset(&native_info, 0, sizeof(native_info));
    native_info.block_count = 15564800u;
    configuration_set = true;
    card_initialized = true;
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_NONE;
    active_config = (pf_sd_config_t){
        .transport = pf_sdio_pio_transport(&test_wire), .clock_hz = 25000000u
    };
    native_host.transport = active_config.transport;
    test_wire.current_clock_hz = 25000000u;
    native_host.data_rx_multiblock_supported = true;
    recovery_calls = reads = sectors_read = single_writes = batch_writes = forget_calls = 0u;
    memset(read_first, 0, sizeof(read_first));
    memset(read_sizes, 0, sizeof(read_sizes));
    recovery_ok = read_ok = single_write_ok = batch_write_ok = true;
    read_fail_unresolved = false;
    recover_failure_call = 0u;
    clock_request_count = 0u;
    memset(clock_requests, 0, sizeof(clock_requests));
    csd_transfer_speed = 0x32u;
    card_init_calls = 0u;
    fail_command = 0u;
    pf_test_sdio_gpio_level = 1u;
    pf_test_sdio_can_add_program = true;
    pf_test_sdio_removed_programs = 0u;
    pf_test_sdio_system_clock_hz = 125000000u;
    pf_test_sdio_time_step = 100u;
    abort_calls = validation_reads = switch_calls = command_count = 0u;
    high_speed_supported = high_speed_selected = four_bit_supported = true;
    fail_high_speed_read = fail_default_read = false;
    r7_value = 0x1aau;
    ocr_value = 0xc0ff8000u;
    status_value = PF_SD_NATIVE_R1_READY_FOR_DATA | PF_SD_NATIVE_R1_STATE_TRAN;
    csd_structure = 1u;
    bad_crc_command = bad_index_command = 0xffu;
    fail_validation_read = 0u;
    r6_value = 0x12340000u;
    fail_switch_check = false;
}

void tearDown(void) {}

bool pf_sd_native_host_init(pf_sd_native_host_t *host, const pf_sd_config_t *config)
{ host->transport = config->transport; test_wire.pio = pio0; test_wire.current_clock_hz = 400000u; return true; }
void pf_sd_native_host_deinit(pf_sd_native_host_t *host) { (void)host; }
bool pf_sd_native_host_set_clock(pf_sd_native_host_t *host, uint32_t hz)
{
    TEST_ASSERT_LESS_THAN_UINT(4u, clock_request_count);
    clock_requests[clock_request_count++] = hz;
    if (hz > SD_HIGH_CLOCK_HZ) return false;
    test_wire.current_clock_hz = hz;
    return true;
}
bool pf_sd_native_host_command(pf_sd_native_host_t *host, uint8_t command, uint32_t argument,
                              uint16_t bits, uint32_t *response, size_t capacity)
{ (void)host; (void)command; (void)argument; (void)bits; (void)response; (void)capacity; return true; }
bool pf_sd_native_host_prepare_initialization(pf_sd_native_host_t *host)
{
    host->bus_width = 1u;
    return pf_sd_native_host_recover(host) &&
           pf_sd_native_host_set_clock(host, 400000u);
}
bool pf_sd_native_host_set_bus_width(pf_sd_native_host_t *host, uint8_t width)
{
    TEST_ASSERT_EQUAL_UINT8(4u, width);
    TEST_ASSERT_EQUAL_UINT8(6u, commands[command_count - 1u]);
    host->bus_width = width;
    return true;
}
void pf_sd_native_host_abort(pf_sd_native_host_t *host)
{ (void)host; ++abort_calls; }
bool pf_sd_native_host_command_bytes(pf_sd_native_host_t *host, uint8_t command, uint32_t argument,
                                    uint16_t bits, uint8_t *response, size_t capacity)
{
    (void)host;
    TEST_ASSERT_LESS_THAN_UINT(sizeof(commands), command_count);
    commands[command_count++] = command;
    if (command == 41u) TEST_ASSERT_EQUAL_HEX32(0x40ff8000u, argument);
    TEST_ASSERT_NOT_EQUAL(16u, command);
    if ((fail_command != 0u) && (command == fail_command)) return false;
    (void)argument;
    if (command == 0u) ++card_init_calls;
    if ((bits == 0u) && (response == NULL) && (capacity == 0u)) return true;
    if ((response == NULL) || (capacity < (bits / 8u))) return false;
    memset(response, 0, capacity);
    if (command == 2u || command == 9u) {
        response[0] = 0x3fu;
        uint8_t *payload = response + 1u;
        if (command == 2u) {
            for (unsigned i = 0u; i < 15u; ++i) {
                payload[i] = (uint8_t)(i + 1u);
            }
        } else {
            const uint32_t c_size = 15199u;
            payload[0] = csd_structure << 6u;
            payload[3] = csd_transfer_speed;
            payload[7] = (uint8_t)(c_size >> 16u);
            payload[8] = (uint8_t)(c_size >> 8u);
            payload[9] = (uint8_t)c_size;
        }
        response[16] = (uint8_t)((pf_sd_crc7(payload, 15u) << 1u) | 1u);
        return true;
    }
    response[0] = (command == 41u) ? 0x3fu : command;
    uint32_t response_argument = 0u;
    if (command == 8u) response_argument = r7_value;
    if (command == 3u) response_argument = r6_value;
    if (command == 41u) response_argument = ocr_value;
    if (command == 13u) response_argument = status_value;
    response[1] = (uint8_t)(response_argument >> 24u);
    response[2] = (uint8_t)(response_argument >> 16u);
    response[3] = (uint8_t)(response_argument >> 8u);
    response[4] = (uint8_t)response_argument;
    response[5] = (uint8_t)((pf_sd_crc7(response, 5u) << 1u) | 1u);
    if (command == 41u) response[5] = 0xffu;
    if (command == bad_crc_command) response[5] ^= 2u;
    if (command == bad_index_command) response[0] ^= 1u;
    return true;
}
bool pf_sd_native_host_read_sector(pf_sd_native_host_t *host, uint32_t sector,
                                  uint8_t block[512])
{
    (void)block;
    TEST_ASSERT_EQUAL_UINT32(0u, sector);
    ++validation_reads;
    bool ok = read_ok && !(fail_high_speed_read && test_wire.current_clock_hz > 25000000u) &&
              !(fail_default_read && test_wire.current_clock_hz == 25000000u) &&
              validation_reads != fail_validation_read;
    if (!ok) host->last_error = PF_SD_DETAIL_DATA_CRC;
    return ok;
}
bool pf_sd_native_host_read_scr(pf_sd_native_host_t *host, uint32_t address, uint8_t scr[8])
{
    (void)host;
    (void)address;
    memset(scr, 0, 8u);
    scr[0] = 0x02u;
    scr[1] = four_bit_supported ? 0x05u : 0x01u;
    return true;
}
bool pf_sd_native_host_read_switch_status(pf_sd_native_host_t *host, uint32_t argument,
                                         uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE])
{
    (void)host;
    ++switch_calls;
    memset(status, 0, PF_SD_NATIVE_SWITCH_STATUS_SIZE);
    if (argument == PF_SD_NATIVE_CMD6_CHECK_HIGH_SPEED) {
        if (fail_switch_check) {
            host->last_error = PF_SD_DETAIL_DATA_CRC;
            return false;
        }
        status[13] = high_speed_supported ? 2u : 0u;
    } else if (argument == PF_SD_NATIVE_CMD6_SWITCH_HIGH_SPEED) {
        status[16] = high_speed_selected ? 1u : 0xfu;
    } else {
        TEST_ASSERT_EQUAL_HEX32(PF_SD_NATIVE_CMD6_SWITCH_DEFAULT, argument);
    }
    return true;
}
bool pf_sd_native_host_read_blocks(pf_sd_native_host_t *host, uint32_t first,
                                  uint8_t *buffer, uint32_t count)
{
    (void)buffer;
    if (reads < sizeof(read_first) / sizeof(read_first[0])) {
        read_first[reads] = first;
        read_sizes[reads] = count;
    }
    ++reads;
    sectors_read += count;
    if (read_fail_unresolved && !read_ok) host->read_stream_unresolved = true;
    return read_ok && count > 0u;
}
bool pf_sd_native_host_write_sector(pf_sd_native_host_t *host, uint32_t first,
                                   const uint8_t *buffer)
{ (void)host; (void)first; (void)buffer; ++single_writes; return single_write_ok; }
bool pf_sd_native_host_write_stream_continues(const pf_sd_native_host_t *host, uint32_t sector)
{ return host->write_stream_open && host->write_stream_next_sector == sector; }
bool pf_sd_native_host_write_blocks(pf_sd_native_host_t *host, uint32_t first,
                                   const uint8_t *buffer, uint32_t count)
{
    (void)buffer; ++batch_writes;
    host->write_stream_open = batch_write_ok;
    host->write_stream_next_sector = first + count;
    return batch_write_ok;
}
bool pf_sd_native_host_recover(pf_sd_native_host_t *host)
{
    ++recovery_calls;
    if (recover_failure_call != 0u && recovery_calls == recover_failure_call) return false;
    if (recovery_ok) {
        host->write_stream_open = false;
        host->read_stream_unresolved = false;
        test_wire.mode_restore_failed = false;
    }
    return recovery_ok;
}
void pf_sd_native_host_forget_removed_card(pf_sd_native_host_t *host)
{
    ++forget_calls;
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
}

void test_init_config_validation_accepts_supported_clocks_and_rejects_bounds(void)
{
    pf_sd_config_t config = active_config;

    config.clock_hz = 400000u;
    TEST_ASSERT_FALSE(config_is_valid(&config));
    config.clock_hz = 999999u;
    TEST_ASSERT_FALSE(config_is_valid(&config));
    config.clock_hz = 1000000u;
    TEST_ASSERT_FALSE(config_is_valid(&config));
    config.clock_hz = 50000000u;
    TEST_ASSERT_FALSE(config_is_valid(&config));
    config.clock_hz = 25000000u;
    TEST_ASSERT_TRUE(config_is_valid(&config));
    config.clock_hz = 399999u;
    TEST_ASSERT_FALSE(config_is_valid(&config));
    config.clock_hz = 50000001u;
    TEST_ASSERT_FALSE(config_is_valid(&config));
    config.clock_hz = 0u;
    TEST_ASSERT_TRUE(config_is_valid(&config));
}

void test_transfer_routes_reads_and_single_or_batched_writes(void)
{
    uint8_t buffer[2u * PF_SD_BLOCK_SIZE] = {0};

    TEST_ASSERT_EQUAL(PF_SD_OK, transfer_blocks(0u, NULL, NULL, 0u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT,
                      pf_sd_native_read_blocks(0u, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT,
                      pf_sd_native_read_blocks(native_info.block_count, buffer, 1u));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_read_blocks(0u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, reads);
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);

    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_write_blocks(0u, NULL, 0u));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_write_blocks(0u, buffer, 0u));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_write_blocks(0u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, single_writes);
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_write_blocks(1u, buffer, 2u));
    TEST_ASSERT_EQUAL_UINT(1u, batch_writes);

    native_host.write_stream_open = true;
    native_host.write_stream_next_sector = 3u;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_write_blocks(3u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(2u, batch_writes);
}

void test_readiness_sync_card_removal_and_info_follow_card_detect(void)
{
    pf_sd_info_t info;

    active_config.transport.present = test_present;
    pf_sd_native_get_info(&info);
    TEST_ASSERT_TRUE(info.presence_known);
    TEST_ASSERT_TRUE(info.present);
    TEST_ASSERT_TRUE(info.initialized);
    TEST_ASSERT_EQUAL_UINT32(native_info.block_count, info.block_count);
    TEST_ASSERT_EQUAL_UINT32(25000000u, info.bus_clock_hz);

    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_sync());
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);

    native_host.write_stream_open = true;
    native_host.read_stream_unresolved = true;
    pf_sd_native_confirm_card_removed();
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_FALSE(native_host.write_stream_open);
    TEST_ASSERT_FALSE(native_host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT(1u, forget_calls);
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_native_sync());

    card_initialized = true;
    pf_test_sdio_gpio_level = 0u;
    TEST_ASSERT_FALSE(pf_sd_native_is_initialized());
    pf_sd_native_get_info(&info);
    TEST_ASSERT_FALSE(info.present);
    TEST_ASSERT_FALSE(info.initialized);
}

void test_transfer_validation_checks_32_bit_sector_overflow(void)
{
    uint8_t block[PF_SD_BLOCK_SIZE] = {0};

    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT,
        pf_sd_validate_transfer(UINT32_MAX, block, 1u, UINT32_MAX));
    TEST_ASSERT_EQUAL(PF_SD_OK,
        pf_sd_validate_transfer(UINT32_MAX, NULL, 0u, 0u));
}

void test_card_init_validates_one_sector_without_high_speed_on_slow_cpu(void)
{
    active_config.clock_hz = 0u;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT32(25000000u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
    TEST_ASSERT_EQUAL_UINT(2u, clock_request_count);
    TEST_ASSERT_EQUAL_UINT32(400000u, clock_requests[0]);
    TEST_ASSERT_EQUAL_UINT32(25000000u, clock_requests[1]);
    TEST_ASSERT_EQUAL_UINT(1u, validation_reads);
    TEST_ASSERT_EQUAL_UINT(0u, switch_calls);
}

void test_card_init_failure_does_not_retry_at_other_clocks(void)
{
    fail_command = 2u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
}

void test_fixed_clock_rejects_other_requests_before_card_commands(void)
{
    active_config.clock_hz = 50000000u;
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(0u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, recovery_calls);
}

void test_runtime_read_failure_is_not_replayed_and_clock_stays_fixed(void)
{
    uint8_t block[PF_SD_BLOCK_SIZE] = {0};
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    read_ok = false;
    read_fail_unresolved = true;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_native_read_blocks(4096u, block, 1u));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT32(25000000u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT(1u, reads);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_native_read_blocks(4096u, block, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, reads);
}

void test_removed_media_never_attempts_read_or_reinitialization(void)
{
    uint8_t block[PF_SD_BLOCK_SIZE] = {0};
    active_config.transport.present = test_present;
    pf_test_sdio_gpio_level = 0u;
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_native_read_blocks(4096u, block, 1u));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
    TEST_ASSERT_EQUAL_UINT(0u, card_init_calls);
}

void test_explicit_reinitialization_after_failed_read_restores_fixed_rate(void)
{
    uint8_t block[PF_SD_BLOCK_SIZE];
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    read_ok = false;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_native_read_blocks(0u, block, 1u));
    read_ok = true;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT32(25000000u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT(2u, card_init_calls);
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_read_blocks(0u, block, 1u));
}

void test_card_advertising_less_than_25mhz_is_rejected_without_fallback(void)
{
    csd_transfer_speed = 0x2au; /* 20 MHz TRAN_SPEED. */
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(1u, clock_request_count);
    TEST_ASSERT_EQUAL_UINT32(400000u, clock_requests[0]);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
}

void test_modern_card_policy_rejects_bad_r7_voltage_legacy_and_ccs(void)
{
    const uint32_t invalid_r7[] = { 0x1abu, 0x2aau, 1u << 22u };
    const pf_sd_detail_error_t errors[] = {
        PF_SD_DETAIL_R7, PF_SD_DETAIL_VOLTAGE, PF_SD_DETAIL_LEGACY_CARD
    };
    for (unsigned i = 0u; i < 3u; ++i) {
        setUp();
        r7_value = invalid_r7[i];
        TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
        TEST_ASSERT_EQUAL(errors[i], native_host.last_error);
        TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
    }
    setUp();
    ocr_value &= ~PF_SD_NATIVE_OCR_HCS;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_OCR_CCS, native_host.last_error);
}

void test_response_crc_index_status_csd_and_width_failures_are_distinct(void)
{
    bad_crc_command = 8u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_RESPONSE_CRC, native_host.last_error);
    TEST_ASSERT_EQUAL_UINT32(1u, native_host.errors.command_crc_errors);
    setUp();
    bad_index_command = 8u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_RESPONSE_INDEX, native_host.last_error);
    setUp();
    status_value = PF_SD_NATIVE_R1_STATE_TRAN;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_CARD_STATUS, native_host.last_error);
    setUp();
    csd_structure = 0u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_LEGACY_CARD, native_host.last_error);
    setUp();
    four_bit_supported = false;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_FOUR_BIT, native_host.last_error);
}

void test_default_speed_lba_zero_failure_aborts_before_high_speed_attempt(void)
{
    pf_test_sdio_system_clock_hz = 250000000u;
    fail_default_read = true;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(1u, validation_reads);
    TEST_ASSERT_EQUAL_UINT(0u, switch_calls);
    TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
    pf_sd_info_t info;
    pf_sd_native_get_info(&info);
    TEST_ASSERT_TRUE(info.initialization_error);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_DATA_CRC, info.detail_error);
}

void test_opt_in_high_speed_checks_support_selection_and_validates_lba_zero(void)
{
    pf_test_sdio_system_clock_hz = 250000000u;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(2u, switch_calls);
    TEST_ASSERT_EQUAL_UINT(2u, validation_reads);
    TEST_ASSERT_EQUAL_UINT(3u, clock_request_count);
    TEST_ASSERT_EQUAL_UINT32(50000000u, clock_requests[2]);
    pf_sd_info_t info;
    pf_sd_native_get_info(&info);
    TEST_ASSERT_TRUE(info.high_speed);
    TEST_ASSERT_EQUAL_UINT32(50000000u, info.actual_clock_hz);
    TEST_ASSERT_EQUAL_UINT16(0x1234u, info.rca);
    TEST_ASSERT_EQUAL_UINT8(4u, info.bus_width);
    TEST_ASSERT_TRUE(info.cid_valid);
    TEST_ASSERT_EQUAL_UINT32(15564800u, info.block_count);
    for (unsigned i = 0u; i < 15u; ++i) {
        TEST_ASSERT_EQUAL_UINT8(i + 1u, info.cid[i]);
    }
    native_host.bus_width = 1u;
    pf_sd_native_get_info(&info);
    TEST_ASSERT_EQUAL_UINT8(1u, info.bus_width);
}

void test_high_speed_failure_has_one_controlled_default_timing_fallback(void)
{
    pf_test_sdio_system_clock_hz = 250000000u;
    fail_high_speed_read = true;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(3u, switch_calls);
    TEST_ASSERT_EQUAL_UINT(3u, validation_reads);
    TEST_ASSERT_EQUAL_UINT(4u, clock_request_count);
    TEST_ASSERT_EQUAL_UINT32(25000000u, clock_requests[3]);
    TEST_ASSERT_FALSE(native_info.high_speed);
    TEST_ASSERT_EQUAL_UINT32(1u, native_info.high_speed_fallbacks);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_DATA_CRC, native_info.high_speed_error);
}

void test_unsupported_or_rejected_high_speed_is_nonfatal_at_25mhz(void)
{
    pf_test_sdio_system_clock_hz = 250000000u;
    high_speed_supported = false;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(1u, switch_calls);
    TEST_ASSERT_EQUAL_UINT(2u, clock_request_count);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_HIGH_SPEED_UNSUPPORTED, native_info.high_speed_error);
    setUp();
    pf_test_sdio_system_clock_hz = 250000000u;
    high_speed_selected = false;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(2u, validation_reads);
    TEST_ASSERT_FALSE(native_info.high_speed);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_HIGH_SPEED_REJECTED, native_info.high_speed_error);
}

void test_acmd41_uses_time_bound_and_one_cleanup_path(void)
{
    pf_test_sdio_time_step = 500000u;
    ocr_value = 0x40ff8000u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_INIT_TIMEOUT, native_host.last_error);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
    TEST_ASSERT_EQUAL_UINT(0u, validation_reads);
}

void test_selected_card_status_precedes_scr_and_host_width_follows_acmd6(void)
{
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    const uint8_t expected[] = {0u, 8u, 55u, 41u, 2u, 3u, 9u, 7u, 13u, 55u, 6u, 13u};
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), command_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, commands, sizeof(expected));
    setUp();
    fail_command = 6u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT8(1u, native_host.bus_width);
}

void test_r6_errors_and_zero_rca_are_rejected(void)
{
    r6_value = 0x12348000u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_CARD_STATUS, native_host.last_error);
    setUp();
    r6_value = 0u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_CARD_STATUS, native_host.last_error);
}

void test_failed_50mhz_and_25mhz_validation_aborts_without_a_second_fallback(void)
{
    pf_test_sdio_system_clock_hz = 250000000u;
    fail_high_speed_read = true;
    fail_validation_read = 3u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(4u, clock_request_count);
    TEST_ASSERT_EQUAL_UINT(3u, validation_reads);
    TEST_ASSERT_EQUAL_UINT32(1u, native_info.high_speed_fallbacks);
    TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
}

void test_cmd6_check_crc_failure_requires_proven_default_speed_operation(void)
{
    pf_test_sdio_system_clock_hz = 250000000u;
    fail_switch_check = true;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(2u, validation_reads);
    TEST_ASSERT_EQUAL_UINT(2u, clock_request_count);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_DATA_CRC, native_info.high_speed_error);
    setUp();
    pf_test_sdio_system_clock_hz = 250000000u;
    fail_switch_check = true;
    fail_validation_read = 2u;
    TEST_ASSERT_EQUAL(PF_SD_INIT_FAILED, pf_sd_native_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
}
