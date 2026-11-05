/**
 * @file wireless.c
 * @brief Shared SDK ownership, expiring RAM candidates and typed USB wireless operations.
 */
#include "app/wireless/wireless.h"
#include "app/usb/usb_hid.h"
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

#define STAGING_LIFETIME_US 600000000ull /**< Ten minutes from staging or successful testing. */

/** @brief Private candidate values; presence distinguishes omission from explicit clearing. */
static struct {
    uint8_t ssid[32]; /**< Opaque candidate SSID. */
    uint8_t ssid_length; /**< Candidate byte count, including embedded zeros. */
    char password[PF_WIFI_PASSWORD_SIZE]; /**< Candidate printable passphrase. */
    uint8_t key[32]; /**< Candidate raw Control key; its bytes do not encode presence. */
    bool key_present; /**< True for a length-32 stage, false for a length-zero clear. */
    uint8_t mask; /**< SSID/password/key presence bits 0/1/2. */
    uint64_t deadline; /**< Owner-clock expiry of all candidate fields. */
    bool expired; /**< Nonsecret marker retained after candidate expiry. */
    bool tested; /**< Exact current pair passed a test. */
} staged;
static bool started; /**< Shared SDK initialization succeeded. */
static bool wifi_enabled; /**< Boot switch and saved SSID permit saved Wi-Fi. */
static bool ble_enabled; /**< Boot switch and saved key permit BLE. */
static bool tcp_cli_enabled; /**< Boot-applied console switch. */
static bool boot_key_saved; /**< Boot-applied saved key gates TCP service. */
static uint8_t init_error; /**< Sticky SDK or BLE initialization failure. */

/** @brief Initialize the SDK once, pinned to the owner core. */
static bool ensure_started(void)
{
    if (started) return true;
    vTaskCoreAffinitySet(NULL, (UBaseType_t)1u << get_core_num());
    const int rc = cyw43_arch_init();
    if (rc != 0) {
        init_error = PF_WIRELESS_ERROR_INIT;
        PF_LOGGING_ERROR("wireless", "SDK radio initialization failed: %d", rc);
        return false;
    }
    started = true;
    init_error = PF_WIRELESS_ERROR_NONE;
    return true;
}

/** @brief Wipe candidates without changing the radio or retained scan. */
static void wipe_staged(void)
{
    mbedtls_platform_zeroize(&staged, sizeof(staged));
}

/** @brief Enforce expiry on every stateful access, including between owner polls. */
static void expire_staged(void)
{
    if (staged.mask == 0u || time_us_64() < staged.deadline) return;
    wipe_staged();
    staged.expired = true;
    pf_wifi_cancel_test(true);
}

/** @brief Resolve a safe candidate pair without ever borrowing another network's saved password. */
static uint8_t candidate_pair(uint8_t ssid[32], uint8_t *length, char password[PF_WIFI_PASSWORD_SIZE])
{
    uint8_t saved_ssid[32];
    const uint8_t saved_length = pf_boot_config_wifi_ssid_bytes(saved_ssid);
    *length = (staged.mask & 1u) != 0u ? staged.ssid_length : saved_length;
    memcpy(ssid, (staged.mask & 1u) != 0u ? staged.ssid : saved_ssid, 32u);
    const bool changed = *length != saved_length || memcmp(ssid, saved_ssid, *length) != 0;
    if (*length != 0u && changed && (staged.mask & 2u) == 0u) return 19u;
    if (*length == 0u) memset(password, 0, PF_WIFI_PASSWORD_SIZE);
    else if ((staged.mask & 2u) != 0u) memcpy(password, staged.password, PF_WIFI_PASSWORD_SIZE);
    else pf_boot_config_wifi_password(password);
    return 0u;
}

/** @copydoc pf_wireless_start */
void pf_wireless_start(void)
{
    wifi_enabled = pf_wifi_prepare();
    tcp_cli_enabled = pf_boot_config_tcp_cli_enabled();
    boot_key_saved = pf_boot_config_ble_key_configured();
    uint8_t key[32];
    pf_boot_config_ble_key(key);
    pf_wireless_auth_set_key(boot_key_saved ? key : NULL);
    mbedtls_platform_zeroize(key, sizeof(key));
    /* BLE is key-gated: load the key before asking Bluetooth whether it may start. */
    ble_enabled = pf_bluetooth_prepare();
    if (!ensure_started()) return;
    (void)pf_tcp_cli_set_auth_required(true);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    if (wifi_enabled) pf_wifi_start();
    if (ble_enabled && !pf_bluetooth_start()) init_error = PF_WIRELESS_ERROR_INIT;
}

/** @copydoc pf_wireless_led_put */
bool pf_wireless_led_put(bool on)
{
    if (!started) return false;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    return true;
}

