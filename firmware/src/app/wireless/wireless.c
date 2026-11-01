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
#include "app/wireless/wifi/wifi.h"
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
    ble_enabled = pf_bluetooth_prepare();
    /* The chip starts even with both radios off because it owns the board LED; radios stay off unless enabled. */
    if (!ensure_started()) return;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    if (wifi_enabled) {
        pf_wifi_start();
        (void)pf_tcp_cli_start();
    }
    if (ble_enabled && !pf_bluetooth_start()) init_error = PF_WIRELESS_ERROR_INIT;
}

/** @copydoc pf_wireless_poll */
void pf_wireless_poll(void)
{
    if (!started) return;
    cyw43_arch_poll();
    pf_wifi_poll();
    /* The unauthenticated TCP CLI serves only the saved network, never a test network or a lost link. */
    pf_tcp_cli_set_serving(pf_wifi_saved_link_up());
}

/** @copydoc pf_wireless_status */
void pf_wireless_status(uint8_t *report)
{
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[PF_WIRELESS_STATUS_VERSION_OFFSET] = 1u;
    report[PF_WIRELESS_STATUS_RADIOS_OFFSET] = pf_boot_config_radios() & (PF_WIRELESS_WIFI | PF_WIRELESS_BLE);
    report[PF_WIRELESS_STATUS_SAVE_STATUS_OFFSET] = save_status;
    report[PF_WIRELESS_STATUS_SAVE_TRANSACTION_OFFSET] = save_transaction;
    const uint8_t wifi_state = !started && wifi_enabled ? PF_WIFI_STATE_FAILED : pf_wifi_state();
    report[PF_WIRELESS_STATUS_WIFI_STATE_OFFSET] = wifi_state;
    if (started && wifi_state == PF_WIFI_STATE_CONNECTED) {
        pf_wifi_ip_address(report + PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET);
    }
    pf_boot_config_wifi_ssid((char *)report + PF_WIRELESS_STATUS_SSID_OFFSET);
    report[PF_WIRELESS_STATUS_PASSWORD_CONFIGURED_OFFSET] = pf_boot_config_wifi_password_configured();
    report[PF_WIRELESS_STATUS_BLE_KEY_CONFIGURED_OFFSET] = pf_boot_config_ble_key_configured();
    report[PF_WIRELESS_STATUS_ERROR_OFFSET] = init_error != PF_WIRELESS_ERROR_NONE ? init_error : pf_wifi_error();
    report[PF_WIRELESS_STATUS_BLUETOOTH_STATE_OFFSET] = started && ble_enabled ? pf_bluetooth_state() : 0u;
    report[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] = pf_tcp_cli_serving();
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
    if (report[REQUEST_VERSION] != 2u || save_transaction == 0u ||
        (flags & ~(REQUEST_KEEP_PASSWORD | REQUEST_KEEP_KEY | REQUEST_USE_TESTED)) != 0u) return false;
    if (storage_busy()) {
        save_status = PF_HID_COMMAND_BUSY;
        return false;
    }
    /* Tested credentials come only from RAM: the request names the SSID and must not carry a password. */
    if (use_tested && (keep_password || memcmp(report + REQUEST_PASSWORD, (uint8_t[64]){0}, 64u) != 0)) {
        return false;
    }
    char tested_ssid[33] = {0};
    char tested_password[64] = {0};
    if (use_tested && (!pf_wifi_tested_credentials(tested_ssid, tested_password) ||
                       memcmp(tested_ssid, report + REQUEST_SSID, sizeof(tested_ssid)) != 0)) {
        memset(tested_ssid, 0, sizeof(tested_ssid));
        memset(tested_password, 0, sizeof(tested_password));
        return false;
    }
    char next_ssid[33];
    char next_password[64];
    uint8_t next_key[32];
    memcpy(next_ssid, report + REQUEST_SSID, sizeof(next_ssid));
    if (use_tested) memcpy(next_password, tested_password, sizeof(next_password));
    else memcpy(next_password, report + REQUEST_PASSWORD, sizeof(next_password));
    memcpy(next_key, report + REQUEST_KEY, sizeof(next_key));
    const uint16_t clock_mhz = (uint16_t)(report[REQUEST_CLOCK_MHZ] | (uint16_t)report[REQUEST_CLOCK_MHZ + 1] << 8u);
    const bool valid = pf_boot_config_set_sysclk(pf_sysclk_profile_from_mhz(clock_mhz)) &&
        pf_boot_config_set_storage_speed((pf_storage_speed_t)report[REQUEST_STORAGE_SPEED]) &&
        pf_boot_config_set_radios(report[REQUEST_RADIOS]) &&
        pf_boot_config_set_wifi_ssid(next_ssid) &&
        (keep_password || pf_boot_config_set_wifi_password(next_password)) &&
        (keep_key || pf_boot_config_set_ble_key(next_key));
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
    if (save_status == PF_HID_COMMAND_COMPLETED && use_tested) pf_wifi_clear_tested_credentials();
    return save_status == PF_HID_COMMAND_COMPLETED;
}
