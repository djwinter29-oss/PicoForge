/**
 * @file wifi.c
 * @brief Asynchronous WPA2 station connection, DHCP diagnostics and paced reconnects.
 */
#include "app/wireless/wifi.h"

#include <string.h>

#include "cyw43.h"
#include "pico/cyw43_arch.h"
#include "platform/boot_config.h"
#include "platform/logging.h"

#define PF_WIFI_SCAN_LIMIT 18u
#define PF_WIFI_SCAN_PAGE_SIZE 6u
/* ponytail: cap results at 18 (three report pages) to keep static RAM bounded on RP2040; raise with a paged protocol. */

/** @brief Sanitized access point data retained between report reads. */
typedef struct {
    uint8_t ssid_length; /**< UTF-8 SSID byte count. */
    uint8_t ssid[32]; /**< SSID; never contains credentials. */
    int8_t rssi; /**< Signal strength in dBm, saturated to int8_t. */
    uint8_t secured; /**< True when authentication is required. */
} pf_wifi_scan_entry_t;

static char wifi_ssid[33]; /**< Boot-applied SSID. */
static char wifi_password[64]; /**< Boot-applied private passphrase. */
static char test_ssid[33]; /**< Temporary SSID under test; never persisted until Feature 31 save. */
static char test_password[64]; /**< Temporary passphrase under test; never persisted until Feature 31 save. */
static pf_wifi_scan_entry_t scan_entries[PF_WIFI_SCAN_LIMIT]; /**< Bounded strongest scan results. */
static uint64_t connect_deadline_us; /**< Wi-Fi association/DHCP attempt deadline. */
static uint64_t retry_us; /**< Next permitted Wi-Fi reconnect attempt. */
static uint8_t wifi_state; /**< 0 disabled, 1 connecting, 2 connected, 3 failed. */
static uint8_t command_transaction; /**< Latest scan/test transaction. */
static uint8_t command_state; /**< 0 idle, 1 scanning, 2 scan complete, 3 scan failed, 4 joining, 5 joined, 6 failed. */
static uint8_t scan_count; /**< Number of retained results, at most PF_WIFI_SCAN_LIMIT. */
static uint8_t scan_page; /**< Selected result page. */
static bool test_active; /**< True while a temporary credential test is joining. */
static bool test_succeeded; /**< Tested credentials remain staged until saved or explicitly cleared. */
static bool saved_wifi_enabled; /**< Saved switch and credential availability for restoring the boot network. */

/** @brief Return whether every byte in a reserved or unused report range is zero. */
static bool bytes_zero(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0u; i < length; ++i) {
        if (bytes[i] != 0u) return false;
    }
    return true;
}

/** @brief Capture unique non-hidden APs, retaining the strongest RSSI for each SSID. */
static int scan_result(void *environment, const cyw43_ev_scan_result_t *result)
{
    (void)environment;
    if (result == NULL || result->ssid_len == 0u || result->ssid_len > sizeof(result->ssid)) return 0;
    uint8_t index = 0u;
    while (index < scan_count && (scan_entries[index].ssid_length != result->ssid_len ||
           memcmp(scan_entries[index].ssid, result->ssid, result->ssid_len) != 0)) ++index;
    if (index == scan_count) {
        if (scan_count == PF_WIFI_SCAN_LIMIT) return 0;
        ++scan_count;
        scan_entries[index].ssid_length = result->ssid_len;
        memcpy(scan_entries[index].ssid, result->ssid, result->ssid_len);
        scan_entries[index].rssi = INT8_MIN;
    }
    if (result->rssi > scan_entries[index].rssi) {
        scan_entries[index].rssi = result->rssi > INT8_MAX ? INT8_MAX
            : result->rssi < INT8_MIN ? INT8_MIN : (int8_t)result->rssi;
        scan_entries[index].secured = result->auth_mode != CYW43_AUTH_OPEN;
    }
    return 0;
}

