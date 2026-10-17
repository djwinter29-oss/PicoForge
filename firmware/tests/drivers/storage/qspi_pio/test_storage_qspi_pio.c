#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "storage_bus.h"
#include "storage_qspi_pio.h"
#include "nor_flash_transport.h"
#include <string.h>

serial_fake_t serial_fake;
serial_pio_t serial_pio[2];
static pf_storage_qspi_pio_t context;

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
    context = (pf_storage_qspi_pio_t){
        .pio = pio0, .pins.clock_gpio = 6, .pins.data_gpio = {0, 1, 2, 3},
        .pins.cs_gpio = 7, .clock_hz = 10000000,
    };
}

void tearDown(void) { pf_storage_qspi_pio_deinit(&context); }

void test_quad_read_clocks_single_command_address_dummy_then_quad_payload(void)
{
    pf_storage_bus_t bus = pf_storage_qspi_pio_bus(&context);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    serial_fake.input = 0xa;
    uint8_t input[2];
    pf_storage_serial_transaction_t command = { .command = 0x6b, .address_bytes = 3,
        .address = 0x123456, .dummy_cycles = 8, .data_lanes = 4, .rx = input, .length = 2 };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_transfer(&bus, &command));
    TEST_ASSERT_EQUAL_UINT(8 + 24 + 8 + 4, serial_fake.symbols);
    const uint8_t expected_command[] = {0, 1, 1, 0, 1, 0, 1, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_command, serial_fake.bytes, sizeof(expected_command));
    TEST_ASSERT_EQUAL_HEX8(0xaa, input[0]);
    TEST_ASSERT_EQUAL_HEX8(0xaa, input[1]);
    TEST_ASSERT_FALSE(serial_fake.selected);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.lanes);
    TEST_ASSERT_EQUAL_UINT32((1u << 6) | 1u, serial_fake.output_mask);
}

void test_quad_write_outputs_most_significant_nibble_first(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_qspi_pio_init(&context));
    uint8_t output = 0xa5;
    pf_storage_serial_transaction_t command = {
        .command = 0x32, .data_lanes = 4, .tx = &output, .length = 1,
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_qspi_pio_transfer(&context, &command));
    TEST_ASSERT_EQUAL_UINT(10, serial_fake.symbols);
    TEST_ASSERT_EQUAL_HEX8(0xa, serial_fake.bytes[8]);
    TEST_ASSERT_EQUAL_HEX8(0x5, serial_fake.bytes[9]);
}

void test_fifo_timeout_releases_cs_programs_and_state_machine(void)
{
    pf_nor_transport_t transport;
    pf_storage_bus_t bus = pf_storage_qspi_pio_bus(&context);
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
    TEST_ASSERT_TRUE(transport.quad_supported);
    serial_fake.fifo_full = true;
    pf_nor_transaction_t command = { .command = 6, .data_lanes = 1 };
    TEST_ASSERT_EQUAL(PF_NOR_TIMEOUT, transport.execute(transport.context, &command));
    TEST_ASSERT_FALSE(serial_fake.selected || context.initialized);
    TEST_ASSERT_EQUAL_UINT(2, serial_fake.removes);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.unclaim);
}

void test_invalid_qspi_shape_does_not_touch_cs_and_clock_reports_quantized_rate(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_qspi_pio_init(&context));
    pf_storage_serial_transaction_t command = { .command = 6, .data_lanes = 2 };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_qspi_pio_transfer(&context, &command));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
    uint32_t actual;
    pf_storage_bus_t bus = pf_storage_qspi_pio_bus(&context);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_set_clock(&bus, 8000000, &actual));
    TEST_ASSERT_EQUAL_UINT32(8000000, actual);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_bus_set_clock(&bus, 1, &actual));
}

void test_program_capacity_failure_releases_claim_without_selecting_device(void)
{
    serial_fake.no_program = true;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_storage_qspi_pio_init(&context));
    TEST_ASSERT_FALSE(context.initialized);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.unclaim);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
}
