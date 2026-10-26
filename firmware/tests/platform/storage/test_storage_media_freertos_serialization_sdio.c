#include "unity.h"
#include "sd_link.h"
#define PF_CONFIG_SDCARD_INTERRUPT_WAIT 1
TEST_INCLUDE_PATH("firmware/tests/support/storage/serialization")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "storage/storage_media_serialization.inc"
void setUp(void) { alarm(15); }
void tearDown(void) { alarm(0); }
void test_sdio_media_contracts_and_concurrent_reservations(void)
{
    check_contracts();
    check_serialization();
}

static unsigned wait_calls;
static bool wait_available;

bool pf_sd_link_enable_wait(const pf_sd_link_t *bus)
{
    TEST_ASSERT_EQUAL_PTR(&wait_calls, bus->context);
    ++wait_calls;
    return wait_available;
}

void test_port_requests_opaque_wait_setup_and_propagates_failure(void)
{
    pf_sd_config_t config = {
        .transport = {.context = &wait_calls, .mode = PF_SD_LINK_NATIVE_SD},
    };
    wait_calls = 0u;
    wait_available = false;
    TEST_ASSERT_FALSE(pf_sd_port_init(NULL));
    TEST_ASSERT_FALSE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(1u, wait_calls);
    wait_available = true;
    TEST_ASSERT_TRUE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(2u, wait_calls);
    config.transport.mode = PF_SD_LINK_SPI;
    TEST_ASSERT_FALSE(pf_sd_port_init(&config));
    TEST_ASSERT_EQUAL_UINT(2u, wait_calls);
}
