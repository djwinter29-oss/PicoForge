// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file bluetooth.c
 * @brief Key-gated BLE GATT diagnostics and control, per-command benchmark proofs and nonpersistent advertising.
 */
#include "app/wireless/bluetooth/bluetooth.h"
#include "app/wireless/wireless.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "ble/att_db.h"
#include "btstack_tlv_none.h"
#include "btstack_tlv_flash_bank.h"
#include "app/wireless/wireless_auth.h"
#include "pico/unique_id.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "app/usb/usb_hid.h"
#include "bluetooth_profile.h"

/** Generated handles are owned by the pinned SDK GATT compiler. */
#define BLE_HANDLE(id) ATT_CHARACTERISTIC_50fc##id##_7c91_4d70_8b32_6e856443f502_01_VALUE_HANDLE
/**
 * Application ATT error for a locked link or rejected proof. Standard Insufficient Authentication (0x05) can
 * trigger OS pairing, which PicoForge does not use as its application authorization mechanism.
 */
#define BLE_ERROR_CONTROL_REJECTED 0x80
#define BLE_ERROR_ENVELOPE 0x81 /**< Invalid control length/version/transaction; proof remains usable. */
#define BLE_ERROR_RATE_LIMITED 0x82 /**< A rejected proof started the two-second retry delay. */
#define BLE_ERROR_CHALLENGE_UNAVAILABLE 0x83 /**< The key or a fresh challenge is unavailable. */
#define BLE_AUTH_RETRY_DELAY_US 2000000u /**< Minimum interval between rejected authentication attempts. */
#define BLE_FAST_ADVERTISING_INTERVAL 96u /**< 60 ms in 0.625 ms advertising units. */
#define BLE_FAST_ADVERTISING_WINDOW_US 30000000u
#define BLE_SLOW_ADVERTISING_INTERVAL 800u /**< 500 ms in 0.625 ms advertising units. */

/** @brief Result of checking a one-use BLE proof, mapped to an application ATT error. */
typedef enum {
    PF_BLE_AUTH_ACCEPTED,
    PF_BLE_AUTH_REJECTED,
    PF_BLE_AUTH_RATE_LIMITED,
    PF_BLE_AUTH_CHALLENGE_UNAVAILABLE,
} pf_ble_auth_result_t;

/** @brief BLE controller/link state encoded in the wireless status report. */
typedef enum {
    PF_BLUETOOTH_STATE_STARTING = 0, /**< Controller is not yet working. */
    PF_BLUETOOTH_STATE_ADVERTISING = 1, /**< Controller is advertising without a connected central. */
    PF_BLUETOOTH_STATE_CONNECTED = 2, /**< A central is connected. */
} pf_bluetooth_state_t;

