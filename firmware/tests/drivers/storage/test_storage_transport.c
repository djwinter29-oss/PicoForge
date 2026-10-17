#include "unity.h"
#include "storage_transport.h"

static unsigned init_calls;
static unsigned abort_calls;
static unsigned deinit_calls;
static unsigned transfer_calls;
static int context;

static pf_storage_transport_result_t initialize(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    ++init_calls;
    return PF_STORAGE_TRANSPORT_OK;
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

static pf_storage_transport_result_t set_clock(void *value, uint32_t requested, uint32_t *actual)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    *actual = requested - 1u;
    return PF_STORAGE_TRANSPORT_OK;
}

static pf_storage_transport_result_t transfer(void *value, const pf_storage_serial_transaction_t *transaction)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    TEST_ASSERT_EQUAL_UINT8(0x9fu, transaction->command);
    ++transfer_calls;
    return PF_STORAGE_TRANSPORT_TIMEOUT;
}

static const pf_storage_transport_ops_t operations = {
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
    pf_storage_transport_t bus = { .context = &context, .ops = &operations, .mode = PF_STORAGE_TRANSPORT_SPI };
    uint32_t actual;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_storage_transport_init(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_storage_transport_set_clock(&bus, 25000000u, &actual));
    TEST_ASSERT_EQUAL_UINT32(24999999u, actual);
    pf_storage_transport_abort(&bus);
    pf_storage_transport_deinit(&bus);
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
    TEST_ASSERT_EQUAL_UINT(1u, deinit_calls);
}

void test_serial_dispatch_propagates_failure_and_native_rejects_serial_shape(void)
{
    pf_storage_transport_t bus = { .context = &context, .ops = &operations, .mode = PF_STORAGE_TRANSPORT_SPI };
    pf_storage_serial_transaction_t transaction = { .command = 0x9fu, .data_lanes = 1u };
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_TIMEOUT, pf_storage_transport_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(1u, transfer_calls);
    bus.mode = PF_STORAGE_TRANSPORT_NATIVE_SD;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_UNSUPPORTED, pf_storage_transport_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(1u, transfer_calls);
}

void test_invalid_transaction_never_calls_backend_and_missing_serial_support_is_explicit(void)
{
    const pf_storage_transport_ops_t native_operations = { .init = initialize };
    pf_storage_transport_t bus = { .context = &context, .ops = &operations, .mode = PF_STORAGE_TRANSPORT_SPI };
    pf_storage_serial_transaction_t transaction = { .command = 0x9fu, .data_lanes = 2u };
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_storage_transport_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
    transaction.data_lanes = 1u;
    bus.ops = &native_operations;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_UNSUPPORTED, pf_storage_transport_transfer(&bus, &transaction));
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
    bus.context = NULL;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_storage_transport_transfer(&bus, &transaction));
}

void test_invalid_lifecycle_handles_and_missing_operations_are_explicit(void)
{
    const pf_storage_transport_ops_t empty = {0};
    pf_storage_transport_t bus = { .context = &context, .ops = &empty, .mode = PF_STORAGE_TRANSPORT_QSPI };
    uint32_t actual = 17u;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_storage_transport_init(NULL));
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_storage_transport_init(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_UNSUPPORTED, pf_storage_transport_set_clock(&bus, 1u, &actual));
    TEST_ASSERT_EQUAL_UINT32(17u, actual);
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_storage_transport_set_clock(&bus, 0u, &actual));
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, pf_storage_transport_set_clock(&bus, 1u, NULL));
    pf_storage_transport_abort(NULL);
    pf_storage_transport_deinit(&bus);
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

static bool absent(const void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    return false;
}

static uint64_t platform_time(void) { return 123456u; }
static void platform_delay(uint32_t duration) { TEST_ASSERT_EQUAL_UINT32(5u, duration); }

