#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "storage_bus.h"
#include "storage_spi.h"
#include "nor_transport.h"
#include <string.h>

serial_fake_t serial_fake;
spi_inst_t serial_spi[2] = {{0}, {1}};
static pf_storage_spi_t sd;
static pf_storage_spi_t nor;
static pf_storage_bus_t nor_bus;
static pf_nor_transport_t transport;

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
    sd = (pf_storage_spi_t){ .instance = spi0, .pins.clock_gpio = 2, .pins.mosi_gpio = 3,
        .pins.miso_gpio = 0, .pins.cs_gpio = 1, .clock_hz = 25000000 };
    nor = (pf_storage_spi_t){ .instance = spi0, .pins.clock_gpio = 2, .pins.mosi_gpio = 3,
        .pins.miso_gpio = 0, .pins.cs_gpio = 5, .clock_hz = 10000000, .timeout_us = 100000 };
    nor_bus = pf_storage_spi_bus(&nor);
}

void tearDown(void)
{
    pf_storage_spi_deinit(&sd);
    pf_storage_bus_deinit(&nor_bus);
}

void test_sd_raw_io_and_nor_transactions_share_one_controller_and_restore_device_clock(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&nor_bus, &transport));
    TEST_ASSERT_FALSE(transport.quad_supported);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.init);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_select(&sd, true));
    uint8_t value = 0xfc, received;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_exchange(&sd, &value, &received, 1));
    TEST_ASSERT_EQUAL_UINT32(25000000, serial_fake.hz);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_select(&sd, false));
    const uint8_t payload[] = {0xaa, 0x55};
    pf_nor_transaction_t command = { .command = 2, .address_bytes = 3, .address = 0x123456,
        .data_lanes = 1, .tx = payload, .length = sizeof(payload) };
    TEST_ASSERT_EQUAL(PF_NOR_OK, transport.execute(transport.context, &command));
    const uint8_t expected[] = {0xfc, 2, 0x12, 0x34, 0x56, 0xaa, 0x55};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, serial_fake.bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(10000000, serial_fake.hz);
    TEST_ASSERT_FALSE(serial_fake.selected);
    pf_storage_bus_deinit(&nor_bus);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.deinit);
    pf_storage_spi_deinit(&sd);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.deinit);
}

void test_nor_fifo_timeout_releases_cs_and_only_its_controller_reference(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&nor_bus, &transport));
    serial_fake.short_io = true;
    pf_nor_transaction_t command = { .command = 6, .data_lanes = 1 };
    TEST_ASSERT_EQUAL(PF_NOR_TIMEOUT, transport.execute(transport.context, &command));
    TEST_ASSERT_FALSE(serial_fake.selected || nor.initialized);
    TEST_ASSERT_TRUE(sd.initialized);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.deinit);
}

void test_sdk_short_bulk_io_is_reported_without_replay_or_implicit_cs_boundary(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_select(&sd, true));
    serial_fake.short_io = true;
    uint8_t output = 0xfc;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_IO_ERROR, pf_storage_spi_exchange(&sd, &output, NULL, 1));
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.length);
    TEST_ASSERT_TRUE(serial_fake.selected);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_select(&sd, false));
}

void test_invalid_or_quad_spi_transactions_never_assert_cs(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_init(&sd));
    pf_storage_serial_transaction_t command = { .command = 6, .data_lanes = 4 };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_spi_transfer(&sd, &command));
    command.data_lanes = 1; command.dummy_cycles = 1;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_spi_transfer(&sd, &command));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
}

void test_selected_device_prevents_another_device_from_clocking_the_bus(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&nor_bus, &transport));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_spi_select(&sd, true));
    pf_storage_spi_t another = sd;
    another.initialized = false;
    another.pins.cs_gpio = 8;
    unsigned cs_high = serial_fake.cs_high;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_storage_spi_init(&another));
    TEST_ASSERT_EQUAL_UINT(cs_high, serial_fake.cs_high);
    pf_nor_transaction_t command = { .command = 6, .data_lanes = 1 };
    TEST_ASSERT_EQUAL(PF_NOR_NOT_READY, transport.execute(transport.context, &command));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.length);
    TEST_ASSERT_TRUE(sd.initialized);
    TEST_ASSERT_TRUE(nor.initialized);
}

void test_generic_spi_dispatch_clocks_dummy_bytes_and_receives_payload(void)
{
    pf_storage_bus_t bus = pf_storage_spi_bus(&sd);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    serial_fake.input = 0x91;
    uint8_t input[2];
    pf_storage_serial_transaction_t command = { .command = 0x0b, .dummy_cycles = 16,
        .data_lanes = 1, .rx = input, .length = sizeof(input) };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_transfer(&bus, &command));
    const uint8_t expected[] = {0x0b, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, serial_fake.bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_HEX8(0x91, input[0]);
    TEST_ASSERT_FALSE(serial_fake.selected);
}
