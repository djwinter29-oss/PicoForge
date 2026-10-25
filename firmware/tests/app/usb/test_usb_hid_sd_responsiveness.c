#include "unity.h"
#include <stdio.h>
#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/runtime")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
TEST_INCLUDE_PATH("firmware/tests/support/config")
#include "storage/responsiveness_fixture.inc"

static void send_control(uint8_t transaction_id, uint8_t opcode, uint8_t flags, const uint8_t arguments[5])
{
    pf_hid_control_request_t request = {
        .version = PF_HID_REPORT_VERSION,
        .transaction_id = transaction_id,
        .opcode = opcode,
        .flags = flags,
    };
    if (arguments != NULL) memcpy(request.arguments, arguments, sizeof(request.arguments));
    tud_hid_set_report_cb(0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          (const uint8_t *)&request, sizeof(request));
}

static pf_hid_control_result_t read_control_result(void)
{
    pf_hid_control_result_t result;
    TEST_ASSERT_EQUAL_UINT16(sizeof(result), tud_hid_get_report_cb(
        0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE, (uint8_t *)&result, sizeof(result)));
    return result;
}

void setUp(void)
{
    config_available = true;
    requests = (void *)1;
    replies = (void *)2;
    cli_session = 9u;
    test_sysclk = saved_sysclk = PF_SYSCLK_200MHZ;
    test_sd_speed = saved_sd_speed = PF_STORAGE_SPEED_DEFAULT;
    settings_save_calls = 0u;
    settings_save_fails = false;
    reset_requests = 0u;
    led_requests = msc_requests = force_requests = 0u;
    queries = queued = recovery_requests = normal_submissions = messages = 0u;
    reply_count = drained_replies = 0u;
    fake_ticks = 0u;
    fake_connected = true;
    fake_done = false;
    msc_enabled = msc_transition_pending = false;
    unavailable = reserved = initialized = mounted = queue_fails = retained_cid = false;
    busy = benchmark_active = benchmark_finished = benchmark_cancelled = false;
    media_service_busy = false;
    cli_connected = cli_stalled = cli_stall_reported = false;
    cli_tx_count = 0u;
    cli_last_progress = 0u;
    control_result = (pf_hid_control_result_t){.version = PF_HID_REPORT_VERSION};
    self_test_result_pending = false;
    benchmark_admission_pending = false;
    benchmark_admission_result_ready = false;
    benchmark_admission_accepted = false;
    benchmark_admission_error = PF_FILE_BENCHMARK_ERROR_NONE;
    benchmark_status = (pf_file_benchmark_status_t){.state = PF_FILE_BENCHMARK_IDLE};
    active_session = 0u;
    cancelled = false;
    (void)pf_hid_take_reboot_request();
}

void tearDown(void) { TEST_ASSERT_FALSE(reserved); }

