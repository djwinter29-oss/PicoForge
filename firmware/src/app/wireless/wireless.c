/**
 * @file wireless.c
 * @brief Shared SDK radio ownership, USB provisioning and combined wireless diagnostics.
 */
#include "app/wireless/wireless.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "app/wireless/bluetooth/bluetooth.h"
#include "app/wireless/wifi/tcp_cli.h"
#include "app/wireless/wifi/mdns.h"
#include "app/wireless/wifi/wifi.h"
#include "app/wireless/wireless_auth.h"
#include "pico/cyw43_arch.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "app/usb/usb_hid.h"
#include "app/usb/usb_msc.h"
#include "app/storage/storage_worker.h"

static bool wifi_enabled; /**< Wi-Fi master switch and credentials allow startup. */
static bool ble_enabled; /**< BLE master switch allows advertising; a key is required only for control. */
static bool started; /**< Shared SDK context initialized successfully. */
static uint8_t init_error; /**< Sticky pf_wireless_error_t for SDK or BLE initialization failure. */
static uint8_t save_status; /**< Last provisioning result, using Feature 20 status values. */
static uint8_t save_transaction; /**< Last USB provisioning transaction ID. */
static bool tcp_cli_enabled; /**< Boot-applied saved-LAN console switch. */
static bool tcp_cli_auth_required; /**< Boot-applied challenge-response switch. */
static uint8_t configured_radios; /**< Cached boot/saved radio mask for status reads. */
static char configured_ssid[PF_WIFI_SSID_SIZE]; /**< Cached saved SSID for status reads. */
static bool password_configured; /**< Cached credential-presence flag. */
static bool ble_key_configured; /**< Cached credential-presence flag. */
static bool restart_required; /**< Saved wireless settings differ from the values applied at boot. */

/** @brief Feature 31 configuration request byte offsets and flags. */
enum {
    REQUEST_VERSION = 0, /**< Payload version, always 2. */
    REQUEST_TRANSACTION = 1, /**< Nonzero host transaction. */
    REQUEST_RADIOS = 2, /**< PF_WIRELESS_* radio switches. */
    REQUEST_FLAGS = 3, /**< REQUEST_KEEP_* and REQUEST_USE_TESTED bits. */
    REQUEST_SSID = 4, /**< 33-byte NUL-terminated SSID. */
    REQUEST_PASSWORD = 37, /**< 64-byte NUL-terminated passphrase. */
    REQUEST_KEY = 101, /**< 32-byte BLE key. */
    REQUEST_CLOCK_MHZ = 133, /**< Little-endian system clock in MHz. */
    REQUEST_STORAGE_SPEED = 135, /**< pf_storage_speed_t. */
    REQUEST_KEEP_PASSWORD = 1, /**< Keep the saved passphrase. */
    REQUEST_KEEP_KEY = 2, /**< Keep the saved BLE key. */
    REQUEST_USE_TESTED = 4, /**< Save the staged tested credentials. */
    REQUEST_DISABLE_TCP_CLI = 8, /**< Disable unauthenticated saved-LAN console access after reboot. */
    REQUEST_KEEP_TCP_CLI = 16, /**< Preserve the saved console switch. */
    REQUEST_REQUIRE_TCP_AUTH = 32, /**< Require a proof made with the configured BLE key. */
    REQUEST_KEEP_TCP_AUTH = 64, /**< Preserve the saved TCP authentication switch. */
};

/** @brief Return whether USB mass storage or the storage worker owns the shared flash/radio admission. */
static bool storage_busy(void)
{
    return pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending() || pf_storage_worker_busy();
}

