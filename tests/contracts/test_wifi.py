"""Exercise the production Wi-Fi state machine without radio hardware."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_lwip_heap_is_word_aligned():
    options = (ROOT / "firmware/src/integration/wireless/lwipopts.h").read_text()
    assert "#define MEM_ALIGNMENT 4" in options


def test_wifi_boot_gating_deadline_retries_and_ipv4(tmp_path):
    pico = tmp_path / "pico"
    pico.mkdir()
    (pico / "cyw43_arch.h").write_text("""
#ifndef CYW43_ARCH_H
#define CYW43_ARCH_H
#include <stdint.h>
#include <stdbool.h>
#define CYW43_AUTH_OPEN 0u
#define CYW43_AUTH_WPA2_AES_PSK 0x00400004u
#define CYW43_AUTH_WPA2_MIXED_PSK 0x00400006u
#define CYW43_AUTH_WPA3_SAE_AES_PSK 0x01000004u
#define CYW43_ITF_STA 0
#define CYW43_LINK_DOWN 0
#define CYW43_LINK_JOIN 1
#define CYW43_LINK_NOIP 2
#define CYW43_LINK_UP 3
#define CYW43_LINK_NONET (-2)
#define CYW43_LINK_BADAUTH (-3)
typedef struct { uint8_t unused; } cyw43_wifi_scan_options_t;
typedef struct {
    uint8_t ssid_len;
    uint8_t ssid[32];
    int16_t rssi;
    uint8_t auth_mode;
} cyw43_ev_scan_result_t;
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip; } netif_t;
typedef struct { netif_t netif[1]; uint32_t wifi_join_state; } cyw43_t;
typedef int (*scan_result_cb_t)(void *, const cyw43_ev_scan_result_t *);
extern cyw43_t cyw43_state;
bool cyw43_wifi_scan_active(cyw43_t *);
int cyw43_wifi_scan(cyw43_t *, cyw43_wifi_scan_options_t *, void *, scan_result_cb_t);
int cyw43_wifi_leave(cyw43_t *, int);
#define netif_ip4_addr(netif) (&(netif)->ip)
uint64_t time_us_64(void);
void cyw43_arch_enable_sta_mode(void);
void cyw43_arch_disable_sta_mode(void);
int cyw43_arch_wifi_connect_async(const char *, const char *, uint32_t);
int cyw43_tcpip_link_status(cyw43_t *, int);
int cyw43_wifi_get_rssi(cyw43_t *, int32_t *);
#endif
""")
    (tmp_path / "cyw43.h").write_text('#include "pico/cyw43_arch.h"\n')
    harness = tmp_path / "wifi.c"
    harness.write_text("""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app/wireless/wifi/wifi.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "pico/cyw43_arch.h"
