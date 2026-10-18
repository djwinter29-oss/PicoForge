#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "bus.h"
#include "serial.h"
#include "qspi_pio.h"
#include "nor_transport.h"
#include "board_resources.h"
#include <string.h>

#define PF_BOARD_STORAGE_QSPI 1
#define pf_board_resources_get default_board_resources_get
#include "../../src/board/board_profile.c"
#undef pf_board_resources_get
static pf_board_resources_t test_profile;
const pf_board_resources_t *pf_board_resources_get(void) { return &test_profile; }
#include "../../src/board/board_storage.c"

serial_fake_t serial_fake;
serial_pio_t serial_pio[2];
static pf_storage_bus_t bus;

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
    test_profile = *default_board_resources_get();
}

void tearDown(void)
{
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
}

void test_quad_profile_prepares_only_a_descriptor_before_device_initialization(void)
{
    pf_nor_transport_t transport;
    test_profile.spi_index = 2u;
    test_profile.native_pins.command_gpio = PF_BOARD_PIN_UNASSIGNED;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&test_profile));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_QSPI, bus.mode);
    TEST_ASSERT_EQUAL_PTR(pio0, serial_storage.pio);
    TEST_ASSERT_EQUAL_UINT32(2u, serial_storage.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, serial_storage.pins.data_gpio[0]);
    TEST_ASSERT_EQUAL_UINT32(7u, serial_storage.pins.cs_gpio);
    TEST_ASSERT_EQUAL_UINT32(1000000u, serial_storage.clock_hz);
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.gpio_initialized);
    TEST_ASSERT_EQUAL_UINT(0u, serial_fake.program_count);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
    TEST_ASSERT_TRUE(transport.quad_supported);
    TEST_ASSERT_TRUE(serial_storage.initialized);
    pf_board_storage_release_pins();
    TEST_ASSERT_TRUE(serial_pins_prepared);
}

void test_failed_acquisition_can_be_cleaned_up_and_retried(void)
{
    pf_nor_transport_t transport;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_board_storage_prepare(NULL));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    serial_fake.no_program = true;
    TEST_ASSERT_EQUAL(PF_NOR_NOT_READY, pf_nor_transport_init(&bus, &transport));
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
    TEST_ASSERT_FALSE(serial_storage.initialized);
    serial_fake.no_program = false;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
}

void test_conflicting_profile_is_rejected_before_pio_or_gpio_changes(void)
{
    test_profile.monitor_uart_tx_gpio = test_profile.qspi_pins.data_gpio[3];
    TEST_ASSERT_FALSE(pf_board_resources_validate(&test_profile));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.gpio_initialized);
    TEST_ASSERT_EQUAL_UINT(0u, serial_fake.program_count);
    TEST_ASSERT_FALSE(serial_pins_prepared);
}

void test_quad_runtime_changes_never_modify_the_authoritative_profile(void)
{
    const pf_board_resources_t *profile = pf_board_resources_get();
    pf_board_resources_t saved = *profile;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_MEMORY(&profile->qspi_pins, &serial_storage.pins, sizeof(serial_storage.pins));
    serial_storage.pins.data_gpio[3] = 9u;
    serial_storage.clock_hz = 2000000u;
    TEST_ASSERT_EQUAL_MEMORY(&saved, profile, sizeof(saved));
    pf_board_storage_release_pins();
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL_MEMORY(&profile->qspi_pins, &serial_storage.pins, sizeof(serial_storage.pins));
    TEST_ASSERT_EQUAL_UINT32(profile->serial_clock_hz, serial_storage.clock_hz);
}
