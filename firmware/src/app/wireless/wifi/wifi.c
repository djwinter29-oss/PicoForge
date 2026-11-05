/**
 * @file wifi.c
 * @brief Asynchronous WPA2/WPA3-SAE station connection, scans, temporary credential tests and paced reconnects.
 */
#include "app/wireless/wifi/wifi.h"

#include <string.h>

#include "mbedtls/platform_util.h"
#include "cyw43.h"
#include "pico/cyw43_arch.h"
#include "app/wireless/wireless.h"
#include "platform/boot_config.h"
#include "platform/logging.h"

#define PF_WIFI_SCAN_LIMIT 18u
/* ponytail: cap results at the protocol's strongest 18 networks to bound static RAM on RP2040. */
/* ponytail: mirrors the pinned cyw43-driver's private "associated" join bit; the driver retries a WPA2 handshake
 * with a wrong passphrase silently instead of reporting BADAUTH. Recheck this bit when updating the driver. */
#define PF_CYW43_JOIN_LINK 0x0400u
#define PF_WIFI_HANDSHAKE_FAILURES 2u
#define PF_WIFI_CONNECT_TIMEOUT_US 30000000u /**< Association plus DHCP deadline. */
#define PF_WIFI_RETRY_DELAY_US 5000000u /**< Pause before rejoining the saved network after a failure. */
#define PF_WIFI_RETRY_MAX_US 60000000u /**< Cap saved-network backoff to one minute. */
#define PF_WIFI_LEAVE_RETRY_DELAY_US 5000000u /**< Pace retries when the driver refuses to leave a test network. */
#define PF_WIFI_RSSI_REFRESH_US 2000000u /**< Avoid querying the radio for each status read. */
#define PF_WIFI_SCAN_TIMEOUT_US 10000000u /**< Scan deadline; the driver has no completion timeout. */
#define PF_WIFI_SSID_MAX (PF_WIFI_SSID_SIZE - 1u)

/** @brief Internal retained test state, independent of scans. */
typedef enum {
    PF_WIFI_COMMAND_IDLE = 0, /**< No retained test result. */
    PF_WIFI_COMMAND_TESTING = 4, /**< Test credentials are joining. */
    PF_WIFI_COMMAND_TEST_JOINED = 5, /**< Test credentials joined and are staged for saving. */
    PF_WIFI_COMMAND_FAILED = 6, /**< Test failed or the command was rejected. */
} pf_wifi_command_state_t;

/** @brief Internal association progress and retained failure reason. */
typedef enum {
    PF_WIFI_DETAIL_NONE = 0, /**< No detail; also used for rejected commands. */
    PF_WIFI_DETAIL_SEARCHING = 1, /**< Finding and associating with the network. */
    PF_WIFI_DETAIL_PASSWORD = 2, /**< Associated; WPA handshake is checking the password. */
    PF_WIFI_DETAIL_DHCP = 3, /**< Password accepted; waiting for a DHCP address. */
    PF_WIFI_DETAIL_NOT_FOUND = 4, /**< Failed: network not found. */
    PF_WIFI_DETAIL_REJECTED = 5, /**< Failed: password rejected (repeated handshake failure or BADAUTH). */
    PF_WIFI_DETAIL_NO_ADDRESS = 6, /**< Failed: password accepted but no DHCP address before the deadline. */
    PF_WIFI_DETAIL_TIMEOUT = 7, /**< Failed: association did not complete before the deadline. */
    PF_WIFI_DETAIL_START_FAILED = 8, /**< Failed: the driver could not start the test connection. */
    PF_WIFI_DETAIL_LINK_LOST = 9, /**< Test link dropped; successful credentials remain staged until expiry. */
} pf_wifi_detail_t;

/** @brief Sanitized access point data retained between report reads. */
typedef struct {
    uint8_t ssid_length; /**< Opaque SSID byte count. */
    uint8_t ssid[PF_WIFI_SSID_MAX]; /**< SSID; never contains credentials. */
    int8_t rssi; /**< Signal strength in dBm, saturated to int8_t. */
    uint8_t security; /**< SDK scan bits: privacy 1, WPA 2, RSN 4; RSN does not distinguish WPA2 from WPA3. */
} pf_wifi_scan_entry_t;

