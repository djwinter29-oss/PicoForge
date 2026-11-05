#include "unity.h"
#include <stdio.h>
#include <string.h>

#define PF_ENABLE_WIRELESS 1
#define PF_ENABLE_STORAGE_BENCHMARK 1
#define PF_FIRMWARE_VERSION_MAJOR 1u
#define PF_FIRMWARE_VERSION_MINOR 2u
#define PF_FIRMWARE_VERSION_PATCH 3u
#include "Mockwireless.h"
TEST_INCLUDE_PATH("firmware/tests/support/runtime")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
TEST_INCLUDE_PATH("firmware/tests/support/config")
#include "storage/responsiveness_fixture.inc"

static bool tcp_client, staged_values;
static const char *tcp_close_notice, *tcp_input;
static unsigned tcp_reads, wireless_calls, saved_calls, discarded_calls;
static uint32_t tcp_output_space, tcp_output_size;
static char tcp_output[128];
static uint8_t save_precondition;

bool pf_tcp_cli_connected(void) { return tcp_client; }
bool pf_tcp_cli_read(char *character)
{
    if (tcp_input == NULL || *tcp_input == '\0') return false;
    *character = *tcp_input++;
    ++tcp_reads;
    return true;
}
uint32_t pf_tcp_cli_write_available(void) { return tcp_output_space; }
uint32_t pf_tcp_cli_write(const char *data, uint32_t size)
{
    if (size > tcp_output_space) size = tcp_output_space;
    TEST_ASSERT_LESS_THAN(sizeof(tcp_output), tcp_output_size + size);
    memcpy(tcp_output + tcp_output_size, data, size);
    tcp_output_size += size;
    tcp_output[tcp_output_size] = '\0';
    tcp_output_space -= size;
    return size;
}
void pf_tcp_cli_flush(void) {}
void pf_tcp_cli_close(const char *notice) { tcp_client = false; tcp_close_notice = notice; }

static void wireless_status(uint8_t *report, int call)
{
    (void)call;
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[0] = 1u;
}
static bool wireless_staged(int call) { (void)call; return staged_values; }
static uint8_t wireless_precondition(bool commit, int call)
{
    (void)commit;
    (void)call;
    return save_precondition;
}
static bool wireless_apply(uint8_t *mask, int call) { (void)call; *mask = 7u; return true; }
static void wireless_saved(int call) { (void)call; ++saved_calls; }
static void wireless_discard(int call) { (void)call; ++discarded_calls; }
static void wireless_command(const pf_hid_control_request_t *request, pf_hid_control_result_t *result, int call)
{
    (void)call;
    ++wireless_calls;
    *result = (pf_hid_control_result_t){ .version = 1u, .transaction_id = request->transaction_id,
        .opcode = request->opcode, .status = PF_HID_COMMAND_COMPLETED };
    if (request->opcode == PF_HID_OPCODE_TCP_PROOF) {
        for (size_t i = 0u; i < 16u; ++i) result->result_data[i] = request->arguments[i] ^ 0x5au;
    }
}
static pf_hid_control_request_t request(uint8_t opcode)
{
    return (pf_hid_control_request_t){.version = 1u, .transaction_id = 7u, .opcode = opcode};
}
static void send_request(const pf_hid_control_request_t *command)
{
    tud_hid_set_report_cb(0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          (const uint8_t *)command, sizeof(*command));
}
static pf_hid_control_result_t result(void)
{
    pf_hid_control_result_t report;
    TEST_ASSERT_EQUAL_UINT16(68u, tud_hid_get_report_cb(0u, 20u, HID_REPORT_TYPE_FEATURE,
        (uint8_t *)&report, sizeof(report)));
    return report;
}

void setUp(void)
{
    Mockwireless_Init();
    requests = (void *)1;
    replies = (void *)2;
    config_available = true;
    busy = media_service_busy = false;
    unavailable = reserved = initialized = mounted = false;
    msc_enabled = msc_transition_pending = false;
    reboot_requested = force_save_pending = force_save_result_read = force_save_reboot_waiting = false;
    self_test_result_pending = ble_dispatch = false;
    tcp_auth_proof_expires_us = 0u;
    memset(&bench, 0, sizeof(bench));
    control_result = ble_result = (pf_hid_control_result_t){.version = 1u};
    led_requests = msc_requests = force_requests = 0u;
    wireless_calls = saved_calls = discarded_calls = 0u;
    staged_values = false;
    save_precondition = 0u;
    pf_wireless_start_Ignore();
    pf_wireless_poll_Ignore();
    pf_wireless_status_StubWithCallback(wireless_status);
    pf_wireless_staged_StubWithCallback(wireless_staged);
    pf_wireless_save_precondition_StubWithCallback(wireless_precondition);
    pf_wireless_apply_staged_StubWithCallback(wireless_apply);
    pf_wireless_saved_StubWithCallback(wireless_saved);
    pf_wireless_discard_StubWithCallback(wireless_discard);
    pf_wireless_command_StubWithCallback(wireless_command);
    test_radios = saved_radios = PF_WIRELESS_WIFI | PF_WIRELESS_BLE;
    test_tcp_cli_enabled = saved_tcp_cli_enabled = true;
    test_sysclk = saved_sysclk = PF_SYSCLK_DEFAULT;
    test_sd_speed = saved_sd_speed = PF_STORAGE_SPEED_DEFAULT;
    settings_save_calls = 0u;
    settings_save_fails = false;
    cli_owner = NULL;
    fake_connected = tcp_client = false;
    tcp_close_notice = tcp_input = NULL;
    tcp_reads = tcp_output_space = tcp_output_size = 0u;
    tcp_output[0] = '\0';
    cli_reboot_requested = false;
    reset_requests = reboot_usb_polls = fake_ticks = 0u;
}

