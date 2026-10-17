#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_TEST_SDIO_WIRE_ONLY 1
#include "sdio_sdk_stubs.h"
#include "../../../../src/drivers/storage/sdio_pio/sdio_pio.c"
#include "../../../../src/drivers/storage/bus.c"
#include "../../../../src/drivers/storage/sd_wire.c"
#include "sdio_sdk_stubs.c"

#if defined(PF_SD_BLOCK_H) || defined(PF_SD_NATIVE_HOST_H) || defined(PF_NOR_H)
#error "Native transport tests must compile and link without device protocol headers or sources"
#endif

bool pf_test_sdio_expect_streams_cleared;
bool pf_test_sdio_streams_were_cleared;
bool pf_test_sdio_can_add_program;
unsigned pf_test_sdio_removed_programs;

static pf_storage_sd_wire_error_t observed_error;
static pf_storage_sd_wire_phase_t observed_phase;
static unsigned waits;

static void on_error(void *context, pf_storage_sd_wire_error_t error, pf_storage_sd_wire_phase_t phase)
{
    TEST_ASSERT_NOT_NULL(context);
    observed_error = error;
    observed_phase = phase;
}

static void on_wait(PIO pio, uint32_t flag)
{
    TEST_ASSERT_EQUAL_PTR(pio0, pio);
    TEST_ASSERT_EQUAL_UINT(1u, flag);
    ++waits;
}

static pf_sdio_pio_config_t pins(void)
{
    return (pf_sdio_pio_config_t){
        .pio = pio0, .pins.command_gpio = 1u, .pins.clock_gpio = 0u, .pins.data_gpio = {2u, 3u, 4u, 5u},
    };
}

void setUp(void)
{
    pf_test_sdio_can_add_program = true;
    pf_test_sdio_removed_programs = 0u;
    pf_test_sdio_dma_busy = false;
    pf_test_sdio_dma_busy_delay = 0u;
    pf_test_sdio_dma_poll_hook = NULL;
    pf_test_sdio_data_wait_ok = true;
    pf_test_sdio_data_irq = true;
    pf_test_sdio_force_tx_stall = false;
    pf_test_sdio_command_fail = 0u;
    pf_test_sdio_command_error = 0u;
    pf_test_sdio_corrupt_command_crc = false;
    pf_test_sdio_command_count = 0u;
    pf_test_sdio_time_step = 1u;
    pf_test_sdio_system_clock_hz = 150000000u;
    pf_test_sdio_data_fifo_words = 1u;
    pf_test_sdio_data_response = 0x0bu;
    pf_test_sdio_rx_wire_size = 0u;
    pf_test_sdio_batch_rx_size = 0u;
    pf_test_sdio_trace_enabled = false;
    pf_test_sdio_clock_auto_toggle = false;
    pf_test_sdio_response_word_limit = 5u;
    pf_test_sdio_clock_rising_edges = 0u;
    observed_error = PF_STORAGE_SD_WIRE_ERROR_NONE;
    waits = 0u;
}

void tearDown(void) {}

void test_rx_uses_system_clock_before_and_after_high_speed_tx(void)
{
    const uint32_t clocks[] = {125000000u, 150000000u, 200000000u, 250000000u};
    for (unsigned index = 0u; index < sizeof(clocks) / sizeof(clocks[0]); ++index) {
        pf_test_sdio_system_clock_hz = clocks[index];
        pf_sdio_pio_t host = {0};
        pf_sdio_pio_config_t config = pins();
        TEST_ASSERT_TRUE(pf_sdio_pio_init(&host, &config));
        TEST_ASSERT_EQUAL_UINT32(256u, pf_test_sdio_sm_divider[host.data_sm]);
        if (clocks[index] >= 200000000u) {
            TEST_ASSERT_TRUE(pf_sdio_pio_set_clock(&host, 50000000u));
            TEST_ASSERT_TRUE(host.data_rx_fast);
            TEST_ASSERT_EQUAL_UINT32(256u, pf_test_sdio_sm_divider[host.data_sm]);
            TEST_ASSERT_TRUE(pf_sdio_pio_tx_prepare(&host));
            TEST_ASSERT_EQUAL_UINT32(host.clock_div_ticks / 2u, pf_test_sdio_sm_divider[host.data_sm]);
            TEST_ASSERT_TRUE(pf_sdio_pio_restore_command(&host));
            TEST_ASSERT_EQUAL_UINT32(256u, pf_test_sdio_sm_divider[host.data_sm]);
        }
        pf_sdio_pio_deinit(&host);
    }
}