/** @brief Station connection owner, distinct from saved-radio configuration and station-mode ownership. */
typedef enum {
    PF_WIFI_LINK_NONE, /**< No saved or temporary network owns the current station connection. */
    PF_WIFI_LINK_SAVED, /**< The station is joining or connected to saved credentials. */
    PF_WIFI_LINK_TEST, /**< The station is joining or connected to temporary test credentials. */
} pf_wifi_link_owner_t;

/** @brief Work deferred until CYW43 reports that the previous station link has dropped. */
typedef enum {
    PF_WIFI_AFTER_LEAVE_NONE, /**< No disconnect is pending. */
    PF_WIFI_AFTER_LEAVE_JOIN_TEST, /**< Join the staged test network once the previous link is down. */
    PF_WIFI_AFTER_LEAVE_RESTORE, /**< Rejoin saved Wi-Fi, or release station mode when it is disabled. */
} pf_wifi_after_leave_t;

static char wifi_ssid[PF_WIFI_SSID_SIZE]; /**< Boot-applied SSID. */
static uint8_t wifi_ssid_length; /**< Explicit boot-applied SSID byte count. */
static char wifi_password[PF_WIFI_PASSWORD_SIZE]; /**< Boot-applied private passphrase. */
static pf_wifi_scan_entry_t scan_entries[PF_WIFI_SCAN_LIMIT]; /**< Bounded strongest scan results. */
static uint64_t connect_deadline_us; /**< Wi-Fi association/DHCP attempt deadline. */
static uint64_t retry_us; /**< Next permitted Wi-Fi reconnect attempt. */
static uint64_t leave_retry_us; /**< Next attempt to leave a test network after a driver error. */
static uint64_t scan_deadline_us; /**< Deadline for the active scan. */
static uint64_t rssi_refresh_us; /**< Earliest next saved-link RSSI query. */
static uint64_t retry_delay_us = PF_WIFI_RETRY_DELAY_US; /**< Exponential saved-network retry delay. */
static int8_t saved_rssi = PF_WIFI_RSSI_UNAVAILABLE; /**< Most recently sampled saved-link RSSI. */
static pf_wifi_state_t wifi_state; /**< Station diagnostic state. */
static pf_wireless_error_t wifi_error; /**< Latest join or test error. */
static uint8_t saved_detail; /**< Report 12 saved-link failure detail, zero when no specific cause is known. */
static pf_wifi_command_state_t command_state; /**< Retained test state; scan state is independent. */
static uint8_t scan_count; /**< Number of retained results, at most PF_WIFI_SCAN_LIMIT. */
static uint8_t scan_state; /**< Idle/scanning/complete/failed, independent of the test. */
static uint16_t scan_generation; /**< Nonzero generation for each accepted scan, skipping zero on wrap. */
static uint8_t test_generation; /**< Nonzero generation for each accepted test, skipping zero on wrap. */
static pf_wifi_detail_t join_detail; /**< Detail reported while a test is joining, joined or failed. */
static pf_wifi_link_owner_t link_owner; /**< Saved/test ownership of the current station connection attempt. */
static bool saved_wifi_enabled; /**< Saved switch and credential availability for restoring the boot network. */
static bool station_enabled; /**< Station mode is owned by this module and may need releasing. */
static bool scan_release_pending; /**< A timed-out driver scan must finish before station mode is released. */
static pf_wifi_after_leave_t after_leave; /**< Single owner of every wait for the previous link to drop. */

/** @brief Temporary test state and credentials; cleared as one unit when discarded or expired. */
static struct {
    char ssid[PF_WIFI_SSID_SIZE]; /**< Temporary SSID; never persisted before save. */
    uint8_t ssid_length; /**< Explicit temporary SSID byte count. */
    char password[PF_WIFI_PASSWORD_SIZE]; /**< Temporary passphrase; wiped when the test ends. */
    uint8_t handshake_failures; /**< Handshakes dropped back to association during the current test. */
    pf_wifi_detail_t wpa2_failure; /**< WPA2 failure detail if the SAE fallback also finds no network. */
    bool handshake_active; /**< Current test was associated at the previous poll. */
    bool succeeded; /**< Tested credentials remain staged until saved, expired or cleared. */
    bool sae; /**< Current test attempt uses WPA3-SAE instead of the initial WPA2 mode. */
} test;

static pf_wifi_auth_state_t saved_auth_state; /**< Saved-network mode, loaded from its credential record. */
static uint16_t reconnect_count; /**< Saturating count of saved-network reconnect attempts after first link loss. */

