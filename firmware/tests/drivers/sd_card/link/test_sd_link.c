#include "unity.h"
#include "sd_link.h"

static unsigned init_calls;
static unsigned abort_calls;
static unsigned deinit_calls;
static int context;

static pf_sd_link_result_t initialize(void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    ++init_calls;
    return PF_SD_LINK_OK;
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

static pf_sd_link_result_t set_clock(void *value, uint32_t requested, uint32_t *actual)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    *actual = requested - 1u;
    return PF_SD_LINK_OK;
}

static const pf_sd_link_ops_t operations = {
    .init = initialize, .abort = abort_bus, .deinit = deinitialize,
    .set_clock = set_clock,
};

void setUp(void)
{
    init_calls = abort_calls = deinit_calls = 0u;
}

void tearDown(void) {}

void test_lifecycle_dispatches_without_owning_context_or_device_protocol(void)
{
    pf_sd_link_t bus = { .context = &context, .ops = &operations, .mode = PF_SD_LINK_SPI };
    uint32_t actual;
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_init(&bus));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_set_clock(&bus, 25000000u, &actual));
    TEST_ASSERT_EQUAL_UINT32(24999999u, actual);
    pf_sd_link_abort(&bus);
    pf_sd_link_deinit(&bus);
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    TEST_ASSERT_EQUAL_UINT(1u, abort_calls);
    TEST_ASSERT_EQUAL_UINT(1u, deinit_calls);
}

void test_invalid_lifecycle_handles_and_missing_operations_are_explicit(void)
{
    const pf_sd_link_ops_t empty = {0};
    pf_sd_link_t bus = { .context = &context, .ops = &empty, .mode = PF_SD_LINK_SPI };
    uint32_t actual = 17u;
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_init(NULL));
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_init(&bus));
    TEST_ASSERT_EQUAL(PF_SD_LINK_UNSUPPORTED, pf_sd_link_set_clock(&bus, 1u, &actual));
    TEST_ASSERT_EQUAL_UINT32(17u, actual);
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_set_clock(&bus, 0u, &actual));
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_set_clock(&bus, 1u, NULL));
    pf_sd_link_abort(NULL);
    pf_sd_link_deinit(&bus);
}

static bool absent(const void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    return false;
}

static uint64_t platform_time(void) { return 123456u; }
static void platform_delay(uint32_t duration) { TEST_ASSERT_EQUAL_UINT32(5u, duration); }

void test_presence_time_and_descriptor_queries_keep_backend_state_opaque(void)
{
    pf_sd_link_ops_t ops = operations;
    ops.time_us = platform_time;
    ops.delay_ms = platform_delay;
    pf_sd_link_t transport = {.context = &context, .ops = &ops, .mode = PF_SD_LINK_SPI};
    pf_sd_link_t copy = transport;
    TEST_ASSERT_TRUE(pf_sd_link_valid(&transport, PF_SD_LINK_SPI));
    TEST_ASSERT_TRUE(pf_sd_link_same(&transport, &copy));
    TEST_ASSERT_FALSE(pf_sd_link_presence_known(&transport));
    TEST_ASSERT_TRUE(pf_sd_link_present(&transport));
    transport.present = absent;
    transport.presence_context = &context;
    TEST_ASSERT_FALSE(pf_sd_link_same(&transport, &copy));
    TEST_ASSERT_TRUE(pf_sd_link_presence_known(&transport));
    TEST_ASSERT_FALSE(pf_sd_link_present(&transport));
    TEST_ASSERT_EQUAL_UINT64(123456u, pf_sd_link_time_us(&transport));
    pf_sd_link_delay_ms(&transport, 5u);
    ops.time_us = NULL;
    TEST_ASSERT_FALSE(pf_sd_link_valid(&transport, PF_SD_LINK_SPI));
    TEST_ASSERT_FALSE(pf_sd_link_present(NULL));
    TEST_ASSERT_FALSE(pf_sd_link_same(NULL, &copy));
}

void test_missing_services_do_not_acquire_backend_resources(void)
{
    pf_sd_link_t transport = {.context = &context, .ops = &operations, .mode = PF_SD_LINK_SPI};
    TEST_ASSERT_FALSE(pf_sd_link_valid(&transport, PF_SD_LINK_SPI));
    TEST_ASSERT_FALSE(pf_sd_link_enable_wait(&transport));
    TEST_ASSERT_EQUAL_UINT(0u, init_calls);
}
