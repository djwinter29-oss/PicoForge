#include "unity.h"
#include "bus.h"
#define PF_CONFIG_SD_CARD_INTERRUPT_WAIT 1
TEST_INCLUDE_PATH("firmware/tests/support/storage/serialization")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "../../support/storage/facade_serialization.inc"
void setUp(void) { alarm(15); }
void tearDown(void) { alarm(0); }
void test_sdio_facade_contracts_and_concurrent_reservations(void)
{
    check_contracts();
    check_serialization();
}

static unsigned wait_calls;
static bool wait_available;

static bool enable_wait(void *context)
{
    TEST_ASSERT_EQUAL_PTR(&wait_calls, context);
    ++wait_calls;
    return wait_available;
}

void test_port_requests_opaque_wait_setup_and_propagates_failure(void)
{
    const pf_storage_bus_ops_t ops = {.enable_wait = enable_wait};
    pf_sd_config_t config = {
        .transport = {.context = &wait_calls, .ops = &ops, .mode = PF_STORAGE_BUS_NATIVE_SD},
    };
    wait_calls = 0u;
    wait_available = false;
    TEST_ASSERT_FALSE(pf_sd_port_init(NULL));
    TEST_ASSERT_FALSE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(1u, wait_calls);
    wait_available = true;
    TEST_ASSERT_TRUE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(2u, wait_calls);
    config.transport.mode = PF_STORAGE_BUS_SPI;
    TEST_ASSERT_FALSE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(2u, wait_calls);
}