static bool control_key_available; /**< A control key is loaded; BLE does not start without one. */
static bool session_authorized; /**< This connection proved the key; until then only identity and nonce read. */
static bool challenge_valid; /**< Nonce issued once and usable for one authentication attempt. */
static bool proof_valid; /**< A 16-byte proof is ready for exactly one subsequent control request. */
static uint8_t proof[16]; /**< Submitted per-command MAC, cleared on use or disconnect. */
static uint8_t nonce[16]; /**< Fresh random challenge, invalidated by use or disconnect. */
/** @brief Protocol domain separating BLE control proofs from TCP CLI proofs. */
static const uint8_t ble_auth_domain[9] = {'P', 'F', 'B', 'L', 'E', 'C', 'T', 'L', '1'};
/** @brief Fixed proof request that unlocks the connection; never dispatched as a control. */
static const uint8_t ble_session_request[9] = {'S', 'E', 'S', 'S', 'I', 'O', 'N', '0', '1'};
static uint64_t auth_retry_us; /**< Failed authentication rate limit; never blocks the owner task. */
/** Properly aligned typed report storage, frozen for the current ATT long read. */
static union {
    uint8_t bytes[PF_WIRELESS_STATUS_SIZE]; /**< ATT payload view. */
    pf_hid_identity_report_t identity; /**< Identity payload. */
    pf_hid_configuration_report_t config; /**< Public board configuration payload. */
    pf_hid_telemetry_report_t telemetry; /**< Runtime telemetry payload. */
    pf_hid_storage_report_t storage; /**< Storage payload. */
    pf_hid_self_test_report_t benchmark; /**< Benchmark payload. */
    pf_hid_control_result_t result; /**< BLE-specific control result. */
} read_buffer;
static uint16_t read_handle; /**< Characteristic owning the frozen read buffer. */
static uint16_t read_length; /**< Valid frozen payload length. */
static uint8_t report_sequence; /**< Sequence for BLE-polled diagnostic snapshots. */
static pf_bluetooth_state_t link_state; /**< Current controller/link state. */
static btstack_packet_callback_registration_t events; /**< Static HCI state/disconnect registration. */
/** Scan-response "PicoForge-<USB serial>" local name; BTstack keeps the pointer, and GAP Device Name reads it. */
static uint8_t scan_response[2u + 10u + 2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
static uint64_t fast_advertising_until_us; /**< Fast-advertising window end after startup or disconnect. */
static char firmware_revision[12]; /**< Public firmware version string, without a NUL on air. */
static uint8_t firmware_revision_length;
static uint8_t database_hash[16]; /**< Public BTstack GATT database hash. */
static hci_con_handle_t service_changed_connection; /**< Unbonded peer awaiting a cache-invalidation indication. */
static uint8_t service_changed_cccd[2]; /**< Current connection's readable indication subscription value. */
static bool service_changed_pending; /**< CCCD enabled; indication waits for the current ATT response to finish. */
static bool service_changed_awaiting_confirmation; /**< Indication submitted but not yet confirmed. */
static bool service_changed_sent; /**< Full-range invalidation confirmed on this connection. */
static bool service_changed_failed; /**< Indication timed out; do not retry before the next connection. */
static uint8_t service_changed_last_error; /**< Last transient indication error, to avoid logging every poll. */
static bool service_changed_disconnect_pending; /**< Timed-out indication requires a link disconnect. */
static hci_con_handle_t service_changed_disconnect_connection; /**< Connection queued for timeout disconnect. */
static uint8_t service_changed_disconnect_last_error; /**< Last disconnect error, to suppress repeated warnings. */
static const uint8_t service_changed_range[4] = {0x01u, 0x00u, 0xffu, 0xffu}; /**< Little-endian handle range. */

/** @brief Set the undirected advertising interval for fast discovery or normal low-duty operation. */
static void set_advertising_interval(uint16_t interval)
{
    bd_addr_t empty = {0};
    gap_advertisements_set_params(interval, interval, 0, 0, empty, 7, 0);
}

/**
 * @brief Keep the SDK Bluetooth TLV nonpersistent: application authorization does not use OS pairing/bonding.
 *
 * The SDK default initializes two flash sectors, overlapping the Pico W settings sector. Interpose before any bank
 * access; only boot_config may write flash. A future bonded-pairing implementation must reserve and arbitrate storage.
 */
const btstack_tlv_t *__wrap_btstack_tlv_flash_bank_init_instance(btstack_tlv_flash_bank_t *context,
                                                               const hal_flash_bank_t *bank, void *bank_context)
{
    (void)context;
    (void)bank;
    (void)bank_context;
    PF_LOGGING_INFO("ble", "BLE uses application proofs; persistent OS bonding is disabled");
    return btstack_tlv_none_init_instance();
}

/** @brief Reset per-connection authorization and read snapshots, then begin/restart peripheral advertising. */
static void advertise(void)
{
    proof_valid = challenge_valid = session_authorized = false;
    read_handle = 0u;
    read_length = 0u;
    service_changed_connection = 0u;
    service_changed_cccd[0] = 0u;
    service_changed_cccd[1] = 0u;
    service_changed_pending = false;
    service_changed_awaiting_confirmation = false;
    service_changed_sent = false;
    service_changed_failed = false;
    service_changed_last_error = 0u;
    service_changed_disconnect_pending = false;
    service_changed_disconnect_connection = 0u;
    service_changed_disconnect_last_error = 0u;
    memset(proof, 0, sizeof(proof));
    memset(nonce, 0, sizeof(nonce));
    set_advertising_interval(BLE_FAST_ADVERTISING_INTERVAL);
    fast_advertising_until_us = time_us_64() + BLE_FAST_ADVERTISING_WINDOW_US;
    gap_advertisements_enable(1);
}

/** @brief Request or retry disconnecting a peer whose Service Changed indication timed out. */
static void disconnect_after_service_changed_timeout(void)
{
    const uint8_t status = gap_disconnect(service_changed_disconnect_connection);
    if (status == ERROR_CODE_SUCCESS || status == ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER) {
        service_changed_disconnect_pending = false;
        service_changed_disconnect_last_error = 0u;
    } else if (status != service_changed_disconnect_last_error) {
        PF_LOGGING_WARNING("ble", "Service Changed timeout disconnect deferred: %u", status);
        service_changed_disconnect_last_error = status;
    }
}

/** @brief Handle controller startup and disconnect without creating tasks or persisting BLE bonds. */
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);
    if (event == ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE &&
        att_event_handle_value_indication_complete_get_conn_handle(packet) == service_changed_connection &&
        att_event_handle_value_indication_complete_get_attribute_handle(packet) ==
            ATT_CHARACTERISTIC_2A05_01_VALUE_HANDLE &&
        service_changed_awaiting_confirmation) {
        const uint8_t status = att_event_handle_value_indication_complete_get_status(packet);
        service_changed_awaiting_confirmation = false;
        if (status == ERROR_CODE_SUCCESS) {
            service_changed_sent = true;
        } else if (status != ATT_HANDLE_VALUE_INDICATION_DISCONNECT) {
            service_changed_pending = false;
            service_changed_failed = true;
            service_changed_disconnect_pending = true;
            service_changed_disconnect_connection =
                att_event_handle_value_indication_complete_get_conn_handle(packet);
            service_changed_disconnect_last_error = 0u;
            PF_LOGGING_WARNING("ble", "Service Changed indication failed: %u; disconnecting", status);
            disconnect_after_service_changed_timeout();
        }
        return;
    }
    /* ponytail: CYW43 uses the legacy event with public addressing; handle enhanced variants if privacy is enabled. */
    if ((event == BTSTACK_EVENT_STATE && btstack_event_state_get_state(packet) == HCI_STATE_WORKING) ||
        event == HCI_EVENT_DISCONNECTION_COMPLETE) {
        link_state = PF_BLUETOOTH_STATE_ADVERTISING;
        advertise();
    } else if (event == HCI_EVENT_LE_META && hci_event_le_meta_get_subevent_code(packet) ==
               HCI_SUBEVENT_LE_CONNECTION_COMPLETE &&
               hci_subevent_le_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
        link_state = PF_BLUETOOTH_STATE_CONNECTED;
        fast_advertising_until_us = 0u;
    }
}

