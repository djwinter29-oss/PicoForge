# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Exercise the production BLE GATT callbacks against a minimal fake BTstack, without radio hardware."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts

UUID = "_7c91_4d70_8b32_6e856443f502_01_VALUE_HANDLE"
HANDLES = {"0001": 3, "0002": 5, "0003": 7, "000a": 9, "000b": 11, "0014": 13, "000c": 15,
           "0100": 17, "0101": 19}


def test_ble_v2_profile_and_direct_client_share_uuid_layout():
    profile = (ROOT / "firmware/src/app/wireless/bluetooth/bluetooth_profile.gatt").read_text()
    direct = (ROOT / "host/direct/web/js/transport/ble.mjs").read_text()
    service_uuid = "50fc0000-7c91-4d70-8b32-6e856443f502"
    assert f"PRIMARY_SERVICE, {service_uuid}" in profile
    assert service_uuid in direct
    assert "CHARACTERISTIC, GAP_APPEARANCE, READ, 00 00" in profile
    assert "PRIMARY_SERVICE, 1801\nCHARACTERISTIC, 2A05, INDICATE,\nCHARACTERISTIC, 2B2A, READ | DYNAMIC," in profile
    assert "PRIMARY_SERVICE, 180A" in profile and "CHARACTERISTIC, 2A26, READ | DYNAMIC," in profile
    assert "50fc0100-7c91-4d70-8b32-6e856443f502" in profile
    assert "50fc0101-7c91-4d70-8b32-6e856443f502" in profile
    assert "50fc0020-7c91-4d70-8b32-6e856443f501" not in profile
    assert "50fc0021-7c91-4d70-8b32-6e856443f501" not in profile


FAKE_BTSTACK = r"""
#ifndef FAKE_BTSTACK_H
#define FAKE_BTSTACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint16_t hci_con_handle_t;
typedef uint8_t bd_addr_t[6];
typedef void (*btstack_packet_handler_t)(uint8_t, uint16_t, uint8_t *, uint16_t);
typedef struct { btstack_packet_handler_t callback; } btstack_packet_callback_registration_t;
typedef uint16_t (*att_read_callback_t)(hci_con_handle_t, uint16_t, uint16_t, uint8_t *, uint16_t);
typedef int (*att_write_callback_t)(hci_con_handle_t, uint16_t, uint16_t, uint16_t, uint8_t *, uint16_t);
typedef struct { int unused; } btstack_tlv_t;
typedef struct { int unused; } btstack_tlv_flash_bank_t;
typedef struct { int unused; } hal_flash_bank_t;
#define HCI_EVENT_PACKET 4
#define ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE 0xb6u
#define BTSTACK_EVENT_STATE 0x60
#define HCI_STATE_WORKING 2
#define HCI_EVENT_DISCONNECTION_COMPLETE 0x05
#define HCI_EVENT_LE_META 0x3e
#define HCI_SUBEVENT_LE_CONNECTION_COMPLETE 0x01
#define ERROR_CODE_SUCCESS 0
#define ERROR_CODE_COMMAND_DISALLOWED 0x0c
#define ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER 0x02
#define ATT_HANDLE_VALUE_INDICATION_TIMEOUT 0x91
#define ATT_HANDLE_VALUE_INDICATION_DISCONNECT 0x92
#define ATT_TRANSACTION_MODE_NONE 0
#define ATT_ERROR_REQUEST_NOT_SUPPORTED 0x06
#define ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH 0x0d
#define ATT_ERROR_WRITE_NOT_PERMITTED 0x03
#define ATT_ERROR_INVALID_OFFSET 0x07
#define ATT_ERROR_VALUE_NOT_ALLOWED 0x13
#define ATT_READ_ERROR_CODE_OFFSET 0xfe00u
#define IO_CAPABILITY_NO_INPUT_NO_OUTPUT 3
#define SM_AUTHREQ_NO_BONDING 0
#define BLUETOOTH_DATA_TYPE_FLAGS 0x01
#define BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS 0x07
#define BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME 0x09
#define HCI_POWER_ON 1
static inline uint8_t hci_event_packet_get_type(const uint8_t *packet) { return packet[0]; }
static inline uint8_t btstack_event_state_get_state(const uint8_t *packet) { return packet[2]; }
static inline uint8_t hci_event_le_meta_get_subevent_code(const uint8_t *packet) { return packet[2]; }
static inline uint8_t hci_subevent_le_connection_complete_get_status(const uint8_t *packet) { return packet[3]; }
static inline uint8_t att_event_handle_value_indication_complete_get_status(const uint8_t *packet)
{ return packet[2]; }
static inline hci_con_handle_t att_event_handle_value_indication_complete_get_conn_handle(const uint8_t *packet)
{ return (hci_con_handle_t)(packet[3] | ((uint16_t)packet[4] << 8)); }
static inline uint16_t att_event_handle_value_indication_complete_get_attribute_handle(const uint8_t *packet)
{ return (uint16_t)(packet[5] | ((uint16_t)packet[6] << 8)); }
uint16_t att_read_callback_handle_blob(const uint8_t *blob, uint16_t blob_size, uint16_t offset, uint8_t *buffer,
                                       uint16_t buffer_size);
void gap_advertisements_enable(int enabled);
void gap_advertisements_set_params(uint16_t minimum, uint16_t maximum, uint8_t type, uint8_t direct_type,
                                   bd_addr_t direct, uint8_t channels, uint8_t filter);
void gap_advertisements_set_data(uint8_t length, uint8_t *data);
void gap_scan_response_set_data(uint8_t length, uint8_t *data);
void l2cap_init(void);
void sm_init(void);
void sm_set_io_capabilities(uint8_t capabilities);
void sm_set_authentication_requirements(uint8_t requirements);
void att_server_init(const uint8_t *profile, att_read_callback_t read, att_write_callback_t write);
void att_server_register_packet_handler(btstack_packet_handler_t handler);
uint8_t att_server_indicate(hci_con_handle_t connection, uint16_t handle, const uint8_t *value, uint16_t size);
uint8_t gap_disconnect(hci_con_handle_t connection);
void hci_add_event_handler(btstack_packet_callback_registration_t *registration);
int hci_power_control(int power);
uint64_t time_us_64(void);
#endif
"""

