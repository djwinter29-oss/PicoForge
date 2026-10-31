/**
 * @file bluetooth.c
 * @brief BLE GATT diagnostics, per-command authorization and nonpersistent advertising.
 */
#include "app/wireless/bluetooth.h"
#include "app/wireless/wireless.h"

#include <string.h>

#include "btstack.h"
#include "btstack_tlv_none.h"
#include "btstack_tlv_flash_bank.h"
#include "app/wireless/bluetooth_auth.h"
#include "pico/rand.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "app/usb/usb_hid.h"
#include "bluetooth_profile.h"

/** Generated handles are owned by the pinned SDK GATT compiler. */
#define BLE_HANDLE(id) ATT_CHARACTERISTIC_50fc##id##_7c91_4d70_8b32_6e856443f501_01_VALUE_HANDLE

static uint8_t ble_key[32]; /**< Boot-applied private control key. */
static bool challenge_valid; /**< Nonce issued once and usable for one authentication attempt. */
static bool proof_valid; /**< A 16-byte proof is ready for exactly one subsequent control request. */
static uint8_t proof[16]; /**< Submitted per-command MAC, cleared on use or disconnect. */
static uint8_t nonce[16]; /**< Fresh random challenge, invalidated by use or disconnect. */
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
static btstack_packet_callback_registration_t events; /**< Static HCI state/disconnect registration. */

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

/** @brief Reset per-connection authorization and begin/restart peripheral advertising. */
static void advertise(void)
{
    proof_valid = challenge_valid = false;
    memset(proof, 0, sizeof(proof));
    memset(nonce, 0, sizeof(nonce));
    gap_advertisements_enable(1);
}

/** @brief Handle controller startup and disconnect without creating tasks or persisting BLE bonds. */
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);
    if ((event == BTSTACK_EVENT_STATE && btstack_event_state_get_state(packet) == HCI_STATE_WORKING) ||
        event == HCI_EVENT_DISCONNECTION_COMPLETE) advertise();
}

/** @brief Freeze a report at the first read so ATT read-blob offsets use the same snapshot. */
static uint16_t read_callback(hci_con_handle_t connection, uint16_t handle, uint16_t offset,
                              uint8_t *buffer, uint16_t buffer_size)
{
    (void)connection;
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
            read_length = sizeof(pf_hid_control_result_t);
        } else if (handle == BLE_HANDLE(001e)) {
            pf_wireless_status(read_buffer.bytes);
            read_length = PF_WIRELESS_STATUS_SIZE;
        } else if (handle == BLE_HANDLE(0020)) {
            if (buffer != NULL) {
                for (size_t i = 0u; i < sizeof(nonce); i += sizeof(uint64_t)) {
                    uint64_t random = get_rand_64();
                    memcpy(nonce + i, &random, sizeof(random));
                }
                challenge_valid = true;
                proof_valid = false;
            }
            memcpy(read_buffer.bytes, nonce, sizeof(nonce));
            read_length = sizeof(nonce);
        }
    }
    if (handle != read_handle) return 0u;
    return att_read_callback_handle_blob(read_buffer.bytes, read_length, offset, buffer, buffer_size);
}

/** @brief Verify a one-use, UID-bound HMAC proof; compare in constant time and rate-limit failures. */
static bool authenticate(const uint8_t *request)
{
    if (!challenge_valid || !proof_valid || time_us_64() < auth_retry_us) return false;
    challenge_valid = false;
    proof_valid = false;
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    bool accepted = pf_wireless_auth_verify(ble_key, nonce, uid.id, request, proof);
    memset(nonce, 0, sizeof(nonce));
    memset(proof, 0, sizeof(proof));
    if (!accepted) {
        auth_retry_us = time_us_64() + 2000000u;
        PF_LOGGING_WARNING("ble", "BLE authorization rejected");
    }
    return accepted;
}

/** @brief Accept only authorization proofs and restricted controls; no BLE credential or flash writes. */
static int write_callback(hci_con_handle_t connection, uint16_t handle, uint16_t transaction_mode,
                           uint16_t offset, uint8_t *buffer, uint16_t size)
{
    (void)connection;
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE || offset != 0u) return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    if (handle == BLE_HANDLE(0021)) {
        if (size != 16u) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        if (!challenge_valid || time_us_64() < auth_retry_us) return ATT_ERROR_INSUFFICIENT_AUTHENTICATION;
        memcpy(proof, buffer, sizeof(proof));
        proof_valid = true;
        return 0;
    }
    if (handle != BLE_HANDLE(0014)) return ATT_ERROR_WRITE_NOT_PERMITTED;
    if (size != sizeof(pf_hid_control_request_t)) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
    if (!authenticate(buffer)) return ATT_ERROR_INSUFFICIENT_AUTHENTICATION;
    pf_hid_control_request_t request;
    memcpy(&request, buffer, sizeof(request));
    return pf_hid_ble_control(&request) ? 0 : ATT_ERROR_REQUEST_NOT_SUPPORTED;
}

/** @copydoc pf_bluetooth_prepare */
bool pf_bluetooth_prepare(void)
{
    pf_boot_config_ble_key(ble_key);
    uint8_t key_bits = 0u;
    for (size_t i = 0u; i < sizeof(ble_key); ++i) key_bits |= ble_key[i];
    return (pf_boot_config_radios() & PF_WIRELESS_BLE) != 0u && key_bits != 0u;
}

/** @copydoc pf_bluetooth_start */
bool pf_bluetooth_start(void)
{
    l2cap_init();
    sm_init();
    att_server_init(profile_data, read_callback, write_callback);
    events.callback = packet_handler;
    hci_add_event_handler(&events);
    const uint8_t advertising[] = {2, BLUETOOTH_DATA_TYPE_FLAGS, 6,
        17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
        0x01, 0xf5, 0x43, 0x64, 0x85, 0x6e, 0x32, 0x8b,
        0x70, 0x4d, 0x91, 0x7c, 0x00, 0x00, 0xfc, 0x50};
    bd_addr_t empty = {0};
    gap_advertisements_set_params(800, 800, 0, 0, empty, 7, 0);
    gap_advertisements_set_data(sizeof(advertising), (uint8_t *)advertising);
    int rc = hci_power_control(HCI_POWER_ON);
    if (rc != 0) {
        PF_LOGGING_ERROR("wireless", "BLE initialization failed: %d", rc);
    }
    return rc == 0;
}
