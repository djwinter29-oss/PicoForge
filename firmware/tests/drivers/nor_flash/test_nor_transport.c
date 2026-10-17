#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "bus.h"
#include "serial.h"
#include "nor_transport.h"
#include <string.h>

serial_fake_t serial_fake;
static pf_storage_bus_t bus;
static pf_nor_transport_t transport;
static pf_storage_bus_result_t init_result;
static pf_storage_bus_result_t transfer_result;
static unsigned init_calls, transfer_calls, deinit_calls;
static int context;

static uint64_t now(void) { return ++serial_fake.now; }
static void delay(uint32_t milliseconds) { serial_fake.now += milliseconds * 1000u; }
static pf_storage_bus_result_t clock_rate(void *value, uint32_t hz, uint32_t *actual)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    *actual = hz;
    return PF_STORAGE_BUS_OK;
}

static pf_storage_bus_result_t initialize(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    ++init_calls;
    return init_result;
}

static void deinitialize(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    ++deinit_calls;
}

static pf_storage_bus_result_t transfer(void *value, const pf_storage_serial_phase_t *phases, size_t count)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    TEST_ASSERT_GREATER_THAN_UINT(0u, count);
    TEST_ASSERT_EQUAL_UINT8(0x9f, phases[0].tx[0]);
    ++transfer_calls;
    return transfer_result;
}

static const pf_storage_serial_ops_t serial = {.transfer = transfer, .lanes = 1u};
static const pf_storage_serial_ops_t quad = {.transfer = transfer, .lanes = 4u};
static const pf_storage_bus_ops_t operations = {
    .init = initialize, .deinit = deinitialize, .serial = &serial, .time_us = now,
    .abort = deinitialize, .set_clock = clock_rate, .delay_ms = delay,
};

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
    transport = (pf_nor_transport_t){0};
    bus = (pf_storage_bus_t){ .context = &context, .ops = &operations, .mode = PF_STORAGE_BUS_SPI };
    init_calls = transfer_calls = deinit_calls = 0;
    init_result = transfer_result = PF_STORAGE_BUS_OK;
}

void tearDown(void) {}

void test_one_adapter_dispatches_spi_and_quad_buses_and_supplies_clock(void)
{
    const pf_storage_bus_mode_t modes[] = {PF_STORAGE_BUS_SPI, PF_STORAGE_BUS_QSPI};
    pf_nor_transaction_t transaction = { .command = 0x9f, .data_lanes = 1 };
    pf_storage_bus_ops_t ops = operations;
    bus.ops = &ops;
    for (unsigned index = 0; index < 2; ++index) {
        bus.mode = modes[index];
        ops.serial = index == 0u ? &serial : &quad;
        TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
        TEST_ASSERT_EQUAL_PTR(&bus, transport.context);
        TEST_ASSERT_EQUAL(index != 0, transport.quad_supported);
        TEST_ASSERT_EQUAL(PF_NOR_OK, transport.execute(transport.context, &transaction));
        TEST_ASSERT_GREATER_THAN_UINT64(0, transport.time_us(transport.context));
    }
    TEST_ASSERT_EQUAL_UINT(2, init_calls);
    TEST_ASSERT_EQUAL_UINT(2, transfer_calls);
    TEST_ASSERT_EQUAL_UINT(0, deinit_calls);
}

void test_invalid_or_native_buses_never_initialize_or_publish_transport(void)
{
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(NULL, &transport));
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(&bus, NULL));
    bus.context = NULL;
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(&bus, &transport));
    bus.context = &context;
    pf_storage_bus_ops_t incomplete = operations;
    bus.ops = &incomplete;
    incomplete.serial = NULL;
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(&bus, &transport));
    incomplete = operations;
    incomplete.deinit = NULL;
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(&bus, &transport));
    bus.ops = &operations;
    bus.mode = PF_STORAGE_BUS_NATIVE_SD;
    TEST_ASSERT_EQUAL(PF_NOR_UNSUPPORTED, pf_nor_transport_init(&bus, &transport));
    incomplete = operations;
    incomplete.serial = NULL;
    bus.ops = &incomplete;
    TEST_ASSERT_EQUAL(PF_NOR_UNSUPPORTED, pf_nor_transport_init(&bus, &transport));
    TEST_ASSERT_EQUAL_UINT(0, init_calls);
    TEST_ASSERT_NULL(transport.execute);
}

void test_init_failure_is_propagated_without_publishing_transport(void)
{
    init_result = PF_STORAGE_BUS_NOT_READY;
    TEST_ASSERT_EQUAL(PF_NOR_NOT_READY, pf_nor_transport_init(&bus, &transport));
    TEST_ASSERT_NULL(transport.execute);
    TEST_ASSERT_EQUAL_UINT(1, init_calls);
    TEST_ASSERT_EQUAL_UINT(0, deinit_calls);
}