/** @brief Validate supported Feature 31 flags and their mutually exclusive choices. */
static bool valid_request_flags(uint8_t flags)
{
    static const uint8_t exclusive_pairs[][2] = {
        {REQUEST_DISABLE_TCP_CLI, REQUEST_KEEP_TCP_CLI},
        {REQUEST_REQUIRE_TCP_AUTH, REQUEST_KEEP_TCP_AUTH},
    };
    const uint8_t supported = REQUEST_KEEP_PASSWORD | REQUEST_KEEP_KEY | REQUEST_USE_TESTED |
        REQUEST_DISABLE_TCP_CLI | REQUEST_KEEP_TCP_CLI | REQUEST_REQUIRE_TCP_AUTH | REQUEST_KEEP_TCP_AUTH;
    if ((flags & (uint8_t)~supported) != 0u) return false;
    for (size_t i = 0u; i < sizeof(exclusive_pairs) / sizeof(exclusive_pairs[0]); ++i) {
        if ((flags & exclusive_pairs[i][0]) != 0u && (flags & exclusive_pairs[i][1]) != 0u) return false;
    }
    return true;
}

/** @brief Initialize the shared SDK once, always on the USB owner task/core. */
static bool ensure_started(void)
{
    if (started) return true;
    vTaskCoreAffinitySet(NULL, (UBaseType_t)1u << get_core_num());
    int rc = cyw43_arch_init();
    if (rc != 0) {
        init_error = PF_WIRELESS_ERROR_INIT;
        PF_LOGGING_ERROR("wireless", "SDK radio initialization failed: %d", rc);
        return false;
    }
    started = true;
    init_error = PF_WIRELESS_ERROR_NONE;
    return true;
}

/** @brief Refresh public saved-setting diagnostics after boot load or a successful configuration save. */
static void refresh_saved_wireless(void)
{
    configured_radios = pf_boot_config_radios() & (PF_WIRELESS_WIFI | PF_WIRELESS_BLE);
    pf_boot_config_wifi_ssid(configured_ssid);
    password_configured = pf_boot_config_wifi_password_configured();
    ble_key_configured = pf_boot_config_ble_key_configured();
    tcp_cli_enabled = pf_boot_config_tcp_cli_enabled();
    tcp_cli_auth_required = pf_boot_config_tcp_cli_auth_required();
    restart_required = pf_boot_config_wireless_restart_required();
}

/** @copydoc pf_wireless_led_put */
bool pf_wireless_led_put(bool on)
{
    if (!started) return false;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    return true;
}

/** @copydoc pf_wireless_start */
void pf_wireless_start(void)
{
    wifi_enabled = pf_wifi_prepare();
    refresh_saved_wireless();
    uint8_t tcp_key[32] = {0};
    pf_boot_config_ble_key(tcp_key);
    pf_wireless_auth_set_key(tcp_key);
    memset(tcp_key, 0, sizeof(tcp_key));
    ble_enabled = pf_bluetooth_prepare();
    if (!pf_tcp_cli_set_auth_required(tcp_cli_auth_required)) {
        init_error = PF_WIRELESS_ERROR_INIT;
        PF_LOGGING_ERROR("wireless", "TCP CLI authentication key unavailable");
        tcp_cli_enabled = false;
    }
    /* The chip starts even with both radios off because it owns the board LED; radios stay off unless enabled. */
    if (!ensure_started()) return;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    if (wifi_enabled) {
        pf_wifi_start();
    }
    if (ble_enabled && !pf_bluetooth_start()) init_error = PF_WIRELESS_ERROR_INIT;
}

/** @copydoc pf_wireless_poll */
void pf_wireless_poll(void)
{
    if (!started) return;
    cyw43_arch_poll();
    pf_wifi_poll();
    const bool saved_link_up = pf_wifi_saved_link_up();
    const bool serve = tcp_cli_enabled && saved_link_up;
    pf_tcp_cli_poll(serve);
    pf_wifi_mdns_poll(saved_link_up, serve && pf_tcp_cli_serving());
}