/** @copydoc pf_wireless_poll */
void pf_wireless_poll(void)
{
    expire_staged();
    if (!started) return;
    cyw43_arch_poll();
    const uint64_t now = time_us_64();
    pf_wifi_poll(now);
    if (staged.mask != 0u && !staged.tested && pf_wifi_test_succeeded()) {
        staged.tested = true;
        staged.deadline = now + STAGING_LIFETIME_US;
    }
    const bool saved_link = pf_wifi_saved_link_up();
    const bool serve = tcp_cli_enabled && boot_key_saved && saved_link;
    pf_tcp_cli_poll(serve, now);
    pf_wifi_mdns_poll(saved_link, serve && pf_tcp_cli_serving(), now);
}

/** @copydoc pf_wireless_status */
void pf_wireless_status(uint8_t *report)
{
    expire_staged();
    memset(report, 0, PF_WIRELESS_STATUS_SIZE);
    report[0] = 1u;
    pf_wifi_status(report);
    if (init_error != 0u) {
        report[2] = 1u;
        if (wifi_enabled && !started) report[1] = 3u;
    }
    report[3] = started && ble_enabled ? pf_bluetooth_state() : 0u;
    uint8_t ssid[32];
    const uint8_t length = pf_boot_config_wifi_ssid_bytes(ssid);
    report[4] = (length != 0u ? 1u : 0u) | (pf_boot_config_ble_key_configured() ? 2u : 0u) |
        (pf_tcp_cli_serving() ? 4u : 0u) | (uint8_t)(staged.mask << 4u) | (staged.tested ? 128u : 0u);
    report[47] = (((staged.mask & 1u) != 0u && staged.ssid_length == 0u) ? 1u : 0u) |
        (((staged.mask & 2u) != 0u && (staged.password[0] == '\0' ||
            ((staged.mask & 1u) != 0u && staged.ssid_length == 0u))) ? 2u : 0u) |
        (((staged.mask & 4u) != 0u && !staged.key_present) ? 4u : 0u) |
        (staged.expired ? 8u : 0u);
}

/** @copydoc pf_wireless_staged */
bool pf_wireless_staged(void)
{
    expire_staged();
    return staged.mask != 0u;
}

/** @copydoc pf_wireless_save_precondition */
uint8_t pf_wireless_save_precondition(bool commit)
{
    expire_staged();
    if (commit && pf_wifi_test_active()) return 25u;
    if (commit && pf_wifi_scan_active()) return 26u;
    if (!commit) return staged.mask != 0u ? 27u : 0u;
    if (staged.expired) return 20u;
    if (staged.mask == 0u) return 21u;
    uint8_t ssid[32], length;
    char password[PF_WIFI_PASSWORD_SIZE] = {0};
    const uint8_t detail = candidate_pair(ssid, &length, password);
    mbedtls_platform_zeroize(password, sizeof(password));
    return detail;
}

/** @copydoc pf_wireless_apply_staged */
bool pf_wireless_apply_staged(uint8_t *commit_mask)
{
    *commit_mask = 0u;
    if (pf_wireless_save_precondition(true) != 0u) return false;
    uint8_t ssid[32], length, saved_ssid[32];
    char password[PF_WIFI_PASSWORD_SIZE] = {0};
    (void)candidate_pair(ssid, &length, password);
    const uint8_t saved_length = pf_boot_config_wifi_ssid_bytes(saved_ssid);
    const bool changed = length != saved_length || memcmp(ssid, saved_ssid, length) != 0;
    bool ok = true;
    if ((staged.mask & 3u) != 0u) {
        const pf_wifi_auth_state_t auth = staged.tested ? pf_wifi_tested_auth_state() :
            changed ? PF_WIFI_AUTH_TRY_WPA2 : pf_boot_config_wifi_auth_state();
        ok = pf_boot_config_set_wifi_ssid_bytes(ssid, length) &&
            pf_boot_config_set_wifi_password(password) && pf_boot_config_set_wifi_auth_state(auth);
    }
    if (ok && (staged.mask & 4u) != 0u) {
        ok = pf_boot_config_set_ble_key(staged.key_present ? staged.key : NULL);
    }
    mbedtls_platform_zeroize(password, sizeof(password));
    if (ok) *commit_mask = staged.mask;
    return ok;
}

/** @copydoc pf_wireless_discard */
void pf_wireless_discard(void)
{
    wipe_staged();
    pf_wifi_cancel_test(true);
}

/** @copydoc pf_wireless_saved */
void pf_wireless_saved(void)
{
    wipe_staged();
    pf_wifi_cancel_test(false);
    if (!pf_boot_config_ble_key_configured()) boot_key_saved = false;
}