/** @brief Return whether the station is joining, associated or addressed. */
static bool link_active(int link)
{
    return link == CYW43_LINK_UP || link == CYW43_LINK_NOIP || link == CYW43_LINK_JOIN;
}

/** @brief Schedule a saved-network retry and increase the next delay up to one minute. */
static void schedule_retry(uint64_t now)
{
    retry_us = now + retry_delay_us;
    if (retry_delay_us < PF_WIFI_RETRY_MAX_US) {
        retry_delay_us *= 2u;
        if (retry_delay_us > PF_WIFI_RETRY_MAX_US) retry_delay_us = PF_WIFI_RETRY_MAX_US;
    }
}

/* ponytail: the pinned cyw43-driver joins WPA3 only as SAE-only, and scan results cannot tell WPA3 from WPA2, so
 * WPA2 (which also joins WPA2/WPA3 transition networks) is tried first when no mode has been proven. Proven modes
 * are stored with saved credentials. Keys are always sent as passphrases, so 64-hex-digit raw PSKs are unsupported. */
/** @brief Select open, WPA2 or WPA3-SAE authentication from the passphrase and the current fallback choice. */
static uint32_t auth_for(const char *password, bool sae)
{
    return password[0] == '\0' ? CYW43_AUTH_OPEN : sae ? CYW43_AUTH_WPA3_SAE_AES_PSK : CYW43_AUTH_WPA2_MIXED_PSK;
}

/** @brief Return whether a temporary credential test is joining. */
static bool testing(void)
{
    return command_state == PF_WIFI_COMMAND_TESTING;
}

/** @brief Drop station mode when saved Wi-Fi is disabled and no scan or test result still needs it. */
static void release_station(void)
{
    if (!station_enabled || saved_wifi_enabled || testing() || test.succeeded ||
        scan_state == 1u || cyw43_wifi_scan_active(&cyw43_state)) return;
    cyw43_arch_disable_sta_mode();
    station_enabled = false;
    link_owner = PF_WIFI_LINK_NONE;
    after_leave = PF_WIFI_AFTER_LEAVE_NONE;
    leave_retry_us = 0u;
    wifi_state = PF_WIFI_STATE_DISABLED;
    wifi_error = PF_WIRELESS_ERROR_NONE;
}

/** @brief Wipe temporary test credentials. */
static void clear_test_credentials(void)
{
    mbedtls_platform_zeroize(&test, sizeof(test));
}

/** @brief Keep the strongest non-hidden APs sorted strongest first, dropping the weakest when the table is full. */
static int scan_result(void *environment, const cyw43_ev_scan_result_t *result)
{
    (void)environment;
    if (scan_state != 1u || result == NULL || result->ssid_len == 0u ||
        result->ssid_len > sizeof(result->ssid)) return 0;
    const int8_t rssi = result->rssi > INT8_MAX ? INT8_MAX
        : result->rssi < INT8_MIN ? INT8_MIN : (int8_t)result->rssi;
    uint8_t index = 0u;
    while (index < scan_count && (scan_entries[index].ssid_length != result->ssid_len ||
           memcmp(scan_entries[index].ssid, result->ssid, result->ssid_len) != 0)) ++index;
    if (index < scan_count) {
        if (rssi <= scan_entries[index].rssi) return 0;
    } else if (scan_count == PF_WIFI_SCAN_LIMIT) {
        index = scan_count - 1u;
        if (rssi <= scan_entries[index].rssi) return 0;
    } else {
        index = scan_count++;
    }
    /* The replaced slot is weaker than the new result, so shifting weaker entries down keeps the table sorted. */
    while (index > 0u && scan_entries[index - 1u].rssi < rssi) {
        scan_entries[index] = scan_entries[index - 1u];
        --index;
    }
    scan_entries[index] = (pf_wifi_scan_entry_t){
        .ssid_length = result->ssid_len,
        .rssi = rssi,
        .security = (uint8_t)(result->auth_mode & 7u),
    };
    memcpy(scan_entries[index].ssid, result->ssid, result->ssid_len);
    return 0;
}

