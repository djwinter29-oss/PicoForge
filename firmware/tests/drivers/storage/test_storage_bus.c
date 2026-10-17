#include "unity.h"
#include "storage_bus.h"

static unsigned init_calls;
static unsigned abort_calls;
static unsigned deinit_calls;
static unsigned transfer_calls;
static int context;

static pf_storage_bus_result_t initialize(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    ++init_calls;
    return PF_STORAGE_BUS_OK;
}

static void abort_bus(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    ++abort_calls;
}

static void deinitialize(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    ++deinit_calls;
}

static pf_storage_bus_result_t set_clock(void *value, uint32_t requested, uint32_t *actual)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    *actual = requested - 1u;
    return PF_STORAGE_BUS_OK;
}

static pf_storage_bus_result_t transfer(void *value, const pf_storage_serial_transaction_t *transaction)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    TEST_ASSERT_EQUAL_UINT8(0x9fu, transaction->command);
    ++transfer_calls;
    return PF_STORAGE_BUS_TIMEOUT;
}

static const pf_storage_bus_ops_t operations = {
    .init = initialize, .abort = abort_bus, .deinit = deinitialize,
    .set_clock = set_clock, .transfer = transfer,
};

void setUp(void)
{
    init_calls = abort_calls = deinit_calls = transfer_calls = 0u;
}

void tearDown(void) {}

void test_lifecycle_dispatches_without_owning_context_or_device_protocol(void)
{
    pf_storage_bus_t bus = { .context = &context, .ops = &operations, .mode = PF_STORAGE_BUS_SPI };
    uint32_t actual;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_set_clock(&bus, 25000000u, &actual));
    TEST_ASSERT_EQUAL_UINT32(24999999u, actual);
    pf_storage_bus_abort(&bus);
    pf_storage_bus_deinit(&bus);
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
    TEST_ASSERT_EQUAL_UINT(1u, deinit_calls);
}

void test_serial_dispatch_propagates_failure_and_native_rejects_serial_shape(void)
{
    pf_storage_bus_t bus = { .context = &context, .ops = &operations, .mode = PF_STORAGE_BUS_SPI };
    pf_storage_serial_transaction_t transaction = { .command = 0x9fu, .data_lanes = 1u };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_TIMEOUT, pf_storage_bus_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(1u, transfer_calls);
    bus.mode = PF_STORAGE_BUS_SDIO_PIO;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_bus_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(1u, transfer_calls);
}

void test_invalid_transaction_never_calls_backend_and_missing_serial_support_is_explicit(void)
{
    const pf_storage_bus_ops_t native_operations = { .init = initialize };
    pf_storage_bus_t bus = { .context = &context, .ops = &operations, .mode = PF_STORAGE_BUS_SPI };
    pf_storage_serial_transaction_t transaction = { .command = 0x9fu, .data_lanes = 2u };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_bus_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
    transaction.data_lanes = 1u;
    bus.ops = &native_operations;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_bus_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
    bus.context = NULL;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_bus_transfer(&bus, &transaction));
}

void test_invalid_lifecycle_handles_and_missing_operations_are_explicit(void)
{
    const pf_storage_bus_ops_t empty = {0};
    pf_storage_bus_t bus = { .context = &context, .ops = &empty, .mode = PF_STORAGE_BUS_QSPI_PIO };
    uint32_t actual = 17u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_bus_init(NULL));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_bus_init(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_bus_set_clock(&bus, 1u, &actual));
    TEST_ASSERT_EQUAL_UINT32(17u, actual);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_bus_set_clock(&bus, 0u, &actual));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_bus_set_clock(&bus, 1u, NULL));
    pf_storage_bus_abort(NULL);
    pf_storage_bus_deinit(&bus);
}

void test_serial_shape_rejects_invalid_addresses_lanes_and_buffer_ownership(void)
{
    uint8_t data;
    pf_storage_serial_transaction_t transaction = { .data_lanes = 1u };
    TEST_ASSERT_TRUE(pf_storage_serial_transaction_valid(&transaction));
    transaction.address = 1u;
    TEST_ASSERT_FALSE(pf_storage_serial_transaction_valid(&transaction));
    transaction.address_bytes = 1u;
    TEST_ASSERT_TRUE(pf_storage_serial_transaction_valid(&transaction));
    transaction.address = 256u;
    TEST_ASSERT_FALSE(pf_storage_serial_transaction_valid(&transaction));
    transaction.address_bytes = 3u;
    transaction.address = 0xffffffu;
    transaction.data_lanes = 4u;
    TEST_ASSERT_TRUE(pf_storage_serial_transaction_valid(&transaction));
    transaction.address = 0x1000000u;
    TEST_ASSERT_FALSE(pf_storage_serial_transaction_valid(&transaction));
    transaction.address = 0u;
    transaction.data_lanes = 2u;
    TEST_ASSERT_FALSE(pf_storage_serial_transaction_valid(&transaction));
    transaction.data_lanes = 1u;
    transaction.length = 1u;
    TEST_ASSERT_FALSE(pf_storage_serial_transaction_valid(&transaction));
    transaction.tx = &data;
    TEST_ASSERT_TRUE(pf_storage_serial_transaction_valid(&transaction));
    transaction.rx = &data;
    TEST_ASSERT_FALSE(pf_storage_serial_transaction_valid(&transaction));
}
