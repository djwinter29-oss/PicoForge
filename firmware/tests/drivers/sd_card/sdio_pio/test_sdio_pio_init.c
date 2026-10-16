#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"

#include <string.h>

#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio.c"
#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c"
#include "../../../../src/drivers/sd_card/sd_card_protocol.c"
#include "sdio_sdk_stubs.c"

pf_sdio_pio_host_t *pf_test_sdio_observed_host;
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

void setUp(void)
{
    memset(&sdio_pio_host, 0, sizeof(sdio_pio_host));
    memset(&active_config, 0, sizeof(active_config));
    memset(card_cid, 0, sizeof(card_cid));
    card_sector_count = 15564800u;
    card_block_addressed = true;
    configuration_set = true;
    card_initialized = true;
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_NONE;
    active_config = (pf_sdio_pio_config_t){
        .pio = pio0,
        .command_gpio = 1u,
        .data_gpio = {2u, 3u, 4u, 5u},
        .clock_gpio = 0u,
        .clock_hz = 25000000u
    };
    sdio_pio_host.current_clock_hz = 25000000u;
    sdio_pio_host.data_rx_multiblock_supported = true;
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
}

void tearDown(void) {}

bool pf_sdio_pio_host_init(pf_sdio_pio_host_t *host, const pf_sdio_pio_config_t *config)
{ (void)config; host->pio = pio0; host->current_clock_hz = 400000u; return true; }
void pf_sdio_pio_host_deinit(pf_sdio_pio_host_t *host) { (void)host; }
bool pf_sdio_pio_host_set_clock(pf_sdio_pio_host_t *host, uint32_t hz)
{
    TEST_ASSERT_LESS_THAN_UINT(4u, clock_request_count);
    clock_requests[clock_request_count++] = hz;
    if (hz > PF_SDIO_PIO_TRANSFER_CLOCK_HZ) return false;
    host->current_clock_hz = hz;
    return true;
}
bool pf_sdio_pio_host_command(pf_sdio_pio_host_t *host, uint8_t command, uint32_t argument,
                              uint16_t bits, uint32_t *response, size_t capacity)
{ (void)host; (void)command; (void)argument; (void)bits; (void)response; (void)capacity; return true; }
bool pf_sdio_pio_host_command_bytes(pf_sdio_pio_host_t *host, uint8_t command, uint32_t argument,
                                    uint16_t bits, uint8_t *response, size_t capacity)
{
    (void)host;
    if ((fail_command != 0u) && (command == fail_command)) return false;
    (void)argument;
    if (command == 0u) ++card_init_calls;
    if ((bits == 0u) && (response == NULL) && (capacity == 0u)) return true;
    if ((response == NULL) || (capacity < (bits / 8u))) return false;
    memset(response, 0, capacity);
    if (command == 2u || command == 9u) {
        uint8_t *payload = response + 1u;
        if (command == 2u) {
            for (unsigned i = 0u; i < 15u; ++i) {
                payload[i] = (uint8_t)(i + 1u);
            }
        } else {
            const uint32_t c_size = 15199u;
            payload[0] = 0x40u;
            payload[3] = csd_transfer_speed;
            payload[7] = (uint8_t)(c_size >> 16u);
            payload[8] = (uint8_t)(c_size >> 8u);
            payload[9] = (uint8_t)c_size;
        }
        response[16] = (uint8_t)((pf_sdio_pio_crc7(payload, 15u) << 1u) | 1u);
        return true;
    }
    response[0] = (command == 41u) ? 0x3fu : command;
    uint32_t response_argument = 0u;
    if (command == 8u) response_argument = 0x1aau;
    if (command == 3u) response_argument = 0x12340000u;
    if (command == 41u) response_argument = 0xc0ff8000u;
    response[1] = (uint8_t)(response_argument >> 24u);
    response[2] = (uint8_t)(response_argument >> 16u);
    response[3] = (uint8_t)(response_argument >> 8u);
    response[4] = (uint8_t)response_argument;
    response[5] = (uint8_t)((pf_sdio_pio_crc7(response, 5u) << 1u) | 1u);
    return true;
}
bool pf_sdio_pio_host_read_sector(pf_sdio_pio_host_t *host, uint32_t sector, bool addressed,
                                  uint8_t block[512])
{ (void)host; (void)sector; (void)addressed; (void)block; return read_ok; }
bool pf_sdio_pio_host_read_scr(pf_sdio_pio_host_t *host, uint32_t address, uint8_t scr[8])
{
    (void)host;
    (void)address;
    memset(scr, 0, 8u);
    scr[0] = 0x02u;
    scr[1] = 0x04u;
    return true;
}
bool pf_sdio_pio_host_read_switch_status(pf_sdio_pio_host_t *host, uint32_t argument,
                                         uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE])
{
    TEST_FAIL_MESSAGE("Fixed 25 MHz initialization must not negotiate CMD6 High Speed");
    (void)host;
    (void)argument;
    (void)status;
    return false;
}
bool pf_sdio_pio_host_read_blocks(pf_sdio_pio_host_t *host, uint32_t first, bool addressed,
                                  uint8_t *buffer, uint32_t count)
{
    (void)addressed;
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
bool pf_sdio_pio_host_write_sector(pf_sdio_pio_host_t *host, uint32_t first, bool addressed,
                                   const uint8_t *buffer)
{ (void)host; (void)first; (void)addressed; (void)buffer; ++single_writes; return single_write_ok; }
bool pf_sdio_pio_host_write_stream_continues(const pf_sdio_pio_host_t *host, uint32_t sector)
{ return host->write_stream_open && host->write_stream_next_sector == sector; }
bool pf_sdio_pio_host_write_blocks(pf_sdio_pio_host_t *host, uint32_t first, bool addressed,
                                   const uint8_t *buffer, uint32_t count)
{
    (void)addressed; (void)buffer; ++batch_writes;
    host->write_stream_open = batch_write_ok;
    host->write_stream_next_sector = first + count;
    return batch_write_ok;
}
bool pf_sdio_pio_host_recover(pf_sdio_pio_host_t *host)
{
    ++recovery_calls;
    if (recover_failure_call != 0u && recovery_calls == recover_failure_call) return false;
    if (recovery_ok) {
        host->write_stream_open = false;
        host->read_stream_unresolved = false;
        host->mode_restore_failed = false;
    }
    return recovery_ok;
}
void pf_sdio_pio_host_forget_removed_card(pf_sdio_pio_host_t *host)
{
    ++forget_calls;
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
}

void test_init_config_validation_accepts_supported_clocks_and_rejects_bounds(void)
{
    pf_sdio_pio_config_t config = active_config;

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
    uint8_t buffer[2u * PF_SD_CARD_BLOCK_SIZE] = {0};

    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, transfer_blocks(0u, NULL, NULL, 0u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT,
                      pf_sdio_pio_read_blocks(0u, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT,
                      pf_sdio_pio_read_blocks(card_sector_count, buffer, 1u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_read_blocks(0u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, reads);
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);

    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_write_blocks(0u, NULL, 0u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_write_blocks(0u, buffer, 0u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_write_blocks(0u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, single_writes);
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_write_blocks(1u, buffer, 2u));
    TEST_ASSERT_EQUAL_UINT(1u, batch_writes);

    sdio_pio_host.write_stream_open = true;
    sdio_pio_host.write_stream_next_sector = 3u;
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_write_blocks(3u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(2u, batch_writes);
}

void test_readiness_sync_card_removal_and_info_follow_card_detect(void)
{
    pf_sd_card_info_t info;

    active_config.use_card_detect = true;
    active_config.card_detect_gpio = 6u;
    active_config.card_detect_active_high = true;
    pf_sdio_pio_get_info(&info);
    TEST_ASSERT_TRUE(info.presence_known);
    TEST_ASSERT_TRUE(info.present);
    TEST_ASSERT_TRUE(info.initialized);
    TEST_ASSERT_EQUAL_UINT32(card_sector_count, info.block_count);
    TEST_ASSERT_EQUAL_UINT32(25000000u, info.bus_clock_hz);

    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_sync());
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);

    sdio_pio_host.write_stream_open = true;
    sdio_pio_host.read_stream_unresolved = true;
    pf_sdio_pio_confirm_card_removed();
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_FALSE(sdio_pio_host.write_stream_open);
    TEST_ASSERT_FALSE(sdio_pio_host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT(1u, forget_calls);
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sdio_pio_sync());

    card_initialized = true;
    pf_test_sdio_gpio_level = 0u;
    TEST_ASSERT_FALSE(pf_sdio_pio_is_initialized());
    pf_sdio_pio_get_info(&info);
    TEST_ASSERT_FALSE(info.present);
    TEST_ASSERT_FALSE(info.initialized);
}

void test_transfer_validation_checks_32_bit_sector_overflow(void)
{
    uint8_t block[PF_SD_CARD_BLOCK_SIZE] = {0};

    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT,
        pf_sd_card_validate_transfer(UINT32_MAX, block, 1u, UINT32_MAX));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK,
        pf_sd_card_validate_transfer(UINT32_MAX, NULL, 0u, 0u));
}

void test_card_init_uses_fixed_clock_without_probing_or_switch_negotiation(void)
{
    active_config.clock_hz = 0u;
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_init(&active_config));
    TEST_ASSERT_EQUAL_UINT32(25000000u, sdio_pio_host.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
    TEST_ASSERT_EQUAL_UINT(2u, clock_request_count);
    TEST_ASSERT_EQUAL_UINT32(400000u, clock_requests[0]);
    TEST_ASSERT_EQUAL_UINT32(25000000u, clock_requests[1]);
}

void test_card_init_failure_does_not_retry_at_other_clocks(void)
{
    fail_command = 2u;
    TEST_ASSERT_EQUAL(PF_SD_CARD_INIT_FAILED, pf_sdio_pio_init(&active_config));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT(1u, recovery_calls);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
}

void test_fixed_clock_rejects_other_requests_before_card_commands(void)
{
    active_config.clock_hz = 50000000u;
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT, pf_sdio_pio_init(&active_config));
    TEST_ASSERT_EQUAL_UINT(0u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, recovery_calls);
}

