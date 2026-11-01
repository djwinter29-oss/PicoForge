/**
 * @file wifi.c
 * @brief Asynchronous WPA2 station connection, scans, temporary credential tests and paced reconnects.
 */
#include "app/wireless/wifi/wifi.h"

#include <string.h>

#include "cyw43.h"
#include "pico/cyw43_arch.h"
#include "app/wireless/wireless.h"
#include "platform/boot_config.h"
#include "platform/logging.h"

#define PF_WIFI_SCAN_LIMIT 18u
#define PF_WIFI_SCAN_PAGE_SIZE 6u
/* ponytail: cap results at 18 (three report pages) to keep static RAM bounded on RP2040; raise with a paged
 * protocol. */
/* ponytail: mirrors the pinned cyw43-driver's private "associated" join bit; the driver retries a WPA2 handshake
 * with a wrong passphrase silently instead of reporting BADAUTH. Recheck this bit when updating the driver. */
#define PF_CYW43_JOIN_LINK 0x0400u
#define PF_WIFI_HANDSHAKE_FAILURES 2u
#define PF_WIFI_CONNECT_TIMEOUT_US 30000000u /**< Association plus DHCP deadline. */
#define PF_WIFI_RETRY_DELAY_US 5000000u /**< Pause before rejoining the saved network after a failure. */
#define PF_WIFI_SCAN_TIMEOUT_US 10000000u /**< Scan deadline; the driver has no completion timeout. */
#define PF_WIFI_SSID_MAX 32u
#define PF_WIFI_PASSWORD_MAX 63u

/** @brief Feature 34 command payload byte offsets. */
enum {
    COMMAND_VERSION = 0, /**< Payload version, always 1. */
    COMMAND_OPERATION = 1, /**< pf_wifi_operation_t. */
    COMMAND_TRANSACTION = PF_WIRELESS_COMMAND_TRANSACTION_OFFSET, /**< Nonzero host transaction. */
    COMMAND_PAGE = 3, /**< Result page for page selection; zero otherwise. */
    COMMAND_SSID_LENGTH = 4, /**< Test SSID byte count. */
    COMMAND_PASSWORD_LENGTH = 5, /**< Test passphrase byte count. */
    COMMAND_SSID = 6, /**< 32 SSID bytes, zero padded. */
    COMMAND_PASSWORD = 38, /**< 63 passphrase bytes, zero padded. */
};

/** @brief Feature 35 result payload byte offsets. */
enum {
    RESULT_VERSION = 0, /**< Payload version, always 1. */
    RESULT_TRANSACTION = 1, /**< Transaction the result belongs to. */
    RESULT_STATE = 2, /**< pf_wifi_command_state_t. */
    RESULT_DETAIL = 3, /**< Scan page in states 0-3; pf_wifi_detail_t in states 4-6. */
    RESULT_COUNT = 4, /**< Retained scan result count. */
    RESULT_WIFI_STATE = 5, /**< pf_wifi_state_t. */
    RESULT_ENTRIES = 6, /**< Six 35-byte entries: SSID length, SSID[32], RSSI, secured. */
    RESULT_ENTRY_SIZE = 35, /**< Bytes per scan entry. */
    RESULT_ADDRESS = 216, /**< IPv4 address while a test is joined. */
};

/** @brief Feature 35 byte 2 scan/test state. */
typedef enum {
    PF_WIFI_COMMAND_IDLE = 0, /**< No scan or test result. */
    PF_WIFI_COMMAND_SCANNING = 1, /**< Scan in progress. */
    PF_WIFI_COMMAND_SCAN_DONE = 2, /**< Sorted scan results are available. */
    PF_WIFI_COMMAND_SCAN_FAILED = 3, /**< The driver refused or did not finish the scan. */
    PF_WIFI_COMMAND_TESTING = 4, /**< Test credentials are joining. */
    PF_WIFI_COMMAND_TEST_JOINED = 5, /**< Test credentials joined and are staged for saving. */
    PF_WIFI_COMMAND_FAILED = 6, /**< Test failed or the command was rejected. */
} pf_wifi_command_state_t;