HARNESS = r"""
#include <assert.h>
#include <string.h>
#include "btstack.h"
#include "app/wireless/bluetooth/bluetooth.h"
#include "app/wireless/wireless_auth.h"
#include "app/wireless/wireless.h"
#include "app/usb/usb_hid.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "pico/rand.h"
#include "bluetooth_profile.h"

#define HANDLE(id) ATT_CHARACTERISTIC_50fc##id##_7c91_4d70_8b32_6e856443f502_01_VALUE_HANDLE
#define SERVICE_CHANGED_CCCD ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE
static att_read_callback_t read_cb;
static att_write_callback_t write_cb;
static btstack_packet_handler_t events;
static btstack_packet_handler_t att_events;
static const uint8_t *advertising, *scan_response;
static uint8_t advertising_length, scan_response_length;
static uint8_t key[32];
static uint64_t now, random_value = 0x0102030405060708ull;
static unsigned controls, advertising_enables, telemetry_builds, storage_builds, benchmark_builds;
static uint16_t advertising_minimum, advertising_maximum;
static uint8_t expected_database_hash[16];
static unsigned indication_count;
static unsigned disconnect_count;
static hci_con_handle_t disconnected_handle;
static uint16_t indication_handle, indication_size;
static uint8_t indication_value[4];
static uint8_t indication_status;
static uint8_t disconnect_status;

uint16_t att_read_callback_handle_blob(const uint8_t *blob, uint16_t blob_size, uint16_t offset, uint8_t *buffer,
                                       uint16_t buffer_size)
{
    if (offset > blob_size) return 0u;
    uint16_t length = (uint16_t)(blob_size - offset);
    if (buffer == NULL) return length;
    if (length > buffer_size) length = buffer_size;
    memcpy(buffer, blob + offset, length);
    return length;
}
void gap_advertisements_enable(int enabled) { if (enabled) ++advertising_enables; }
void gap_advertisements_set_params(uint16_t minimum, uint16_t maximum, uint8_t type, uint8_t direct_type,
                                   bd_addr_t direct, uint8_t channels, uint8_t filter)
{ advertising_minimum = minimum; advertising_maximum = maximum; (void)type; (void)direct_type;
  (void)direct; (void)channels; (void)filter; }
void gap_advertisements_set_data(uint8_t length, uint8_t *data) { advertising_length = length; advertising = data; }
void gap_scan_response_set_data(uint8_t length, uint8_t *data) { scan_response_length = length; scan_response = data; }
void l2cap_init(void) {}
void sm_init(void) {}
void sm_set_io_capabilities(uint8_t capabilities) { assert(capabilities == IO_CAPABILITY_NO_INPUT_NO_OUTPUT); }
void sm_set_authentication_requirements(uint8_t requirements) { assert(requirements == SM_AUTHREQ_NO_BONDING); }
void att_server_init(const uint8_t *profile, att_read_callback_t read, att_write_callback_t write)
{ (void)profile; read_cb = read; write_cb = write; }
void att_server_register_packet_handler(btstack_packet_handler_t handler) { att_events = handler; }
void hci_add_event_handler(btstack_packet_callback_registration_t *registration) { events = registration->callback; }
int hci_power_control(int power) { assert(power == HCI_POWER_ON); return 0; }
uint8_t gap_disconnect(hci_con_handle_t connection)
{ ++disconnect_count; disconnected_handle = connection; return disconnect_status; }
uint8_t att_server_indicate(hci_con_handle_t connection, uint16_t handle, const uint8_t *value, uint16_t size)
{
    assert(connection == 1u && size == sizeof(indication_value));
    ++indication_count;
    indication_handle = handle;
    indication_size = size;
    memcpy(indication_value, value, size);
    return indication_status;
}
uint64_t time_us_64(void) { return now; }
bool gatt_server_get_database_hash(uint8_t hash[16])
{ memcpy(hash, expected_database_hash, sizeof(expected_database_hash)); return true; }
uint64_t get_rand_64(void) { return random_value++; }
void pico_get_unique_board_id(pico_unique_board_id_t *uid) { memset(uid->id, 0x5a, sizeof(uid->id)); }
void pico_get_unique_board_id_string(char *out, unsigned length)
{ memset(out, 'F', length - 1u); out[length - 1u] = '\0'; }
const btstack_tlv_t *btstack_tlv_none_init_instance(void) { static btstack_tlv_t none; return &none; }
void pf_boot_config_ble_key(uint8_t out[32]) { memcpy(out, key, sizeof(key)); }
bool pf_boot_config_ble_key_configured(void)
{ uint8_t bits = 0u; for (unsigned i = 0u; i < 32u; ++i) bits |= key[i]; return bits != 0u; }
uint8_t pf_boot_config_radios(void) { return PF_WIRELESS_BLE; }
/* Fake verifier: the proof is the nonce with byte 0 XOR request byte 0; the real HMAC is in test_wireless_auth.py. */
void pf_wireless_auth_set_key(const uint8_t k[32]) { (void)k; }
bool pf_wireless_auth_available(void) { return key[0] != 0u; }
bool pf_wireless_auth_new_nonce(uint8_t nonce[16])
{
    for (unsigned i = 0u; i < 16u; i += sizeof(uint64_t)) {
        uint64_t random = get_rand_64();
        memcpy(nonce + i, &random, sizeof(random));
    }
    return true;
}
void pf_wireless_auth_device_id(uint8_t uid[8]) { memset(uid, 0x5a, 8u); }
bool pf_wireless_auth_verify(const uint8_t nonce[16], const uint8_t domain[9], const uint8_t request[9],
                             const uint8_t proof[16])
{
    return memcmp(domain, "PFBLECTL1", 9u) == 0 && (uint8_t)(nonce[0] ^ request[0]) == proof[0] &&
           memcmp(nonce + 1, proof + 1, 15u) == 0;
}
void pf_wireless_status(uint8_t *report) { memset(report, 0xa5, PF_WIRELESS_STATUS_SIZE); }
void pf_hid_build_identity_report(pf_hid_identity_report_t *report) { memset(report, 1, sizeof(*report)); }
void pf_hid_build_configuration_report(pf_hid_configuration_report_t *report) { memset(report, 2, sizeof(*report)); }
void pf_hid_build_telemetry_report(pf_hid_telemetry_report_t *report, uint8_t sequence)
{ ++telemetry_builds; memset(report, sequence, sizeof(*report)); }
void pf_hid_build_storage_report(pf_hid_storage_report_t *report, uint8_t sequence)
{ ++storage_builds; memset(report, sequence, sizeof(*report)); }
void pf_hid_build_self_test_report(pf_hid_self_test_report_t *report, uint8_t sequence)
{ ++benchmark_builds; memset(report, sequence, sizeof(*report)); }
bool pf_hid_self_test_supported(void) { return true; }
bool pf_hid_ble_control(const pf_hid_control_request_t *request) { (void)request; ++controls; return true; }
void pf_hid_ble_result(pf_hid_control_result_t *report) { memset(report, 0, sizeof(*report)); }
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; return true; }

static void event(uint8_t type, uint8_t a, uint8_t b)
{
    uint8_t packet[4] = {type, 0u, a, b};
    events(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
}

static void indication_event(uint8_t status, hci_con_handle_t connection, uint16_t handle)
{
    uint8_t packet[7] = {ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE, 5u, status,
                         (uint8_t)connection, (uint8_t)(connection >> 8),
                         (uint8_t)handle, (uint8_t)(handle >> 8)};
    assert(att_events != NULL);
    att_events(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
}

static void read_nonce(uint8_t nonce[16])
{
    assert(read_cb(1u, HANDLE(0100), 0u, NULL, 0u) == 16u);
    assert(read_cb(1u, HANDLE(0100), 0u, nonce, 16u) == 16u);
}

/* Turn a fresh nonce into the fake proof for a request starting with first. */
static uint8_t *prove(uint8_t nonce[16], uint8_t first)
{
    nonce[0] ^= first;
    return nonce;
}

static int proof_write(uint8_t *proof) { return write_cb(1u, HANDLE(0101), ATT_TRANSACTION_MODE_NONE, 0u, proof, 16u); }

static void __attribute__((noinline)) clobber_stack(void)
{
    volatile uint8_t junk[512];
    for (unsigned i = 0u; i < sizeof(junk); ++i) junk[i] = 0xee;
}

int main(void)
{
    uint8_t request[PF_HID_BLE_CONTROL_PAYLOAD_SIZE] = {1u, 7u, 2u, 1u};
    uint8_t led[PF_HID_BLE_CONTROL_PAYLOAD_SIZE] = {1u, 8u, PF_HID_OPCODE_LED, 0u, 1u};
    uint8_t nonce[16], other[16], buffer[64];
    const uint16_t rejected_read = ATT_READ_ERROR_CODE_OFFSET + 0x80u;

    /* Bluetooth never starts without a saved control key, even when its switch is on. */
    assert(!pf_bluetooth_prepare());
    key[0] = 1u;
    assert(pf_bluetooth_prepare() && pf_bluetooth_start());
    assert(att_events != NULL);
    clobber_stack();
    assert(advertising_length == 21u && advertising[0] == 2u && advertising[2] == 6u &&
           advertising[5] == 0x02u && advertising[20] == 0x50u);
    assert(scan_response_length == 2u + 10u + 16u && memcmp(scan_response + 2, "PicoForge-FFFF", 14u) == 0);
    assert(advertising_minimum == 96u && advertising_maximum == 96u);
    for (unsigned i = 0u; i < sizeof(expected_database_hash); ++i) expected_database_hash[i] = (uint8_t)i;
    assert(pf_bluetooth_state() == 0u);
    event(BTSTACK_EVENT_STATE, HCI_STATE_WORKING, 0u);
    assert(pf_bluetooth_state() == 1u && advertising_enables == 1u);
    assert(read_cb(1u, ATT_CHARACTERISTIC_2A26_01_VALUE_HANDLE, 0u, buffer, sizeof(buffer)) == 5u);
    assert(memcmp(buffer, "1.1.1", 5u) == 0);
    assert(read_cb(1u, ATT_CHARACTERISTIC_2B2A_01_VALUE_HANDLE, 0u, buffer, sizeof(buffer)) == 16u);
    assert(memcmp(buffer, expected_database_hash, sizeof(expected_database_hash)) == 0);
    event(HCI_EVENT_LE_META, HCI_SUBEVENT_LE_CONNECTION_COMPLETE, 1u);
    assert(pf_bluetooth_state() == 1u);
    event(HCI_EVENT_LE_META, HCI_SUBEVENT_LE_CONNECTION_COMPLETE, ERROR_CODE_SUCCESS);
    assert(pf_bluetooth_state() == 2u);
    uint8_t cccd_value[2];
    assert(read_cb(1u, ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE, 0u,
                   cccd_value, sizeof(cccd_value)) == 2u);
    assert(cccd_value[0] == 0u && cccd_value[1] == 0u);
    uint8_t cccd[] = {2u, 0u};
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    assert(read_cb(1u, ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE, 0u,
                   cccd_value, sizeof(cccd_value)) == 2u);
    assert(cccd_value[0] == 2u && cccd_value[1] == 0u);
    indication_status = 1u;
    pf_bluetooth_poll();
    assert(indication_count == 1u);
    indication_status = 0u;
    pf_bluetooth_poll();
    assert(indication_count == 2u && indication_handle == ATT_CHARACTERISTIC_2A05_01_VALUE_HANDLE &&
           indication_size == 4u && indication_value[0] == 1u && indication_value[1] == 0u &&
           indication_value[2] == 0xffu && indication_value[3] == 0xffu);
    /* No indication-confirmation event is delivered by this harness: never resubmit on this connection. */
    cccd[0] = 0u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    cccd[0] = 2u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    pf_bluetooth_poll();
    assert(indication_count == 2u);
    disconnect_status = ERROR_CODE_COMMAND_DISALLOWED;
    indication_event(ATT_HANDLE_VALUE_INDICATION_TIMEOUT, 1u, ATT_CHARACTERISTIC_2A05_01_VALUE_HANDLE);
    assert(disconnect_count == 1u && disconnected_handle == 1u && pf_bluetooth_state() == 2u);
    cccd[0] = 2u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    pf_bluetooth_poll();
    assert(indication_count == 2u && disconnect_count == 2u);
    disconnect_status = ERROR_CODE_SUCCESS;
    pf_bluetooth_poll();
    assert(indication_count == 2u && disconnect_count == 3u && disconnected_handle == 1u);
    cccd[0] = 2u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    pf_bluetooth_poll();
    assert(indication_count == 2u && disconnect_count == 3u);
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0u, 0u);
    assert(pf_bluetooth_state() == 1u && advertising_enables == 2u);
    event(HCI_EVENT_LE_META, HCI_SUBEVENT_LE_CONNECTION_COMPLETE, ERROR_CODE_SUCCESS);
    assert(pf_bluetooth_state() == 2u);
    assert(read_cb(1u, ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE, 0u,
                   cccd_value, sizeof(cccd_value)) == 2u);
    assert(cccd_value[0] == 0u && cccd_value[1] == 0u);
    cccd[0] = 2u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    pf_bluetooth_poll();
    assert(indication_count == 3u);
    indication_event(ERROR_CODE_SUCCESS, 1u, ATT_CHARACTERISTIC_2A05_01_VALUE_HANDLE);
    cccd[0] = 0u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    cccd[0] = 2u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    pf_bluetooth_poll();
    assert(indication_count == 3u && disconnect_count == 3u);
    cccd[0] = 1u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) ==
           ATT_ERROR_VALUE_NOT_ALLOWED);
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, 1u) ==
           ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    cccd[0] = 0u;
    assert(write_cb(1u, SERVICE_CHANGED_CCCD, ATT_TRANSACTION_MODE_NONE, 0u, cccd, sizeof(cccd)) == 0);
    assert(read_cb(1u, ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE, 0u,
                   cccd_value, sizeof(cccd_value)) == 2u);
    assert(cccd_value[0] == 0u && cccd_value[1] == 0u);

    /* Before the session proof, only the name, identity and nonce read; every write, LED included, is refused. */
    assert(read_cb(1u, ATT_CHARACTERISTIC_GAP_DEVICE_NAME_01_VALUE_HANDLE, 0u, buffer, sizeof(buffer)) == 26u);
    assert(read_cb(1u, HANDLE(0001), 0u, buffer, sizeof(buffer)) == sizeof(pf_hid_identity_report_t));
    const uint16_t locked_handles[] = {HANDLE(0002), HANDLE(0003), HANDLE(000a), HANDLE(000b), HANDLE(0014),
                                       HANDLE(000c)};
    for (unsigned i = 0u; i < sizeof(locked_handles) / sizeof(locked_handles[0]); ++i) {
        assert(read_cb(1u, locked_handles[i], 0u, NULL, 0u) == rejected_read);
        assert(read_cb(1u, locked_handles[i], 0u, buffer, sizeof(buffer)) == rejected_read);
    }
    assert(telemetry_builds == 0u && storage_builds == 0u && benchmark_builds == 0u);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, led, sizeof(led)) == 0x80);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x80);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, 1u) == 0x80);
    assert(controls == 0u);

    /* A control proof cannot unlock the session; a wrong proof starts a two-second lockout. */
    read_nonce(nonce);
    assert(write_cb(1u, HANDLE(0101), ATT_TRANSACTION_MODE_NONE, 0u, nonce, 15u) ==
           ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    assert(proof_write(prove(nonce, 1u)) == 0x80);
    read_nonce(nonce);
    now = 1999999u;
    assert(proof_write(prove(nonce, 'S')) == 0x82);
    now = 2000000u;
    assert(read_cb(1u, HANDLE(0003), 0u, NULL, 0u) == rejected_read);
    assert(proof_write(nonce) == 0);
    assert(proof_write(nonce) == 0x83);

    /* BTstack length probes, including discovery, must not build reports or advance sequence numbers. */
    for (unsigned i = 0; i < 2u; ++i) {
        assert(read_cb(1u, HANDLE(0003), 0u, NULL, 0u) == sizeof(pf_hid_telemetry_report_t));
        assert(read_cb(1u, HANDLE(000a), 0u, NULL, 0u) == sizeof(pf_hid_storage_report_t));
        assert(read_cb(1u, HANDLE(000b), 0u, NULL, 0u) == sizeof(pf_hid_self_test_report_t));
    }
    assert(telemetry_builds == 0u && storage_builds == 0u && benchmark_builds == 0u);
    assert(read_cb(1u, HANDLE(0003), 0u, buffer, 8u) == 8u && buffer[0] == 0u);
    assert(read_cb(1u, HANDLE(0003), 8u, NULL, 0u) == sizeof(pf_hid_telemetry_report_t));
    /* A length query for another handle must not invalidate an in-progress long read. */
    assert(read_cb(1u, HANDLE(000a), 0u, NULL, 0u) == sizeof(pf_hid_storage_report_t));
    assert(read_cb(1u, HANDLE(0003), 8u, buffer, 8u) == 8u && buffer[0] == 0u);
    assert(read_cb(1u, HANDLE(000a), 8u, buffer, 8u) ==
           ATT_READ_ERROR_CODE_OFFSET + ATT_ERROR_INVALID_OFFSET);
    assert(read_cb(1u, HANDLE(000a), 0u, buffer, 8u) == 8u && buffer[0] == 1u);
    assert(read_cb(1u, HANDLE(000b), 0u, buffer, 8u) == 8u && buffer[0] == 2u);
    assert(read_cb(1u, HANDLE(000b), sizeof(pf_hid_self_test_report_t) + 1u, buffer, 8u) ==
           ATT_READ_ERROR_CODE_OFFSET + ATT_ERROR_INVALID_OFFSET);
    assert(telemetry_builds == 1u && storage_builds == 1u && benchmark_builds == 1u);

    /* Unlocked readers get full status, including the SSID and LAN address. */
    assert(read_cb(1u, HANDLE(000c), 0u, buffer, sizeof(buffer)) == PF_WIRELESS_STATUS_SIZE);
    for (unsigned i = 5u; i < 42u; ++i) {
        assert(buffer[i] == 0xa5);
    }

    /* On an unlocked link the LED needs no proof, but each other control needs a fresh one-use proof. */
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, led, sizeof(led)) == 0 && controls == 1u);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x83);
    read_nonce(nonce);
    assert(proof_write(prove(nonce, 1u)) == 0);
    /* Envelope rejection comes before proof validation and preserves the pending proof. */
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, 8u) == 0x81);
    request[0] = 2u;
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x81);
    request[0] = 1u;
    request[1] = 0u;
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x81);
    request[1] = 7u;
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0);
    assert(controls == 2u);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x83);
    assert(proof_write(nonce) == 0x83);

    /* A new nonce invalidates an earlier proof; a wrong proof starts a two-second lockout. */
    read_nonce(nonce);
    assert(proof_write(prove(nonce, 1u)) == 0);
    read_nonce(other);
    assert(memcmp(nonce, other, 16u) != 0);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x80);
    read_nonce(nonce);
    assert(proof_write(nonce) == 0);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x80);
    read_nonce(nonce);
    now = 3999999u;
    assert(proof_write(prove(nonce, 1u)) == 0x82);
    now = 4000000u;
    assert(proof_write(nonce) == 0);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0);
    assert(controls == 3u);

    /* An LED write neither consumes nor rejects a pending proof. */
    read_nonce(nonce);
    assert(proof_write(prove(nonce, 1u)) == 0);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, led, sizeof(led)) == 0 && controls == 4u);
    assert(write_cb(1u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0);
    assert(controls == 5u);

    /* A disconnect drops the session, the pending proof and the frozen long-read snapshot. */
    now = 30000000u;
    read_nonce(nonce);
    assert(proof_write(prove(nonce, 1u)) == 0);
    assert(read_cb(1u, HANDLE(0001), 0u, buffer, 8u) == 8u);
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0u, 0u);
    assert(pf_bluetooth_state() == 1u && advertising_enables == 3u);
    event(HCI_EVENT_LE_META, HCI_SUBEVENT_LE_CONNECTION_COMPLETE, ERROR_CODE_SUCCESS);
    read_nonce(nonce);
    assert(proof_write(nonce) == 0x80);
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0u, 0u);
    event(HCI_EVENT_LE_META, HCI_SUBEVENT_LE_CONNECTION_COMPLETE, ERROR_CODE_SUCCESS);
    assert(proof_write(nonce) == 0x82);
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0u, 0u);
    assert(pf_bluetooth_state() == 1u && advertising_enables == 5u);
    assert(read_cb(2u, ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE, 0u,
                   cccd_value, sizeof(cccd_value)) == 2u);
    assert(cccd_value[0] == 0u && cccd_value[1] == 0u);
    assert(advertising_minimum == 96u && advertising_maximum == 96u);
    now = 59999999u;
    pf_bluetooth_poll();
    assert(advertising_minimum == 96u && advertising_maximum == 96u);
    now = 60000000u;
    pf_bluetooth_poll();
    assert(advertising_minimum == 800u && advertising_maximum == 800u);
    assert(read_cb(2u, HANDLE(0001), 8u, buffer, 8u) ==
           ATT_READ_ERROR_CODE_OFFSET + ATT_ERROR_INVALID_OFFSET);
    assert(read_cb(2u, HANDLE(000c), 0u, buffer, sizeof(buffer)) == rejected_read);
    assert(write_cb(2u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, led, sizeof(led)) == 0x80);
    assert(write_cb(2u, HANDLE(0014), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) == 0x80);
    assert(write_cb(2u, HANDLE(0002), ATT_TRANSACTION_MODE_NONE, 0u, request, sizeof(request)) ==
           ATT_ERROR_WRITE_NOT_PERMITTED);
    assert(controls == 5u);
    return 0;
}
"""