/** @brief Begin an asynchronous saved-network join; retries are paced after explicit failures. */
static void connect_wifi(uint64_t now)
{
    link_owner = PF_WIFI_LINK_SAVED;
    const bool sae = saved_auth_state == PF_WIFI_AUTH_TRY_SAE || saved_auth_state == PF_WIFI_AUTH_SAE;
    const int rc = cyw43_wifi_join(&cyw43_state, wifi_ssid_length, (const uint8_t *)wifi_ssid,
        strlen(wifi_password), (const uint8_t *)wifi_password, auth_for(wifi_password, sae), NULL, CYW43_CHANNEL_NONE);
    wifi_state = rc == 0 ? PF_WIFI_STATE_CONNECTING : PF_WIFI_STATE_FAILED;
    wifi_error = rc == 0 ? PF_WIRELESS_ERROR_NONE : PF_WIRELESS_ERROR_JOIN;
    saved_detail = 0u;
    connect_deadline_us = now + PF_WIFI_CONNECT_TIMEOUT_US;
    if (rc != 0) schedule_retry(now);
    if (rc != 0) PF_LOGGING_ERROR("wifi", "Wi-Fi connection request failed: %d", rc);
}

/** @brief Leave a test-owned link; saved Wi-Fi is restored, or station mode released, once CYW43 reports it down. */
static void leave_test_link(uint64_t now)
{
    if (link_owner != PF_WIFI_LINK_TEST) return;
    if (cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) != 0) {
        leave_retry_us = now + PF_WIFI_LEAVE_RETRY_DELAY_US;
        PF_LOGGING_ERROR("wifi", "Wi-Fi test disconnect request failed");
    } else {
        leave_retry_us = 0u;
    }
    link_owner = PF_WIFI_LINK_NONE;
    after_leave = PF_WIFI_AFTER_LEAVE_RESTORE;
}

/** @brief End a failed test and rejoin the saved network as soon as the test link has disconnected. */
static void fail_test(pf_wifi_detail_t detail, pf_wireless_error_t error, uint64_t now)
{
    command_state = PF_WIFI_COMMAND_FAILED;
    join_detail = detail;
    after_leave = PF_WIFI_AFTER_LEAVE_NONE;
    leave_test_link(now);
    mbedtls_platform_zeroize(test.password, sizeof(test.password));
    test.succeeded = false;
    if (saved_wifi_enabled) {
        wifi_state = PF_WIFI_STATE_FAILED;
        wifi_error = error;
    } else {
        wifi_state = PF_WIFI_STATE_DISABLED;
        wifi_error = PF_WIRELESS_ERROR_NONE;
    }
    retry_us = now;
    release_station();
}

/** @brief Drop temporary credentials and, when requested, rejoin saved Wi-Fi after the test link is down. */
static void drop_test(bool rejoin_saved, uint64_t now)
{
    const bool owned_link = link_owner == PF_WIFI_LINK_TEST;
    leave_test_link(now);
    clear_test_credentials();
    if (owned_link && rejoin_saved && saved_wifi_enabled) {
        wifi_state = PF_WIFI_STATE_FAILED;
        wifi_error = PF_WIRELESS_ERROR_NONE;
        retry_us = now;
        return;
    }
    if (!saved_wifi_enabled) release_station();
}

/** @brief Begin joining the staged test network, failing the test when the driver refuses to start. */
static void start_test_join(uint64_t now)
{
    after_leave = PF_WIFI_AFTER_LEAVE_NONE;
    const int rc = cyw43_wifi_join(&cyw43_state, test.ssid_length, (const uint8_t *)test.ssid,
        strlen(test.password), (const uint8_t *)test.password, auth_for(test.password, test.sae), NULL,
        CYW43_CHANNEL_NONE);
    if (rc != 0) {
        fail_test(PF_WIFI_DETAIL_START_FAILED, PF_WIRELESS_ERROR_JOIN, now);
        PF_LOGGING_ERROR("wifi", "Wi-Fi test connection request failed: %d", rc);
    }
}

/** @brief Finish a scan whose results are already sorted, or fail it at the deadline. */
static void poll_scan(uint64_t now)
{
    if (cyw43_wifi_scan_active(&cyw43_state)) {
        if (now >= scan_deadline_us) {
            scan_state = 3u;
            scan_count = 0u;
            scan_release_pending = true;
            PF_LOGGING_WARNING("wifi", "Wi-Fi scan timed out");
        }
        return;
    }
    scan_state = 2u;
    release_station();
}

