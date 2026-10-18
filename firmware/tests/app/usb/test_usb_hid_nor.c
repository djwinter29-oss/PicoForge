#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#define HID_SOURCE "../../../src/app/usb/usb_hid.c"
#include HID_SOURCE

void setUp(void) {}
void tearDown(void) {}

void test_nor_memory_report_does_not_configure_or_advertise_sd_media(void)
{
    pf_hid_memory_report_t report;
    memset(&report, 0xff, sizeof(report));
    TEST_ASSERT_TRUE(pf_hid_build_memory_report(&report, 7u));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_REPORT_VERSION, report.version);
    TEST_ASSERT_EQUAL_UINT8(7u, report.sequence);
    TEST_ASSERT_EQUAL_UINT8(0u, report.component_type);
    TEST_ASSERT_EQUAL_UINT8(0u, report.flags);
    TEST_ASSERT_EQUAL_UINT32(0u, report.block_count);
    TEST_ASSERT_EQUAL_UINT32(0u, report.bus_clock_hz);
}
