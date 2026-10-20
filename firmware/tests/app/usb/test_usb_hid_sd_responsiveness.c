#include "unity.h"
#include <stdio.h>
TEST_INCLUDE_PATH("firmware/tests/support/storage")
TEST_INCLUDE_PATH("firmware/tests/support/config")
#include "storage/responsiveness_fixture.inc"

void setUp(void)
{
    config_available = true;
    requests = (void *)1;
    cli_session = 9;
    test_settings = (pf_settings_t){.system_clock_mhz = 200u, .sdio_bus_mhz = 25u, .spi_bus_mhz = 50u};
    settings_save_calls = 0u;
    reset_requests = 0u;
    msc_enabled = false;
        msc_transition_pending = false;
    busy = false;
}
void tearDown(void) { TEST_ASSERT_FALSE(reserved); }

void test_runtime_report_publishes_usage_with_independent_layout_and_exact_offsets(void)
{
    pf_hid_status_report_t report;
    pf_hid_build_status_report(&report, 17u);
    TEST_ASSERT_EQUAL_UINT(25u, sizeof(report));
    TEST_ASSERT_EQUAL_UINT8(1u, report.version);
    TEST_ASSERT_EQUAL_UINT8(17u, report.sequence);
    TEST_ASSERT_EQUAL_UINT16(1234u, report.cpu_usage_core0_basis_points);
    TEST_ASSERT_EQUAL_UINT16(5678u, report.cpu_usage_core1_basis_points);
    TEST_ASSERT_EQUAL_UINT32(65536u, report.ram_used_bytes);
    TEST_ASSERT_EQUAL_UINT32(532480u, report.ram_total_bytes);
    const uint8_t *bytes = (const uint8_t *)&report;
    const uint8_t expected[] = {0xd2u, 0x04u, 0x2eu, 0x16u, 0u, 0u, 1u, 0u, 0u, 0x20u, 8u, 0u};
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

void test_settings_feature_report_encodes_saved_numeric_values(void)
{
    pf_hid_settings_report_t report;
    pf_hid_build_settings_report(&report);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_SETTINGS_REPORT_VERSION, report.version);
    TEST_ASSERT_EQUAL_UINT16(200u, report.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(25u, report.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(50u, report.spi_bus_mhz);
    uint8_t payload[sizeof(report)];
    TEST_ASSERT_EQUAL_UINT16(sizeof(report), tud_hid_get_report_cb(0u, PF_HID_SETTINGS_REPORT_ID,
        HID_REPORT_TYPE_FEATURE, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_MEMORY(&report, payload, sizeof(report));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(0u, PF_HID_SETTINGS_REPORT_ID,
        HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
}

void test_settings_feature_report_validates_and_saves_only_when_storage_is_idle(void)
{
    uint8_t command[] = {PF_HID_SETTINGS_REPORT_VERSION, 250u, 0u, 50u, 25u};
    tud_hid_set_report_cb(1u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_INPUT, command, sizeof(command));
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, NULL, sizeof(command));
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command) - 1u);
    command[0] = 0xffu;
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    command[0] = PF_HID_SETTINGS_REPORT_VERSION;
    command[1] = 100u;
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    TEST_ASSERT_EQUAL_UINT(0u, settings_save_calls);

    command[1] = 250u;
    msc_enabled = true;
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    msc_enabled = false;
    busy = true;
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    busy = false;
    TEST_ASSERT_EQUAL_UINT(0u, settings_save_calls);

    msc_transition_pending = true;
    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    msc_transition_pending = false;
    TEST_ASSERT_EQUAL_UINT(0u, settings_save_calls);

    tud_hid_set_report_cb(0u, PF_HID_SETTINGS_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    TEST_ASSERT_EQUAL_UINT(1u, settings_save_calls);
    TEST_ASSERT_EQUAL_UINT16(250u, test_settings.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(50u, test_settings.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(25u, test_settings.spi_bus_mhz);
}

void test_reboot_feature_command_is_validated_and_storage_gated(void)
{
    const uint8_t command[] = {PF_HID_REBOOT_REPORT_VERSION, 1u};
    uint8_t invalid[] = {0xffu, 1u};

    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());
    tud_hid_set_report_cb(1u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_INPUT, command, sizeof(command));
    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, invalid, sizeof(invalid));
    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command) - 1u);
    invalid[0] = PF_HID_REBOOT_REPORT_VERSION;
    invalid[1] = 0u;
    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, invalid, sizeof(invalid));
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());

    msc_enabled = true;
    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    msc_enabled = false;
    msc_transition_pending = true;
    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    msc_transition_pending = false;
    busy = true;
    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    busy = false;
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());

    tud_hid_set_report_cb(0u, PF_HID_REBOOT_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    TEST_ASSERT_TRUE(pf_hid_take_reboot_request());
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());
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

void test_stalled_full_reply_path_cancels_and_drains_completion_before_new_work(void)
{
    replies = (void *)2;
    busy = true;
    benchmark_active = false;
    active_session = cli_session;
    cancelled = false;
    cli_connected = fake_connected = true;
    cli_stalled = cli_stall_reported = false;
    cli_tx_count = USB_CLI_TX_SIZE;
    cli_last_progress = fake_ticks = 0;
    reply_count = 8;
    drained_replies = 0;
    fake_done = false;
    storage_request_t request = { .session = cli_session };
    TEST_ASSERT_FALSE(send_reply("blocked", &request));
    TEST_ASSERT_TRUE(cli_stalled && cancelled && busy);
    TEST_ASSERT_EQUAL_UINT(8, drained_replies);
    TEST_ASSERT_EQUAL_UINT(USB_CLI_STALL_TIMEOUT, fake_ticks);
    reply_count = 1;
    fake_done = true;
    cdc_cli_task();
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
    TEST_ASSERT_TRUE(pf_storage_worker_submit("sd", "status", false, cli_session));
    fake_connected = false;
    cdc_cli_task();
    TEST_ASSERT_FALSE(cli_connected || cli_stalled);
    TEST_ASSERT_EQUAL_UINT(0, cli_tx_count);
    fake_connected = true;
}