/** @brief Feature 35 byte 3 during connection-test states 4-6. */
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
} pf_wifi_detail_t;

/** @brief Sanitized access point data retained between report reads. */
typedef struct {
    uint8_t ssid_length; /**< UTF-8 SSID byte count. */
    uint8_t ssid[PF_WIFI_SSID_MAX]; /**< SSID; never contains credentials. */
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
static uint64_t scan_deadline_us; /**< Deadline for the active scan. */
static uint8_t wifi_state; /**< pf_wifi_state_t. */
static uint8_t wifi_error; /**< pf_wireless_error_t for the latest join or test. */
static uint8_t command_transaction; /**< Latest accepted scan/test transaction. */
static uint8_t command_state; /**< pf_wifi_command_state_t. */
static uint8_t rejected_transaction; /**< One-shot rejection result, separate from accepted operation state. */
static uint8_t scan_count; /**< Number of retained results, at most PF_WIFI_SCAN_LIMIT. */
static uint8_t scan_page; /**< Selected result page. */
static uint8_t join_detail; /**< pf_wifi_detail_t reported while a test is joining, joined or failed. */
static uint8_t handshake_failures; /**< Handshakes dropped back to association during the current test. */
static bool handshake_active; /**< Current test was associated at the previous poll. */
static bool on_test_network; /**< The station last joined test credentials rather than the saved network. */
static bool test_waiting_for_disconnect; /**< Wait for the previous station link to drop before testing credentials. */
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

/** @brief Return whether the station is joining, associated or addressed. */
static bool link_active(int link)
{
    return link == CYW43_LINK_UP || link == CYW43_LINK_NOIP || link == CYW43_LINK_JOIN;
}

/* ponytail: WPA2-PSK only. In the pinned cyw43-driver, both WPA3 join modes set SAE-only authentication,
 * scan results cannot tell WPA3 from WPA2, and keys are always sent as passphrases, so a 64-hex-digit key is not
 * used as a raw PSK. WPA2/WPA3 transition networks still join. Upgrade path: persist a per-network auth mode,
 * then qualify SAE on hardware. */
/** @brief Select open or WPA2 authentication from the passphrase. */
static uint32_t auth_for(const char *password)
{
    return password[0] == '\0' ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK;
}

/** @brief Return whether a temporary credential test is joining. */
static bool testing(void)
{
    return command_state == PF_WIFI_COMMAND_TESTING;
}

/** @brief Drop station mode when saved Wi-Fi is disabled and no scan or test result still needs it. */
static void release_station(void)
{
    if (saved_wifi_enabled || testing() || test_succeeded || command_state == PF_WIFI_COMMAND_SCANNING) return;
    cyw43_arch_disable_sta_mode();
}

/** @brief Wipe temporary test credentials. */
static void clear_test_credentials(void)
{
    memset(test_ssid, 0, sizeof(test_ssid));
    memset(test_password, 0, sizeof(test_password));
}

/** @brief Retain the strongest non-hidden APs, replacing the weakest when the bounded table is full. */
static int scan_result(void *environment, const cyw43_ev_scan_result_t *result)
{
    (void)environment;
    if (command_state != PF_WIFI_COMMAND_SCANNING || result == NULL || result->ssid_len == 0u ||
        result->ssid_len > sizeof(result->ssid)) return 0;
    const int8_t rssi = result->rssi > INT8_MAX ? INT8_MAX
        : result->rssi < INT8_MIN ? INT8_MIN : (int8_t)result->rssi;
    uint8_t index = 0u;
    while (index < scan_count && (scan_entries[index].ssid_length != result->ssid_len ||
           memcmp(scan_entries[index].ssid, result->ssid, result->ssid_len) != 0)) ++index;
    if (index == scan_count) {
        if (scan_count < PF_WIFI_SCAN_LIMIT) {
            index = scan_count++;
        } else {
            index = 0u;
            for (uint8_t candidate = 1u; candidate < scan_count; ++candidate) {
                if (scan_entries[candidate].rssi < scan_entries[index].rssi) index = candidate;
            }
            if (rssi <= scan_entries[index].rssi) return 0;
        }
        scan_entries[index].ssid_length = result->ssid_len;
        memcpy(scan_entries[index].ssid, result->ssid, result->ssid_len);
    } else if (rssi <= scan_entries[index].rssi) {
        return 0;
    }
    scan_entries[index].rssi = rssi;
    scan_entries[index].secured = result->auth_mode != CYW43_AUTH_OPEN;
    return 0;
}

/** @brief Begin an asynchronous saved-network join; retries are paced after explicit failures. */
static void connect_wifi(void)
{
    on_test_network = false;
    const int rc = cyw43_arch_wifi_connect_async(wifi_ssid, wifi_password, auth_for(wifi_password));
    const uint64_t now = time_us_64();
    wifi_state = rc == 0 ? PF_WIFI_STATE_CONNECTING : PF_WIFI_STATE_FAILED;
    wifi_error = rc == 0 ? PF_WIRELESS_ERROR_NONE : PF_WIRELESS_ERROR_JOIN;
    connect_deadline_us = now + PF_WIFI_CONNECT_TIMEOUT_US;
    retry_us = now + PF_WIFI_RETRY_DELAY_US;
    if (rc != 0) PF_LOGGING_ERROR("wifi", "Wi-Fi connection request failed: %d", rc);
}

/** @brief End the active test as failed and drop its credentials; the saved network rejoins after the retry delay. */
static void fail_test(uint8_t detail, uint8_t error)
{
    clear_test_credentials();
    test_waiting_for_disconnect = false;
    command_state = PF_WIFI_COMMAND_FAILED;
    join_detail = detail;
    wifi_state = PF_WIFI_STATE_FAILED;
    wifi_error = error;
    retry_us = time_us_64() + PF_WIFI_RETRY_DELAY_US;
    release_station();
}

/** @brief Begin joining the staged test network, failing the test when the driver refuses to start. */
static void start_test_join(void)
{
    test_waiting_for_disconnect = false;
    const int rc = cyw43_arch_wifi_connect_async(test_ssid, test_password, auth_for(test_password));
    if (rc != 0) {
        fail_test(PF_WIFI_DETAIL_START_FAILED, PF_WIRELESS_ERROR_JOIN);
        PF_LOGGING_ERROR("wifi", "Wi-Fi test connection request failed: %d", rc);
    }
}

/** @brief Finish a scan by sorting retained results strongest first, or fail it at the deadline. */
static void poll_scan(uint64_t now)
{
    if (cyw43_wifi_scan_active(&cyw43_state)) {
        if (now >= scan_deadline_us) {
            command_state = PF_WIFI_COMMAND_SCAN_FAILED;
            PF_LOGGING_WARNING("wifi", "Wi-Fi scan timed out");
        }
        return;
    }
    for (uint8_t i = 1u; i < scan_count; ++i) {
        const pf_wifi_scan_entry_t current = scan_entries[i];
        uint8_t j = i;
        while (j > 0u && scan_entries[j - 1u].rssi < current.rssi) {
            scan_entries[j] = scan_entries[j - 1u];
            --j;
        }
        scan_entries[j] = current;
    }
    command_state = PF_WIFI_COMMAND_SCAN_DONE;
    release_station();
}

/** @brief Advance a credential test through disconnect, association, handshake and DHCP phases. */
static void poll_test(int link, uint64_t now)
{
    if (test_waiting_for_disconnect) {
        if (!link_active(link)) start_test_join();
        else if (now >= connect_deadline_us) fail_test(PF_WIFI_DETAIL_TIMEOUT, PF_WIRELESS_ERROR_TIMEOUT);
        return;
    }
    if (link == CYW43_LINK_UP) {
        command_state = PF_WIFI_COMMAND_TEST_JOINED;
        join_detail = PF_WIFI_DETAIL_NONE;
        test_succeeded = true;
        wifi_state = PF_WIFI_STATE_CONNECTED;
        wifi_error = PF_WIRELESS_ERROR_NONE;
        return;
    }
    const bool associated = (cyw43_state.wifi_join_state & PF_CYW43_JOIN_LINK) != 0u;
    /* A completed handshake reports NOIP, so dropping association while still joining is a failed handshake. */
    if (handshake_active && !associated && link == CYW43_LINK_JOIN) ++handshake_failures;
    handshake_active = associated;
    const uint8_t failure = link == CYW43_LINK_NONET ? PF_WIFI_DETAIL_NOT_FOUND
        : link == CYW43_LINK_BADAUTH || handshake_failures >= PF_WIFI_HANDSHAKE_FAILURES
            ? PF_WIFI_DETAIL_REJECTED
        : link < 0 ? PF_WIFI_DETAIL_TIMEOUT
        : now < connect_deadline_us ? PF_WIFI_DETAIL_NONE
        : link == CYW43_LINK_NOIP ? PF_WIFI_DETAIL_NO_ADDRESS : PF_WIFI_DETAIL_TIMEOUT;
    if (failure == PF_WIFI_DETAIL_NONE) {
        join_detail = link == CYW43_LINK_NOIP ? PF_WIFI_DETAIL_DHCP
            : associated ? PF_WIFI_DETAIL_PASSWORD : PF_WIFI_DETAIL_SEARCHING;
        return;
    }
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    fail_test(failure, failure == PF_WIFI_DETAIL_NO_ADDRESS || (failure == PF_WIFI_DETAIL_TIMEOUT && link >= 0)
        ? PF_WIRELESS_ERROR_TIMEOUT : PF_WIRELESS_ERROR_JOIN);
}

/** @brief Track the saved or joined-test link and pace saved-network rejoins outside scans. */
static void poll_station(int link, uint64_t now)
{
    /* A joined result describes a live tested link; its staged credentials stay valid for saving. */
    if (command_state == PF_WIFI_COMMAND_TEST_JOINED && link != CYW43_LINK_UP) command_state = PF_WIFI_COMMAND_IDLE;
    if (link == CYW43_LINK_UP) {
        wifi_state = PF_WIFI_STATE_CONNECTED;
        wifi_error = PF_WIRELESS_ERROR_NONE;
    } else if (wifi_state == PF_WIFI_STATE_CONNECTING && (link < 0 || now >= connect_deadline_us)) {
        wifi_state = PF_WIFI_STATE_FAILED;
        wifi_error = link < 0 ? PF_WIRELESS_ERROR_JOIN : PF_WIRELESS_ERROR_TIMEOUT;
        retry_us = now + PF_WIFI_RETRY_DELAY_US;
        PF_LOGGING_WARNING("wifi", "Wi-Fi association/DHCP failed: %d", link);
    } else if (wifi_state == PF_WIFI_STATE_CONNECTED) {
        wifi_state = PF_WIFI_STATE_FAILED;
        retry_us = now + PF_WIFI_RETRY_DELAY_US;
        PF_LOGGING_WARNING("wifi", "Wi-Fi link lost");
    } else if (wifi_state == PF_WIFI_STATE_FAILED && saved_wifi_enabled &&
               command_state != PF_WIFI_COMMAND_SCANNING && now >= retry_us) {
        connect_wifi();
    }
}

/** @brief Start a scan when no test or driver scan is active. */
static bool start_scan(const uint8_t *report)
{
    if (!bytes_zero(report + COMMAND_PAGE, PF_WIRELESS_COMMAND_SIZE - COMMAND_PAGE) || testing() ||
        cyw43_wifi_scan_active(&cyw43_state)) return false;
    cyw43_arch_enable_sta_mode();
    memset(scan_entries, 0, sizeof(scan_entries));
    scan_count = 0u;
    scan_page = 0u;
    command_state = PF_WIFI_COMMAND_SCANNING;
    scan_deadline_us = time_us_64() + PF_WIFI_SCAN_TIMEOUT_US;
    cyw43_wifi_scan_options_t options = {0};
    if (cyw43_wifi_scan(&cyw43_state, &options, NULL, scan_result) != 0) command_state = PF_WIFI_COMMAND_SCAN_FAILED;
    return true;
}

/** @brief Select a populated page of completed scan results. */
static bool select_page(const uint8_t *report)
{
    const uint8_t page = report[COMMAND_PAGE];
    if (command_state != PF_WIFI_COMMAND_SCAN_DONE || page * PF_WIFI_SCAN_PAGE_SIZE >= scan_count ||
        !bytes_zero(report + COMMAND_SSID_LENGTH, PF_WIRELESS_COMMAND_SIZE - COMMAND_SSID_LENGTH)) return false;
    scan_page = page;
    return true;
}

/** @brief Validate and stage test credentials, leaving any live link before joining them. */
static bool start_test(const uint8_t *report)
{
    const uint8_t ssid_length = report[COMMAND_SSID_LENGTH];
    const uint8_t password_length = report[COMMAND_PASSWORD_LENGTH];
    if (report[COMMAND_PAGE] != 0u || command_state == PF_WIFI_COMMAND_SCANNING || testing() ||
        cyw43_wifi_scan_active(&cyw43_state) || ssid_length == 0u || ssid_length > PF_WIFI_SSID_MAX ||
        password_length > PF_WIFI_PASSWORD_MAX || (password_length != 0u && password_length < 8u) ||
        memchr(report + COMMAND_SSID, '\0', ssid_length) != NULL ||
        !bytes_zero(report + COMMAND_SSID + ssid_length, PF_WIFI_SSID_MAX - ssid_length) ||
        !bytes_zero(report + COMMAND_PASSWORD + password_length, PF_WIFI_PASSWORD_MAX - password_length)) {
        return false;
    }
    for (size_t i = 0u; i < password_length; ++i) {
        const uint8_t character = report[COMMAND_PASSWORD + i];
        if (character < 32u || character > 126u) return false;
    }
    cyw43_arch_enable_sta_mode();
    /* The previous link must drop first, or its CYW43_LINK_UP would look like a successful test. */
    const bool wait_for_disconnect = link_active(cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));
    if (wait_for_disconnect && cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) != 0) return false;
    clear_test_credentials();
    memcpy(test_ssid, report + COMMAND_SSID, ssid_length);
    memcpy(test_password, report + COMMAND_PASSWORD, password_length);
    test_succeeded = false;
    on_test_network = true;
    test_waiting_for_disconnect = wait_for_disconnect;
    command_state = PF_WIFI_COMMAND_TESTING;
    join_detail = PF_WIFI_DETAIL_SEARCHING;
    handshake_failures = 0u;
    handshake_active = false;
    wifi_state = PF_WIFI_STATE_CONNECTING;
    connect_deadline_us = time_us_64() + PF_WIFI_CONNECT_TIMEOUT_US;
    if (!wait_for_disconnect) start_test_join();
    return true;
}