/** @brief Begin an asynchronous WPA2 connection; retries are paced after explicit failures. */
static void connect_wifi(uint8_t *error)
{
    const uint32_t auth = wifi_password[0] == '\0' ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK;
    int rc = cyw43_arch_wifi_connect_async(wifi_ssid, wifi_password, auth);
    wifi_state = rc == 0 ? 1u : 3u;
    if (error != NULL) *error = rc == 0 ? 0u : 2u;
    connect_deadline_us = time_us_64() + 30000000u;
    retry_us = time_us_64() + 5000000u;
    if (rc != 0) PF_LOGGING_ERROR("wifi", "Wi-Fi connection request failed: %d", rc);
}

/** @copydoc pf_wifi_prepare */
bool pf_wifi_prepare(void)
{
    pf_boot_config_wifi_ssid(wifi_ssid);
    pf_boot_config_wifi_password(wifi_password);
    saved_wifi_enabled = (pf_boot_config_radios() & PF_WIRELESS_WIFI) != 0u && wifi_ssid[0] != '\0';
    return saved_wifi_enabled;
}

/** @copydoc pf_wifi_start */
void pf_wifi_start(uint8_t *error)
{
    pf_wifi_enable();
    if (wifi_ssid[0] != '\0') connect_wifi(error);
}

/** @copydoc pf_wifi_enable */
void pf_wifi_enable(void)
{
    cyw43_arch_enable_sta_mode();
}

/** @copydoc pf_wifi_poll */
void pf_wifi_poll(uint8_t *error)
{
    if (command_state == 1u) {
        if (!cyw43_wifi_scan_active(&cyw43_state)) {
            for (uint8_t i = 1u; i < scan_count; ++i) {
                pf_wifi_scan_entry_t current = scan_entries[i];
                uint8_t j = i;
                while (j > 0u && scan_entries[j - 1u].rssi < current.rssi) {
                    scan_entries[j] = scan_entries[j - 1u];
                    --j;
                }
                scan_entries[j] = current;
            }
            command_state = 2u;
        }
        return;
    }
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    uint64_t now = time_us_64();
    if (test_active) {
        if (link == CYW43_LINK_UP) {
            command_state = 5u;
            test_active = false;
            test_succeeded = true;
            wifi_state = 2u;
            *error = 0u;
        } else if (link < 0 || now >= connect_deadline_us) {
            memset(test_ssid, 0, sizeof(test_ssid));
            memset(test_password, 0, sizeof(test_password));
            command_state = 6u;
            test_active = false;
            wifi_state = 3u;
            *error = link < 0 ? 2u : 3u;
        }
        return;
    }
    if (link == CYW43_LINK_UP) {
        wifi_state = 2u;
        *error = 0u;
    } else if (wifi_state == 1u && (link < 0 || now >= connect_deadline_us)) {
        wifi_state = 3u;
        *error = link < 0 ? 2u : 3u;
        retry_us = now + 5000000u;
        PF_LOGGING_WARNING("wifi", "Wi-Fi association/DHCP failed: %d", link);
    } else if (wifi_state == 2u) {
        wifi_state = 3u;
        retry_us = now + 5000000u;
        PF_LOGGING_WARNING("wifi", "Wi-Fi link lost");
    } else if (wifi_state == 3u && now >= retry_us) connect_wifi(error);
}

/** @copydoc pf_wifi_state */
uint8_t pf_wifi_state(void)
{
    return wifi_state;
}

