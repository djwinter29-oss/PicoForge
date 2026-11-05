#include "unity.h"
#include <stdio.h>
#include <string.h>

#define PF_ENABLE_WIRELESS 1
#define PF_ENABLE_STORAGE_BENCHMARK 1
#define PF_FIRMWARE_VERSION_MAJOR 1u
#define PF_FIRMWARE_VERSION_MINOR 2u
#define PF_FIRMWARE_VERSION_PATCH 3u
#include "Mockwireless.h"
#include "Mockwireless_auth.h"
TEST_INCLUDE_PATH("firmware/tests/support/runtime")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
TEST_INCLUDE_PATH("firmware/tests/support/config")
#include "storage/responsiveness_fixture.inc"

static unsigned provisioning_calls;
static unsigned wireless_command_calls, wireless_reject_calls, wireless_result_calls;
static uint8_t wireless_command[PF_WIRELESS_COMMAND_SIZE];
static uint8_t wireless_result[PF_WIRELESS_RESULT_SIZE];
static uint8_t rejected_transaction;
static bool tcp_auth_proof_available = true;
static bool tcp_client;
static const char *tcp_close_notice;
static unsigned tcp_closes;
static const char *tcp_input;
static unsigned tcp_reads;
static uint32_t tcp_output_space;
static char tcp_output[128];
static uint32_t tcp_output_size;
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
void pf_tcp_cli_close(const char *notice)
{
    tcp_client = false;
    tcp_close_notice = notice;
    ++tcp_closes;
}
static void mock_wireless_status(uint8_t *report, int call_count)
{
    (void)call_count;
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[PF_WIRELESS_STATUS_VERSION_OFFSET] = 1u;
}

static bool mock_wireless_configure(const uint8_t *report, int call_count)
{
    (void)report;
    (void)call_count;
    ++provisioning_calls;
    return true;
}

static void mock_wireless_command(const uint8_t *report, int call_count)
{
    (void)call_count;
    ++wireless_command_calls;
    memcpy(wireless_command, report, sizeof(wireless_command));
}

static void mock_wireless_reject_command(uint8_t transaction, int call_count)
{
    (void)call_count;
    ++wireless_reject_calls;
    rejected_transaction = transaction;
}

static void mock_wireless_result_report(uint8_t *report, int call_count)
{
    (void)call_count;
    ++wireless_result_calls;
    memcpy(report, wireless_result, sizeof(wireless_result));
}

static bool mock_tcp_auth_proof(const uint8_t nonce[16], uint8_t proof[16], int call_count)
{
    (void)call_count;
    if (!tcp_auth_proof_available) return false;
    for (size_t i = 0u; i < 16u; ++i) proof[i] = (uint8_t)(nonce[i] ^ 0x5au);
    return true;
}

void setUp(void)
{
    Mockwireless_Init();
    Mockwireless_auth_Init();
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
    wireless_command_calls = wireless_reject_calls = wireless_result_calls = 0u;
    memset(wireless_command, 0, sizeof(wireless_command));
    memset(wireless_result, 0, sizeof(wireless_result));
    wireless_result[0] = 1u;
    rejected_transaction = 0u;
    tcp_auth_proof_available = true;
    pf_wireless_start_Ignore();
    pf_wireless_poll_Ignore();
    pf_wireless_status_StubWithCallback(mock_wireless_status);
    pf_wireless_configure_StubWithCallback(mock_wireless_configure);
    pf_wireless_command_StubWithCallback(mock_wireless_command);
    pf_wireless_reject_command_StubWithCallback(mock_wireless_reject_command);
    pf_wireless_result_report_StubWithCallback(mock_wireless_result_report);
    pf_wireless_auth_tcp_cli_proof_StubWithCallback(mock_tcp_auth_proof);
    test_radios = saved_radios = PF_WIRELESS_WIFI | PF_WIRELESS_BLE;
    settings_save_fails = false;
    cli_owner = NULL;
    fake_connected = tcp_client = false;
    tcp_close_notice = NULL;
    tcp_closes = 0u;
    tcp_input = NULL;
    tcp_reads = tcp_output_space = tcp_output_size = 0u;
    tcp_output[0] = '\0';
    cli_reboot_requested = false;
    reset_requests = reboot_usb_polls = fake_ticks = 0u;
}

