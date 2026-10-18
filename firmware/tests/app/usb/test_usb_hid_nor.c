#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#define HID_SOURCE "../../../src/app/usb/usb_hid.c"
#include HID_SOURCE

static bool media_busy;
static pf_nor_media_diagnostics_t media_info;
bool pf_nor_media_try_lock(void) { return !media_busy; }
void pf_nor_media_unlock(void) {}
void pf_nor_media_get_diagnostics(pf_nor_media_diagnostics_t *info) { *info = media_info; }

static unsigned force_requests, safe_requests;
void pf_usb_msc_request_force_return(void) { ++force_requests; }
void pf_usb_msc_request(bool enabled) { (void)enabled; ++safe_requests; }
bool pf_usb_msc_is_enabled(void) { return true; }
bool pf_usb_msc_transition_pending(void) { return false; }
bool pf_usb_msc_request_rejected(void) { return false; }
bool pf_led_set(bool on) { (void)on; return true; }
bool pf_storage_worker_benchmark_start(uint32_t size, bool host_owned)
{ (void)size; (void)host_owned; return false; }
bool pf_storage_worker_benchmark_cancel(void) { return false; }

void setUp(void)
{
    force_requests = safe_requests = 0u;
    media_busy = false;
    memset(&media_info, 0, sizeof(media_info));
}
void tearDown(void) {}

void test_force_return_requires_distinct_exact_hid_command_and_advertises_support(void)
{
    uint8_t command[] = {2u, 2u, 0u};
    tud_hid_set_report_cb(0u, PF_HID_MSC_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 2u);
    TEST_ASSERT_EQUAL_UINT(0u, force_requests);
    command[0] = 3u;
    tud_hid_set_report_cb(0u, PF_HID_MSC_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 3u);
    TEST_ASSERT_EQUAL_UINT(0u, force_requests);
    tud_hid_set_report_cb(0u, PF_HID_MSC_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 2u);
    TEST_ASSERT_EQUAL_UINT(1u, force_requests);
    TEST_ASSERT_EQUAL_UINT(0u, safe_requests);
    command[0] = 2u;
    command[1] = 0u;
    tud_hid_set_report_cb(0u, PF_HID_MSC_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 2u);
    TEST_ASSERT_EQUAL_UINT(1u, safe_requests);
    TEST_ASSERT_EQUAL_UINT(1u, force_requests);
    pf_hid_msc_status_report_t status;
    pf_hid_build_msc_status_report(&status);
    TEST_ASSERT_EQUAL_UINT8(3u, status.version);
    TEST_ASSERT_TRUE(status.enabled);
}

void test_nor_memory_report_does_not_configure_or_advertise_sd_media(void)
{
    pf_hid_memory_report_t report;
    memset(&report, 0xff, sizeof(report));
    TEST_ASSERT_TRUE(pf_hid_build_memory_report(&report, 7u));
    TEST_ASSERT_EQUAL_UINT8(3u, report.version);
    TEST_ASSERT_EQUAL_UINT8(7u, report.sequence);
    TEST_ASSERT_EQUAL_UINT8(2u, report.component_type);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_MEMORY_CONFIGURED, report.flags);
    TEST_ASSERT_EQUAL_UINT32(0u, report.block_count);
    TEST_ASSERT_EQUAL_UINT32(0u, report.bus_clock_hz);
}

void test_nor_telemetry_reports_identified_chip_and_logical_capacity_without_blocking(void)
{
    media_info = (pf_nor_media_diagnostics_t){
        .ready = true, .bus_clock_hz = 992063u,
        .capacity_bytes = 16777216u, .sector_count = 6000u,
    };
    pf_hid_memory_report_t report;
    TEST_ASSERT_TRUE(pf_hid_build_memory_report(&report, 1u));
    TEST_ASSERT_EQUAL_UINT8(0x3fu, report.flags);
    TEST_ASSERT_EQUAL_UINT32(992063u, report.bus_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(6000u, report.block_count);
    const uint8_t identity[16] = {0xefu, 0x40u, 0x18u, 0u, 0u, 0u, 1u};
    TEST_ASSERT_EQUAL_MEMORY(identity, report.cid, sizeof(identity));
    media_busy = true;
    TEST_ASSERT_FALSE(pf_hid_build_memory_report(&report, 2u));
}