void test_feature_reports_match_wire_sizes_and_populate_identity_configuration(void)
{
    pf_hid_identity_report_t identity;
    pf_hid_build_identity_report(&identity);
    TEST_ASSERT_EQUAL_UINT(PF_HID_IDENTITY_PAYLOAD_SIZE, sizeof(identity));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_REPORT_VERSION, identity.version);
    TEST_ASSERT_EQUAL_UINT16(PF_HID_PRODUCT_ID, identity.product_id);
    TEST_ASSERT_EQUAL_UINT8(PF_SYSTEM_MCU_RP2040, identity.mcu);
    TEST_ASSERT_EQUAL_UINT8(0u, identity.board_model);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_CAP_LED | PF_HID_CAP_REBOOT | PF_HID_CAP_SETTINGS |
                           PF_HID_CAP_SDIO | PF_HID_CAP_SELF_TEST, identity.capabilities);
    TEST_ASSERT_EQUAL_UINT32(532480u, identity.ram_total_bytes);
    TEST_ASSERT_EQUAL_UINT32(65536u, identity.ram_reserved_bytes);

    pf_hid_configuration_report_t configuration;
    pf_hid_build_configuration_report(&configuration);
    TEST_ASSERT_EQUAL_UINT(PF_HID_CONFIGURATION_PAYLOAD_SIZE, sizeof(configuration));
    TEST_ASSERT_EQUAL_UINT16(200u, configuration.stored_sysclk_profile);
    TEST_ASSERT_EQUAL_UINT8(0u, configuration.stored_sdcard_profile);
    TEST_ASSERT_EQUAL_UINT8(0u, configuration.monitor_uart_index);
    TEST_ASSERT_EQUAL_UINT8(2u, configuration.storage_gpios[0]);
    TEST_ASSERT_EQUAL_UINT8(3u, configuration.storage_gpios[1]);
    TEST_ASSERT_EQUAL_UINT8(4u, configuration.storage_gpios[2]);
    TEST_ASSERT_EQUAL_UINT8(7u, configuration.storage_gpios[5]);
    TEST_ASSERT_EQUAL_UINT8(2u, configuration.reset_cause);

    uint8_t payload[PF_HID_IDENTITY_PAYLOAD_SIZE];
    TEST_ASSERT_EQUAL_UINT16(sizeof(identity), tud_hid_get_report_cb(
        0u, PF_HID_IDENTITY_REPORT_ID, HID_REPORT_TYPE_FEATURE, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_MEMORY(&identity, payload, sizeof(identity));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, PF_HID_IDENTITY_REPORT_ID, HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
}

void test_input_reports_keep_telemetry_and_storage_samples_independent(void)
{
    pf_hid_telemetry_report_t telemetry;
    pf_hid_build_telemetry_report(&telemetry, 17u);
    TEST_ASSERT_EQUAL_UINT(PF_HID_TELEMETRY_PAYLOAD_SIZE, sizeof(telemetry));
    TEST_ASSERT_EQUAL_UINT8(17u, telemetry.sequence);
    TEST_ASSERT_EQUAL_UINT64(1230u, telemetry.uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(125000000u, telemetry.system_clock_hz);
    TEST_ASSERT_EQUAL_UINT16(1234u, telemetry.core0_service_load_basis_points);
    TEST_ASSERT_EQUAL_UINT16(5678u, telemetry.core1_service_load_basis_points);

    pf_hid_storage_report_t storage;
    unavailable = true;
    pf_hid_build_storage_report(&storage, 23u);
    TEST_ASSERT_EQUAL_UINT(PF_HID_STORAGE_PAYLOAD_SIZE, sizeof(storage));
    TEST_ASSERT_EQUAL_UINT8(23u, storage.sequence);
    TEST_ASSERT_FALSE(storage.sd_flags & PF_HID_STORAGE_SAMPLE_VALID);
    TEST_ASSERT_FALSE(reserved);

    unavailable = false;
    initialized = true;
    retained_cid = true;
    pf_hid_build_storage_report(&storage, 24u);
    TEST_ASSERT_TRUE(storage.sd_flags & PF_HID_STORAGE_SAMPLE_VALID);
    TEST_ASSERT_TRUE(storage.sd_flags & PF_HID_STORAGE_INITIALIZED);
    TEST_ASSERT_TRUE(storage.sd_flags & PF_HID_STORAGE_CID_VALID);
    TEST_ASSERT_EQUAL_UINT32(25000000u, storage.bus_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(128u, storage.block_count);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_BUS_MODE_SDIO, storage.bus_mode);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_SPEED_MODE_STANDARD, storage.speed_mode);

    uint8_t payload[PF_HID_STORAGE_PAYLOAD_SIZE];
    TEST_ASSERT_EQUAL_UINT16(sizeof(storage), tud_hid_get_report_cb(
        0u, PF_HID_STORAGE_REPORT_ID, HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
}

void test_control_feature_reports_transaction_results_and_rejects_malformed_envelopes(void)
{
    const uint8_t led_arguments[5] = {1u, 0u, 0u, 0u, 0u};
    send_control(7u, PF_HID_OPCODE_LED, 0u, led_arguments);
    pf_hid_control_result_t result = read_control_result();
    TEST_ASSERT_EQUAL_UINT8(7u, result.transaction_id);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_OPCODE_LED, result.opcode);
    TEST_ASSERT_EQUAL_UINT8(2u, result.status);
    TEST_ASSERT_EQUAL_UINT(1u, led_requests);
    TEST_ASSERT_TRUE(requested_state);

    pf_hid_control_request_t invalid = {
        .version = 0xffu,
        .transaction_id = 8u,
        .opcode = PF_HID_OPCODE_LED,
    };
    tud_hid_set_report_cb(0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          (const uint8_t *)&invalid, sizeof(invalid));
    const pf_hid_control_result_t unchanged = read_control_result();
    TEST_ASSERT_EQUAL_MEMORY(&result, &unchanged, sizeof(result));

    send_control(0u, PF_HID_OPCODE_LED, 0u, led_arguments);
    const pf_hid_control_result_t still_unchanged = read_control_result();
    TEST_ASSERT_EQUAL_MEMORY(&result, &still_unchanged, sizeof(result));
    const uint8_t invalid_arguments[5] = {2u, 0u, 0u, 0u, 0u};
    send_control(8u, PF_HID_OPCODE_LED, 0u, invalid_arguments);
    result = read_control_result();
    TEST_ASSERT_EQUAL_UINT8(8u, result.transaction_id);
    TEST_ASSERT_EQUAL_UINT8(3u, result.status);
    TEST_ASSERT_EQUAL_UINT8(3u, result.error_reason);
    TEST_ASSERT_EQUAL_UINT(1u, led_requests);
}

void test_save_settings_command_updates_next_boot_profile_and_returns_saved_values(void)
{
    const uint8_t arguments[5] = {150u, 0u, 1u, 0u, 0u};
    const uint8_t bad_profile[5] = {150u, 0u, 2u, 0u, 0u};
    const uint8_t bad_clock[5] = {100u, 0u, 1u, 0u, 0u};
    send_control(9u, PF_HID_OPCODE_SAVE_SETTINGS, 0u, arguments);
    const pf_hid_control_result_t result = read_control_result();
    TEST_ASSERT_EQUAL_UINT8(2u, result.status);
    TEST_ASSERT_EQUAL_UINT(1u, settings_save_calls);
    TEST_ASSERT_EQUAL(PF_SYSCLK_150MHZ, saved_sysclk);
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_HIGH, saved_sd_speed);
    TEST_ASSERT_EQUAL_UINT16(150u, result.result_data[0] | ((uint16_t)result.result_data[1] << 8u));
    TEST_ASSERT_EQUAL_UINT8(1u, result.result_data[2]);
    TEST_ASSERT_TRUE(pf_hid_take_reboot_request());
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());

    /* Rejected or failed saves leave the RAM copy equal to the saved one. */
    test_sysclk = saved_sysclk = PF_SYSCLK_200MHZ;
    test_sd_speed = saved_sd_speed = PF_STORAGE_SPEED_DEFAULT;
    send_control(10u, PF_HID_OPCODE_SAVE_SETTINGS, 0u, bad_profile);
    TEST_ASSERT_EQUAL_UINT8(3u, read_control_result().status);
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, test_sysclk);
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());
    send_control(13u, PF_HID_OPCODE_SAVE_SETTINGS, 0u, bad_clock);
    TEST_ASSERT_EQUAL_UINT8(3u, read_control_result().status);
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, test_sd_speed);
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());

    settings_save_fails = true;
    send_control(11u, PF_HID_OPCODE_SAVE_SETTINGS, 0u, arguments);
    TEST_ASSERT_EQUAL_UINT8(6u, read_control_result().status);
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, test_sysclk);
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, test_sd_speed);
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());

    busy = true;
    send_control(12u, PF_HID_OPCODE_SAVE_SETTINGS, 0u, arguments);
    TEST_ASSERT_EQUAL_UINT8(5u, read_control_result().status);
    TEST_ASSERT_EQUAL_UINT(2u, settings_save_calls);
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, test_sysclk);
    TEST_ASSERT_FALSE(pf_hid_take_reboot_request());
}

