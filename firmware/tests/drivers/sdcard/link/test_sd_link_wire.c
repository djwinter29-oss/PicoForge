#include "unity.h"
#include "sd_link_wire.h"
#include "sd_link.h"

void setUp(void) {}
void tearDown(void) {}

void test_unsupported_native_operations_do_not_touch_a_serial_backend(void)
{
    int context;
    const pf_sd_link_ops_t operations = {0};
    pf_sd_link_t bus = {.context = &context, .ops = &operations, .mode = PF_SD_LINK_SPI};
    uint32_t words[5] = {0}, complete_words = 0u;
    uint8_t frame[6] = {0};
    size_t received = 5u;
    bool complete = false;
    TEST_ASSERT_FALSE(pf_sd_link_wire_valid(&bus));
    TEST_ASSERT_FALSE(pf_sd_link_wire_state(&bus).ready);
    TEST_ASSERT_FALSE(pf_sd_link_wire_hooks(&bus, NULL, NULL, NULL));
    TEST_ASSERT_FALSE(pf_sd_link_wire_clocks(&bus, 80u, 100u));
    TEST_ASSERT_FALSE(pf_sd_link_wire_idle(&bus));
    TEST_ASSERT_FALSE(pf_sd_link_wire_exchange(&bus, frame, sizeof(frame), 48u,
                                                frame, sizeof(frame), &received, 100u));
    TEST_ASSERT_EQUAL_UINT(0u, received);
    TEST_ASSERT_FALSE(pf_sd_link_wire_rx_start(&bus, words, 5u, 40u, 1u));
    TEST_ASSERT_FALSE(pf_sd_link_wire_rx_finish(&bus, 100u));
    TEST_ASSERT_FALSE(pf_sd_link_wire_rx_poll(&bus, 0u, 100u, &complete));
    TEST_ASSERT_FALSE(pf_sd_link_wire_rx_progress(&bus, words, 5u, &complete_words));
    TEST_ASSERT_FALSE(pf_sd_link_wire_tx_start(&bus, words, 5u, 40u));
    TEST_ASSERT_FALSE(pf_sd_link_wire_tx_finish(&bus, 100u, words));
    TEST_ASSERT_FALSE(pf_sd_link_wire_wait_high(&bus, 100u));
    pf_sd_link_wire_rx_cancel(&bus);
    pf_sd_link_wire_rx_wait(&bus);
}