/** @brief Drop a staged or running test; rejoin the saved network only when the test owned the link. */
static bool clear_test(const uint8_t *report)
{
    if (!bytes_zero(report + COMMAND_PAGE, PF_WIRELESS_COMMAND_SIZE - COMMAND_PAGE) ||
        command_state == PF_WIFI_COMMAND_SCANNING) return false;
    const bool owned_link = testing() || test_succeeded;
    if (owned_link) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    test_waiting_for_disconnect = false;
    test_succeeded = false;
    clear_test_credentials();
    command_state = PF_WIFI_COMMAND_IDLE;
    if (!saved_wifi_enabled) wifi_state = PF_WIFI_STATE_DISABLED;
    else if (owned_link) connect_wifi();
    release_station();
    return true;
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
void pf_wifi_start(void)
{
    cyw43_arch_enable_sta_mode();
    if (wifi_ssid[0] != '\0') connect_wifi();
}

/** @copydoc pf_wifi_poll */
void pf_wifi_poll(void)
{
    const uint64_t now = time_us_64();
    const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (command_state == PF_WIFI_COMMAND_SCANNING) poll_scan(now);
    if (testing()) poll_test(link, now);
    else poll_station(link, now);
}

/** @copydoc pf_wifi_state */
uint8_t pf_wifi_state(void)
{
    return wifi_state;
}

/** @copydoc pf_wifi_saved_link_up */
bool pf_wifi_saved_link_up(void)
{
    return wifi_state == PF_WIFI_STATE_CONNECTED && !on_test_network;
}

/** @copydoc pf_wifi_error */
uint8_t pf_wifi_error(void)
{
    return wifi_error;
}

/** @copydoc pf_wifi_command */
bool pf_wifi_command(const uint8_t report[PF_WIRELESS_COMMAND_SIZE])
{
    const uint8_t transaction = report[COMMAND_TRANSACTION];
    if (transaction == 0u || report[COMMAND_VERSION] != 1u) return false;
    bool accepted;
    switch (report[COMMAND_OPERATION]) {
    case PF_WIFI_OPERATION_SCAN: accepted = start_scan(report); break;
    case PF_WIFI_OPERATION_PAGE: accepted = select_page(report); break;
    case PF_WIFI_OPERATION_TEST: accepted = start_test(report); break;
    case PF_WIFI_OPERATION_CLEAR: accepted = clear_test(report); break;
    default: accepted = false; break;
    }
    if (accepted) command_transaction = transaction;
    return accepted;
}

/** @copydoc pf_wifi_reject_command */
void pf_wifi_reject_command(uint8_t transaction)
{
    const bool running = testing() || command_state == PF_WIFI_COMMAND_SCANNING;
    if (transaction == 0u || (running && transaction == command_transaction)) return;
    rejected_transaction = transaction;
}

/** @copydoc pf_wifi_result */
void pf_wifi_result(uint8_t report[PF_WIRELESS_RESULT_SIZE])
{
    memset(report, 0, PF_WIRELESS_RESULT_SIZE);
    report[RESULT_VERSION] = 1u;
    report[RESULT_WIFI_STATE] = wifi_state;
    if (rejected_transaction != 0u) {
        report[RESULT_TRANSACTION] = rejected_transaction;
        report[RESULT_STATE] = PF_WIFI_COMMAND_FAILED;
        rejected_transaction = 0u;
        return;
    }
    report[RESULT_TRANSACTION] = command_transaction;
    report[RESULT_STATE] = command_state;
    report[RESULT_DETAIL] = command_state >= PF_WIFI_COMMAND_TESTING ? join_detail : scan_page;
    report[RESULT_COUNT] = scan_count;
    const uint8_t start = (uint8_t)(scan_page * PF_WIFI_SCAN_PAGE_SIZE);
    for (uint8_t i = 0u; i < PF_WIFI_SCAN_PAGE_SIZE && start + i < scan_count; ++i) {
        const pf_wifi_scan_entry_t *entry = &scan_entries[start + i];
        uint8_t *item = report + RESULT_ENTRIES + (uint16_t)i * RESULT_ENTRY_SIZE;
        item[0] = entry->ssid_length;
        memcpy(item + 1u, entry->ssid, entry->ssid_length);
        item[1u + PF_WIFI_SSID_MAX] = (uint8_t)entry->rssi;
        item[2u + PF_WIFI_SSID_MAX] = entry->secured;
    }
    if (command_state == PF_WIFI_COMMAND_TEST_JOINED) pf_wifi_ip_address(report + RESULT_ADDRESS);
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
    clear_test_credentials();
    test_succeeded = false;
    command_state = PF_WIFI_COMMAND_IDLE;
}

/** @copydoc pf_wifi_ip_address */
void pf_wifi_ip_address(uint8_t address[4])
{
    memcpy(address, &netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])->addr, 4u);
}
