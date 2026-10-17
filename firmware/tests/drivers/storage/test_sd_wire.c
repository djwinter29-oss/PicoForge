#include "unity.h"
#include "sd_wire.h"
#include "storage_transport.h"

void setUp(void) {}
void tearDown(void) {}

void test_unsupported_native_operations_do_not_touch_a_serial_backend(void)
{
    int context;
    const pf_storage_transport_ops_t operations = {0};
    pf_storage_transport_t bus = {.context = &context, .ops = &operations, .mode = PF_STORAGE_TRANSPORT_SPI};
    uint32_t words[5] = {0}, complete_words = 0u;
    uint8_t frame[6] = {0};
    size_t received = 5u;
    bool complete = false;
    TEST_ASSERT_FALSE(pf_storage_native_valid(&bus));
    TEST_ASSERT_FALSE(pf_storage_native_state(&bus).ready);
    TEST_ASSERT_FALSE(pf_storage_native_hooks(&bus, NULL, NULL, NULL));
    TEST_ASSERT_FALSE(pf_storage_native_clocks(&bus, 80u, 100u));
    TEST_ASSERT_FALSE(pf_storage_native_idle(&bus));
    TEST_ASSERT_FALSE(pf_storage_native_exchange(&bus, frame, sizeof(frame), 48u,
                                                frame, sizeof(frame), &received, 100u));
    TEST_ASSERT_EQUAL_UINT(0u, received);
    TEST_ASSERT_FALSE(pf_storage_native_rx_start(&bus, words, 5u, 40u, 1u));
    TEST_ASSERT_FALSE(pf_storage_native_rx_finish(&bus, 100u));
    TEST_ASSERT_FALSE(pf_storage_native_rx_poll(&bus, 0u, 100u, &complete));
    TEST_ASSERT_FALSE(pf_storage_native_rx_progress(&bus, words, 5u, &complete_words));
    TEST_ASSERT_FALSE(pf_storage_native_tx_start(&bus, words, 5u, 40u));
    TEST_ASSERT_FALSE(pf_storage_native_tx_finish(&bus, 100u, words));
    TEST_ASSERT_FALSE(pf_storage_native_wait_high(&bus, 100u));
    pf_storage_native_rx_cancel(&bus);
    pf_storage_native_rx_wait(&bus);
}