/** @brief Advance a credential test through disconnect, association, handshake and DHCP phases. */
static void poll_test(int link, uint64_t now)
{
    if (after_leave == PF_WIFI_AFTER_LEAVE_JOIN_TEST) {
        if (!link_active(link)) start_test_join(now);
        else if (now >= connect_deadline_us) fail_test(PF_WIFI_DETAIL_TIMEOUT, PF_WIRELESS_ERROR_TIMEOUT, now);
        return;
    }
    if (link == CYW43_LINK_UP) {
        command_state = PF_WIFI_COMMAND_TEST_JOINED;
        join_detail = PF_WIFI_DETAIL_NONE;
        test.succeeded = true;
        wifi_state = PF_WIFI_STATE_CONNECTED;
        wifi_error = PF_WIRELESS_ERROR_NONE;
        return;
    }
    const bool associated = (cyw43_state.wifi_join_state & PF_CYW43_JOIN_LINK) != 0u;
    /* A completed handshake reports NOIP, so dropping association while still joining is a failed handshake. */
    if (test.handshake_active && !associated && link == CYW43_LINK_JOIN) ++test.handshake_failures;
    test.handshake_active = associated;
    pf_wifi_detail_t failure = PF_WIFI_DETAIL_NONE;
    if (link == CYW43_LINK_NONET) failure = PF_WIFI_DETAIL_NOT_FOUND;
    else if (link == CYW43_LINK_BADAUTH || test.handshake_failures >= PF_WIFI_HANDSHAKE_FAILURES) {
        failure = PF_WIFI_DETAIL_REJECTED;
    } else if (link < 0) failure = PF_WIFI_DETAIL_TIMEOUT;
    else if (now >= connect_deadline_us) {
        failure = link == CYW43_LINK_NOIP ? PF_WIFI_DETAIL_NO_ADDRESS : PF_WIFI_DETAIL_TIMEOUT;
    }
    if (failure == PF_WIFI_DETAIL_NONE) {
        join_detail = link == CYW43_LINK_NOIP ? PF_WIFI_DETAIL_DHCP
            : associated ? PF_WIFI_DETAIL_PASSWORD : PF_WIFI_DETAIL_SEARCHING;
        return;
    }
    /* A WPA3-only network fails a WPA2 join before authentication; retry once with SAE within the same deadline. */
    if (!test.sae && test.password[0] != '\0' &&
        (failure == PF_WIFI_DETAIL_NOT_FOUND || (failure == PF_WIFI_DETAIL_TIMEOUT && link < 0))) {
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        test.sae = true;
        test.wpa2_failure = failure;
        test.handshake_failures = 0u;
        test.handshake_active = false;
        join_detail = PF_WIFI_DETAIL_SEARCHING;
        after_leave = PF_WIFI_AFTER_LEAVE_JOIN_TEST;
        return;
    }
    if (test.sae && failure == PF_WIFI_DETAIL_NOT_FOUND) failure = test.wpa2_failure;
    fail_test(failure, failure == PF_WIFI_DETAIL_NO_ADDRESS || (failure == PF_WIFI_DETAIL_TIMEOUT && link >= 0)
        ? PF_WIRELESS_ERROR_TIMEOUT : PF_WIRELESS_ERROR_JOIN, now);
}

