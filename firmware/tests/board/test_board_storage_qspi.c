#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "storage_bus.h"
#include "storage_qspi_pio.h"
#include "nor_flash_transport.h"
#include <string.h>

#define PF_BOARD_STORAGE_QSPI 1
#include "../../src/board/board_profile.c"
#include "../../src/board/board_storage.c"

serial_fake_t serial_fake;
serial_pio_t serial_pio[2];
static pf_storage_bus_t bus;

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
}

void tearDown(void)
{
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
}

void test_quad_profile_prepares_only_a_descriptor_before_device_initialization(void)
{
    pf_nor_transport_t transport;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_board_storage_prepare(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_QSPI_PIO, bus.mode);
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
