/**
 * @file wireless.c
 * @brief SDK poll-mode Wi-Fi and BLE, serialized with USB control on the USB owner task.
 */
#include "app/wireless/wireless.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "btstack.h"
#include "btstack_tlv_none.h"
#include "btstack_tlv_flash_bank.h"
#include "app/wireless/wireless_auth.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "app/usb/usb_hid.h"
#include "app/usb/usb_msc.h"
#include "app/storage/storage_worker.h"
#include "picoforge.h"

/** Generated handles are owned by the pinned SDK GATT compiler. */
#define BLE_HANDLE(id) ATT_CHARACTERISTIC_50fc##id##_7c91_4d70_8b32_6e856443f501_01_VALUE_HANDLE

static pf_wireless_config_t settings; /**< Boot-applied credentials, never exposed by reads. */
static bool started; /**< SDK context initialized successfully. */
static bool challenge_valid; /**< Nonce issued once and usable for one authentication attempt. */
static bool proof_valid; /**< A 16-byte proof is ready for exactly one subsequent control request. */
static uint8_t proof[16]; /**< Submitted per-command MAC, cleared on use or disconnect. */
static uint8_t nonce[16]; /**< Fresh random challenge, invalidated by use or disconnect. */
static uint64_t auth_retry_us; /**< Failed authentication rate limit; never blocks the owner task. */
static uint64_t connect_deadline_us; /**< Wi-Fi association/DHCP attempt deadline. */
static uint64_t retry_us; /**< Next permitted Wi-Fi reconnect attempt. */
static uint8_t wifi_state; /**< 0 disabled, 1 connecting, 2 connected, 3 failed. */
static uint8_t error_code; /**< 0 none, 1 initialization, 2 join failure, 3 connection timeout. */
static uint8_t save_status; /**< Last provisioning result, using Feature 20 status values. */
static uint8_t save_transaction; /**< Last USB provisioning transaction ID. */
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

/** @brief Begin an asynchronous WPA2 connection; retries are paced after explicit failures. */
static void connect_wifi(void)
{
    int rc = cyw43_arch_wifi_connect_async(settings.ssid, settings.password, CYW43_AUTH_WPA2_AES_PSK);
    wifi_state = rc == 0 ? 1u : 3u;
    error_code = rc == 0 ? 0u : 2u;
    connect_deadline_us = time_us_64() + 30000000u;
    retry_us = time_us_64() + 5000000u;
    if (rc != 0) PF_LOGGING_ERROR("wifi", "Wi-Fi connection request failed: %d", rc);
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
    bool accepted = pf_wireless_auth_verify(settings.ble_key, nonce, uid.id, request, proof);
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

/** @copydoc pf_wireless_start */
void pf_wireless_start(void)
{
    pf_boot_config_wireless(&settings);
    if (settings.flags == 0u) return;
    /* SDK poll contexts and CYW43 IRQ setup must remain on their initialization core. */
    vTaskCoreAffinitySet(NULL, (UBaseType_t)1u << get_core_num());
    int rc = cyw43_arch_init();
    if (rc != 0) {
        wifi_state = settings.flags & PF_WIRELESS_WIFI ? 3u : 0u;
        error_code = 1u;
        PF_LOGGING_ERROR("wireless", "SDK radio initialization failed: %d", rc);
        return;
    }
    started = true;
    if (settings.flags & PF_WIRELESS_WIFI) {
        cyw43_arch_enable_sta_mode();
        connect_wifi();
    }
    if (settings.flags & PF_WIRELESS_BLE) {
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
        rc = hci_power_control(HCI_POWER_ON);
        if (rc != 0) {
            error_code = 1u;
            PF_LOGGING_ERROR("wireless", "BLE initialization failed: %d", rc);
        }
    }
}

/** @copydoc pf_wireless_poll */
void pf_wireless_poll(void)
{
    if (!started) return;
    cyw43_arch_poll();
    if (!(settings.flags & PF_WIRELESS_WIFI)) return;
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    uint64_t now = time_us_64();
    if (link == CYW43_LINK_UP) {
        wifi_state = 2u;
        error_code = 0u;
    } else if (wifi_state == 1u && (link < 0 || now >= connect_deadline_us)) {
        wifi_state = 3u;
        error_code = link < 0 ? 2u : 3u;
        retry_us = now + 5000000u;
        PF_LOGGING_WARNING("wifi", "Wi-Fi association/DHCP failed: %d", link);
    } else if (wifi_state == 2u) {
        wifi_state = 3u;
        retry_us = now + 5000000u;
        PF_LOGGING_WARNING("wifi", "Wi-Fi link lost");
    } else if (wifi_state == 3u && now >= retry_us) connect_wifi();
}

/** @copydoc pf_wireless_status */
void pf_wireless_status(uint8_t *report)
{
    pf_wireless_config_t saved;
    pf_boot_config_wireless(&saved);
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[0] = 1u;
    report[1] = saved.flags;
    report[2] = save_status;
    report[3] = save_transaction;
    report[4] = wifi_state;
    if (started && wifi_state == 2u) {
        memcpy(report + 5u, &netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])->addr, 4u);
    }
    memcpy(report + 9u, saved.ssid, sizeof(saved.ssid));
    report[42] = saved.password[0] != '\0';
    for (size_t i = 0u; i < sizeof(saved.ble_key); ++i) report[43] |= saved.ble_key[i] != 0u;
    report[44] = error_code;
    memset(&saved, 0, sizeof(saved));
}

/** @copydoc pf_wireless_configure */
bool pf_wireless_configure(const uint8_t *report)
{
    save_transaction = report[1];
    save_status = PF_HID_COMMAND_INVALID;
    if (report[0] != 1u || report[1] == 0u || (report[2] & ~3u) != 0u || (report[3] & ~3u) != 0u ||
        report[133] != 0u || report[134] != 0u || report[135] != 0u) return false;
    if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending() || pf_storage_worker_busy()) {
        save_status = PF_HID_COMMAND_BUSY;
        return false;
    }
    pf_wireless_config_t old;
    pf_wireless_config_t next;
    pf_boot_config_wireless(&old);
    next = old;
    next.flags = report[2];
    memcpy(next.ssid, report + 4u, sizeof(next.ssid));
    if (!(report[3] & 1u)) memcpy(next.password, report + 37u, sizeof(next.password));
    if (!(report[3] & 2u)) memcpy(next.ble_key, report + 101u, sizeof(next.ble_key));
    bool valid = pf_boot_config_set_wireless(&next);
    bool saved = valid && pf_boot_config_save();
    if (!saved) {
        (void)pf_boot_config_set_wireless(&old);
        save_status = valid ? PF_HID_COMMAND_FAILED : PF_HID_COMMAND_INVALID;
        PF_LOGGING_ERROR("wireless", "Wireless configuration %s", valid ? "save unconfirmed" : "invalid");
    } else save_status = PF_HID_COMMAND_COMPLETED;
    memset(&next, 0, sizeof(next));
    memset(&old, 0, sizeof(old));
    return saved;
}