void test_self_test_command_rejects_bad_size_then_queues_valid_work(void)
{
    initialized = mounted = true;
    const uint8_t invalid_arguments[5] = {1u, 1u, 4u, 0u, 0u};
    send_control(11u, PF_HID_OPCODE_SELF_TEST, 0u, invalid_arguments);
    pf_hid_control_result_t result = read_control_result();
    TEST_ASSERT_EQUAL_UINT8(3u, result.status);
    TEST_ASSERT_EQUAL_UINT8(3u, result.error_reason);
    TEST_ASSERT_EQUAL_UINT(0u, queued);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_SIZE, benchmark_status.request_error);

    const uint8_t valid_arguments[5] = {1u, 32u, 0u, 0u, 0u};
    send_control(12u, PF_HID_OPCODE_SELF_TEST, 0u, valid_arguments);
    result = read_control_result();
    TEST_ASSERT_EQUAL_UINT8(1u, result.status);
    TEST_ASSERT_EQUAL_UINT(1u, queued);
    TEST_ASSERT_TRUE(busy && benchmark_active);

    benchmark_status = (pf_file_benchmark_status_t){
        .state = PF_FILE_BENCHMARK_QUEUED,
        .target_bytes = 32u * 1024u * 1024u,
    };
    benchmark_admission_pending = false;
    benchmark_admission_result_ready = true;
    benchmark_admission_accepted = true;
    benchmark_admission_error = PF_FILE_BENCHMARK_ERROR_NONE;
    pf_hid_poll_async_commands();
    TEST_ASSERT_EQUAL_UINT8(2u, read_control_result().status);

    pf_hid_self_test_report_t self_test;
    pf_hid_build_self_test_report(&self_test, 42u);
    TEST_ASSERT_EQUAL_UINT(PF_HID_SELF_TEST_PAYLOAD_SIZE, sizeof(self_test));
    TEST_ASSERT_EQUAL_UINT8(42u, self_test.sequence);
    TEST_ASSERT_EQUAL_UINT8(1u, self_test.state);
    uint8_t payload[PF_HID_SELF_TEST_PAYLOAD_SIZE];
    TEST_ASSERT_EQUAL_UINT16(sizeof(self_test), tud_hid_get_report_cb(
        0u, PF_HID_SELF_TEST_REPORT_ID, HID_REPORT_TYPE_INPUT, payload, sizeof(payload)));
}