/** @brief Freeze a report at the first read so ATT read-blob offsets use the same snapshot. */
static uint16_t read_callback(hci_con_handle_t connection, uint16_t handle, uint16_t offset,
                              uint8_t *buffer, uint16_t buffer_size)
{
    (void)connection;
    if (handle == ATT_CHARACTERISTIC_GAP_DEVICE_NAME_01_VALUE_HANDLE) {
        return att_read_callback_handle_blob(scan_response + 2u, sizeof(scan_response) - 2u, offset, buffer,
                                             buffer_size);
    }
    if (handle == ATT_CHARACTERISTIC_2A26_01_VALUE_HANDLE) {
        return att_read_callback_handle_blob((const uint8_t *)firmware_revision, firmware_revision_length,
                                             offset, buffer, buffer_size);
    }
    if (handle == ATT_CHARACTERISTIC_2B2A_01_VALUE_HANDLE) {
        if (buffer == NULL) return sizeof(database_hash);
        if (offset == 0u && !gatt_server_get_database_hash(database_hash)) {
            PF_LOGGING_ERROR("ble", "GATT database hash unavailable");
            return 0u;
        }
        return att_read_callback_handle_blob(database_hash, sizeof(database_hash), offset, buffer, buffer_size);
    }
    if (handle == ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE) {
        return att_read_callback_handle_blob(service_changed_cccd, sizeof(service_changed_cccd), offset, buffer,
                                             buffer_size);
    }
    /* Identity carries the UID that the session proof binds; everything else waits for the unlock. */
    if (!session_authorized && handle != BLE_HANDLE(0001) && handle != BLE_HANDLE(0100)) {
        return (uint16_t)(ATT_READ_ERROR_CODE_OFFSET + BLE_ERROR_CONTROL_REJECTED);
    }
    if (buffer == NULL) {
        switch (handle) {
        case BLE_HANDLE(0001): return sizeof(pf_hid_identity_report_t);
        case BLE_HANDLE(0002): return sizeof(pf_hid_configuration_report_t);
        case BLE_HANDLE(0003): return sizeof(pf_hid_telemetry_report_t);
        case BLE_HANDLE(000a): return sizeof(pf_hid_storage_report_t);
        case BLE_HANDLE(000b): return pf_hid_self_test_supported() ? sizeof(pf_hid_self_test_report_t) : 0u;
        case BLE_HANDLE(0014): return PF_HID_BLE_CONTROL_PAYLOAD_SIZE;
        case BLE_HANDLE(000c): return PF_WIRELESS_STATUS_SIZE;
        case BLE_HANDLE(0100): return sizeof(nonce);
        default: return 0u;
        }
    }
    if (offset == 0u) {
        read_handle = handle;
        read_length = 0u;
        if (handle == BLE_HANDLE(0001)) {
            pf_hid_build_identity_report(&read_buffer.identity);
            read_length = sizeof(pf_hid_identity_report_t);
        } else if (handle == BLE_HANDLE(0002)) {
            pf_hid_build_configuration_report(&read_buffer.config);
            read_length = sizeof(pf_hid_configuration_report_t);
        } else if (handle == BLE_HANDLE(0003)) {
            pf_hid_build_telemetry_report(&read_buffer.telemetry, report_sequence++);
            read_length = sizeof(pf_hid_telemetry_report_t);
        } else if (handle == BLE_HANDLE(000a)) {
            pf_hid_build_storage_report(&read_buffer.storage, report_sequence++);
            read_length = sizeof(pf_hid_storage_report_t);
        } else if (handle == BLE_HANDLE(000b) && pf_hid_self_test_supported()) {
            pf_hid_build_self_test_report(&read_buffer.benchmark, report_sequence++);
            read_length = sizeof(pf_hid_self_test_report_t);
        } else if (handle == BLE_HANDLE(0014)) {
            pf_hid_ble_result(&read_buffer.result);
            read_length = PF_HID_BLE_CONTROL_PAYLOAD_SIZE;
        } else if (handle == BLE_HANDLE(000c)) {
            /* Unlocked readers see the saved SSID and LAN address, as USB readers do. */
            pf_wireless_status(read_buffer.bytes);
            read_length = PF_WIRELESS_STATUS_SIZE;
        } else if (handle == BLE_HANDLE(0100)) {
            challenge_valid = false;
            proof_valid = false;
            memset(nonce, 0, sizeof(nonce));
            if (!pf_wireless_auth_new_nonce(nonce)) return 0u;
            challenge_valid = true;
            memcpy(read_buffer.bytes, nonce, sizeof(nonce));
            read_length = sizeof(nonce);
        }
    }
    if (handle != read_handle || offset > read_length) {
        return (uint16_t)(ATT_READ_ERROR_CODE_OFFSET + ATT_ERROR_INVALID_OFFSET);
    }
    return att_read_callback_handle_blob(read_buffer.bytes, read_length, offset, buffer, buffer_size);
}