void test_runtime_read_failure_is_not_replayed_and_clock_stays_fixed(void)
{
    uint8_t block[PF_SD_CARD_BLOCK_SIZE] = {0};
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_init(&active_config));
    read_ok = false;
    read_fail_unresolved = true;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sdio_pio_read_blocks(4096u, block, 1u));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT32(25000000u, sdio_pio_host.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT(1u, reads);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sdio_pio_read_blocks(4096u, block, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, reads);
}

void test_removed_media_never_attempts_read_or_reinitialization(void)
{
    uint8_t block[PF_SD_CARD_BLOCK_SIZE] = {0};
    active_config.use_card_detect = true;
    active_config.card_detect_gpio = 6u;
    active_config.card_detect_active_high = true;
    pf_test_sdio_gpio_level = 0u;
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sdio_pio_read_blocks(4096u, block, 1u));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
    TEST_ASSERT_EQUAL_UINT(0u, card_init_calls);
}

void test_explicit_reinitialization_after_failed_read_restores_fixed_rate(void)
{
    uint8_t block[PF_SD_CARD_BLOCK_SIZE];
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_init(&active_config));
    read_ok = false;
    TEST_ASSERT_EQUAL(PF_SD_CARD_IO_ERROR, pf_sdio_pio_read_blocks(0u, block, 1u));
    read_ok = true;
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_init(&active_config));
    TEST_ASSERT_EQUAL_UINT32(25000000u, sdio_pio_host.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT(2u, card_init_calls);
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sdio_pio_read_blocks(0u, block, 1u));
}

void test_card_advertising_less_than_25mhz_is_rejected_without_fallback(void)
{
    csd_transfer_speed = 0x2au; /* 20 MHz TRAN_SPEED. */
    TEST_ASSERT_EQUAL(PF_SD_CARD_INIT_FAILED, pf_sdio_pio_init(&active_config));
    TEST_ASSERT_FALSE(card_initialized);
    TEST_ASSERT_EQUAL_UINT(1u, card_init_calls);
    TEST_ASSERT_EQUAL_UINT(1u, clock_request_count);
    TEST_ASSERT_EQUAL_UINT32(400000u, clock_requests[0]);
    TEST_ASSERT_EQUAL_UINT(0u, reads);
}