void test_failed_self_test_preflight_preserves_last_test_result(void)
{
    initialized = mounted = true;
    benchmark_status = (pf_file_benchmark_status_t){
        .state = PF_FILE_BENCHMARK_PASSED,
        .target_bytes = 1048576u,
        .written_bytes = 1048576u,
        .read_bytes = 1048576u,
    };
    const uint8_t arguments[5] = {1u, 32u, 0u, 0u, 0u};
    send_control(13u, PF_HID_OPCODE_SELF_TEST, 0u, arguments);
    TEST_ASSERT_EQUAL_UINT8(1u, read_control_result().status);

    benchmark_status.request_error = PF_FILE_BENCHMARK_ERROR_SPACE;
    benchmark_admission_pending = false;
    benchmark_admission_result_ready = true;
    benchmark_admission_accepted = false;
    benchmark_admission_error = PF_FILE_BENCHMARK_ERROR_SPACE;
    benchmark_finished = true;
    pf_hid_poll_async_commands();

    const pf_hid_control_result_t result = read_control_result();
    TEST_ASSERT_EQUAL_UINT8(6u, result.status);
    TEST_ASSERT_EQUAL_UINT8(6u, result.error_reason);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_PASSED, benchmark_status.state);
    TEST_ASSERT_EQUAL_UINT32(1048576u, benchmark_status.target_bytes);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_SPACE, benchmark_status.request_error);
}

void test_stalled_full_reply_path_cancels_and_drains_completion_before_new_work(void)
{
    busy = true;
    benchmark_active = false;
    active_session = cli_session;
    cancelled = false;
    cli_connected = fake_connected = true;
    cli_stalled = cli_stall_reported = false;
    cli_tx_count = USB_CLI_TX_SIZE;
    cli_last_progress = fake_ticks = 0u;
    reply_count = 8u;
    drained_replies = 0u;
    fake_done = false;
    storage_request_t request = {.session = cli_session};
    TEST_ASSERT_FALSE(send_reply("blocked", &request));
    TEST_ASSERT_TRUE(cli_stalled && cancelled && busy);
    TEST_ASSERT_EQUAL_UINT(8u, drained_replies);
    TEST_ASSERT_EQUAL_UINT(USB_CLI_STALL_TIMEOUT, fake_ticks);
    reply_count = 1u;
    fake_done = true;
    cdc_cli_task();
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
    TEST_ASSERT_TRUE(pf_storage_worker_submit("storage", "status", false, cli_session));
    fake_connected = false;
    cdc_cli_task();
    TEST_ASSERT_FALSE(cli_connected || cli_stalled);
    TEST_ASSERT_EQUAL_UINT(0u, cli_tx_count);
    fake_connected = true;
}