void test_presence_time_and_serial_capability_queries_keep_backend_state_opaque(void)
{
    pf_storage_transport_ops_t ops = operations;
    ops.time_us = platform_time;
    ops.delay_ms = platform_delay;
    pf_storage_transport_t transport = {.context = &context, .ops = &ops, .mode = PF_STORAGE_TRANSPORT_SPI};
    pf_storage_transport_t copy = transport;
    bool quad = true;
    TEST_ASSERT_TRUE(pf_storage_transport_same(&transport, &copy));
    TEST_ASSERT_FALSE(pf_storage_transport_presence_known(&transport));
    TEST_ASSERT_TRUE(pf_storage_transport_present(&transport));
    transport.present = absent;
    transport.presence_context = &context;
    TEST_ASSERT_FALSE(pf_storage_transport_same(&transport, &copy));
    TEST_ASSERT_TRUE(pf_storage_transport_presence_known(&transport));
    TEST_ASSERT_FALSE(pf_storage_transport_present(&transport));
    TEST_ASSERT_EQUAL_UINT64(123456u, pf_storage_transport_time_us(&transport));
    pf_storage_transport_delay_ms(&transport, 5u);
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_storage_serial_valid(&transport, &quad));
    TEST_ASSERT_FALSE(quad);
    transport.mode = PF_STORAGE_TRANSPORT_QSPI;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_OK, pf_storage_serial_valid(&transport, &quad));
    TEST_ASSERT_TRUE(quad);
    ops.time_us = NULL;
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT,
                      pf_storage_serial_valid(&transport, &quad));
    TEST_ASSERT_FALSE(pf_storage_transport_present(NULL));
    TEST_ASSERT_FALSE(pf_storage_transport_same(NULL, &copy));
}

void test_unsupported_raw_serial_and_native_operations_do_not_touch_backend(void)
{
    pf_storage_transport_t transport = {.context = &context, .ops = &operations, .mode = PF_STORAGE_TRANSPORT_SPI};
    uint32_t words[5] = {0}, complete_words = 0u;
    uint8_t frame[6] = {0};
    bool complete = false;
    TEST_ASSERT_FALSE(pf_storage_transport_valid(&transport, PF_STORAGE_TRANSPORT_SPI));
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_UNSUPPORTED, pf_storage_serial_select(&transport, true));
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_INVALID_ARGUMENT,
                      pf_storage_serial_exchange(&transport, NULL, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_STORAGE_TRANSPORT_UNSUPPORTED,
                      pf_storage_serial_exchange(&transport, frame, NULL, sizeof(frame)));
    TEST_ASSERT_FALSE(pf_storage_transport_enable_wait(&transport));
    TEST_ASSERT_FALSE(pf_storage_native_state(&transport).ready);
    TEST_ASSERT_FALSE(pf_storage_native_hooks(&transport, NULL, NULL, NULL));
    TEST_ASSERT_FALSE(pf_storage_native_prepare(&transport));
    TEST_ASSERT_FALSE(pf_storage_native_idle(&transport));
    TEST_ASSERT_FALSE(pf_storage_native_command(&transport, frame, 48u, words, 5u));
    TEST_ASSERT_FALSE(pf_storage_native_rx_start(&transport, words, 5u, 40u, 1u));
    TEST_ASSERT_FALSE(pf_storage_native_rx_finish(&transport, 100u));
    TEST_ASSERT_FALSE(pf_storage_native_rx_poll(&transport, 0u, 100u, &complete));
    TEST_ASSERT_FALSE(pf_storage_native_rx_progress(&transport, words, 5u, &complete_words));
    TEST_ASSERT_FALSE(pf_storage_native_tx_prepare(&transport));
    TEST_ASSERT_FALSE(pf_storage_native_tx_start(&transport, words, 5u, 40u));
    TEST_ASSERT_FALSE(pf_storage_native_tx_finish(&transport, 100u, words));
    TEST_ASSERT_FALSE(pf_storage_native_wait_high(&transport, 100u));
    pf_storage_native_rx_cancel(&transport);
    pf_storage_native_rx_wait(&transport);
    TEST_ASSERT_EQUAL_UINT(0u, init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
}

void test_matches_spi_pins_to_controller_roles(void)
{
    TEST_ASSERT_TRUE(pf_storage_spi_pins_match_controller(0u, 2u, 3u, 4u));
    TEST_ASSERT_TRUE(pf_storage_spi_pins_match_controller(0u, 6u, 7u, 0u));
    TEST_ASSERT_TRUE(pf_storage_spi_pins_match_controller(1u, 10u, 11u, 8u));
    TEST_ASSERT_TRUE(pf_storage_spi_pins_match_controller(0u, 34u, 35u, 36u));
    TEST_ASSERT_FALSE(pf_storage_spi_pins_match_controller(1u, 2u, 3u, 4u));
    TEST_ASSERT_FALSE(pf_storage_spi_pins_match_controller(0u, 2u, 3u, 5u));
    TEST_ASSERT_FALSE(pf_storage_spi_pins_match_controller(0u, 2u, 11u, 4u));
    TEST_ASSERT_FALSE(pf_storage_spi_pins_match_controller(2u, 2u, 3u, 4u));
}