/** @copydoc pf_wireless_status */
void pf_wireless_status(uint8_t *report)
{
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[PF_WIRELESS_STATUS_VERSION_OFFSET] = 4u;
    report[PF_WIRELESS_STATUS_RADIOS_OFFSET] = configured_radios;
    report[PF_WIRELESS_STATUS_SAVE_STATUS_OFFSET] = save_status;
    report[PF_WIRELESS_STATUS_SAVE_TRANSACTION_OFFSET] = save_transaction;
    const uint8_t wifi_state = !started && wifi_enabled ? PF_WIFI_STATE_FAILED : pf_wifi_state();
    report[PF_WIRELESS_STATUS_WIFI_STATE_OFFSET] = wifi_state;
    if (started && pf_wifi_saved_link_up()) {
        pf_wifi_ip_address(report + PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET);
    }
    memcpy(report + PF_WIRELESS_STATUS_SSID_OFFSET, configured_ssid, sizeof(configured_ssid));
    report[PF_WIRELESS_STATUS_PASSWORD_CONFIGURED_OFFSET] = password_configured;
    report[PF_WIRELESS_STATUS_BLE_KEY_CONFIGURED_OFFSET] = ble_key_configured;
    report[PF_WIRELESS_STATUS_ERROR_OFFSET] = init_error != PF_WIRELESS_ERROR_NONE ? init_error : pf_wifi_error();
    report[PF_WIRELESS_STATUS_BLUETOOTH_STATE_OFFSET] = started && ble_enabled ? pf_bluetooth_state() : 0u;
    report[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] = pf_tcp_cli_serving();
    report[PF_WIRELESS_STATUS_SERVICES_OFFSET] = (tcp_cli_enabled ? 0u : 1u) |
        (tcp_cli_auth_required ? 2u : 0u);
    pf_wifi_auth_state_t auth_state;
    uint16_t reconnects;
    int8_t rssi;
    pf_wifi_link_diagnostics(&auth_state, &reconnects, &rssi);
    report[PF_WIRELESS_STATUS_AUTH_STATE_OFFSET] = (uint8_t)auth_state;
    report[PF_WIRELESS_STATUS_RECONNECT_COUNT_OFFSET] = (uint8_t)reconnects;
    report[PF_WIRELESS_STATUS_RECONNECT_COUNT_OFFSET + 1u] = (uint8_t)(reconnects >> 8u);
    report[PF_WIRELESS_STATUS_RSSI_OFFSET] = (uint8_t)rssi;
    report[PF_WIRELESS_STATUS_RESTART_REQUIRED_OFFSET] = restart_required;
}

/** @copydoc pf_wireless_command */
void pf_wireless_command(const uint8_t *report)
{
    const uint8_t transaction = report[PF_WIRELESS_COMMAND_TRANSACTION_OFFSET];
    if (transaction == 0u) return;
    if (storage_busy() || !ensure_started()) {
        pf_wifi_reject_command(transaction);
        return;
    }
    if (!pf_wifi_command(report)) pf_wifi_reject_command(transaction);
}

/** @copydoc pf_wireless_reject_command */
void pf_wireless_reject_command(uint8_t transaction)
{
    pf_wifi_reject_command(transaction);
}

/** @copydoc pf_wireless_result_report */
void pf_wireless_result_report(uint8_t *report)
{
    pf_wifi_result(report);
}