void tearDown(void)
{
    Mockwireless_Verify();
    Mockwireless_auth_Verify();
    Mockwireless_Destroy();
    Mockwireless_auth_Destroy();
    TEST_ASSERT_FALSE(reserved);
}

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

void test_usb_tcp_cli_auth_report_signs_only_a_valid_one_use_request(void)
{
    uint8_t request[PF_WIRELESS_AUTH_REQUEST_SIZE] = {PF_HID_REPORT_VERSION, 7u};
    for (size_t i = 0u; i < 16u; ++i) request[2u + i] = (uint8_t)i;
    tud_hid_set_report_cb(0u, PF_WIRELESS_AUTH_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          request, sizeof(request));
    uint8_t result[PF_WIRELESS_AUTH_RESULT_SIZE] = {0};
    TEST_ASSERT_EQUAL_UINT16(PF_WIRELESS_AUTH_RESULT_SIZE, tud_hid_get_report_cb(
        0u, PF_WIRELESS_AUTH_REPORT_ID, HID_REPORT_TYPE_FEATURE, result, sizeof(result)));
    TEST_ASSERT_EQUAL_UINT8(PF_HID_REPORT_VERSION, result[0]);
    TEST_ASSERT_EQUAL_UINT8(7u, result[1]);
    TEST_ASSERT_EQUAL_UINT8(1u, result[2]);
    for (size_t i = 0u; i < 16u; ++i) TEST_ASSERT_EQUAL_UINT8((uint8_t)i ^ 0x5au, result[3u + i]);
    memset(result, 0xff, sizeof(result));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, PF_WIRELESS_AUTH_REPORT_ID, HID_REPORT_TYPE_FEATURE, result, sizeof(result)));

    tcp_auth_proof_available = false;
    request[1] = 8u;
    tud_hid_set_report_cb(0u, PF_WIRELESS_AUTH_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          request, sizeof(request));
    TEST_ASSERT_EQUAL_UINT16(PF_WIRELESS_AUTH_RESULT_SIZE, tud_hid_get_report_cb(
        0u, PF_WIRELESS_AUTH_REPORT_ID, HID_REPORT_TYPE_FEATURE, result, sizeof(result)));
    TEST_ASSERT_EQUAL_UINT8(8u, result[1]);
    TEST_ASSERT_EQUAL_UINT8(0u, result[2]);
    TEST_ASSERT_EQUAL_UINT8(0u, result[3]);

    request[1] = 9u;
    request[18] = 1u;
    tud_hid_set_report_cb(0u, PF_WIRELESS_AUTH_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          request, sizeof(request));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, PF_WIRELESS_AUTH_REPORT_ID, HID_REPORT_TYPE_FEATURE, result, sizeof(result)));
}