/** @brief Track the saved or joined-test link and pace saved-network rejoins outside scans. */
static void poll_station(int link, uint64_t now)
{
    if (after_leave == PF_WIFI_AFTER_LEAVE_RESTORE) {
        if (link_active(link)) {
            if (leave_retry_us != 0u && now >= leave_retry_us) {
                if (cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) == 0) {
                    leave_retry_us = 0u;
                } else {
                    leave_retry_us = now + PF_WIFI_LEAVE_RETRY_DELAY_US;
                    PF_LOGGING_ERROR("wifi", "Wi-Fi test disconnect retry failed");
                }
            }
            return;
        }
        if (saved_wifi_enabled && pf_wifi_scan_active()) return;
        after_leave = PF_WIFI_AFTER_LEAVE_NONE;
        leave_retry_us = 0u;
        if (saved_wifi_enabled) connect_wifi(now);
        else release_station();
        return;
    }
    /* A joined result describes a live tested link; its staged credentials stay valid for saving. */
    if (command_state == PF_WIFI_COMMAND_TEST_JOINED && link != CYW43_LINK_UP) {
        command_state = PF_WIFI_COMMAND_FAILED;
        join_detail = PF_WIFI_DETAIL_LINK_LOST;
        link_owner = PF_WIFI_LINK_NONE;
        if (saved_wifi_enabled) {
            wifi_state = PF_WIFI_STATE_FAILED;
            wifi_error = PF_WIRELESS_ERROR_JOIN;
            schedule_retry(now);
        } else {
            wifi_state = PF_WIFI_STATE_DISABLED;
            wifi_error = PF_WIRELESS_ERROR_NONE;
        }
        PF_LOGGING_WARNING("wifi", "Test Wi-Fi link lost");
        return;
    }
    if (link == CYW43_LINK_UP) {
        wifi_state = PF_WIFI_STATE_CONNECTED;
        wifi_error = PF_WIRELESS_ERROR_NONE;
        saved_detail = 0u;
        retry_delay_us = PF_WIFI_RETRY_DELAY_US;
        retry_us = 0u;
        if (link_owner == PF_WIFI_LINK_SAVED) {
            if (saved_auth_state == PF_WIFI_AUTH_TRY_WPA2) saved_auth_state = PF_WIFI_AUTH_WPA2;
            else if (saved_auth_state == PF_WIFI_AUTH_TRY_SAE) saved_auth_state = PF_WIFI_AUTH_SAE;
        }
    } else if (wifi_state == PF_WIFI_STATE_CONNECTING && (link < 0 || now >= connect_deadline_us)) {
        wifi_state = PF_WIFI_STATE_FAILED;
        wifi_error = link < 0 ? PF_WIRELESS_ERROR_JOIN : PF_WIRELESS_ERROR_TIMEOUT;
        saved_detail = link == CYW43_LINK_NONET ? 2u : link == CYW43_LINK_BADAUTH ? 3u :
            link == CYW43_LINK_NOIP ? 4u : 5u;
        schedule_retry(now);
        /* Probe the alternate mode only while the saved mode is unknown, and never for an open network. */
        if (wifi_password[0] != '\0' &&
            (saved_auth_state == PF_WIFI_AUTH_TRY_WPA2 || saved_auth_state == PF_WIFI_AUTH_TRY_SAE) &&
            link != CYW43_LINK_BADAUTH && link != CYW43_LINK_NOIP) {
            saved_auth_state = saved_auth_state == PF_WIFI_AUTH_TRY_WPA2
                ? PF_WIFI_AUTH_TRY_SAE : PF_WIFI_AUTH_TRY_WPA2;
        }
        PF_LOGGING_WARNING("wifi", "Wi-Fi association/DHCP failed: %d", link);
    } else if (wifi_state == PF_WIFI_STATE_CONNECTED) {
        wifi_state = PF_WIFI_STATE_FAILED;
        wifi_error = PF_WIRELESS_ERROR_JOIN;
        saved_detail = 0u;
        if (reconnect_count != UINT16_MAX) ++reconnect_count;
        schedule_retry(now);
        PF_LOGGING_WARNING("wifi", "Wi-Fi link lost");
    } else if (wifi_state == PF_WIFI_STATE_FAILED && saved_wifi_enabled &&
               scan_state != 1u && !cyw43_wifi_scan_active(&cyw43_state) &&
               now >= retry_us) {
        connect_wifi(now);
    }
}

/** @copydoc pf_wifi_scan */
void pf_wifi_scan(void)
{
    cyw43_arch_enable_sta_mode();
    station_enabled = true;
    scan_release_pending = false;
    memset(scan_entries, 0, sizeof(scan_entries));
    scan_count = 0u;
    if (++scan_generation == 0u) ++scan_generation;
    scan_state = 1u;
    scan_deadline_us = time_us_64() + PF_WIFI_SCAN_TIMEOUT_US;
    cyw43_wifi_scan_options_t options = {0};
    if (cyw43_wifi_scan(&cyw43_state, &options, NULL, scan_result) != 0) {
        scan_state = 3u;
        scan_release_pending = true;
        PF_LOGGING_WARNING("wifi", "Wi-Fi scan request failed");
        release_station();
    }
}