/** @brief Test whether an unused argument range is zero. */
static bool bytes_zero(const uint8_t *bytes, size_t count)
{
    for (size_t i = 0u; i < count; ++i) if (bytes[i] != 0u) return false;
    return true;
}

/** @brief Set a rejection without leaving result data or secret bytes behind. */
static void reject(pf_hid_control_result_t *result, uint8_t status, uint8_t error, uint8_t detail)
{
    result->status = status;
    result->error_reason = error;
    memset(result->result_data, 0, sizeof(result->result_data));
    result->result_data[0] = detail;
}

/** @copydoc pf_wireless_command */
void pf_wireless_command(const pf_hid_control_request_t *request, pf_hid_control_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->version = 1u;
    result->transaction_id = request->transaction_id;
    result->opcode = request->opcode;
    result->status = PF_HID_COMMAND_COMPLETED;
    const uint8_t op = request->opcode;
    const uint8_t *args = request->arguments;
    if (op < 6u || op > 13u) {
        reject(result, 4u, 7u, 0u);
        return;
    }
    uint8_t detail = 0u;
    size_t used = 0u;
    if (op <= 8u) {
        const uint8_t length = args[0];
        if (op == 6u && length > 32u) detail = 16u;
        if (op == 7u && (length > 63u || (length != 0u && length < 8u))) detail = 17u;
        if (op == 8u && length != 0u && length != 32u) detail = 18u;
        used = length <= (op == 7u ? 63u : 32u) ? (size_t)length + 1u : 64u;
        if (op == 7u && detail == 0u) {
            for (size_t i = 1u; i < used; ++i) if (args[i] < 32u || args[i] > 126u) detail = 17u;
        }
    } else used = op == 12u ? 3u : op == 13u ? 16u : 0u;
    if (request->flags != 0u || !bytes_zero(args + used, 64u - used)) {
        reject(result, 3u, 1u, 0u);
        return;
    }
    if (detail != 0u) {
        reject(result, 3u, 3u, detail);
        return;
    }
    expire_staged();
    if (((op >= 6u && op <= 9u) || op == 11u) && pf_wifi_test_active()) detail = 25u;
    else if ((op == 9u || op == 11u || op == 12u) && pf_wifi_scan_active()) detail = 26u;
    if (detail != 0u) {
        reject(result, 5u, 5u, detail);
        return;
    }
    if (op <= 8u) {
        if (op != 8u) {
            pf_wifi_cancel_test(true);
            staged.tested = false;
        }
        staged.expired = false;
        staged.deadline = time_us_64() + STAGING_LIFETIME_US;
        if (op == 6u) {
            memset(staged.ssid, 0, sizeof(staged.ssid));
            staged.ssid_length = args[0];
            memcpy(staged.ssid, args + 1u, args[0]);
            staged.mask |= 1u;
            if (args[0] == 0u) {
                mbedtls_platform_zeroize(staged.password, sizeof(staged.password));
                staged.mask |= 2u;
            }
        } else if (op == 7u) {
            mbedtls_platform_zeroize(staged.password, sizeof(staged.password));
            memcpy(staged.password, args + 1u, args[0]);
            staged.mask |= 2u;
        } else {
            mbedtls_platform_zeroize(staged.key, sizeof(staged.key));
            staged.key_present = args[0] == 32u;
            memcpy(staged.key, args + 1u, args[0]);
            staged.mask |= 4u;
        }
        return;
    }
    if (op == 10u) {
        pf_wireless_discard();
        return;
    }
    if (op == 12u) {
        detail = pf_wifi_scan_entry(args[0], (uint16_t)(args[1] | (uint16_t)args[2] << 8u),
            result->result_data);
        if (detail != 0u) reject(result, 3u, 3u, detail);
        return;
    }
    if (op == 13u) {
        if (!pf_boot_config_ble_key_configured()) reject(result, 3u, 3u, 24u);
        else if (!pf_wireless_auth_tcp_cli_proof(args, result->result_data)) reject(result, 6u, 6u, 0u);
        return;
    }
    uint8_t ssid[32], length = 0u;
    char password[PF_WIFI_PASSWORD_SIZE] = {0};
    if (op == 9u) {
        detail = staged.expired ? 20u : (staged.mask & 3u) == 0u ? 21u :
            candidate_pair(ssid, &length, password);
        if (detail == 0u && length == 0u) detail = 19u;
        if (detail != 0u) {
            mbedtls_platform_zeroize(password, sizeof(password));
            reject(result, 3u, 3u, detail);
            return;
        }
    }
    if (!ensure_started()) reject(result, 6u, 6u, 0u);
    else if (op == 11u) pf_wifi_scan();
    else {
        staged.tested = false;
        if (!pf_wifi_test(ssid, length, password)) reject(result, 6u, 6u, 0u);
    }
    mbedtls_platform_zeroize(password, sizeof(password));
}
