/**
 * @file wireless.c
 * @brief Shared SDK radio ownership, USB provisioning and combined wireless diagnostics.
 */
#include "app/wireless/wireless.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "app/wireless/bluetooth.h"
#include "app/wireless/wifi.h"
#include "pico/cyw43_arch.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "app/usb/usb_hid.h"
#include "app/usb/usb_msc.h"
#include "app/storage/storage_worker.h"

static bool wifi_enabled; /**< Wi-Fi master switch and credentials allow startup. */
static bool ble_enabled; /**< BLE master switch and key allow startup. */
static bool started; /**< Shared SDK context initialized successfully. */
static uint8_t error_code; /**< 0 none, 1 initialization, 2 join failure, 3 connection timeout. */
static uint8_t save_status; /**< Last provisioning result, using Feature 20 status values. */
static uint8_t save_transaction; /**< Last USB provisioning transaction ID. */

/** @brief Initialize the shared SDK once, always on the USB owner task/core. */
static bool ensure_started(void)
{
    if (started) return true;
    vTaskCoreAffinitySet(NULL, (UBaseType_t)1u << get_core_num());
    int rc = cyw43_arch_init();
    if (rc != 0) {
        error_code = 1u;
        PF_LOGGING_ERROR("wireless", "SDK radio initialization failed: %d", rc);
        return false;
    }
    started = true;
    return true;
}

/** @copydoc pf_wireless_start */
void pf_wireless_start(void)
{
    wifi_enabled = pf_wifi_prepare();
    ble_enabled = pf_bluetooth_prepare();
    if (!wifi_enabled && !ble_enabled) return;
    if (!ensure_started()) return;
    if (wifi_enabled) pf_wifi_start(&error_code);
    if (ble_enabled && !pf_bluetooth_start()) error_code = 1u;
}

/** @copydoc pf_wireless_poll */
void pf_wireless_poll(void)
{
    if (!started) return;
    cyw43_arch_poll();
    pf_wifi_poll(&error_code);
}

/** @copydoc pf_wireless_status */
void pf_wireless_status(uint8_t *report)
{
    char saved_ssid[33];
    char saved_password[64];
    uint8_t saved_key[32];
    pf_boot_config_wifi_ssid(saved_ssid);
    pf_boot_config_wifi_password(saved_password);
    pf_boot_config_ble_key(saved_key);
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[0] = 1u;
    report[1] = pf_boot_config_radios() & (PF_WIRELESS_WIFI | PF_WIRELESS_BLE);
    uint8_t key_bits = 0u;
    for (size_t i = 0u; i < sizeof(saved_key); ++i) key_bits |= saved_key[i];
    report[2] = save_status;
    report[3] = save_transaction;
    report[4] = !started && wifi_enabled ? 3u : pf_wifi_state();
    if (started && report[4] == 2u) pf_wifi_ip_address(report + 5u);
    memcpy(report + 9u, saved_ssid, sizeof(saved_ssid));
    report[42] = saved_password[0] != '\0';
    report[43] = key_bits != 0u;
    report[44] = error_code;
    memset(saved_ssid, 0, sizeof(saved_ssid));
    memset(saved_password, 0, sizeof(saved_password));
    memset(saved_key, 0, sizeof(saved_key));
}

/** @copydoc pf_wireless_command */
void pf_wireless_command(const uint8_t *report)
{
    if (report[2] == 0u) return;
    if (report[0] != 1u || report[1] < 1u || report[1] > 4u) {
        pf_wifi_reject_command(report[2]);
        return;
    }
    if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending() || pf_storage_worker_busy()) {
        pf_wifi_reject_command(report[2]);
        return;
    }
    if (report[1] == 2u && !started) {
        pf_wifi_reject_command(report[2]);
        return;
    }
    if (!ensure_started()) {
        pf_wifi_reject_command(report[2]);
        return;
    }
    pf_wifi_enable();
    if (!pf_wifi_command(report)) pf_wifi_reject_command(report[2]);
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
    save_transaction = report[1];
    save_status = PF_HID_COMMAND_INVALID;
    if (report[0] != 2u || report[1] == 0u || (report[3] & ~7u) != 0u) return false;
    if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending() || pf_storage_worker_busy()) {
        save_status = PF_HID_COMMAND_BUSY;
        return false;
    }
    char next_ssid[33];
    char next_password[64];
    uint8_t next_key[32];
    char tested_ssid[33] = {0};
    char tested_password[64] = {0};
    const bool use_tested = (report[3] & 4u) != 0u;
    if (use_tested && ((report[3] & 1u) != 0u || memcmp(report + 37u, (uint8_t[64]){0}, 64u) != 0)) {
        return false;
    }
    const bool tested_available = !use_tested || pf_wifi_tested_credentials(tested_ssid, tested_password);
    if (use_tested && (!tested_available || memcmp(tested_ssid, report + 4u, sizeof(tested_ssid)) != 0)) {
        memset(tested_ssid, 0, sizeof(tested_ssid));
        memset(tested_password, 0, sizeof(tested_password));
        return false;
    }
    if (use_tested) memcpy(next_ssid, tested_ssid, sizeof(next_ssid));
    else memcpy(next_ssid, report + 4u, sizeof(next_ssid));
    memcpy(next_password, report + 37u, sizeof(next_password));
    memcpy(next_key, report + 101u, sizeof(next_key));
    const uint16_t clock_mhz = (uint16_t)(report[133] | (uint16_t)report[134] << 8u);
    bool valid = pf_boot_config_set_sysclk(pf_sysclk_profile_from_mhz(clock_mhz)) &&
        pf_boot_config_set_storage_speed((pf_storage_speed_t)report[135]) &&
        pf_boot_config_set_radios(report[2]) &&
        pf_boot_config_set_wifi_ssid(next_ssid) &&
        ((report[3] & 1u) || (use_tested
            ? pf_boot_config_set_wifi_password(tested_password)
            : pf_boot_config_set_wifi_password(next_password))) &&
        ((report[3] & 2u) || pf_boot_config_set_ble_key(next_key));
    if (!valid) {
        pf_boot_config_discard();
        save_status = PF_HID_COMMAND_INVALID;
        PF_LOGGING_ERROR("wireless", "Wireless configuration invalid");
    } else if (pf_boot_config_save()) save_status = PF_HID_COMMAND_COMPLETED;
    else {
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