void test_filesystem_encoder_matches_shared_exfat_vector(void)
{
    FILE *fixture = fopen("tests/fixtures/hid-filesystem.csv", "r");
    TEST_ASSERT_NOT_NULL(fixture);
    char row[128];
    bool found = false;
    while (fgets(row, sizeof(row), fixture)) {
        if (strncmp(row, "exfat,", 6) != 0) continue;
        unsigned bytes[4];
        TEST_ASSERT_EQUAL_INT(4, sscanf(row + 6, "%2x%2x%2x%2x", &bytes[0], &bytes[1], &bytes[2], &bytes[3]));
        pf_hid_filesystem_report_t report;
        pf_hid_build_filesystem_report(&report, (uint8_t)bytes[2]);
        uint8_t expected[] = { (uint8_t)bytes[1], (uint8_t)bytes[2], (uint8_t)bytes[3] };
        TEST_ASSERT_EQUAL_UINT8(PF_HID_FILESYSTEM_REPORT_ID, bytes[0]);
        TEST_ASSERT_EQUAL_MEMORY(expected, &report, sizeof(expected));
        found = true;
    }
    fclose(fixture);
    TEST_ASSERT_TRUE(found);
}

void test_set_report_validates_callback_envelope_and_command_fields(void)
{
    led_requests = msc_requests = force_requests = 0;
    uint8_t command[] = {PF_HID_REPORT_VERSION, 1u};
    const uint8_t ids[] = {PF_HID_LED_COMMAND_REPORT_ID, PF_HID_MSC_COMMAND_REPORT_ID};
    for (unsigned i = 0; i < sizeof(ids); ++i) {
        tud_hid_set_report_cb(1u, ids[i], HID_REPORT_TYPE_FEATURE, command, sizeof(command));
        tud_hid_set_report_cb(0u, ids[i], HID_REPORT_TYPE_INPUT, command, sizeof(command));
        tud_hid_set_report_cb(0u, ids[i], HID_REPORT_TYPE_FEATURE, NULL, sizeof(command));
        tud_hid_set_report_cb(0u, ids[i], HID_REPORT_TYPE_FEATURE, command, 0u);
        tud_hid_set_report_cb(0u, ids[i], HID_REPORT_TYPE_FEATURE, command, 1u);
        command[0] = 0xffu;
        tud_hid_set_report_cb(0u, ids[i], HID_REPORT_TYPE_FEATURE, command, sizeof(command));
        command[0] = PF_HID_REPORT_VERSION;
        command[1] = 0xffu;
        tud_hid_set_report_cb(0u, ids[i], HID_REPORT_TYPE_FEATURE, command, sizeof(command));
        command[1] = 1u;
    }
    tud_hid_set_report_cb(0u, 0xffu, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    TEST_ASSERT_EQUAL_UINT(0u, led_requests + msc_requests + force_requests);
    tud_hid_set_report_cb(0u, ids[0], HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    TEST_ASSERT_EQUAL_UINT(1u, led_requests);
    TEST_ASSERT_TRUE(requested_state);
    command[1] = 0u;
    tud_hid_set_report_cb(0u, ids[1], HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    TEST_ASSERT_EQUAL_UINT(1u, msc_requests);
    TEST_ASSERT_FALSE(requested_state);
    uint8_t force[] = {PF_HID_MSC_REPORT_VERSION, 2u, 0u};
    tud_hid_set_report_cb(0u, ids[1], HID_REPORT_TYPE_FEATURE, force, sizeof(force));
    TEST_ASSERT_EQUAL_UINT(0u, force_requests);
    tud_hid_set_report_cb(0u, ids[1], HID_REPORT_TYPE_FEATURE, force, 2u);
    TEST_ASSERT_EQUAL_UINT(1u, force_requests);
}

void test_set_report_self_test_requires_exact_size_and_zero_cancel_payload(void)
{
    unavailable = false;
    busy = benchmark_active = benchmark_finished = benchmark_cancelled = false;
    media_service_busy = false;
    initialized = mounted = true;
    queue_fails = false;
    queued = 0;
    uint8_t command[] = {PF_HID_REPORT_VERSION, 1u, 32u, 0u, 0u, 0u, 0u};
    for (unsigned size = 0; size < sizeof(command); ++size) {
        if (size == sizeof(pf_hid_self_test_command_t)) continue;
        tud_hid_set_report_cb(0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, size);
    }
    TEST_ASSERT_EQUAL_UINT(0u, queued);
    command[0] = 0xffu;
    tud_hid_set_report_cb(0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 6u);
    command[0] = PF_HID_REPORT_VERSION;
    command[1] = 2u;
    tud_hid_set_report_cb(0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 6u);
    TEST_ASSERT_EQUAL_UINT(0u, queued);
    command[1] = 1u;
    tud_hid_set_report_cb(0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 6u);
    TEST_ASSERT_EQUAL_UINT(1u, queued);
    command[1] = 0u;
    tud_hid_set_report_cb(0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 6u);
    TEST_ASSERT_FALSE(benchmark_cancelled);
    command[2] = 0u;
    tud_hid_set_report_cb(0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, 6u);
    TEST_ASSERT_TRUE(benchmark_cancelled);
    busy = benchmark_active = false;
}