/** @copydoc pf_wireless_configure */
bool pf_wireless_configure(const uint8_t *report)
{
    const uint8_t flags = report[REQUEST_FLAGS];
    const bool keep_password = (flags & REQUEST_KEEP_PASSWORD) != 0u;
    const bool keep_key = (flags & REQUEST_KEEP_KEY) != 0u;
    const bool use_tested = (flags & REQUEST_USE_TESTED) != 0u;
    save_transaction = report[REQUEST_TRANSACTION];
    save_status = PF_HID_COMMAND_INVALID;
    if (report[REQUEST_VERSION] != 2u || save_transaction == 0u || !valid_request_flags(flags)) return false;
    if (storage_busy()) {
        save_status = PF_HID_COMMAND_BUSY;
        return false;
    }
    /* Tested credentials come only from RAM: the request names the SSID and must not carry a password. */
    if (use_tested && (keep_password ||
        memcmp(report + REQUEST_PASSWORD, (uint8_t[PF_WIFI_PASSWORD_SIZE]){0}, PF_WIFI_PASSWORD_SIZE) != 0)) {
        return false;
    }
    char tested_ssid[PF_WIFI_SSID_SIZE] = {0};
    char tested_password[PF_WIFI_PASSWORD_SIZE] = {0};
    char old_ssid[PF_WIFI_SSID_SIZE] = {0};
    pf_boot_config_wifi_ssid(old_ssid);
    const pf_wifi_auth_state_t old_auth_state = pf_boot_config_wifi_auth_state();
    if (use_tested && (!pf_wifi_tested_credentials(tested_ssid, tested_password) ||
                       memcmp(tested_ssid, report + REQUEST_SSID, sizeof(tested_ssid)) != 0)) {
        memset(tested_ssid, 0, sizeof(tested_ssid));
        memset(tested_password, 0, sizeof(tested_password));
        return false;
    }
    char next_ssid[PF_WIFI_SSID_SIZE];
    char next_password[PF_WIFI_PASSWORD_SIZE];
    uint8_t next_key[32];
    memcpy(next_ssid, report + REQUEST_SSID, sizeof(next_ssid));
    if (use_tested) memcpy(next_password, tested_password, sizeof(next_password));
    else memcpy(next_password, report + REQUEST_PASSWORD, sizeof(next_password));
    memcpy(next_key, report + REQUEST_KEY, sizeof(next_key));
    const pf_wifi_auth_state_t next_auth_state = use_tested ? pf_wifi_tested_auth_state()
        : keep_password && memcmp(old_ssid, next_ssid, sizeof(old_ssid)) == 0
            ? old_auth_state : PF_WIFI_AUTH_TRY_WPA2;
    const uint16_t clock_mhz = (uint16_t)(report[REQUEST_CLOCK_MHZ] | (uint16_t)report[REQUEST_CLOCK_MHZ + 1] << 8u);
    const bool valid = pf_boot_config_set_sysclk(pf_sysclk_profile_from_mhz(clock_mhz)) &&
        pf_boot_config_set_storage_speed((pf_storage_speed_t)report[REQUEST_STORAGE_SPEED]) &&
        pf_boot_config_set_radios(report[REQUEST_RADIOS]) &&
        ((flags & REQUEST_KEEP_TCP_CLI) != 0u ||
         pf_boot_config_set_tcp_cli_enabled((flags & REQUEST_DISABLE_TCP_CLI) == 0u)) &&
        pf_boot_config_set_wifi_ssid(next_ssid) &&
        (keep_password || pf_boot_config_set_wifi_password(next_password)) &&
        (keep_key || pf_boot_config_set_ble_key(next_key)) &&
        ((flags & REQUEST_KEEP_TCP_AUTH) != 0u ||
         pf_boot_config_set_tcp_cli_auth_required((flags & REQUEST_REQUIRE_TCP_AUTH) != 0u)) &&
        pf_boot_config_set_wifi_auth_state(next_auth_state);
    if (!valid) {
        pf_boot_config_discard();
        save_status = PF_HID_COMMAND_INVALID;
        PF_LOGGING_ERROR("wireless", "Wireless configuration invalid");
    } else if (pf_boot_config_save()) {
        save_status = PF_HID_COMMAND_COMPLETED;
    } else {
        save_status = PF_HID_COMMAND_FAILED;
        PF_LOGGING_ERROR("wireless", "Wireless configuration save unconfirmed");
    }
    memset(next_ssid, 0, sizeof(next_ssid));
    memset(next_password, 0, sizeof(next_password));
    memset(next_key, 0, sizeof(next_key));
    memset(tested_ssid, 0, sizeof(tested_ssid));
    memset(tested_password, 0, sizeof(tested_password));
    memset(old_ssid, 0, sizeof(old_ssid));
    if (save_status == PF_HID_COMMAND_COMPLETED) refresh_saved_wireless();
    if (save_status == PF_HID_COMMAND_COMPLETED && use_tested) pf_wifi_clear_tested_credentials();
    return save_status == PF_HID_COMMAND_COMPLETED;
}
