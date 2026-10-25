#include "unity.h"
#include <stdio.h>
#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/runtime")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
TEST_INCLUDE_PATH("firmware/tests/support/config")
#include "storage/responsiveness_fixture.inc"

void setUp(void)
{
    requests = (void *)1;
    replies = (void *)2;
    busy = media_service_busy = false;
    unavailable = reserved = initialized = mounted = false;
    normal_submissions = queued = 0u;
    cli_session = 1u;
}

void tearDown(void) { TEST_ASSERT_FALSE(reserved); }

void test_disabled_benchmark_is_not_advertised_and_feature_report_is_unavailable(void)
{
    pf_hid_identity_report_t identity;
    pf_hid_build_identity_report(&identity);
    TEST_ASSERT_EQUAL_UINT8(0u, identity.capabilities & PF_HID_CAP_SELF_TEST);
    TEST_ASSERT_TRUE(identity.capabilities & PF_HID_CAP_SDIO);
    TEST_ASSERT_TRUE(identity.capabilities & PF_HID_CAP_SETTINGS);
    pf_hid_self_test_report_t report;
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_FEATURE, (uint8_t *)&report, sizeof(report)));
}

void test_disabled_benchmark_rejects_start_and_cancel_without_queuing_work(void)
{
    for (uint8_t action = 0u; action <= 1u; ++action) {
        pf_hid_control_request_t request = {
            .version = PF_HID_REPORT_VERSION, .transaction_id = action + 1u,
            .opcode = PF_HID_OPCODE_SELF_TEST, .arguments = {action, 32u, 0u, 0u, 0u},
        };
        tud_hid_set_report_cb(0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                             (const uint8_t *)&request, sizeof(request));
        pf_hid_poll_async_commands();
        TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_UNSUPPORTED, control_result.status);
        TEST_ASSERT_EQUAL_UINT8(PF_HID_ERROR_CAPABILITY, control_result.error_reason);
        TEST_ASSERT_FALSE(pf_storage_worker_busy());
    }
    TEST_ASSERT_EQUAL_UINT(0u, queued);
}

void test_disabled_benchmark_preserves_normal_command_admission_and_cancellation(void)
{
    TEST_ASSERT_TRUE(pf_storage_worker_submit("fs", "info", false, cli_session));
    TEST_ASSERT_EQUAL_UINT(1u, normal_submissions);
    busy = true;
    pf_storage_worker_cancel(cli_session);
    TEST_ASSERT_TRUE(cancelled);
}
