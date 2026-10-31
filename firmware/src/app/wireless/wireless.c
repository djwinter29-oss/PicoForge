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

/** @copydoc pf_wireless_start */
void pf_wireless_start(void)
{
    wifi_enabled = pf_wifi_prepare();
    ble_enabled = pf_bluetooth_prepare();
    if (!wifi_enabled && !ble_enabled) return;
    /* SDK poll contexts and CYW43 IRQ setup must remain on their initialization core. */
    vTaskCoreAffinitySet(NULL, (UBaseType_t)1u << get_core_num());
    int rc = cyw43_arch_init();
    if (rc != 0) {
        error_code = 1u;
        PF_LOGGING_ERROR("wireless", "SDK radio initialization failed: %d", rc);
        return;
    }
    started = true;
    if (wifi_enabled) pf_wifi_start(&error_code);
    if (ble_enabled && !pf_bluetooth_start()) error_code = 1u;
}

/** @copydoc pf_wireless_poll */
void pf_wireless_poll(void)
{
    if (!started) return;
    cyw43_arch_poll();
    if (wifi_enabled) pf_wifi_poll(&error_code);
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
    report[1] = (saved_ssid[0] != '\0' ? PF_WIRELESS_WIFI : 0u);
    uint8_t key_bits = 0u;
    for (size_t i = 0u; i < sizeof(saved_key); ++i) key_bits |= saved_key[i];
    if (key_bits != 0u) report[1] |= PF_WIRELESS_BLE;
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

/** @copydoc pf_wireless_configure */
bool pf_wireless_configure(const uint8_t *report)
{
    save_transaction = report[1];
    save_status = PF_HID_COMMAND_INVALID;
    if (report[0] != 2u || report[1] == 0u || (report[3] & ~3u) != 0u) return false;
    if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending() || pf_storage_worker_busy()) {
        save_status = PF_HID_COMMAND_BUSY;
        return false;
    }
    char next_ssid[33];
    char next_password[64];
    uint8_t next_key[32];
    memcpy(next_ssid, report + 4u, sizeof(next_ssid));
    memcpy(next_password, report + 37u, sizeof(next_password));
    memcpy(next_key, report + 101u, sizeof(next_key));
    const uint16_t clock_mhz = (uint16_t)(report[133] | (uint16_t)report[134] << 8u);
    bool valid = pf_boot_config_set_sysclk(pf_sysclk_profile_from_mhz(clock_mhz)) &&
        pf_boot_config_set_storage_speed((pf_storage_speed_t)report[135]) &&
        pf_boot_config_set_radios(report[2]) &&
        pf_boot_config_set_wifi_ssid(next_ssid) &&
        ((report[3] & 1u) || pf_boot_config_set_wifi_password(next_password)) &&
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
    return save_status == PF_HID_COMMAND_COMPLETED;
}