void test_only_io_errors_and_timeouts_deinitialize_without_replay(void)
{
    const pf_storage_bus_result_t results[] = {
        PF_STORAGE_BUS_INVALID_ARGUMENT, PF_STORAGE_BUS_NOT_READY, PF_STORAGE_BUS_UNSUPPORTED,
        PF_STORAGE_BUS_IO_ERROR, PF_STORAGE_BUS_TIMEOUT,
    };
    const pf_nor_result_t expected[] = {
        PF_NOR_INVALID_ARGUMENT, PF_NOR_NOT_READY, PF_NOR_UNSUPPORTED, PF_NOR_IO_ERROR, PF_NOR_TIMEOUT,
    };
    pf_nor_transaction_t transaction = { .command = 0x9f, .data_lanes = 1 };
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
    for (unsigned index = 0; index < 5; ++index) {
        transfer_result = results[index];
        TEST_ASSERT_EQUAL(expected[index], transport.execute(transport.context, &transaction));
        TEST_ASSERT_EQUAL_UINT(index + 1, transfer_calls);
        TEST_ASSERT_EQUAL_UINT(index < 3 ? 0 : index - 2, deinit_calls);
    }
    transaction.data_lanes = 2;
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, transport.execute(transport.context, &transaction));
    TEST_ASSERT_EQUAL_UINT(5, transfer_calls);
    TEST_ASSERT_EQUAL_UINT(2, deinit_calls);
}

static uint8_t payload[2];

static pf_storage_bus_result_t encoded_transfer(void *value,
                                                       const pf_storage_serial_phase_t *phases, size_t count)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    const uint8_t expected[] = {0x9fu, 0x12u, 0x34u, 0x56u};
    TEST_ASSERT_EQUAL_UINT(3u, count);
    TEST_ASSERT_EQUAL_UINT(32u, phases[0].bit_count);
    TEST_ASSERT_EQUAL_UINT(1u, phases[0].lanes);
    TEST_ASSERT_NULL(phases[0].rx);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, phases[0].tx, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(5u, phases[1].bit_count);
    TEST_ASSERT_EQUAL_UINT(1u, phases[1].lanes);
    TEST_ASSERT_NULL(phases[1].tx);
    TEST_ASSERT_NULL(phases[1].rx);
    TEST_ASSERT_EQUAL_UINT(16u, phases[2].bit_count);
    TEST_ASSERT_EQUAL_UINT(4u, phases[2].lanes);
    TEST_ASSERT_NULL(phases[2].tx);
    TEST_ASSERT_EQUAL_PTR(payload, phases[2].rx);
    return PF_STORAGE_BUS_OK;
}

void test_nor_adapter_builds_opcode_big_endian_address_dummy_and_payload_phases(void)
{
    pf_storage_serial_ops_t wire = {.transfer = encoded_transfer, .lanes = 4u};
    pf_storage_bus_ops_t ops = operations;
    ops.serial = &wire;
    bus.ops = &ops;
    bus.mode = PF_STORAGE_BUS_QSPI;
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_transport_init(&bus, &transport));
    pf_nor_transaction_t transaction = {
        .command = 0x9fu, .address_bytes = 3u, .address = 0x123456u,
        .dummy_cycles = 5u, .data_lanes = 4u, .rx = payload, .length = sizeof(payload),
    };
    TEST_ASSERT_EQUAL(PF_NOR_OK, transport.execute(transport.context, &transaction));
}

void test_nor_shape_validates_address_width_and_payload_bit_count_before_dispatch(void)
{
    pf_nor_transaction_t transaction = {.data_lanes = 1u};
    TEST_ASSERT_TRUE(pf_nor_transaction_valid(&transaction));
    transaction.address = 1u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.address_bytes = 1u;
    TEST_ASSERT_TRUE(pf_nor_transaction_valid(&transaction));
    transaction.address = 0x100u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.address_bytes = 3u;
    transaction.address = 0xffffffu;
    TEST_ASSERT_TRUE(pf_nor_transaction_valid(&transaction));
    transaction.address = 0x1000000u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.address = 0u;
    transaction.address_bytes = 4u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.address_bytes = 0u;
    transaction.length = 1u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.tx = payload;
    TEST_ASSERT_TRUE(pf_nor_transaction_valid(&transaction));
    transaction.rx = payload;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.tx = NULL;
    transaction.length = SIZE_MAX / 8u + 1u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
}