/** @copydoc pf_wifi_scan_entry */
uint8_t pf_wifi_scan_entry(uint8_t index, uint16_t generation, uint8_t data[63])
{
    if (generation != scan_generation) return 22u;
    if (scan_state != 2u || index >= scan_count) return 23u;
    const pf_wifi_scan_entry_t *entry = &scan_entries[index];
    data[0] = (uint8_t)scan_generation;
    data[1] = (uint8_t)(scan_generation >> 8u);
    data[2] = index;
    data[3] = entry->ssid_length;
    memcpy(data + 4u, entry->ssid, entry->ssid_length);
    data[36] = (uint8_t)entry->rssi;
    data[37] = entry->security;
    return 0u;
}

/** @copydoc pf_wifi_test */
bool pf_wifi_test(const uint8_t *ssid, uint8_t ssid_length, const char *password)
{
    if (++test_generation == 0u) ++test_generation;
    clear_test_credentials();
    memcpy(test.ssid, ssid, ssid_length);
    test.ssid_length = ssid_length;
    memcpy(test.password, password, strlen(password) + 1u);
    /* The previous link must drop first, or its CYW43_LINK_UP would look like a successful test. */
    const bool wait_for_disconnect = station_enabled &&
        link_active(cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));
    if (wait_for_disconnect && cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) != 0) {
        command_state = PF_WIFI_COMMAND_FAILED;
        join_detail = PF_WIFI_DETAIL_START_FAILED;
        mbedtls_platform_zeroize(test.password, sizeof(test.password));
        if (link_owner == PF_WIFI_LINK_TEST) {
            link_owner = PF_WIFI_LINK_NONE;
            after_leave = PF_WIFI_AFTER_LEAVE_RESTORE;
            leave_retry_us = time_us_64() + PF_WIFI_LEAVE_RETRY_DELAY_US;
        }
        return false;
    }
    cyw43_arch_enable_sta_mode();
    station_enabled = true;
    link_owner = PF_WIFI_LINK_TEST;
    after_leave = wait_for_disconnect ? PF_WIFI_AFTER_LEAVE_JOIN_TEST : PF_WIFI_AFTER_LEAVE_NONE;
    command_state = PF_WIFI_COMMAND_TESTING;
    join_detail = PF_WIFI_DETAIL_SEARCHING;
    wifi_state = PF_WIFI_STATE_CONNECTING;
    const uint64_t now = time_us_64();
    connect_deadline_us = now + PF_WIFI_CONNECT_TIMEOUT_US;
    if (!wait_for_disconnect) start_test_join(now);
    return true;
}

/** @copydoc pf_wifi_cancel_test */
void pf_wifi_cancel_test(bool restore)
{
    const uint64_t now = time_us_64();
    command_state = PF_WIFI_COMMAND_IDLE;
    if (!restore) {
        clear_test_credentials();
        return;
    }
    drop_test(true, now);
    if (!saved_wifi_enabled) wifi_state = PF_WIFI_STATE_DISABLED;
    else if (after_leave == PF_WIFI_AFTER_LEAVE_NONE && wifi_state == PF_WIFI_STATE_FAILED &&
             !pf_wifi_scan_active()) connect_wifi(now);
}

/** @copydoc pf_wifi_prepare */
bool pf_wifi_prepare(void)
{
    wifi_ssid_length = pf_boot_config_wifi_ssid_bytes((uint8_t *)wifi_ssid);
    pf_boot_config_wifi_password(wifi_password);
    saved_auth_state = pf_boot_config_wifi_auth_state();
    reconnect_count = 0u;
    retry_delay_us = PF_WIFI_RETRY_DELAY_US;
    retry_us = 0u;
    rssi_refresh_us = 0u;
    saved_rssi = PF_WIFI_RSSI_UNAVAILABLE;
    saved_detail = 0u;
    saved_wifi_enabled = (pf_boot_config_radios() & PF_WIRELESS_WIFI) != 0u && wifi_ssid_length != 0u;
    return saved_wifi_enabled;
}

/** @copydoc pf_wifi_start */
void pf_wifi_start(void)
{
    cyw43_arch_enable_sta_mode();
    station_enabled = true;
    if (wifi_ssid_length != 0u) connect_wifi(time_us_64());
}