/** @brief Verify a one-use, UID-bound HMAC proof; compare in constant time and rate-limit failures. */
static pf_ble_auth_result_t authenticate(const uint8_t *request)
{
    if (time_us_64() < auth_retry_us) return PF_BLE_AUTH_RATE_LIMITED;
    if (!control_key_available || !challenge_valid) return PF_BLE_AUTH_CHALLENGE_UNAVAILABLE;
    if (!proof_valid) return PF_BLE_AUTH_REJECTED;
    challenge_valid = false;
    proof_valid = false;
    bool accepted = pf_wireless_auth_verify(nonce, ble_auth_domain, request, proof);
    memset(nonce, 0, sizeof(nonce));
    memset(proof, 0, sizeof(proof));
    if (!accepted) {
        auth_retry_us = time_us_64() + BLE_AUTH_RETRY_DELAY_US;
        PF_LOGGING_WARNING("ble", "BLE authorization rejected");
    }
    return accepted ? PF_BLE_AUTH_ACCEPTED : PF_BLE_AUTH_REJECTED;
}

/** @brief Convert proof state to its documented application ATT error. */
static int auth_error(pf_ble_auth_result_t result)
{
    switch (result) {
    case PF_BLE_AUTH_ACCEPTED: return 0;
    case PF_BLE_AUTH_RATE_LIMITED: return BLE_ERROR_RATE_LIMITED;
    case PF_BLE_AUTH_CHALLENGE_UNAVAILABLE: return BLE_ERROR_CHALLENGE_UNAVAILABLE;
    case PF_BLE_AUTH_REJECTED: return BLE_ERROR_CONTROL_REJECTED;
    }
    return BLE_ERROR_CONTROL_REJECTED;
}