void test_native_descriptor_lifecycle_has_no_serial_or_device_protocol_dependency(void)
{
    pf_sdio_pio_t host = {.config = pins()};
    pf_storage_bus_t bus = pf_sdio_pio_bus(&host);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NATIVE_SD, bus.mode);
    TEST_ASSERT_NULL(bus.ops->serial);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    TEST_ASSERT_TRUE(host.dma_claimed);
    TEST_ASSERT_EQUAL(PF_SDIO_PIO_CLOCK_UNOWNED, host.clock_owner);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_storage_bus_init(&bus));
    TEST_ASSERT_TRUE(host.dma_claimed);
    uint32_t actual;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_set_clock(&bus, 25000000u, &actual));
    TEST_ASSERT_EQUAL_UINT32(25000000u, actual);
    TEST_ASSERT_TRUE(pf_storage_sd_wire_valid(&bus));
    pf_storage_bus_abort(&bus);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    pf_storage_bus_deinit(&bus);
    TEST_ASSERT_NULL(host.pio);
    TEST_ASSERT_FALSE(host.dma_claimed);
}

void test_common_lifecycle_retains_configuration_and_wait_binding_for_retry_and_reinitialization(void)
{
    pf_sdio_pio_t host = {.config = pins(), .wait_hook = on_wait};
    pf_storage_bus_t transport = pf_sdio_pio_bus(&host);
    TEST_ASSERT_TRUE(pf_storage_bus_valid(&transport, PF_STORAGE_BUS_NATIVE_SD));
    pf_test_sdio_can_add_program = false;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_storage_bus_init(&transport));
    TEST_ASSERT_EQUAL_PTR(pio0, host.config.pio);
    TEST_ASSERT_TRUE(host.wait_hook == on_wait);
    pf_test_sdio_can_add_program = true;
    for (unsigned attempt = 0u; attempt < 2u; ++attempt) {
        TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&transport));
        TEST_ASSERT_TRUE(pf_storage_sd_wire_state(&transport).ready);
        TEST_ASSERT_TRUE(host.wait_hook == on_wait);
        TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&transport, on_error, &host, NULL));
        TEST_ASSERT_TRUE(host.error_hook == on_error);
        pf_storage_bus_deinit(&transport);
        TEST_ASSERT_FALSE(pf_storage_sd_wire_state(&transport).ready);
        TEST_ASSERT_EQUAL_PTR(pio0, host.config.pio);
        TEST_ASSERT_TRUE(host.wait_hook == on_wait);
    }
}

void test_native_raw_command_and_tx_do_not_build_crc_or_interpret_response_tokens(void)
{
    pf_sdio_pio_t host = {0};
    pf_sdio_pio_config_t config = pins();
    TEST_ASSERT_TRUE(pf_sdio_pio_init(&host, &config));
    uint8_t frame[6] = {0x46u, 0x11u, 0x22u, 0x33u, 0x44u, 0u};
    uint8_t response[6];
    size_t received;
    /* Deliberately invalid CRC: the physical transport must transmit the caller's frame unchanged. */
    TEST_ASSERT_TRUE(pf_sdio_pio_exchange(&host, frame, sizeof(frame), 48u,
                                        response, sizeof(response), &received, 100000u));
    TEST_ASSERT_EQUAL_UINT8(6u, pf_test_sdio_last_command);
    TEST_ASSERT_EQUAL_UINT(6u, received);
    TEST_ASSERT_TRUE(pf_sdio_pio_tx_prepare(&host));
    uint32_t image[2] = {0x12345678u, 0xabcdef01u};
    TEST_ASSERT_TRUE(pf_sdio_pio_tx_start(&host, image, 2u, 16u));
    uint32_t token;
    TEST_ASSERT_TRUE(pf_sdio_pio_tx_finish(&host, 1000u, &token));
    TEST_ASSERT_EQUAL_UINT32(0x0bu, token);
    TEST_ASSERT_TRUE(pf_sdio_pio_restore_command(&host));
    pf_sdio_pio_deinit(&host);
}

