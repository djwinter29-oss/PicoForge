#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "storage_transport.h"
#include "serial.h"
#include "qspi_pio.h"
#include "nor_transport.h"
#include <string.h>

serial_fake_t serial_fake;
serial_pio_t serial_pio[2];
static pf_qspi_pio_t context;

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
    context = (pf_qspi_pio_t){
        .pio = pio0, .pins.clock_gpio = 6, .pins.data_gpio = {0, 1, 2, 3},
        .pins.cs_gpio = 7, .clock_hz = 10000000,
    };
}

void tearDown(void) { pf_qspi_pio_deinit(&context); }

void test_quad_read_clocks_single_command_address_dummy_then_quad_payload(void)
{
    pf_storage_transport_t bus = pf_qspi_pio_transport(&context);
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_storage_transport_init(&bus));
    serial_fake.input = 0xa;
    uint8_t input[2];
    const uint8_t header[] = {0x6b, 0x12, 0x34, 0x56};
    pf_storage_serial_phase_t phases[] = {
        {.tx = header, .bit_count = sizeof(header) * 8u, .lanes = 1u},
        {.bit_count = 8u, .lanes = 1u},
        {.rx = input, .bit_count = sizeof(input) * 8u, .lanes = 4u},
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_storage_serial_transfer(&bus, phases, 3u));
    TEST_ASSERT_EQUAL_UINT(8 + 24 + 8 + 4, serial_fake.symbols);
    const uint8_t expected_command[] = {0, 1, 1, 0, 1, 0, 1, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_command, serial_fake.bytes, sizeof(expected_command));
    TEST_ASSERT_EQUAL_HEX8(0xaa, input[0]);
    TEST_ASSERT_EQUAL_HEX8(0xaa, input[1]);
    TEST_ASSERT_EQUAL_UINT(1u, serial_fake.cs_low);
    TEST_ASSERT_FALSE(serial_fake.selected);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.lanes);
    TEST_ASSERT_EQUAL_UINT32((1u << 6) | 1u, serial_fake.output_mask);
}

void test_quad_write_outputs_most_significant_nibble_first(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_qspi_pio_init(&context));
    uint8_t output = 0xa5;
    uint8_t header = 0x32;
    pf_storage_serial_phase_t phases[] = {
        {.tx = &header, .bit_count = 8u, .lanes = 1u},
        {.tx = &output, .bit_count = 8u, .lanes = 4u},
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_qspi_pio_transfer(&context, phases, 2u));
    TEST_ASSERT_EQUAL_UINT(10, serial_fake.symbols);
    TEST_ASSERT_EQUAL_HEX8(0xa, serial_fake.bytes[8]);
    TEST_ASSERT_EQUAL_HEX8(0x5, serial_fake.bytes[9]);
}

void test_fifo_timeout_releases_cs_programs_and_state_machine(void)
{
    pf_nor_transport_t transport;
    pf_storage_transport_t bus = pf_qspi_pio_transport(&context);
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
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_qspi_pio_init(&context));
    pf_storage_serial_phase_t phases[] = {
        {.bit_count = 8u, .lanes = 1u}, {.bit_count = 8u, .lanes = 2u},
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_qspi_pio_transfer(&context, phases, 2u));
    uint8_t data = 0u;
    phases[1] = (pf_storage_serial_phase_t){.tx = &data, .rx = &data, .bit_count = 8u, .lanes = 4u};
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_qspi_pio_transfer(&context, phases, 2u));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
    uint32_t actual;
    pf_storage_transport_t bus = pf_qspi_pio_transport(&context);
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_storage_transport_set_clock(&bus, 8000000, &actual));
    TEST_ASSERT_EQUAL_UINT32(8000000, actual);
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_storage_transport_set_clock(&bus, 1, &actual));
}

void test_program_capacity_failure_releases_claim_without_selecting_device(void)
{
    serial_fake.no_program = true;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_NOT_READY, pf_qspi_pio_init(&context));
    TEST_ASSERT_FALSE(context.initialized);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.unclaim);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
}

void test_raw_quad_transfer_needs_no_opcode_and_clocks_partial_bytes_in_single_lane(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_qspi_pio_init(&context));
    uint8_t data = 0xa5u;
    const pf_storage_serial_phase_t phases[] = {
        {.bit_count = 3u, .lanes = 1u}, {.tx = &data, .bit_count = 8u, .lanes = 4u},
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_qspi_pio_transfer(&context, phases, 2u));
    const uint8_t expected[] = {1u, 1u, 1u, 0xau, 0x5u};
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), serial_fake.symbols);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, serial_fake.bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(1u, serial_fake.cs_low);
    TEST_ASSERT_FALSE(serial_fake.selected);
}

void test_quad_pin_allocation_rejects_swapped_and_out_of_range_lanes_before_acquisition(void)
{
    context.pins.data_gpio[1] = 2u;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_qspi_pio_init(&context));
    context.pins.data_gpio[1] = 1u;
    context.pins.data_gpio[3] = NUM_BANK0_GPIOS;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_qspi_pio_init(&context));
    TEST_ASSERT_FALSE(context.initialized);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.program_count);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
}