void tearDown(void)
{
    Mockwireless_Verify();
    Mockwireless_Destroy();
    TEST_ASSERT_FALSE(reserved);
}

void test_usb_and_ble_slots_are_separate_and_ble_rejects_assigned_and_unknown_opcodes(void)
{
    control_result.transaction_id = 99u;
    pf_hid_control_request_t command = request(PF_HID_OPCODE_LED);
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_COMPLETED, ble_result.status);
    TEST_ASSERT_EQUAL_UINT8(99u, control_result.transaction_id);
    command.opcode = PF_HID_OPCODE_SELF_TEST;
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_UNSUPPORTED, ble_result.status);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_ERROR_CAPABILITY, ble_result.error_reason);
    command.opcode = 255u;
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_ERROR_OPCODE, ble_result.error_reason);
}

void test_ble_led_remains_available_during_forced_save_but_reboot_is_busy_with_detail_one(void)
{
    force_save_pending = true;
    msc_enabled = true;
    pf_hid_control_request_t command = request(PF_HID_OPCODE_LED);
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_COMPLETED, ble_result.status);
    command.opcode = PF_HID_OPCODE_REBOOT;
    command.flags = 1u;
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_BUSY, ble_result.status);
    TEST_ASSERT_EQUAL_UINT8(1u, ble_result.result_data[0]);
}

void test_ble_reboot_and_save_cannot_discard_usb_staging(void)
{
    staged_values = true;
    pf_hid_control_request_t command = request(PF_HID_OPCODE_REBOOT);
    command.flags = 1u;
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    TEST_ASSERT_EQUAL_UINT8(28u, ble_result.result_data[0]);
    command = request(PF_HID_OPCODE_SAVE_SETTINGS);
    command.arguments[3] = 7u;
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_BUSY, ble_result.status);
    TEST_ASSERT_EQUAL_UINT8(28u, ble_result.result_data[0]);
    TEST_ASSERT_EQUAL_UINT(0u, settings_save_calls);
    TEST_ASSERT_EQUAL_UINT(0u, discarded_calls);
}

void test_save_opcode_five_confirms_switches_and_atomic_wireless_commit_mask(void)
{
    pf_hid_control_request_t command = request(PF_HID_OPCODE_SAVE_SETTINGS);
    command.flags = PF_HID_SAVE_SETTINGS_COMMIT_WIRELESS;
    command.arguments[0] = 200u;
    command.arguments[2] = 1u;
    command.arguments[3] = 5u;
    send_request(&command);
    pf_hid_control_result_t report = result();
    TEST_ASSERT_EQUAL_UINT8(5u, report.opcode);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_COMPLETED, report.status);
    TEST_ASSERT_EQUAL_UINT8(5u, report.result_data[3]);
    TEST_ASSERT_EQUAL_UINT8(7u, report.result_data[4]);
    TEST_ASSERT_EQUAL_UINT(1u, saved_calls);
    TEST_ASSERT_EQUAL_UINT8(1u, saved_radios);
    TEST_ASSERT_TRUE(saved_tcp_cli_enabled);
    TEST_ASSERT_TRUE(pf_hid_take_reboot_request());
}

void test_save_state_rejection_preserves_candidates_and_follows_busy_precedence(void)
{
    pf_hid_control_request_t command = request(PF_HID_OPCODE_SAVE_SETTINGS);
    command.arguments[3] = 7u;
    save_precondition = 27u;
    send_request(&command);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_INVALID, control_result.status);
    TEST_ASSERT_EQUAL_UINT8(27u, control_result.result_data[0]);
    busy = true;
    send_request(&command);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_BUSY, control_result.status);
    TEST_ASSERT_EQUAL_UINT8(1u, control_result.result_data[0]);
    TEST_ASSERT_EQUAL_UINT(0u, settings_save_calls);
    TEST_ASSERT_EQUAL_UINT(0u, saved_calls + discarded_calls);
}