/** @brief Unlock the connection, then accept proofs, the LED identify control and proven controls; never flash. */
static int write_callback(hci_con_handle_t connection, uint16_t handle, uint16_t transaction_mode,
                           uint16_t offset, uint8_t *buffer, uint16_t size)
{
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE || offset != 0u) return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    if (handle == ATT_CHARACTERISTIC_2A05_01_CLIENT_CONFIGURATION_HANDLE) {
        if (size != 2u) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        const uint16_t configuration = (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
        if (configuration != 0u && configuration != 2u) return ATT_ERROR_VALUE_NOT_ALLOWED;
        service_changed_cccd[0] = buffer[0];
        service_changed_cccd[1] = buffer[1];
        service_changed_pending = configuration == 2u && !service_changed_sent &&
                                  !service_changed_failed &&
                                  !service_changed_disconnect_pending &&
                                  !service_changed_awaiting_confirmation;
        service_changed_connection = connection;
        return 0;
    }
    if (handle == BLE_HANDLE(0101)) {
        if (size != 16u) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        if (time_us_64() < auth_retry_us) return BLE_ERROR_RATE_LIMITED;
        if (!control_key_available || !challenge_valid) return BLE_ERROR_CHALLENGE_UNAVAILABLE;
        memcpy(proof, buffer, sizeof(proof));
        proof_valid = true;
        /* The first proof on a connection unlocks it; later proofs wait for exactly one control write. */
        if (session_authorized) return 0;
        const pf_ble_auth_result_t result = authenticate(ble_session_request);
        session_authorized = result == PF_BLE_AUTH_ACCEPTED;
        return auth_error(result);
    }
    if (handle != BLE_HANDLE(0014)) return ATT_ERROR_WRITE_NOT_PERMITTED;
    if (!session_authorized) return BLE_ERROR_CONTROL_REJECTED;
    if (size != PF_HID_BLE_CONTROL_PAYLOAD_SIZE || buffer[0] != PF_HID_REPORT_VERSION || buffer[1] == 0u) {
        return BLE_ERROR_ENVELOPE;
    }
    pf_hid_control_request_t request = {0};
    memcpy(&request, buffer, PF_HID_BLE_CONTROL_PAYLOAD_SIZE);
    /* On an unlocked link the LED only identifies a board; it skips the proof without touching nonce/rate state. */
    if (request.opcode != PF_HID_OPCODE_LED) {
        const pf_ble_auth_result_t result = authenticate(buffer);
        if (result != PF_BLE_AUTH_ACCEPTED) return auth_error(result);
    }
    return pf_hid_ble_control(&request) ? 0 : ATT_ERROR_REQUEST_NOT_SUPPORTED;
}

