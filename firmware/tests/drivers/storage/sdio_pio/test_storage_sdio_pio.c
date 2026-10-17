#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_TEST_SDIO_WIRE_ONLY 1
#include "sdio_sdk_stubs.h"
#include "../../../../src/drivers/storage/sdio_pio/storage_sdio_pio.c"
#include "../../../../src/drivers/storage/storage_bus.c"
#include "sdio_sdk_stubs.c"

#if defined(PF_SD_BLOCK_H) || defined(PF_SD_NATIVE_HOST_H) || defined(PF_NOR_H)
#error "Native transport tests must compile and link without device protocol headers or sources"
#endif

bool pf_test_sdio_expect_streams_cleared;
bool pf_test_sdio_streams_were_cleared;
bool pf_test_sdio_can_add_program;
unsigned pf_test_sdio_removed_programs;

static pf_storage_sdio_error_t observed_error;
static pf_storage_sdio_phase_t observed_phase;
static unsigned waits;

static void on_error(void *context, pf_storage_sdio_error_t error, pf_storage_sdio_phase_t phase)
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

static pf_storage_sdio_pio_config_t pins(void)
{
    return (pf_storage_sdio_pio_config_t){
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
    observed_error = PF_STORAGE_SDIO_ERROR_NONE;
    waits = 0u;
}

void tearDown(void) {}

void test_native_descriptor_lifecycle_has_no_serial_or_device_protocol_dependency(void)
{
    pf_storage_sdio_pio_t host = {.config = pins()};
    pf_storage_bus_t bus = pf_storage_sdio_pio_bus(&host);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_SDIO_PIO, bus.mode);
    TEST_ASSERT_NULL(bus.ops->transfer);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    TEST_ASSERT_TRUE(host.dma_claimed);
    TEST_ASSERT_EQUAL(PF_STORAGE_SDIO_CLOCK_UNOWNED, host.clock_owner);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_storage_bus_init(&bus));
    TEST_ASSERT_TRUE(host.dma_claimed);
    uint32_t actual;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_set_clock(&bus, 25000000u, &actual));
    TEST_ASSERT_EQUAL_UINT32(25000000u, actual);
    pf_storage_serial_transaction_t serial = {.data_lanes = 1u};
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_bus_transfer(&bus, &serial));
    pf_storage_bus_abort(&bus);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    pf_storage_bus_deinit(&bus);
    TEST_ASSERT_NULL(host.pio);
    TEST_ASSERT_FALSE(host.dma_claimed);
}

void test_native_raw_command_and_tx_do_not_build_crc_or_interpret_response_tokens(void)
{
    pf_storage_sdio_pio_t host = {0};
    pf_storage_sdio_pio_config_t config = pins();
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_init(&host, &config));
    uint8_t frame[6] = {0x46u, 0x11u, 0x22u, 0x33u, 0x44u, 0u};
    uint32_t response[2];
    /* Deliberately invalid CRC: the physical transport must transmit the caller's frame unchanged. */
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_command(&host, frame, 48u, response, 2u));
    TEST_ASSERT_EQUAL_UINT8(6u, pf_test_sdio_last_command);
    TEST_ASSERT_EQUAL_UINT(2u, host.last_response_word_count);
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_tx_prepare(&host));
    uint32_t image[2] = {0x12345678u, 0xabcdef01u};
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_tx_start(&host, image, 2u, 16u));
    uint32_t token;
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_tx_finish(&host, 1000u, &token));
    TEST_ASSERT_EQUAL_UINT32(0x0bu, token);
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_restore_command(&host));
    pf_storage_sdio_pio_deinit(&host);
}

void test_native_invalid_pin_routing_and_raw_dma_shape_are_rejected_before_transfer(void)
{
    pf_storage_sdio_pio_t host = {0};
    pf_storage_sdio_pio_config_t config = pins();
    config.pins.data_gpio[2]++;
    TEST_ASSERT_FALSE(pf_storage_sdio_pio_init(&host, &config));
    TEST_ASSERT_NULL(host.pio);
    config = pins();
    config.pins.clock_gpio = 9u;
    TEST_ASSERT_FALSE(pf_storage_sdio_pio_init(&host, &config));
    config = pins();
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_init(&host, &config));
    uint32_t buffer[2];
    TEST_ASSERT_FALSE(pf_storage_sdio_pio_rx_start(&host, buffer, 2u, 8u, 1u));
    TEST_ASSERT_FALSE(pf_storage_sdio_pio_rx_start(&host, NULL, 1u, 8u, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    pf_storage_sdio_pio_deinit(&host);
}

void test_native_rx_timeout_reports_generic_diagnostics_and_uses_bounded_wait_hook(void)
{
    pf_storage_sdio_pio_t host = {0};
    pf_storage_sdio_pio_config_t config = pins();
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_init(&host, &config));
    host.error_hook = on_error;
    host.hook_context = &host;
    host.wait_hook = on_wait;
    uint32_t buffer[2];
    TEST_ASSERT_TRUE(pf_storage_sdio_pio_rx_start(&host, buffer, 2u, 16u, 1u));
    pf_test_sdio_dma_busy = true;
    TEST_ASSERT_FALSE(pf_storage_sdio_pio_rx_finish(&host, 10u));
    TEST_ASSERT_GREATER_THAN_UINT(0u, waits);
    TEST_ASSERT_EQUAL(PF_STORAGE_SDIO_ERROR_DMA_TIMEOUT, observed_error);
    TEST_ASSERT_EQUAL(PF_STORAGE_SDIO_PHASE_DATA, observed_phase);
    pf_storage_sdio_pio_rx_cleanup(&host);
    TEST_ASSERT_FALSE(pf_test_sdio_dma_busy);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    pf_storage_sdio_pio_deinit(&host);
}
