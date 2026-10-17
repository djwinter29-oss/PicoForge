#include "unity.h"
#include "storage_transport.h"
#define PF_CONFIG_SD_CARD_INTERRUPT_WAIT 1
#include "../../../../../src/drivers/sd_card/ports/freertos/sd_wait.c"

static unsigned calls;
static bool wait_available;

static bool enable_wait(void *context)
{
    TEST_ASSERT_EQUAL_PTR(&calls, context);
    ++calls;
    return wait_available;
}

void setUp(void) { calls = 0u; wait_available = false; }
void tearDown(void) {}

void test_port_requests_opaque_wait_setup_and_propagates_failure(void)
{
    const pf_storage_transport_ops_t ops = {.enable_wait = enable_wait};
    pf_sd_config_t config = {
        .transport = PF_SD_TRANSPORT_SDIO_PIO,
        .bus.native.transport = {.context = &calls, .ops = &ops, .mode = PF_STORAGE_TRANSPORT_NATIVE_SD},
    };
    TEST_ASSERT_FALSE(pf_sd_port_init(NULL));
    TEST_ASSERT_FALSE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(1u, calls);
    wait_available = true;
    TEST_ASSERT_TRUE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(2u, calls);
    config.transport = PF_SD_TRANSPORT_SPI;
    TEST_ASSERT_FALSE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(2u, calls);
}
