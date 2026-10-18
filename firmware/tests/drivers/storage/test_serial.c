#include "unity.h"
#include "serial.h"
#include "bus.h"

static unsigned transfer_calls;
static int context;

static pf_storage_bus_result_t initialize(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    return PF_STORAGE_BUS_OK;
}

static void release(void *value) { TEST_ASSERT_EQUAL_PTR(&context, value); }
static uint64_t now(void) { return 0u; }
static void delay(uint32_t duration) { (void)duration; }

static pf_storage_bus_result_t set_clock(void *value, uint32_t requested, uint32_t *actual)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    *actual = requested;
    return PF_STORAGE_BUS_OK;
}

static pf_storage_bus_result_t transfer(void *value, const pf_storage_serial_phase_t *phases, size_t count)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    TEST_ASSERT_EQUAL_UINT(1u, count);
    TEST_ASSERT_EQUAL_UINT(8u, phases[0].bit_count);
    ++transfer_calls;
    return PF_STORAGE_BUS_TIMEOUT;
}

static const pf_storage_serial_ops_t serial = {.transfer = transfer, .lanes = 1u};
static const pf_storage_bus_ops_t operations = {
    .init = initialize, .abort = release, .deinit = release, .set_clock = set_clock,
    .time_us = now, .delay_ms = delay, .serial = &serial,
};

void setUp(void) { transfer_calls = 0u; }
void tearDown(void) {}

void test_dispatch_propagates_failure_and_native_rejects_serial_shape(void)
{
    pf_storage_bus_t bus = {.context = &context, .ops = &operations, .mode = PF_STORAGE_BUS_SPI};
    pf_storage_serial_phase_t phase = {.bit_count = 8u, .lanes = 1u};
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_TIMEOUT, pf_storage_serial_transfer(&bus, &phase, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, transfer_calls);
    bus.mode = PF_STORAGE_BUS_NATIVE_SD;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_serial_transfer(&bus, &phase, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, transfer_calls);
}

void test_invalid_phases_and_missing_capability_never_call_backend(void)
{
    const pf_storage_bus_ops_t empty = {0};
    pf_storage_bus_t bus = {.context = &context, .ops = &operations, .mode = PF_STORAGE_BUS_SPI};
    pf_storage_serial_phase_t phase = {.bit_count = 8u, .lanes = 2u};
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_serial_transfer(&bus, &phase, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
    phase.lanes = 1u;
    bus.ops = &empty;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_serial_transfer(&bus, &phase, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
    bus.context = NULL;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_serial_transfer(&bus, &phase, 1u));
}

void test_shape_rejects_invalid_lanes_sizes_and_unsupported_widths(void)
{
    uint8_t data;
    pf_storage_serial_phase_t phase = {.bit_count = 8u, .lanes = 1u};
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_serial_phases_valid(&phase, 1u, 1u));
    phase.bit_count = 0u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_serial_phases_valid(&phase, 1u, 1u));
    phase.bit_count = 3u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_serial_phases_valid(&phase, 1u, 1u));
    phase.tx = &data;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_serial_phases_valid(&phase, 1u, 1u));
    phase.bit_count = 8u;
    phase.rx = &data;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_serial_phases_valid(&phase, 1u, 1u));
    phase.lanes = 4u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_serial_phases_valid(&phase, 1u, 1u));
    phase.rx = NULL;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_serial_phases_valid(&phase, 1u, 4u));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_serial_phases_valid(&phase, 1u, 1u));
    phase.lanes = 2u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_serial_phases_valid(&phase, 1u, 4u));
}

void test_missing_manual_serial_operations_are_explicit_without_transferring(void)
{
    pf_storage_bus_t bus = {.context = &context, .ops = &operations, .mode = PF_STORAGE_BUS_SPI};
    uint8_t frame[6] = {0};
    TEST_ASSERT_FALSE(pf_storage_serial_raw_valid(&bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_storage_serial_select(&bus, true));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_INVALID_ARGUMENT, pf_storage_serial_exchange(&bus, NULL, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED,
                      pf_storage_serial_exchange(&bus, frame, NULL, sizeof(frame)));
    TEST_ASSERT_EQUAL_UINT(0u, transfer_calls);
}
