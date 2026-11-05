/**
 * @file wireless.c
 * @brief Shared SDK radio ownership, USB provisioning and combined wireless diagnostics.
 */
#include "app/wireless/wireless.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "mbedtls/platform_util.h"
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
static bool ble_enabled; /**< BLE switch is on and a control key is saved; BLE never runs without a key. */
static bool started; /**< Shared SDK context initialized successfully. */
static uint8_t init_error; /**< Sticky SDK or BLE initialization failure. */
static uint8_t save_status; /**< Last provisioning result, using Feature 20 status values. */
static uint8_t save_transaction; /**< Last USB provisioning transaction ID. */
static bool tcp_cli_enabled; /**< Boot-applied saved-LAN console switch. */
static uint8_t configured_radios; /**< Cached boot/saved radio mask for status reads. */
static char configured_ssid[PF_WIFI_SSID_SIZE]; /**< Cached saved SSID for status reads. */
static bool password_configured; /**< Cached credential-presence flag. */
static bool ble_key_configured; /**< Cached credential-presence flag. */
static bool restart_required; /**< Saved wireless settings differ from the values applied at boot. */

/** @brief Feature 31 configuration request byte offsets and flags. */
enum {
    REQUEST_VERSION = 0, /**< Payload version, always 1. */
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
    REQUEST_DISABLE_TCP_CLI = 8, /**< Disable saved-LAN console access after reboot. */
};

/** @brief Return whether USB mass storage or the storage worker owns the shared flash/radio admission. */
static bool storage_busy(void)
{
    return pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending() || pf_storage_worker_busy();
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
    mbedtls_platform_zeroize(tcp_key, sizeof(tcp_key));
    ble_enabled = pf_bluetooth_prepare();
    /* The chip starts even with both radios off because it owns the board LED; radios stay off unless enabled. */
    if (!ensure_started()) return;
    /* A saved control key always protects the TCP CLI; without one the console is open on the saved LAN. */
    (void)pf_tcp_cli_set_auth_required(pf_wireless_auth_available());
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
    const uint64_t now = time_us_64();
    pf_wifi_poll(now);
    const bool saved_link_up = pf_wifi_saved_link_up();
    const bool serve = tcp_cli_enabled && saved_link_up;
    pf_tcp_cli_poll(serve, now);
    pf_wifi_mdns_poll(saved_link_up, serve && pf_tcp_cli_serving(), now);
}

/** @copydoc pf_wireless_status */
void pf_wireless_status(uint8_t *report)
{
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[PF_WIRELESS_STATUS_VERSION_OFFSET] = PF_WIRELESS_STATUS_VERSION;
    report[PF_WIRELESS_STATUS_RADIOS_OFFSET] = configured_radios;
    report[PF_WIRELESS_STATUS_SAVE_STATUS_OFFSET] = save_status;
    report[PF_WIRELESS_STATUS_SAVE_TRANSACTION_OFFSET] = save_transaction;
    const pf_wifi_state_t wifi_state = !started && wifi_enabled ? PF_WIFI_STATE_FAILED : pf_wifi_state();
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
    report[PF_WIRELESS_STATUS_SERVICES_OFFSET] =
        (tcp_cli_enabled ? 0u : PF_WIRELESS_STATUS_SERVICE_TCP_CLI_DISABLED) |
        (pf_tcp_cli_auth_required() ? PF_WIRELESS_STATUS_SERVICE_TCP_CLI_AUTH_REQUIRED : 0u);
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
    const uint8_t supported = REQUEST_KEEP_PASSWORD | REQUEST_KEEP_KEY | REQUEST_USE_TESTED | REQUEST_DISABLE_TCP_CLI;
    if (report[REQUEST_VERSION] != 1u || save_transaction == 0u || (flags & (uint8_t)~supported) != 0u) return false;
    if (storage_busy()) {
        save_status = PF_HID_COMMAND_BUSY;
        return false;
    }
    /* Tested credentials come only from RAM: the request names the SSID and must not carry a password. */
    if (use_tested && (keep_password ||
        memcmp(report + REQUEST_PASSWORD, (uint8_t[PF_WIFI_PASSWORD_SIZE]){0}, PF_WIFI_PASSWORD_SIZE) != 0)) {
        return false;
    }
    /** @brief Stack-owned provisioning copies, wiped together on every exit after staging. */
    struct {
        char tested_ssid[PF_WIFI_SSID_SIZE]; /**< SSID from the staged credential test. */
        char tested_password[PF_WIFI_PASSWORD_SIZE]; /**< Passphrase from the staged credential test. */
        char old_ssid[PF_WIFI_SSID_SIZE]; /**< Saved SSID used to preserve its proven authentication mode. */
        char next_ssid[PF_WIFI_SSID_SIZE]; /**< Requested SSID. */
        char next_password[PF_WIFI_PASSWORD_SIZE]; /**< Requested or tested passphrase. */
        uint8_t next_key[32]; /**< Requested shared control key. */
    } credentials = {0};
    pf_boot_config_wifi_ssid(credentials.old_ssid);
    const pf_wifi_auth_state_t old_auth_state = pf_boot_config_wifi_auth_state();
    if (use_tested && (!pf_wifi_tested_credentials(credentials.tested_ssid, credentials.tested_password) ||
        memcmp(credentials.tested_ssid, report + REQUEST_SSID, sizeof(credentials.tested_ssid)) != 0)) {
        goto cleanup;
    }
    memcpy(credentials.next_ssid, report + REQUEST_SSID, sizeof(credentials.next_ssid));
    if (use_tested) {
        memcpy(credentials.next_password, credentials.tested_password, sizeof(credentials.next_password));
    } else {
        memcpy(credentials.next_password, report + REQUEST_PASSWORD, sizeof(credentials.next_password));
    }
    memcpy(credentials.next_key, report + REQUEST_KEY, sizeof(credentials.next_key));
    const pf_wifi_auth_state_t next_auth_state = use_tested ? pf_wifi_tested_auth_state()
        : keep_password && memcmp(credentials.old_ssid, credentials.next_ssid, sizeof(credentials.old_ssid)) == 0
            ? old_auth_state : PF_WIFI_AUTH_TRY_WPA2;
    const uint16_t clock_mhz = (uint16_t)(report[REQUEST_CLOCK_MHZ] | (uint16_t)report[REQUEST_CLOCK_MHZ + 1] << 8u);
    const bool valid = pf_boot_config_set_sysclk(pf_sysclk_profile_from_mhz(clock_mhz)) &&
        pf_boot_config_set_storage_speed((pf_storage_speed_t)report[REQUEST_STORAGE_SPEED]) &&
        pf_boot_config_set_radios(report[REQUEST_RADIOS]) &&
        pf_boot_config_set_tcp_cli_enabled((flags & REQUEST_DISABLE_TCP_CLI) == 0u) &&
        pf_boot_config_set_wifi_ssid(credentials.next_ssid) &&
        (keep_password || pf_boot_config_set_wifi_password(credentials.next_password)) &&
        (keep_key || pf_boot_config_set_ble_key(credentials.next_key)) &&
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
cleanup:
    mbedtls_platform_zeroize(&credentials, sizeof(credentials));
    if (save_status == PF_HID_COMMAND_COMPLETED) refresh_saved_wireless();
    if (save_status == PF_HID_COMMAND_COMPLETED && use_tested) pf_wifi_clear_tested_credentials();
    return save_status == PF_HID_COMMAND_COMPLETED;
}