/** @copydoc pf_wifi_poll */
void pf_wifi_poll(uint64_t now)
{
    if (scan_state == 1u) poll_scan(now);
    if (station_enabled) {
        const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (testing()) poll_test(link, now);
        else poll_station(link, now);
    }
    if (pf_wifi_saved_link_up()) {
        if (now >= rssi_refresh_us) {
            int32_t value = 0;
            if (cyw43_wifi_get_rssi(&cyw43_state, &value) == 0) {
                saved_rssi = value > INT8_MAX ? INT8_MAX : value < INT8_MIN ? INT8_MIN : (int8_t)value;
            } else {
                saved_rssi = PF_WIFI_RSSI_UNAVAILABLE;
            }
            rssi_refresh_us = now + PF_WIFI_RSSI_REFRESH_US;
        }
    } else {
        saved_rssi = PF_WIFI_RSSI_UNAVAILABLE;
    }
    if (scan_release_pending && !cyw43_wifi_scan_active(&cyw43_state)) {
        scan_release_pending = false;
        release_station();
    }
}

/** @copydoc pf_wifi_state */
pf_wifi_state_t pf_wifi_state(void)
{
    return wifi_state;
}

/** @copydoc pf_wifi_saved_link_up */
bool pf_wifi_saved_link_up(void)
{
    return saved_wifi_enabled && station_enabled && wifi_state == PF_WIFI_STATE_CONNECTED &&
        link_owner == PF_WIFI_LINK_SAVED;
}

/** @copydoc pf_wifi_error */
pf_wireless_error_t pf_wifi_error(void)
{
    return wifi_error;
}

/** @copydoc pf_wifi_test_active */
bool pf_wifi_test_active(void) { return testing(); }

/** @copydoc pf_wifi_scan_active */
bool pf_wifi_scan_active(void)
{
    return scan_state == 1u || (station_enabled && cyw43_wifi_scan_active(&cyw43_state));
}

/** @copydoc pf_wifi_test_succeeded */
bool pf_wifi_test_succeeded(void) { return test.succeeded; }

/** @copydoc pf_wifi_status */
void pf_wifi_status(uint8_t report[PF_WIRELESS_STATUS_SIZE])
{
    report[2] = 0u;
    memset(report + 5u, 0, 37u);
    report[1] = command_state == PF_WIFI_COMMAND_TESTING ? 4u :
        command_state == PF_WIFI_COMMAND_TEST_JOINED ? 5u :
        command_state == PF_WIFI_COMMAND_FAILED ? 6u : (uint8_t)wifi_state;
    if (command_state == PF_WIFI_COMMAND_FAILED) {
        report[2] = join_detail == PF_WIFI_DETAIL_NOT_FOUND ? 2u :
            join_detail == PF_WIFI_DETAIL_REJECTED ? 3u :
            join_detail == PF_WIFI_DETAIL_NO_ADDRESS ? 4u :
            join_detail == PF_WIFI_DETAIL_LINK_LOST ? 6u : 5u;
    } else if (command_state == PF_WIFI_COMMAND_IDLE) {
        report[2] = saved_detail;
    }
    if (station_enabled && cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
        pf_wifi_ip_address(report + 5u);
    }
    const bool candidate = test.ssid_length != 0u;
    report[9] = candidate ? test.ssid_length : wifi_ssid_length;
    memcpy(report + 10u, candidate ? test.ssid : wifi_ssid, report[9]);
    report[42] = scan_state;
    report[43] = (uint8_t)scan_generation;
    report[44] = (uint8_t)(scan_generation >> 8u);
    report[45] = scan_state == 2u ? scan_count : 0u;
    report[46] = test_generation;
}

/** @copydoc pf_wifi_tested_auth_state */
pf_wifi_auth_state_t pf_wifi_tested_auth_state(void)
{
    return test.sae ? PF_WIFI_AUTH_SAE : PF_WIFI_AUTH_WPA2;
}

/** @copydoc pf_wifi_link_diagnostics */
void pf_wifi_link_diagnostics(pf_wifi_auth_state_t *auth_state, uint16_t *reconnects, int8_t *rssi)
{
    if (auth_state != NULL) *auth_state = saved_auth_state;
    if (reconnects != NULL) *reconnects = reconnect_count;
    if (rssi != NULL) {
        *rssi = PF_WIFI_RSSI_UNAVAILABLE;
        if (pf_wifi_saved_link_up()) *rssi = saved_rssi;
    }
}

/** @copydoc pf_wifi_ip_address */
void pf_wifi_ip_address(uint8_t address[4])
{
    memcpy(address, &netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])->addr, 4u);
}