cyw43_t cyw43_state;
static uint64_t now;
static unsigned joins, sta_calls, logs;
static int join_result, link;
static uint8_t radios = PF_WIRELESS_WIFI;
static pf_wifi_auth_state_t configured_auth = PF_WIFI_AUTH_TRY_WPA2;
static int32_t current_rssi = -47;
static const char *ssid = "network", *password = "test-password";
static const char *expected_ssid = "network", *expected_password = "test-password";
static uint32_t expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
static bool scan_active;
static unsigned leaves;
static int scan_status;
static scan_result_cb_t scan_callback;
uint8_t pf_boot_config_radios(void) { return radios; }
pf_wifi_auth_state_t pf_boot_config_wifi_auth_state(void) { return configured_auth; }
void pf_boot_config_wifi_ssid(char out[33]) { strcpy(out, ssid); }
void pf_boot_config_wifi_password(char out[64]) { strcpy(out, password); }
uint64_t time_us_64(void) { return now; }
void cyw43_arch_enable_sta_mode(void) { ++sta_calls; }
void pf_wifi_mdns_poll(bool saved_link_up) { (void)saved_link_up; }
static unsigned sta_releases;
void cyw43_arch_disable_sta_mode(void) { ++sta_releases; link = CYW43_LINK_DOWN; }
int cyw43_arch_wifi_connect_async(const char *s, const char *p, uint32_t auth) {
    assert(strcmp(s, expected_ssid) == 0 && strcmp(p, expected_password) == 0);
    assert(auth == expected_auth);
    ++joins;
    return join_result;
}
bool cyw43_wifi_scan_active(cyw43_t *state) { assert(state == &cyw43_state); return scan_active; }
int cyw43_wifi_scan(cyw43_t *state, cyw43_wifi_scan_options_t *options, void *environment,
                    scan_result_cb_t callback) {
    assert(state == &cyw43_state && options != NULL && environment == NULL);
    scan_callback = callback;
    scan_active = scan_status == 0;
    return scan_status;
}
int cyw43_wifi_leave(cyw43_t *state, int itf) {
    assert(state == &cyw43_state && itf == CYW43_ITF_STA);
    ++leaves;
    return 0;
}
int cyw43_tcpip_link_status(cyw43_t *state, int itf) {
    assert(state == &cyw43_state && itf == CYW43_ITF_STA);
    return link;
}
int cyw43_wifi_get_rssi(cyw43_t *state, int32_t *rssi) {
    assert(state == &cyw43_state);
    *rssi = current_rssi;
    return 0;
}
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...) {
    (void)level; (void)format;
    assert(strcmp(tag, "wifi") == 0);
    ++logs;
    return true;
}
int main(void) {
    uint8_t address[4], expected[4] = {192, 168, 1, 42};
    assert(pf_wifi_state() == 0);
    radios = PF_WIRELESS_BLE;
    assert(!pf_wifi_prepare());
    radios = PF_WIRELESS_WIFI;
    ssid = "";
    assert(!pf_wifi_prepare());
    ssid = "network";
    password = "";
    assert(pf_wifi_prepare());
    password = "test-password";
    assert(pf_wifi_prepare() && joins == 0 && sta_calls == 0);
    pf_wifi_start();
    assert(joins == 1 && sta_calls == 1 && pf_wifi_state() == 1 && pf_wifi_error() == 0);
    now = 29999999;
    pf_wifi_poll();
    assert(pf_wifi_state() == 1 && joins == 1);
    now = 30000000;
    pf_wifi_poll();
    assert(pf_wifi_state() == 3 && pf_wifi_error() == 3 && logs == 1);
    now = 34999999;
    pf_wifi_poll();
    assert(joins == 1);
    /* No authentication progress may mean a WPA3-only network, so the saved rejoin alternates to SAE. */
    expected_auth = CYW43_AUTH_WPA3_SAE_AES_PSK;
    now = 35000000;
    pf_wifi_poll();
    assert(joins == 2 && pf_wifi_state() == 1 && pf_wifi_error() == 0);
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    assert(pf_wifi_state() == 2 && pf_wifi_error() == 0 && pf_wifi_saved_link_up());
    memcpy(&cyw43_state.netif[0].ip.addr, expected, sizeof(expected));
    pf_wifi_ip_address(address);
    assert(memcmp(address, expected, sizeof(expected)) == 0);
    link = 0;
    pf_wifi_poll();
    assert(pf_wifi_state() == 3 && logs == 2);
    now += 4999999;
    pf_wifi_poll();
    assert(joins == 2);
    ++now;
    join_result = -1;
    pf_wifi_poll();
    assert(joins == 3 && pf_wifi_state() == 3 && pf_wifi_error() == 2 && logs == 3);
    now += 10000000;
    join_result = 0;
    pf_wifi_poll();
    assert(joins == 4 && pf_wifi_state() == 1);
    link = -1;
    pf_wifi_poll();
    assert(pf_wifi_state() == 3 && pf_wifi_error() == 2 && logs == 4);

    uint8_t command[101] = {1, 3, 10};
    expected_ssid = "tested-network";
    expected_password = "test-pass";
    expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
    link = 0;
    command[4] = (uint8_t)strlen(expected_ssid);
    command[5] = (uint8_t)strlen(expected_password);
    memcpy(command + 6, expected_ssid, strlen(expected_ssid));
    memcpy(command + 38, expected_password, strlen(expected_password));
    assert(pf_wifi_command(command));
    assert(pf_wifi_state() == 1);
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    /* A joined test network is never the trusted saved network that the TCP CLI may serve. */
    assert(pf_wifi_state() == 2 && !pf_wifi_saved_link_up());
    pf_wifi_auth_state_t saved_auth;
    pf_wifi_link_diagnostics(&saved_auth, NULL, NULL);
    assert(saved_auth == PF_WIFI_AUTH_SAE);
    char tested_ssid[33], tested_password[64];
    assert(pf_wifi_tested_credentials(tested_ssid, tested_password));
    assert(strcmp(tested_ssid, expected_ssid) == 0 && strcmp(tested_password, expected_password) == 0);

    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 1; command[2] = 11;
    assert(pf_wifi_command(command) && scan_active && scan_callback != NULL);
    cyw43_ev_scan_result_t ap = {0};
    memcpy(ap.ssid, "nearby", 6); ap.ssid_len = 6; ap.rssi = -50; ap.auth_mode = 1;
    scan_callback(NULL, &ap);
    ap.rssi = -35;
    scan_callback(NULL, &ap);
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.ssid, "open", 4); ap.ssid_len = 4; ap.rssi = -45; ap.auth_mode = CYW43_AUTH_OPEN;
    scan_callback(NULL, &ap);
    scan_active = false;
    pf_wifi_poll();
    uint8_t result[PF_WIRELESS_RESULT_SIZE];
    pf_wifi_result(result);
    assert(result[1] == 11 && result[2] == 2 && result[4] == 2 && result[5] == 2);
    assert(result[6] == 6 && memcmp(result + 7, "nearby", 6) == 0);
    assert((int8_t)result[39] == -35 && result[40] == 1);

    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 2; command[2] = 12;
    assert(pf_wifi_command(command));
    pf_wifi_result(result);
    assert(result[1] == 12 && result[2] == 2 && result[3] == 0 && result[4] == 2);
    assert(pf_wifi_tested_credentials(tested_ssid, tested_password));

    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 1; command[2] = 19;
    assert(pf_wifi_command(command));
    for (unsigned index = 0u; index < 19u; ++index) {
        memset(&ap, 0, sizeof(ap));
        if (index == 18u) {
            memcpy(ap.ssid, "strong", 6u);
            ap.ssid_len = 6u;
            ap.rssi = -10;
        } else {
            (void)snprintf((char *)ap.ssid, sizeof(ap.ssid), "ap%02u", index);
            ap.ssid_len = 4u;
            ap.rssi = -90;
        }
        ap.auth_mode = CYW43_AUTH_OPEN;
        scan_callback(NULL, &ap);
    }
    scan_active = false;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 2 && result[4] == 18);
    assert(result[6] == 6 && memcmp(result + 7, "strong", 6u) == 0);
    assert((int8_t)result[39] == -10);

    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 4; command[2] = 13;
    expected_ssid = "network";
    expected_password = "test-password";
    expected_auth = CYW43_AUTH_WPA3_SAE_AES_PSK;
    assert(pf_wifi_command(command));
    assert(leaves == 1 && joins == 5 && pf_wifi_state() == 3);
    link = CYW43_LINK_DOWN;
    pf_wifi_poll();
    assert(joins == 6 && pf_wifi_state() == 1);
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));

    password = "";
    expected_password = "";
    expected_auth = CYW43_AUTH_OPEN;
    assert(pf_wifi_prepare());
    unsigned before_open_join = joins;
    pf_wifi_start();
    assert(joins == before_open_join + 1 && pf_wifi_state() == 1);

    radios = PF_WIRELESS_BLE;
    assert(!pf_wifi_prepare());
    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 3; command[2] = 14;
    expected_ssid = "missing";
    expected_password = "missing-pass";
    expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
    command[4] = (uint8_t)strlen(expected_ssid);
    command[5] = (uint8_t)strlen(expected_password);
    memcpy(command + 6, expected_ssid, strlen(expected_ssid));
    memcpy(command + 38, expected_password, strlen(expected_password));
    link = 0;
    assert(pf_wifi_command(command));
    unsigned failed_joins = joins, failed_leaves = leaves;
    link = -2;
    pf_wifi_poll();
    pf_wifi_result(result);
    /* WPA2 found no network, so the test retries once with WPA3-SAE before failing. */
    assert(result[2] == 4 && result[3] == 1 && leaves == failed_leaves + 1 && joins == failed_joins);
    expected_auth = CYW43_AUTH_WPA3_SAE_AES_PSK;
    pf_wifi_poll();
    assert(joins == failed_joins + 1);
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 6 && result[3] == 4 && leaves == failed_leaves + 2);
    assert(pf_wifi_state() == 0 && pf_wifi_error() == 0);
    expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
    link = 0;
    now += 60000000;
    pf_wifi_poll();
    assert(joins == failed_joins + 1 && pf_wifi_state() == 0);

    /* Live phases, then a wrong passphrase: the driver re-associates silently after each failed handshake. */
    command[2] = 15;
    link = CYW43_LINK_DOWN;
    cyw43_state.wifi_join_state = 0x0001;
    assert(pf_wifi_command(command));
    link = CYW43_LINK_JOIN;
    pf_wifi_result(result);
    assert(result[2] == 4 && result[3] == 1);
    cyw43_state.wifi_join_state = 0x0601;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 4 && result[3] == 2);
    cyw43_state.wifi_join_state = 0x0001;
    pf_wifi_poll();
    cyw43_state.wifi_join_state = 0x0601;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 4 && result[3] == 2 && pf_wifi_state() == 1);
    failed_leaves = leaves;
    cyw43_state.wifi_join_state = 0x0001;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 6 && result[3] == 5 && leaves == failed_leaves + 1 && pf_wifi_error() == 0);
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));

    /* A password accepted without DHCP reports the address phase, then a specific timeout. */
    command[2] = 16;
    link = CYW43_LINK_DOWN;
    assert(pf_wifi_command(command));
    link = CYW43_LINK_NOIP;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 4 && result[3] == 3);
    now += 30000000;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 6 && result[3] == 6 && pf_wifi_state() == 0 && pf_wifi_error() == 0);

    command[2] = 17;
    link = CYW43_LINK_DOWN;
    cyw43_state.wifi_join_state = 0x0001;
    assert(pf_wifi_command(command));
    link = CYW43_LINK_NONET;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 4 && result[3] == 1);
    /* A WPA3-only network: SAE gets past association, then a wrong key reports the SAE result. */
    expected_auth = CYW43_AUTH_WPA3_SAE_AES_PSK;
    const unsigned sae_joins = joins;
    pf_wifi_poll();
    assert(joins == sae_joins + 1);
    link = CYW43_LINK_BADAUTH;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 6 && result[3] == 5 && pf_wifi_error() == 0);
    expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;

    /* SAE succeeds on a WPA3-only network after WPA2 association fails outright. */
    command[2] = 33;
    link = CYW43_LINK_DOWN;
    assert(pf_wifi_command(command));
    link = -1;
    pf_wifi_poll();
    expected_auth = CYW43_AUTH_WPA3_SAE_AES_PSK;
    pf_wifi_poll();
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 33 && result[2] == 5);
    assert(pf_wifi_tested_credentials(tested_ssid, tested_password));
    expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
    uint8_t clear_sae[101] = {1u, 4u, 34u};
    assert(pf_wifi_command(clear_sae));
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));

    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 1; command[2] = 18;
    scan_active = false;
    assert(pf_wifi_command(command));
    pf_wifi_result(result);
    assert(result[2] == 1 && result[3] == 0);
    uint8_t cancel_scan[101] = {1u, 4u, 19u};
    assert(!pf_wifi_command(cancel_scan));
    pf_wifi_reject_command(cancel_scan[2]);
    pf_wifi_result(result);
    assert(result[1] == 19u && result[2] == 6u);
    scan_active = false;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 18u && result[2] == 2u);

    /* A rejected request reports separately and preserves the accepted test operation. */
    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 3; command[2] = 20;
    expected_ssid = "accepted-test";
    expected_password = "accepted-pass";
    expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
    link = CYW43_LINK_DOWN;
    command[4] = (uint8_t)strlen(expected_ssid);
    command[5] = (uint8_t)strlen(expected_password);
    memcpy(command + 6u, expected_ssid, strlen(expected_ssid));
    memcpy(command + 38u, expected_password, strlen(expected_password));
    assert(pf_wifi_command(command));
    uint8_t duplicate[101] = {1u, 1u, 20u};
    assert(!pf_wifi_command(duplicate));
    pf_wifi_reject_command(duplicate[2]);
    pf_wifi_result(result);
    assert(result[1] == 20u && result[2] == 4u);
    uint8_t rejected[101] = {1u, 1u, 21u};
    assert(!pf_wifi_command(rejected));
    pf_wifi_reject_command(rejected[2]);
    pf_wifi_result(result);
    assert(result[1] == 21u && result[2] == 6u);
    link = CYW43_LINK_JOIN;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 20u && result[2] == 4u);
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 20u && result[2] == 5u);
    assert(pf_wifi_tested_credentials(tested_ssid, tested_password));
    assert(strcmp(tested_ssid, expected_ssid) == 0 && strcmp(tested_password, expected_password) == 0);

    /* A prior live link must be gone before a new credential test can complete. */
    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 3; command[2] = 22;
    expected_ssid = "fresh-test";
    expected_password = "fresh-pass";
    command[4] = (uint8_t)strlen(expected_ssid);
    command[5] = (uint8_t)strlen(expected_password);
    memcpy(command + 6u, expected_ssid, strlen(expected_ssid));
    memcpy(command + 38u, expected_password, strlen(expected_password));
    const unsigned joins_before_test = joins;
    const unsigned leaves_before_test = leaves;
    assert(pf_wifi_command(command));
    assert(joins == joins_before_test && leaves == leaves_before_test + 1u);
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 22u && result[2] == 4u && !pf_wifi_tested_credentials(tested_ssid, tested_password));
    link = CYW43_LINK_DOWN;
    pf_wifi_poll();
    assert(joins == joins_before_test + 1u);
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 22u && result[2] == 5u);
    assert(pf_wifi_tested_credentials(tested_ssid, tested_password));
    assert(strcmp(tested_ssid, expected_ssid) == 0 && strcmp(tested_password, expected_password) == 0);

    /* A failed new authentication is not mistaken for the still-up previous station link. */
    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 3; command[2] = 23;
    expected_ssid = "failed-test";
    expected_password = "wrong-pass";
    command[4] = (uint8_t)strlen(expected_ssid);
    command[5] = (uint8_t)strlen(expected_password);
    memcpy(command + 6u, expected_ssid, strlen(expected_ssid));
    memcpy(command + 38u, expected_password, strlen(expected_password));
    link = CYW43_LINK_UP;
    const unsigned joins_before_failed_test = joins;
    assert(pf_wifi_command(command));
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(joins == joins_before_failed_test && result[1] == 23u && result[2] == 4u);
    link = CYW43_LINK_DOWN;
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));
    pf_wifi_poll();
    assert(joins == joins_before_failed_test + 1u);
    link = CYW43_LINK_BADAUTH;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 23u && result[2] == 6u && result[3] == 5u);
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));

    command[2] = 24;
    expected_ssid = "start-failed";
    expected_password = "wrong-pass";
    memset(command + 6u, 0, 32u);
    memset(command + 38u, 0, 63u);
    command[4] = (uint8_t)strlen(expected_ssid);
    command[5] = (uint8_t)strlen(expected_password);
    memcpy(command + 6u, expected_ssid, strlen(expected_ssid));
    memcpy(command + 38u, expected_password, strlen(expected_password));
    link = CYW43_LINK_DOWN;
    join_result = -1;
    assert(pf_wifi_command(command));
    pf_wifi_result(result);
    assert(result[1] == 24u && result[2] == 6u && result[3] == 8u && pf_wifi_error() == 0);
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));
    join_result = 0;

    /* Clearing with no staged test leaves a connected saved network alone. */
    radios = PF_WIRELESS_WIFI;
    ssid = expected_ssid = "network";
    password = expected_password = "test-password";
    assert(pf_wifi_prepare());
    pf_wifi_start();
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    assert(pf_wifi_state() == 2);
    unsigned joins_before_clear = joins, leaves_before_clear = leaves;
    uint8_t clear[101] = {1u, 4u, 25u};
    assert(pf_wifi_command(clear));
    pf_wifi_poll();
    assert(joins == joins_before_clear && leaves == leaves_before_clear && pf_wifi_state() == 2);

    /* Link loss is tracked during a scan, rejoin waits for the scan, and a stuck scan times out. */
    uint8_t scan[101] = {1u, 1u, 26u};
    assert(pf_wifi_command(scan));
    link = CYW43_LINK_DOWN;
    pf_wifi_poll();
    assert(pf_wifi_state() == 3);
    now += 9999999;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 1u && joins == joins_before_clear);
    ++now;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 26u && result[2] == 3u && joins == joins_before_clear);
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.ssid, "late", 4u); ap.ssid_len = 4u; ap.rssi = -20;
    scan_callback(NULL, &ap);
    pf_wifi_result(result);
    assert(result[4] == 0u);
    assert(!pf_wifi_command(scan));
    scan_active = false;

    /* A joined test result is withdrawn when its link drops; the verified credentials stay staged. */
    memset(command, 0, sizeof(command));
    command[0] = 1; command[1] = 3; command[2] = 27;
    expected_ssid = "dropped-test";
    expected_password = "dropped-pass";
    command[4] = (uint8_t)strlen(expected_ssid);
    command[5] = (uint8_t)strlen(expected_password);
    memcpy(command + 6u, expected_ssid, strlen(expected_ssid));
    memcpy(command + 38u, expected_password, strlen(expected_password));
    link = CYW43_LINK_DOWN;
    assert(pf_wifi_command(command));
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 27u && result[2] == 5u);
    link = CYW43_LINK_DOWN;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[1] == 27u && result[2] == 6u && result[3] == 9u && pf_wifi_state() == 3);
    assert(pf_wifi_tested_credentials(tested_ssid, tested_password));
    assert(strcmp(tested_ssid, "dropped-test") == 0);
    pf_wifi_result(result);
    assert(result[220] == 88u && result[221] == 2u);

    /* Malformed commands never enable station mode. */
    const unsigned sta_before_malformed = sta_calls;
    uint8_t malformed[101] = {1u, 1u, 28u, 1u};
    assert(!pf_wifi_command(malformed));
    malformed[1] = 3u;
    assert(!pf_wifi_command(malformed));
    assert(sta_calls == sta_before_malformed);

    /* With saved Wi-Fi disabled, station mode is released after a scan and after clearing a staged test. */
    uint8_t clear_staged[101] = {1u, 4u, 29u};
    expected_ssid = "network";
    expected_password = "test-password";
    assert(pf_wifi_command(clear_staged));
    expected_ssid = "dropped-test";
    expected_password = "dropped-pass";
    radios = PF_WIRELESS_BLE;
    assert(!pf_wifi_prepare());
    link = CYW43_LINK_DOWN;
    const unsigned releases_before_scan = sta_releases;
    scan[2] = 30u;
    assert(pf_wifi_command(scan) && sta_calls == sta_before_malformed + 1u);
    scan_active = false;
    pf_wifi_poll();
    assert(sta_releases == releases_before_scan + 1u);
    command[2] = 31;
    assert(pf_wifi_command(command) && sta_calls == sta_before_malformed + 2u);
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    assert(sta_releases == releases_before_scan + 1u);
    clear_staged[2] = 32u;
    assert(pf_wifi_command(clear_staged));
    assert(sta_releases == releases_before_scan + 2u && pf_wifi_state() == 0 && !pf_wifi_saved_link_up());

    /* A timed-out driver scan releases the station exactly once, only after the driver finishes. */
    assert(pf_wifi_command(scan));
    const unsigned before_timeout = sta_releases;
    now += 10000000u;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[0] == 2u && result[2] == 3u && sta_releases == before_timeout);
    pf_wifi_poll();
    assert(sta_releases == before_timeout);
    scan_active = false;
    pf_wifi_poll();
    assert(sta_releases == before_timeout + 1u && pf_wifi_state() == 0);
    pf_wifi_poll();
    assert(sta_releases == before_timeout + 1u);

    /* Refusing to start a scan also releases station mode. */
    scan_status = -1;
    assert(pf_wifi_command(scan));
    pf_wifi_result(result);
    assert(result[2] == 3u && sta_releases == before_timeout + 2u);
    scan_status = 0;

    /* Version 2 preserves the SDK privacy/WPA/RSN scan flags instead of collapsing them to a boolean. */
    assert(pf_wifi_command(scan));
    for (unsigned security = 0; security < 8u; ++security) {
        memset(&ap, 0, sizeof(ap));
        ap.ssid[0] = (uint8_t)('a' + security);
        ap.ssid_len = 1u;
        ap.rssi = (int16_t)(-10 - (int)security);
        ap.auth_mode = (uint8_t)security;
        scan_callback(NULL, &ap);
    }
    scan_active = false;
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[0] == 2u && result[4] == 8u);
    for (unsigned security = 0; security < 6u; ++security) assert(result[40u + 35u * security] == security);

    /* Staged credentials expire at exactly ten minutes even when no host polls the result. */
    link = CYW43_LINK_DOWN;
    assert(pf_wifi_command(command));
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    now += 599999999u;
    pf_wifi_poll();
    assert(pf_wifi_tested_credentials(tested_ssid, tested_password));
    ++now;
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));
    pf_wifi_poll();
    pf_wifi_result(result);
    assert(result[2] == 6u && result[3] == 10u && pf_wifi_state() == 0 && !pf_wifi_saved_link_up());
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));

    /* Expiry while a saved link has already rejoined must not disturb that link. */
    radios = PF_WIRELESS_WIFI;
    ssid = "network";
    password = "test-password";
    assert(pf_wifi_prepare());
    link = CYW43_LINK_DOWN;
    assert(pf_wifi_command(command));
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    const uint64_t expires = now + 600000000u;
    link = CYW43_LINK_DOWN;
    pf_wifi_poll();
    now += 5000000u;
    expected_ssid = "network";
    expected_password = "test-password";
    pf_wifi_poll();
    link = CYW43_LINK_UP;
    pf_wifi_poll();
    const unsigned leaves_on_saved = leaves;
    now = expires;
    pf_wifi_poll();
    assert(pf_wifi_saved_link_up() && leaves == leaves_on_saved);
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));
    return 0;
}
""")
    binary = tmp_path / "wifi"
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/wifi/wifi.c"), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True)
