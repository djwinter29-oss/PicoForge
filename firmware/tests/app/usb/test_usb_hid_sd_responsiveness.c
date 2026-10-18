#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "../../support/storage/responsiveness_fixture.inc"

void setUp(void)
{
    config_available = true;
    requests = (void *)1;
    cli_session = 9;
}
void tearDown(void) { TEST_ASSERT_FALSE(reserved); }

void test_runtime_report_publishes_usage_with_independent_layout_and_exact_offsets(void)
{
    pf_hid_status_report_t report;
    pf_hid_build_status_report(&report, 17u);
    TEST_ASSERT_EQUAL_UINT(23u, sizeof(report));
    TEST_ASSERT_EQUAL_UINT8(3u, report.version);
    TEST_ASSERT_EQUAL_UINT8(17u, report.sequence);
    TEST_ASSERT_EQUAL_UINT16(1234u, report.cpu_usage_basis_points);
    TEST_ASSERT_EQUAL_UINT32(65536u, report.ram_used_bytes);
    TEST_ASSERT_EQUAL_UINT32(532480u, report.ram_total_bytes);
    const uint8_t *bytes = (const uint8_t *)&report;
    const uint8_t expected[] = {0xd2u, 0x04u, 0u, 0u, 1u, 0u, 0u, 0x20u, 8u, 0u};
    TEST_ASSERT_EQUAL_MEMORY(expected, bytes + 13u, sizeof(expected));
    uint8_t payload[sizeof(report)];
    TEST_ASSERT_EQUAL_UINT16(sizeof(report), tud_hid_get_report_cb(0u, PF_HID_STATUS_REPORT_ID,
        HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(0u, PF_HID_STATUS_REPORT_ID,
        HID_REPORT_TYPE_INPUT, payload, sizeof(payload) - 1u));
}

void test_hid_identity_reports_firmware_version_without_changing_payload_size(void)
{
    pf_hid_info_report_t report;
    memset(&report, 0xa5, sizeof(report));
    pf_hid_build_info_report(&report);
    TEST_ASSERT_EQUAL_UINT(42u, sizeof(report));
    TEST_ASSERT_EQUAL_UINT8(3u, report.version);
    TEST_ASSERT_EQUAL_UINT8(1u, report.firmware_major);
    TEST_ASSERT_EQUAL_UINT8(2u, report.firmware_minor);
    TEST_ASSERT_EQUAL_UINT8(3u, report.firmware_patch);
    TEST_ASSERT_EQUAL_UINT8(0u, report.reserved);
    uint8_t payload[sizeof(report)];
    TEST_ASSERT_EQUAL_UINT16(sizeof(report), tud_hid_get_report_cb(0, PF_HID_INFO_REPORT_ID,
        HID_REPORT_TYPE_FEATURE, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_MEMORY(&report, payload, sizeof(report));
}

void test_file_benchmark_rename_preserves_hid_report_encoding(void)
{
    benchmark_active = benchmark_finished = false;
    benchmark_status = (pf_file_benchmark_status_t){
        .state = PF_FILE_BENCHMARK_PREPARING,
        .error = PF_FILE_BENCHMARK_ERROR_MEDIA,
        .request_error = PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED,
        .flags = PF_FILE_BENCHMARK_FILE_REMAINS | PF_FILE_BENCHMARK_VOLUME_MOUNTED,
        .target_bytes = 1048576u,
        .filesystem_result = FR_DISK_ERR,
    };
    pf_hid_self_test_status_t report;
    pf_hid_build_self_test_report(&report, 42u);
    TEST_ASSERT_EQUAL_UINT(32u, sizeof(report));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_REPORT_VERSION, report.version);
    TEST_ASSERT_EQUAL_UINT8(42u, report.sequence);
    TEST_ASSERT_EQUAL_UINT8(2u, report.state);
    TEST_ASSERT_EQUAL_UINT8(5u, report.error);
    TEST_ASSERT_EQUAL_UINT8(11u, report.request_error);
    TEST_ASSERT_EQUAL_UINT8(3u, report.flags);
    TEST_ASSERT_EQUAL_UINT8(FR_DISK_ERR, report.filesystem_result);
    TEST_ASSERT_EQUAL_UINT32(1048576u, report.target_bytes);
    TEST_ASSERT_EQUAL_UINT8(0u, report.reserved);
}

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
        TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(32, false));
        TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_BUSY, benchmark_status.request_error);
        TEST_ASSERT_TRUE(!busy && !benchmark_active && !reserved && !queries && !queued);
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
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(32, false));
    TEST_ASSERT_TRUE(benchmark_status.request_error == PF_FILE_BENCHMARK_ERROR_MEDIA && !reserved);
    initialized = true;
    TEST_ASSERT_TRUE(pf_hid_build_memory_report(&report, 8));
    TEST_ASSERT_TRUE(report.flags & PF_HID_MEMORY_INITIALIZED);
    TEST_ASSERT_TRUE(report.bus_clock_hz == 25000000 && report.block_count == 128);
    uint8_t payload[sizeof(report)];
    TEST_ASSERT_EQUAL_UINT16(sizeof(report), tud_hid_get_report_cb(0, PF_HID_MEMORY_REPORT_ID,
        HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(32, false));
    TEST_ASSERT_TRUE(benchmark_status.request_error == PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED && !reserved);
    mounted = true;
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(32, true));
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_USB_OWNER, benchmark_status.request_error);
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(0, false));
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_SIZE, benchmark_status.request_error);
    queue_fails = true;
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(32, false));
    TEST_ASSERT_TRUE(!busy && !benchmark_active);
    queue_fails = false;
    TEST_ASSERT_TRUE(pf_storage_worker_benchmark_start(32, false));
    TEST_ASSERT_TRUE(busy && benchmark_active && queued == 1);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_QUEUED, benchmark_status.state);
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(32, false));
    TEST_ASSERT_TRUE(benchmark_status.request_error == PF_FILE_BENCHMARK_ERROR_BUSY && queued == 1);

    benchmark_finished = true;
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