void test_native_invalid_pin_routing_and_raw_dma_shape_are_rejected_before_transfer(void)
{
    pf_sdio_pio_t host = {0};
    pf_sdio_pio_config_t config = pins();
    config.pins.data_gpio[2]++;
    TEST_ASSERT_FALSE(pf_sdio_pio_init(&host, &config));
    TEST_ASSERT_NULL(host.pio);
    config = pins();
    config.pins.clock_gpio = 9u;
    TEST_ASSERT_FALSE(pf_sdio_pio_init(&host, &config));
    config = pins();
    TEST_ASSERT_TRUE(pf_sdio_pio_init(&host, &config));
    uint32_t buffer[2];
    TEST_ASSERT_FALSE(pf_sdio_pio_rx_start(&host, buffer, 2u, 8u, 1u));
    TEST_ASSERT_FALSE(pf_sdio_pio_rx_start(&host, NULL, 1u, 8u, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    pf_sdio_pio_deinit(&host);
}

void test_native_rx_timeout_reports_generic_diagnostics_and_uses_bounded_wait_hook(void)
{
    pf_sdio_pio_t host = {0};
    pf_sdio_pio_config_t config = pins();
    TEST_ASSERT_TRUE(pf_sdio_pio_init(&host, &config));
    host.error_hook = on_error;
    host.hook_context = &host;
    host.wait_hook = on_wait;
    uint32_t buffer[2];
    TEST_ASSERT_TRUE(pf_sdio_pio_rx_start(&host, buffer, 2u, 16u, 1u));
    pf_test_sdio_dma_busy = true;
    TEST_ASSERT_FALSE(pf_sdio_pio_rx_finish(&host, 10u));
    TEST_ASSERT_GREATER_THAN_UINT(0u, waits);
    TEST_ASSERT_EQUAL(PF_STORAGE_SD_WIRE_ERROR_DMA_TIMEOUT, observed_error);
    TEST_ASSERT_EQUAL(PF_STORAGE_SD_WIRE_PHASE_DATA, observed_phase);
    pf_sdio_pio_rx_cleanup(&host);
    TEST_ASSERT_FALSE(pf_test_sdio_dma_busy);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    pf_sdio_pio_deinit(&host);
}

void test_native_clocks_use_requested_count_current_rate_and_deadline_without_card_commands(void)
{
    pf_sdio_pio_t host = {.config = pins()};
    pf_storage_bus_t bus = pf_sdio_pio_bus(&host);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&bus, on_error, &host, NULL));
    uint32_t actual;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_set_clock(&bus, 1000000u, &actual));
    pf_test_sdio_clock_gpio = host.config.pins.clock_gpio;
    pf_test_sdio_gpio_level = 0u;
    pf_test_sdio_clock_auto_toggle = true;
    TEST_ASSERT_TRUE(pf_storage_sd_wire_clocks(&bus, 13u, 1000u));
    TEST_ASSERT_EQUAL_UINT(13u, pf_test_sdio_clock_rising_edges);
    TEST_ASSERT_EQUAL_UINT32(actual, pf_storage_sd_wire_state(&bus).current_clock_hz);
    TEST_ASSERT_EQUAL(PF_SDIO_PIO_CLOCK_UNOWNED, host.clock_owner);
    TEST_ASSERT_FALSE(pf_storage_sd_wire_clocks(&bus, 1000u, 10u));
    TEST_ASSERT_LESS_THAN_UINT(1000u, pf_test_sdio_clock_rising_edges - 13u);
    pf_test_sdio_clock_auto_toggle = false;
    TEST_ASSERT_FALSE(pf_storage_sd_wire_clocks(&bus, 13u, 10u));
    TEST_ASSERT_EQUAL(PF_STORAGE_SD_WIRE_ERROR_PIO_TIMEOUT, observed_error);
    TEST_ASSERT_EQUAL(PF_STORAGE_SD_WIRE_PHASE_SETUP, observed_phase);
    TEST_ASSERT_EQUAL(PF_SDIO_PIO_CLOCK_UNOWNED, host.clock_owner);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    pf_storage_bus_deinit(&bus);
}

void test_native_raw_exchange_returns_short_long_and_partial_bytes_without_packing_leaks(void)
{
    pf_sdio_pio_t host = {.config = pins()};
    pf_storage_bus_t bus = pf_sdio_pio_bus(&host);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    uint8_t frame[] = {0x46u, 0x11u, 0x22u, 0x33u, 0x44u, 0u};
    uint8_t response[17];
    size_t received;
    TEST_ASSERT_TRUE(pf_storage_sd_wire_exchange(&bus, frame, sizeof(frame), 48u,
                                               response, sizeof(response), &received, 1000u));
    const uint8_t expected[] = {6u, 0u, 0u, 0u, 0u, 1u};
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), received);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, response, received);
    TEST_ASSERT_TRUE(pf_storage_sd_wire_exchange(&bus, frame, sizeof(frame), 136u,
                                               response, sizeof(response), &received, 1000u));
    TEST_ASSERT_EQUAL_UINT(17u, received);
    for (size_t index = 0u; index < received; ++index) {
        TEST_ASSERT_EQUAL_HEX8(0x80u + index, response[index]);
    }
    pf_test_sdio_response_word_limit = 1u;
    memset(response, 0x55, sizeof(response));
    TEST_ASSERT_FALSE(pf_storage_sd_wire_exchange(&bus, frame, sizeof(frame), 48u,
                                                response, sizeof(response), &received, 10u));
    TEST_ASSERT_EQUAL_UINT(4u, received);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, response, received);
    TEST_ASSERT_EQUAL_HEX8(0x55u, response[received]);
    unsigned calls = pf_test_sdio_command_count;
    TEST_ASSERT_FALSE(pf_storage_sd_wire_exchange(&bus, frame, sizeof(frame), 48u,
                                                response, 5u, &received, 100u));
    TEST_ASSERT_EQUAL_UINT(0u, received);
    TEST_ASSERT_FALSE(pf_storage_sd_wire_exchange(&bus, frame, 5u, 48u,
                                                response, sizeof(response), &received, 100u));
    TEST_ASSERT_EQUAL_UINT(0u, received);
    TEST_ASSERT_EQUAL_UINT(calls, pf_test_sdio_command_count);
    pf_storage_bus_deinit(&bus);
}