/** @copydoc pf_wifi_command */
bool pf_wifi_command(const uint8_t report[101])
{
    const uint8_t operation = report[1];
    command_transaction = report[2];
    if (command_transaction == 0u || report[0] != 1u || operation < 1u || operation > 4u) return false;
    if (operation == 1u) {
        if (!bytes_zero(report + 3u, 98u)) return false;
        if (test_active || cyw43_wifi_scan_active(&cyw43_state)) return false;
        memset(scan_entries, 0, sizeof(scan_entries));
        scan_count = 0u;
        scan_page = 0u;
        command_state = 1u;
        cyw43_wifi_scan_options_t options = {0};
        if (cyw43_wifi_scan(&cyw43_state, &options, NULL, scan_result) != 0) {
            command_state = 3u;
            return false;
        }
        return true;
    }
    if (operation == 2u) {
        if (report[3] >= (PF_WIFI_SCAN_LIMIT + PF_WIFI_SCAN_PAGE_SIZE - 1u) / PF_WIFI_SCAN_PAGE_SIZE ||
            command_state != 2u || report[3] * PF_WIFI_SCAN_PAGE_SIZE >= scan_count ||
            !bytes_zero(report + 4u, 97u)) return false;
        scan_page = report[3];
        return true;
    }
    if (operation == 3u) {
        uint8_t ssid_length = report[4];
        uint8_t password_length = report[5];
        if (report[3] != 0u || command_state == 1u || test_active || ssid_length == 0u || ssid_length > 32u ||
            password_length > 63u || (password_length != 0u && password_length < 8u) ||
            memchr(report + 6u, '\0', ssid_length) != NULL ||
            !bytes_zero(report + 6u + ssid_length, 32u - ssid_length) ||
            !bytes_zero(report + 38u + password_length, 63u - password_length)) return false;
        for (size_t i = 0u; i < password_length; ++i) {
            if (report[38u + i] < 32u || report[38u + i] > 126u) return false;
        }
        memset(test_ssid, 0, sizeof(test_ssid));
        memset(test_password, 0, sizeof(test_password));
        test_succeeded = false;
        memcpy(test_ssid, report + 6u, ssid_length);
        memcpy(test_password, report + 38u, password_length);
        test_active = true;
        command_state = 4u;
        wifi_state = 1u;
        connect_deadline_us = time_us_64() + 30000000u;
        const int rc = cyw43_arch_wifi_connect_async(test_ssid, test_password,
            password_length == 0u ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK);
        if (rc != 0) {
            test_active = false;
            command_state = 6u;
            memset(test_ssid, 0, sizeof(test_ssid));
            memset(test_password, 0, sizeof(test_password));
            return false;
        }
        return true;
    }
    if (operation == 4u) {
        if (!bytes_zero(report + 3u, 98u)) return false;
        if (test_active || test_succeeded || command_state == 5u) {
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        }
        test_active = false;
        test_succeeded = false;
        memset(test_ssid, 0, sizeof(test_ssid));
        memset(test_password, 0, sizeof(test_password));
        wifi_state = 0u;
        if (saved_wifi_enabled) connect_wifi(NULL);
        command_state = 0u;
        return true;
    }
    return false;
}

/** @copydoc pf_wifi_reject_command */
void pf_wifi_reject_command(uint8_t transaction)
{
    command_transaction = transaction;
    command_state = 6u;
    test_active = false;
    test_succeeded = false;
    memset(test_ssid, 0, sizeof(test_ssid));
    memset(test_password, 0, sizeof(test_password));
}

/** @copydoc pf_wifi_result */
void pf_wifi_result(uint8_t report[220])
{
    memset(report, 0, 220u);
    report[0] = 1u;
    report[1] = command_transaction;
    report[2] = command_state;
    report[3] = scan_page;
    report[4] = scan_count;
    report[5] = wifi_state;
    const uint8_t start = (uint8_t)(scan_page * PF_WIFI_SCAN_PAGE_SIZE);
    for (uint8_t i = 0u; i < PF_WIFI_SCAN_PAGE_SIZE && start + i < scan_count; ++i) {
        const pf_wifi_scan_entry_t *entry = &scan_entries[start + i];
        uint8_t *item = report + 6u + (uint16_t)i * 35u;
        item[0] = entry->ssid_length;
        memcpy(item + 1u, entry->ssid, entry->ssid_length);
        item[33] = (uint8_t)entry->rssi;
        item[34] = entry->secured;
    }
    if (command_state == 5u) {
        pf_wifi_ip_address(report + 216u);
    }
}

/** @copydoc pf_wifi_tested_credentials */
bool pf_wifi_tested_credentials(char ssid[33], char password[64])
{
    if (!test_succeeded || test_ssid[0] == '\0') return false;
    memcpy(ssid, test_ssid, sizeof(test_ssid));
    memcpy(password, test_password, sizeof(test_password));
    return true;
}

/** @copydoc pf_wifi_clear_tested_credentials */
void pf_wifi_clear_tested_credentials(void)
{
    memset(test_ssid, 0, sizeof(test_ssid));
    memset(test_password, 0, sizeof(test_password));
    test_succeeded = false;
    command_state = 0u;
}

/** @copydoc pf_wifi_ip_address */
void pf_wifi_ip_address(uint8_t address[4])
{
    memcpy(address, &netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])->addr, 4u);
}
