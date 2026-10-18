#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "bus.h"
#include "serial.h"
#include "spi.h"
#include "nor_transport.h"
#include "board_resources.h"
#include <string.h>

#define PF_BOARD_STORAGE_QSPI 0
#define pf_board_resources_get default_board_resources_get
#include "board/board_profile.c"
#undef pf_board_resources_get
static pf_board_resources_t test_profile;
const pf_board_resources_t *pf_board_resources_get(void) { return &test_profile; }
#include "board/board_storage.c"

serial_fake_t serial_fake;
spi_inst_t serial_spi[2] = {{0}, {1}};
static pf_storage_bus_t bus;

void setUp(void)
{
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
    memset(&serial_fake, 0, sizeof(serial_fake));
    test_profile = *default_board_resources_get();
}

void tearDown(void)
{
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
}

void test_effective_spi_clock_is_zero_until_initialized(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, pf_board_storage_clock_hz());
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    serial_fake.hz = 31250000u;
    TEST_ASSERT_EQUAL_UINT32(31250000u, pf_board_storage_clock_hz());
}

void test_spi_board_owns_wp_hold_and_rejects_duplicate_init_without_changing_transport(void)
{
    pf_nor_transport_t transport;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.init);
    TEST_ASSERT_BITS_HIGH((1u << 5) | (1u << 6), serial_fake.high_pins);
    TEST_ASSERT_BITS_HIGH((1u << 5) | (1u << 6), serial_fake.output_pins);
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
    TEST_ASSERT_FALSE(transport.quad_supported);
    pf_board_storage_release_pins();
    TEST_ASSERT_BITS_HIGH((1u << 5) | (1u << 6), serial_fake.output_pins);
    TEST_ASSERT_TRUE(serial_pins_prepared);
    TEST_ASSERT_EQUAL_UINT32(100000, serial_storage.timeout_us);
    TEST_ASSERT_EQUAL_UINT32(40000000, serial_storage.clock_hz);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.init);
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
    TEST_ASSERT_BITS_LOW((1u << 5) | (1u << 6), serial_fake.output_pins);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.deinit);
}

void test_failed_transfer_requires_explicit_board_shutdown_before_reinitialization(void)
{
    pf_nor_transport_t transport;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
    serial_fake.short_io = true;
    pf_nor_transaction_t command = { .command = 6, .data_lanes = 1 };
    TEST_ASSERT_EQUAL(PF_NOR_TIMEOUT, transport.execute(transport.context, &command));
    TEST_ASSERT_FALSE(serial_storage.initialized);
    TEST_ASSERT_BITS_HIGH((1u << 5) | (1u << 6), serial_fake.output_pins);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_board_storage_prepare(&bus));
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
    TEST_ASSERT_BITS_LOW((1u << 5) | (1u << 6), serial_fake.output_pins);
    serial_fake.short_io = false;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
}

void test_invalid_output_and_idle_deinit_do_not_touch_pins(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_board_storage_prepare(NULL));
    pf_board_storage_release_pins();
    TEST_ASSERT_EQUAL_UINT32(0, serial_fake.gpio_initialized);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.init);
}

void test_conflicting_profile_is_rejected_before_idle_pin_or_bus_changes(void)
{
    test_profile.serial_hold_gpio = test_profile.monitor_uart_tx_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&test_profile));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.gpio_initialized);
    TEST_ASSERT_EQUAL_UINT(0u, serial_fake.init);
    TEST_ASSERT_FALSE(serial_pins_prepared);
}

void test_pin_preparation_can_be_released_after_bus_initialization_fails(void)
{
    pf_nor_transport_t transport;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    uint32_t requested = serial_storage.clock_hz;
    serial_storage.clock_hz = 0u;
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(&bus, &transport));
    serial_storage.clock_hz = requested;
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
    TEST_ASSERT_BITS_LOW((1u << 5) | (1u << 6), serial_fake.output_pins);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
}

void test_runtime_changes_do_not_modify_profile_and_next_preparation_restores_configuration(void)
{
    const pf_board_resources_t *profile = pf_board_resources_get();
    pf_board_resources_t saved = *profile;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_MEMORY(&profile->spi_pins, &serial_storage.pins, sizeof(serial_storage.pins));
    TEST_ASSERT_EQUAL_UINT32(profile->serial_timeout_us, serial_storage.timeout_us);
    serial_storage.pins.cs_gpio = 9u;
    serial_storage.clock_hz = 2000000u;
    TEST_ASSERT_EQUAL_MEMORY(&saved, profile, sizeof(saved));
    pf_board_storage_release_pins();
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_MEMORY(&profile->spi_pins, &serial_storage.pins, sizeof(serial_storage.pins));
    TEST_ASSERT_EQUAL_UINT32(profile->serial_clock_hz, serial_storage.clock_hz);
}