/** @copydoc pf_bluetooth_prepare */
bool pf_bluetooth_prepare(void)
{
    control_key_available = pf_wireless_auth_available();
    const bool enabled = (pf_boot_config_radios() & PF_WIRELESS_BLE) != 0u;
    if (enabled && !control_key_available) PF_LOGGING_WARNING("ble", "Bluetooth is off until a control key is saved");
    return enabled && control_key_available;
}

/** @copydoc pf_bluetooth_state */
uint8_t pf_bluetooth_state(void)
{
    return link_state;
}

/** @copydoc pf_bluetooth_start */
bool pf_bluetooth_start(void)
{
    pf_hid_identity_report_t identity;
    pf_hid_build_identity_report(&identity);
    const int version_length = snprintf(firmware_revision, sizeof(firmware_revision), "%u.%u.%u",
        (unsigned)identity.firmware_major, (unsigned)identity.firmware_minor,
        (unsigned)identity.firmware_patch);
    if (version_length <= 0 || (size_t)version_length >= sizeof(firmware_revision)) {
        PF_LOGGING_ERROR("ble", "Firmware revision formatting failed");
        return false;
    }
    firmware_revision_length = (uint8_t)version_length;
    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_NO_BONDING);
    att_server_init(profile_data, read_callback, write_callback);
    att_server_register_packet_handler(packet_handler);
    events.callback = packet_handler;
    hci_add_event_handler(&events);
    /* BTstack keeps this pointer and reads it whenever advertising is (re)enabled; it must outlive this call. */
    static const uint8_t advertising[] = {2, BLUETOOTH_DATA_TYPE_FLAGS, 6,
        17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
        0x02, 0xf5, 0x43, 0x64, 0x85, 0x6e, 0x32, 0x8b,
        0x70, 0x4d, 0x91, 0x7c, 0x00, 0x00, 0xfc, 0x50};
    char serial[2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1u];
    pico_get_unique_board_id_string(serial, sizeof(serial));
    scan_response[0] = sizeof(scan_response) - 1u;
    scan_response[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(scan_response + 2u, "PicoForge-", 10u);
    memcpy(scan_response + 12u, serial, sizeof(serial) - 1u);
    set_advertising_interval(BLE_FAST_ADVERTISING_INTERVAL);
    gap_advertisements_set_data(sizeof(advertising), (uint8_t *)advertising);
    gap_scan_response_set_data(sizeof(scan_response), scan_response);
    int rc = hci_power_control(HCI_POWER_ON);
    if (rc != 0) {
        PF_LOGGING_ERROR("wireless", "BLE initialization failed: %d", rc);
    }
    return rc == 0;
}

/** @copydoc pf_bluetooth_poll */
void pf_bluetooth_poll(void)
{
    if (service_changed_disconnect_pending) {
        disconnect_after_service_changed_timeout();
    } else if (service_changed_pending && link_state == PF_BLUETOOTH_STATE_CONNECTED &&
               !service_changed_awaiting_confirmation) {
        const uint8_t status = att_server_indicate(service_changed_connection,
            ATT_CHARACTERISTIC_2A05_01_VALUE_HANDLE, service_changed_range, sizeof(service_changed_range));
        if (status == ERROR_CODE_SUCCESS) {
            service_changed_pending = false;
            service_changed_awaiting_confirmation = true;
            service_changed_last_error = 0u;
        } else if (status != service_changed_last_error) {
            PF_LOGGING_WARNING("ble", "Service Changed indication deferred: %u", status);
            service_changed_last_error = status;
        }
    }
    if (link_state != PF_BLUETOOTH_STATE_ADVERTISING || fast_advertising_until_us == 0u ||
        time_us_64() < fast_advertising_until_us) return;
    gap_advertisements_enable(0);
    set_advertising_interval(BLE_SLOW_ADVERTISING_INTERVAL);
    gap_advertisements_enable(1);
    fast_advertising_until_us = 0u;
}
