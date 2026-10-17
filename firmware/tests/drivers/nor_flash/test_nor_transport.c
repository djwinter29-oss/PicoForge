#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "storage_transport.h"
#include "nor_transport.h"
#include <string.h>

serial_fake_t serial_fake;
static pf_storage_transport_t bus;
static pf_nor_transport_t transport;
static pf_storage_transport_result_t init_result;
static pf_storage_transport_result_t transfer_result;
static unsigned init_calls, transfer_calls, deinit_calls;
static int context;

static uint64_t now(void) { return ++serial_fake.now; }

static pf_storage_transport_result_t initialize(void *value)
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

static pf_storage_transport_result_t transfer(void *value, const pf_storage_serial_transaction_t *transaction)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    TEST_ASSERT_EQUAL_UINT8(0x9f, transaction->command);
    ++transfer_calls;
    return transfer_result;
}

static const pf_storage_transport_ops_t operations = {
    .init = initialize, .deinit = deinitialize, .transfer = transfer, .time_us = now,
};

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
    transport = (pf_nor_transport_t){0};
    bus = (pf_storage_transport_t){ .context = &context, .ops = &operations, .mode = PF_STORAGE_TRANSPORT_SPI };
    init_calls = transfer_calls = deinit_calls = 0;
    init_result = transfer_result = PF_STORAGE_TRANSPORT_OK;
}

void tearDown(void) {}

void test_one_adapter_dispatches_spi_and_quad_buses_and_supplies_clock(void)
{
    const pf_storage_transport_mode_t modes[] = {PF_STORAGE_TRANSPORT_SPI, PF_STORAGE_TRANSPORT_QSPI_PIO};
    pf_nor_transaction_t transaction = { .command = 0x9f, .data_lanes = 1 };
    for (unsigned index = 0; index < 2; ++index) {
        bus.mode = modes[index];
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
    pf_storage_transport_ops_t incomplete = operations;
    bus.ops = &incomplete;
    incomplete.transfer = NULL;
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(&bus, &transport));
    incomplete = operations;
    incomplete.deinit = NULL;
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_transport_init(&bus, &transport));
    bus.ops = &operations;
    bus.mode = PF_STORAGE_TRANSPORT_SDIO_PIO;
    TEST_ASSERT_EQUAL(PF_NOR_UNSUPPORTED, pf_nor_transport_init(&bus, &transport));
    incomplete = operations;
    incomplete.transfer = NULL;
    bus.ops = &incomplete;
    TEST_ASSERT_EQUAL(PF_NOR_UNSUPPORTED, pf_nor_transport_init(&bus, &transport));
    TEST_ASSERT_EQUAL_UINT(0, init_calls);
    TEST_ASSERT_NULL(transport.execute);
}

void test_init_failure_is_propagated_without_publishing_transport(void)
{
    init_result = PF_STORAGE_TRANSPORT_NOT_READY;
    TEST_ASSERT_EQUAL(PF_NOR_NOT_READY, pf_nor_transport_init(&bus, &transport));
    TEST_ASSERT_NULL(transport.execute);
    TEST_ASSERT_EQUAL_UINT(1, init_calls);
    TEST_ASSERT_EQUAL_UINT(0, deinit_calls);
}

void test_only_io_errors_and_timeouts_deinitialize_without_replay(void)
{
    const pf_storage_transport_result_t results[] = {
        PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, PF_STORAGE_TRANSPORT_NOT_READY, PF_STORAGE_TRANSPORT_UNSUPPORTED,
        PF_STORAGE_TRANSPORT_IO_ERROR, PF_STORAGE_TRANSPORT_TIMEOUT,
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
