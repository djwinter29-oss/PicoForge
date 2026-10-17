#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "../support/storage/responsiveness_fixture.inc"

void setUp(void)
{
    config_available = true;
    requests = (void *)1;
    cli_session = 9;
}
void tearDown(void) { TEST_ASSERT_FALSE(reserved); }

void test_hid_unavailable_card_preserves_payload_and_does_not_block_usb_or_queue_work(void)
{
    pf_hid_memory_report_t report, sentinel;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    report = sentinel;
    unavailable = true;
    unsigned usb_iterations = 0;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        TEST_ASSERT_FALSE(pf_hid_build_memory_report(&report, 7));
        TEST_ASSERT_EQUAL_MEMORY(&sentinel, &report, sizeof(report));
        TEST_ASSERT_FALSE(pf_storage_worker_self_test_start(32, false));
        TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_BUSY, self_test_status.request_error);
        TEST_ASSERT_TRUE(!busy && !self_test_active && !reserved && !queries && !queued);
        uint8_t payload[sizeof(pf_hid_memory_report_t)];
        memset(payload, 0xa5, sizeof(payload));
        TEST_ASSERT_EQUAL_UINT16(0, tud_hid_get_report_cb(0, PF_HID_MEMORY_REPORT_ID,
            HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
        for (unsigned i = 0; i < sizeof(payload); ++i) TEST_ASSERT_EQUAL_HEX8(0xa5, payload[i]);
        TEST_ASSERT_EQUAL_UINT16(sizeof(pf_hid_status_report_t),
            tud_hid_get_report_cb(0, PF_HID_STATUS_REPORT_ID, HID_REPORT_TYPE_INPUT,
                                  payload, sizeof(payload)));
        ++usb_iterations;
    }
    TEST_ASSERT_EQUAL_UINT(1000, usb_iterations);
    unavailable = false;
    TEST_ASSERT_TRUE(pf_hid_build_memory_report(&report, 7));
    TEST_ASSERT_TRUE(report.version == PF_HID_REPORT_VERSION && report.sequence == 7);
    TEST_ASSERT_FALSE(report.flags & PF_HID_MEMORY_INITIALIZED);
    TEST_ASSERT_FALSE(pf_storage_worker_self_test_start(32, false));
    TEST_ASSERT_TRUE(self_test_status.request_error == PF_SELF_TEST_ERROR_CARD && !reserved);
    initialized = true;
    TEST_ASSERT_TRUE(pf_hid_build_memory_report(&report, 8));
    TEST_ASSERT_TRUE(report.flags & PF_HID_MEMORY_INITIALIZED);
    TEST_ASSERT_TRUE(report.bus_clock_hz == 25000000 && report.block_count == 128);
    uint8_t payload[sizeof(report)];
    TEST_ASSERT_EQUAL_UINT16(sizeof(report), tud_hid_get_report_cb(0, PF_HID_MEMORY_REPORT_ID,
        HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
    TEST_ASSERT_FALSE(pf_storage_worker_self_test_start(32, false));
    TEST_ASSERT_TRUE(self_test_status.request_error == PF_SELF_TEST_ERROR_NOT_MOUNTED && !reserved);
    mounted = true;
    TEST_ASSERT_FALSE(pf_storage_worker_self_test_start(32, true));
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_USB_OWNER, self_test_status.request_error);
    TEST_ASSERT_FALSE(pf_storage_worker_self_test_start(0, false));
    TEST_ASSERT_EQUAL(PF_SELF_TEST_ERROR_SIZE, self_test_status.request_error);
    queue_fails = true;
    TEST_ASSERT_FALSE(pf_storage_worker_self_test_start(32, false));
    TEST_ASSERT_TRUE(!busy && !self_test_active);
    queue_fails = false;
    TEST_ASSERT_TRUE(pf_storage_worker_self_test_start(32, false));
    TEST_ASSERT_TRUE(busy && self_test_active && queued == 1);
    TEST_ASSERT_EQUAL(PF_SELF_TEST_QUEUED, self_test_status.state);
    TEST_ASSERT_FALSE(pf_storage_worker_self_test_start(32, false));
    TEST_ASSERT_TRUE(self_test_status.request_error == PF_SELF_TEST_ERROR_BUSY && queued == 1);

    self_test_finished = true;
    char recovery[] = "recover --confirm", unconfirmed[] = "recover", status[] = "status";
    storage_command(NULL, recovery, "sd");
    TEST_ASSERT_TRUE(recovery_requests == 1 && normal_submissions == 0 && messages == 1);
    storage_command(NULL, unconfirmed, "sd");
    storage_command(NULL, status, "sd");
    storage_command(NULL, recovery, "fs");
    TEST_ASSERT_TRUE(recovery_requests == 1 && normal_submissions == 3 && messages == 4);
}

void test_hid_retained_card_identity_is_hidden_until_initialized(void)
{
    unavailable = false;
    retained_cid = true;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        initialized = attempt == 1;
        pf_hid_memory_report_t report;
        TEST_ASSERT_TRUE(pf_hid_build_memory_report(&report, 7));
        TEST_ASSERT_EQUAL_UINT8(initialized ? PF_HID_MEMORY_INITIALIZED | PF_HID_MEMORY_CID_VALID |
            PF_HID_MEMORY_CAPACITY_VALID : 0, report.flags & (PF_HID_MEMORY_INITIALIZED |
            PF_HID_MEMORY_CID_VALID | PF_HID_MEMORY_CAPACITY_VALID));
        TEST_ASSERT_EQUAL_UINT32(initialized ? 25000000 : 0, report.bus_clock_hz);
        TEST_ASSERT_EQUAL_UINT32(initialized ? 128 : 0, report.block_count);
        for (unsigned i = 0; i < sizeof(report.cid); ++i) {
            TEST_ASSERT_EQUAL_HEX8(initialized ? 0x5a : 0, report.cid[i]);
        }
        pf_hid_memory_report_t control_report;
        TEST_ASSERT_EQUAL_UINT16(sizeof(control_report), tud_hid_get_report_cb(0, PF_HID_MEMORY_REPORT_ID,
            HID_REPORT_TYPE_INPUT, (uint8_t *)&control_report, sizeof(control_report)));
        report.sequence = 0;
        TEST_ASSERT_EQUAL_MEMORY(&report, &control_report, sizeof(report));
    }
    retained_cid = false;
}