void test_wireless_scan_result_requires_a_full_feature_buffer(void)
{
    uint8_t buffer[PF_WIRELESS_RESULT_SIZE];
    memset(buffer, 0xa5, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, PF_WIRELESS_RESULT_REPORT_ID, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer) - 1u));
    TEST_ASSERT_EQUAL_UINT16(0u, tud_hid_get_report_cb(
        0u, PF_WIRELESS_RESULT_REPORT_ID, HID_REPORT_TYPE_INPUT, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_UINT(0u, wireless_result_calls);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xa5u, buffer, sizeof(buffer));

    wireless_result[1] = 7u;
    wireless_result[sizeof(wireless_result) - 1u] = 0x5au;
    TEST_ASSERT_EQUAL_UINT16(PF_WIRELESS_RESULT_SIZE, tud_hid_get_report_cb(
        0u, PF_WIRELESS_RESULT_REPORT_ID, HID_REPORT_TYPE_FEATURE, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_UINT(1u, wireless_result_calls);
    TEST_ASSERT_EQUAL_MEMORY(wireless_result, buffer, sizeof(buffer));
}

void test_wireless_scan_command_validates_framing_and_forwards_once(void)
{
    uint8_t command[PF_WIRELESS_COMMAND_SIZE] = {1u, 1u, 7u};
    tud_hid_set_report_cb(1u, PF_WIRELESS_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    tud_hid_set_report_cb(0u, PF_WIRELESS_COMMAND_REPORT_ID, HID_REPORT_TYPE_INPUT, command, sizeof(command));
    tud_hid_set_report_cb(0u, PF_WIRELESS_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, NULL, sizeof(command));
    tud_hid_set_report_cb(0u, PF_WIRELESS_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command) - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, wireless_command_calls);
    TEST_ASSERT_EQUAL_UINT(0u, wireless_reject_calls);

    tud_hid_set_report_cb(0u, PF_WIRELESS_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
    TEST_ASSERT_EQUAL_UINT(1u, wireless_command_calls);
    TEST_ASSERT_EQUAL_UINT(0u, wireless_reject_calls);
    TEST_ASSERT_EQUAL_MEMORY(command, wireless_command, sizeof(command));
}

void test_wireless_scan_command_is_rejected_during_save_or_reboot(void)
{
    uint8_t command[PF_WIRELESS_COMMAND_SIZE] = {1u, 1u, 7u};
    bool *gates[] = {&force_save_pending, &force_save_reboot_waiting, &reboot_requested};
    for (unsigned i = 0u; i < sizeof(gates) / sizeof(gates[0]); ++i) {
        *gates[i] = true;
        tud_hid_set_report_cb(0u, PF_WIRELESS_COMMAND_REPORT_ID, HID_REPORT_TYPE_FEATURE, command, sizeof(command));
        *gates[i] = false;
        TEST_ASSERT_EQUAL_UINT(i + 1u, wireless_reject_calls);
        TEST_ASSERT_EQUAL_UINT8(command[2], rejected_transaction);
        TEST_ASSERT_EQUAL_UINT(0u, wireless_command_calls);
    }
}

static uint8_t save_radios(uint8_t transaction_id, uint8_t radios)
{
    pf_hid_control_request_t request = {.version = 1u, .transaction_id = transaction_id,
        .opcode = PF_HID_OPCODE_SAVE_SETTINGS, .arguments = {150u, 0u, 1u, radios, 0u}};
    tud_hid_set_report_cb(0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          (const uint8_t *)&request, sizeof(request));
    return control_result.status;
}

void test_radio_enables_are_reported_in_board_flags_and_saved_by_opcode_6(void)
{
    pf_hid_configuration_report_t configuration;
    pf_hid_build_configuration_report(&configuration);
    TEST_ASSERT_EQUAL_HEX8(0x18u, configuration.board_flags & 0x18u);

    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_INVALID, save_radios(1u, 4u));
    TEST_ASSERT_EQUAL_HEX8(PF_WIRELESS_WIFI | PF_WIRELESS_BLE, test_radios);
    settings_save_fails = true;
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_FAILED, save_radios(2u, PF_WIRELESS_BLE));
    TEST_ASSERT_EQUAL_HEX8(PF_WIRELESS_WIFI | PF_WIRELESS_BLE, test_radios);
    settings_save_fails = false;
    TEST_ASSERT_EQUAL_UINT8(PF_HID_COMMAND_COMPLETED, save_radios(3u, PF_WIRELESS_BLE));
    TEST_ASSERT_EQUAL_UINT8(PF_WIRELESS_BLE, control_result.result_data[3]);
    TEST_ASSERT_TRUE(pf_hid_take_reboot_request());
    pf_hid_build_configuration_report(&configuration);
    TEST_ASSERT_EQUAL_HEX8(0x10u, configuration.board_flags & 0x18u);
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
    fake_ticks = 0u;
    tcp_client = true;
    cli_task();
    TEST_ASSERT_EQUAL_PTR(&tcp_transport, cli_owner);
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
    TEST_ASSERT_EQUAL_UINT(0u, cli_tx_count);
    TEST_ASSERT_EQUAL_UINT(1u, reset_requests);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(PF_HID_REBOOT_DELAY_MS, fake_ticks);
}