void test_wireless_report_twelve_is_read_only_and_legacy_reports_are_unassigned(void)
{
    uint8_t buffer[68];
    TEST_ASSERT_EQUAL_UINT16(48u, tud_hid_get_report_cb(
        0u, 12u, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, 12u, HID_REPORT_TYPE_FEATURE, buffer, 47u));
    for (uint8_t id = 30u; id <= 39u; ++id) {
        TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
            0u, id, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer)));
    }
}

void test_tcp_proof_get_is_retryable_until_expiry_and_ble_does_not_replace_it(void)
{
    pf_hid_control_request_t command = request(PF_HID_OPCODE_TCP_PROOF);
    send_request(&command);
    pf_hid_control_result_t first = result(), second = result();
    TEST_ASSERT_EQUAL_MEMORY(&first, &second, sizeof(first));
    TEST_ASSERT_EQUAL_UINT8(0x5au, first.result_data[0]);
    command = request(PF_HID_OPCODE_LED);
    TEST_ASSERT_TRUE(pf_hid_ble_control(&command));
    second = result();
    TEST_ASSERT_EQUAL_MEMORY(&first, &second, sizeof(first));
    command.version = 2u;
    send_request(&command);
    second = result();
    TEST_ASSERT_EQUAL_MEMORY(&first, &second, sizeof(first));
    fake_ticks += 10000u;
    second = result();
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_FAILED, second.status);
    TEST_ASSERT_EACH_EQUAL_UINT8(0u, second.result_data, sizeof(second.result_data));
}

void test_full_argument_validation_rejects_trailing_bytes_before_side_effects(void)
{
    pf_hid_control_request_t command = request(PF_HID_OPCODE_LED);
    command.arguments[63] = 1u;
    send_request(&command);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_INVALID, control_result.status);
    TEST_ASSERT_EQUAL_UINT8(PF_HID_ERROR_MALFORMED, control_result.error_reason);
    TEST_ASSERT_EQUAL_UINT(0u, led_requests);
}

void test_cli_session_belongs_to_the_first_transport_and_turns_the_other_away(void)
{
    fake_connected = tcp_client = true;
    cli_task();
    TEST_ASSERT_EQUAL_PTR(&cdc_transport, cli_owner);
    TEST_ASSERT_FALSE(tcp_client);
    TEST_ASSERT_NOT_NULL(strstr(tcp_close_notice, "USB CDC session"));
    fake_connected = false;
    tcp_client = true;
    cli_task();
    TEST_ASSERT_EQUAL_PTR(&tcp_transport, cli_owner);
    fake_connected = true;
    cli_task();
    TEST_ASSERT_EQUAL_PTR(&tcp_transport, cli_owner);
    tcp_client = false;
    cli_task();
    TEST_ASSERT_EQUAL_PTR(&cdc_transport, cli_owner);
}

void test_stalled_tcp_session_is_closed_instead_of_waiting_for_the_client(void)
{
    tcp_client = true;
    cli_task();
    cli_tx_count = 1u;
    cli_last_progress = 0u;
    fake_ticks = USB_CLI_STALL_TIMEOUT;
    cli_task();
    TEST_ASSERT_NULL(cli_owner);
    TEST_ASSERT_FALSE(tcp_client || cli_stalled);
    TEST_ASSERT_EQUAL_UINT(0u, cli_tx_count);
}

void test_cli_pauses_dispatch_during_hid_save_reboot_and_benchmark_admission(void)
{
    tcp_client = true;
    cli_task();
    tcp_input = "x";
    bool *gates[] = { &force_save_pending, &force_save_reboot_waiting, &reboot_requested, &self_test_result_pending };
    for (unsigned i = 0u; i < sizeof(gates) / sizeof(gates[0]); ++i) {
        *gates[i] = true;
        cli_task();
        TEST_ASSERT_EQUAL_UINT(0u, tcp_reads);
        *gates[i] = false;
    }
    cli_task();
    TEST_ASSERT_EQUAL_UINT(1u, tcp_reads);
}

void test_cli_reboot_stops_new_input_and_flushes_tcp_acknowledgement_before_reset(void)
{
    tcp_client = true;
    cli_task();
    static const char ack[] = "OK config saved; rebooting\r\n";
    for (size_t i = 0u; i < sizeof(ack) - 1u; ++i) cli_write_character(ack[i], NULL);
    tcp_input = "board set led on\r";
    cli_request_reboot();
    cli_task();
    TEST_ASSERT_EQUAL_UINT(0u, tcp_reads);
    tcp_output_space = sizeof(tcp_output) - 1u;
    reboot_after_usb_flush();
    TEST_ASSERT_EQUAL_STRING(ack, tcp_output);
    TEST_ASSERT_EQUAL_UINT(1u, reset_requests);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(PF_HID_REBOOT_DELAY_MS, fake_ticks);
}
