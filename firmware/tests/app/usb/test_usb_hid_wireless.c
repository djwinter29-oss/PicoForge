#include "unity.h"
#include <stdio.h>
#include <string.h>

#define PF_ENABLE_WIRELESS 1
#define PF_ENABLE_STORAGE_BENCHMARK 1
#define PF_FIRMWARE_VERSION_MAJOR 1u
#define PF_FIRMWARE_VERSION_MINOR 2u
#define PF_FIRMWARE_VERSION_PATCH 3u
TEST_INCLUDE_PATH("firmware/tests/support/runtime")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
TEST_INCLUDE_PATH("firmware/tests/support/config")
#include "storage/responsiveness_fixture.inc"

static unsigned provisioning_calls;
void pf_wireless_start(void) {}
void pf_wireless_poll(void) {}
void pf_wireless_status(uint8_t *report) { memset(report, 0, PF_WIRELESS_STATUS_SIZE); report[0] = 1u; }
bool pf_wireless_configure(const uint8_t *report) { (void)report; ++provisioning_calls; return true; }

void setUp(void)
{
    requests = (void *)1;
    replies = (void *)2;
    config_available = true;
    busy = media_service_busy = false;
    unavailable = reserved = initialized = mounted = false;
    msc_enabled = msc_transition_pending = false;
    reboot_requested = force_save_pending = force_save_reboot_waiting = false;
    self_test_result_pending = self_test_from_ble = ble_dispatch = false;
    memset(&bench, 0, sizeof(bench));
    control_result = ble_result = (pf_hid_control_result_t){.version = 1u};
    provisioning_calls = led_requests = msc_requests = force_requests = 0u;
}

void tearDown(void) { TEST_ASSERT_FALSE(reserved); }

void test_ble_dispatch_rejects_reboot_storage_and_flash_writes_without_touching_usb_result(void)
{
    control_result.transaction_id = 99u;
    pf_hid_control_request_t request = {.version = 1u, .transaction_id = 1u, .opcode = PF_HID_OPCODE_LED};
    TEST_ASSERT_TRUE(pf_hid_ble_control(&request));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_COMPLETED, ble_result.status);
    TEST_ASSERT_EQUAL_UINT8(99u, control_result.transaction_id);
    TEST_ASSERT_EQUAL_UINT(1u, led_requests);
    const uint8_t prohibited[] = {PF_HID_OPCODE_REBOOT, PF_HID_OPCODE_MSC, PF_HID_OPCODE_SAVE_SETTINGS};
    for (unsigned i = 0u; i < sizeof(prohibited); ++i) {
        request.opcode = prohibited[i];
        TEST_ASSERT_FALSE(pf_hid_ble_control(&request));
    }
    TEST_ASSERT_FALSE(reboot_requested);
    TEST_ASSERT_EQUAL_UINT(0u, msc_requests);
    TEST_ASSERT_EQUAL_UINT(0u, provisioning_calls);
    pf_hid_identity_report_t identity;
    pf_hid_build_identity_report(&identity);
    TEST_ASSERT_TRUE(identity.capabilities & PF_HID_CAP_WIRELESS);
}

void test_ble_dispatch_refuses_interference_during_save_and_pending_admission(void)
{
    pf_hid_control_request_t request = {.version = 1u, .transaction_id = 1u, .opcode = PF_HID_OPCODE_LED};
    force_save_pending = true;
    TEST_ASSERT_TRUE(pf_hid_ble_control(&request));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_BUSY, ble_result.status);
    force_save_pending = false;
    self_test_result_pending = true;
    TEST_ASSERT_TRUE(pf_hid_ble_control(&request));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_BUSY, ble_result.status);
    TEST_ASSERT_EQUAL_UINT(0u, led_requests);
}

void test_wireless_feature_has_sanitized_status_and_no_private_readback(void)
{
    uint8_t buffer[PF_WIRELESS_REQUEST_SIZE] = {1u, 3u};
    TEST_ASSERT_EQUAL_UINT16(PF_WIRELESS_STATUS_SIZE, tud_hid_get_report_cb(
        0u, PF_WIRELESS_REPORT_ID, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, PF_WIRELESS_CONFIG_REPORT_ID, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer)));
    tud_hid_set_report_cb(0u, PF_WIRELESS_CONFIG_REPORT_ID, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer) - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, provisioning_calls);
    tud_hid_set_report_cb(0u, PF_WIRELESS_CONFIG_REPORT_ID, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT(1u, provisioning_calls);
    TEST_ASSERT_TRUE(reboot_requested);
}
