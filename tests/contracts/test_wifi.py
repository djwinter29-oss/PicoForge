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
#include <stdint.h>
#define CYW43_AUTH_WPA2_AES_PSK 0x00400004u
#define CYW43_ITF_STA 0
#define CYW43_LINK_UP 3
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip; } netif_t;
typedef struct { netif_t netif[1]; } cyw43_t;
extern cyw43_t cyw43_state;
#define netif_ip4_addr(netif) (&(netif)->ip)
uint64_t time_us_64(void);
void cyw43_arch_enable_sta_mode(void);
int cyw43_arch_wifi_connect_async(const char *, const char *, uint32_t);
int cyw43_tcpip_link_status(cyw43_t *, int);
""")
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
uint8_t pf_boot_config_radios(void) { return radios; }
void pf_boot_config_wifi_ssid(char out[33]) { strcpy(out, ssid); }
void pf_boot_config_wifi_password(char out[64]) { strcpy(out, password); }
uint64_t time_us_64(void) { return now; }
void cyw43_arch_enable_sta_mode(void) { ++sta_calls; }
int cyw43_arch_wifi_connect_async(const char *s, const char *p, uint32_t auth) {
    assert(strcmp(s, "network") == 0 && strcmp(p, "test-password") == 0);
    assert(auth == CYW43_AUTH_WPA2_AES_PSK);
    ++joins;
    return join_result;
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
    assert(!pf_wifi_prepare());
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