def test_ble_requires_key_session_proof_and_disconnect_reset(tmp_path):
    (tmp_path / "pico").mkdir()
    (tmp_path / "btstack.h").write_text(FAKE_BTSTACK)
    (tmp_path / "btstack_tlv_none.h").write_text(
        '#include "btstack.h"\nconst btstack_tlv_t *btstack_tlv_none_init_instance(void);\n')
    (tmp_path / "btstack_tlv_flash_bank.h").write_text('#include "btstack.h"\n')
    (tmp_path / "pico/rand.h").write_text("#include <stdint.h>\nuint64_t get_rand_64(void);\n")
    (tmp_path / "pico/unique_id.h").write_text(
        "#ifndef FAKE_UID_H\n#define FAKE_UID_H\n#include <stdint.h>\n#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8\n"
        "typedef struct { uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES]; } pico_unique_board_id_t;\n"
        "void pico_get_unique_board_id(pico_unique_board_id_t *uid);\n"
        "void pico_get_unique_board_id_string(char *out, unsigned length);\n#endif\n")
    (tmp_path / "bluetooth_profile.h").write_text(
        "#include <stdint.h>\nstatic const uint8_t profile_data[] = {0};\n"
        "#define ATT_CHARACTERISTIC_GAP_DEVICE_NAME_01_VALUE_HANDLE 1\n"
        "#define ATT_CHARACTERISTIC_2A26_01_VALUE_HANDLE 23\n"
        "#define ATT_CHARACTERISTIC_2B2A_01_VALUE_HANDLE 25\n"
        "#define ATT_CHARACTERISTIC_2A05_01_VALUE_HANDLE 27\n"
        "#define ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE 28\n"
        + "".join(f"#define ATT_CHARACTERISTIC_50fc{name}{UUID} {handle}\n" for name, handle in HANDLES.items()))
    (tmp_path / "ble").mkdir()
    (tmp_path / "ble/att_db.h").write_text("#include <stdbool.h>\n#include <stdint.h>\n"
        "bool gatt_server_get_database_hash(uint8_t hash[16]);\n")
    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS)
    binary = tmp_path / "bluetooth"
    compiled = subprocess.run([
        "gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-DPF_ENABLE_WIRELESS=1",
        "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/bluetooth/bluetooth.c"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary)], check=True)
