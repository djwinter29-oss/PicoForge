"""Exercise the production Wi-Fi state machine without radio hardware."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


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
#define CYW43_ITF_STA 0
#define CYW43_LINK_UP 3
typedef struct { uint8_t unused; } cyw43_wifi_scan_options_t;
typedef struct {
    uint8_t ssid_len;
    uint8_t ssid[32];
    int16_t rssi;
    uint8_t auth_mode;
} cyw43_ev_scan_result_t;
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip; } netif_t;
typedef struct { netif_t netif[1]; } cyw43_t;
typedef int (*scan_result_cb_t)(void *, const cyw43_ev_scan_result_t *);
extern cyw43_t cyw43_state;
bool cyw43_wifi_scan_active(cyw43_t *);
int cyw43_wifi_scan(cyw43_t *, cyw43_wifi_scan_options_t *, void *, scan_result_cb_t);
void cyw43_wifi_leave(cyw43_t *, int);
#define netif_ip4_addr(netif) (&(netif)->ip)
uint64_t time_us_64(void);
void cyw43_arch_enable_sta_mode(void);
int cyw43_arch_wifi_connect_async(const char *, const char *, uint32_t);
int cyw43_tcpip_link_status(cyw43_t *, int);
#endif
""")
    (tmp_path / "cyw43.h").write_text('#include "pico/cyw43_arch.h"\n')
    harness = tmp_path / "wifi.c"
    harness.write_text("""
#include <assert.h>
#include <string.h>
#include "app/wireless/wifi.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
#include "pico/cyw43_arch.h"
cyw43_t cyw43_state;
static uint64_t now;
static unsigned joins, sta_calls, logs;
static int join_result, link;
static uint8_t radios = PF_WIRELESS_WIFI;
static const char *ssid = "network", *password = "test-password";
static const char *expected_ssid = "network", *expected_password = "test-password";
static uint32_t expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
static bool scan_active;
static unsigned leaves;
static scan_result_cb_t scan_callback;
uint8_t pf_boot_config_radios(void) { return radios; }
void pf_boot_config_wifi_ssid(char out[33]) { strcpy(out, ssid); }
void pf_boot_config_wifi_password(char out[64]) { strcpy(out, password); }
uint64_t time_us_64(void) { return now; }
void cyw43_arch_enable_sta_mode(void) { ++sta_calls; }
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
    scan_active = true;
    return 0;
}
void cyw43_wifi_leave(cyw43_t *state, int itf) {
    assert(state == &cyw43_state && itf == CYW43_ITF_STA);
    ++leaves;
}
int cyw43_tcpip_link_status(cyw43_t *state, int itf) {
    assert(state == &cyw43_state && itf == CYW43_ITF_STA);
    return link;
}
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...) {
    (void)level; (void)format;
    assert(strcmp(tag, "wifi") == 0);
    ++logs;
    return true;
}
int main(void) {
    uint8_t error = 0, address[4], expected[4] = {192, 168, 1, 42};
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
    pf_wifi_start(&error);
    assert(joins == 1 && sta_calls == 1 && pf_wifi_state() == 1 && error == 0);
    now = 29999999;
    pf_wifi_poll(&error);
    assert(pf_wifi_state() == 1 && joins == 1);
    now = 30000000;
    pf_wifi_poll(&error);
    assert(pf_wifi_state() == 3 && error == 3 && logs == 1);
    now = 34999999;
    pf_wifi_poll(&error);
    assert(joins == 1);
    now = 35000000;
    pf_wifi_poll(&error);
    assert(joins == 2 && pf_wifi_state() == 1 && error == 0);
    link = CYW43_LINK_UP;
    pf_wifi_poll(&error);
    assert(pf_wifi_state() == 2 && error == 0);
    memcpy(&cyw43_state.netif[0].ip.addr, expected, sizeof(expected));
    pf_wifi_ip_address(address);
    assert(memcmp(address, expected, sizeof(expected)) == 0);
    link = 0;
    pf_wifi_poll(&error);
    assert(pf_wifi_state() == 3 && logs == 2);
    now += 4999999;
    pf_wifi_poll(&error);
    assert(joins == 2);
    ++now;
    join_result = -1;
    pf_wifi_poll(&error);
    assert(joins == 3 && pf_wifi_state() == 3 && error == 2 && logs == 3);
    now += 5000000;
    join_result = 0;
    pf_wifi_poll(&error);
    assert(joins == 4 && pf_wifi_state() == 1);
    link = -1;
    pf_wifi_poll(&error);
    assert(pf_wifi_state() == 3 && error == 2 && logs == 4);

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
    pf_wifi_poll(&error);
    assert(pf_wifi_state() == 2);
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
    pf_wifi_poll(&error);
    uint8_t result[220];
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
    command[0] = 1; command[1] = 4; command[2] = 13;
    expected_ssid = "network";
    expected_password = "test-password";
    expected_auth = CYW43_AUTH_WPA2_MIXED_PSK;
    assert(pf_wifi_command(command));
    assert(leaves == 1 && joins == 6 && pf_wifi_state() == 1);
    assert(!pf_wifi_tested_credentials(tested_ssid, tested_password));

    password = "";
    expected_password = "";
    expected_auth = CYW43_AUTH_OPEN;
    assert(pf_wifi_prepare());
    unsigned before_open_join = joins;
    pf_wifi_start(&error);
    assert(joins == before_open_join + 1 && pf_wifi_state() == 1);
    return 0;
}
""")
    binary = tmp_path / "wifi"
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/wifi.c"), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True)
