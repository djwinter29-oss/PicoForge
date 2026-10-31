"""Exercise production USB wireless provisioning without SDK or flash hardware."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_board_and_wireless_configuration_saves_once_or_discards(tmp_path):
    for name, text in {
        "FreeRTOS.h": "typedef unsigned UBaseType_t;\n",
        "task.h": "void vTaskCoreAffinitySet(void *, UBaseType_t);\n",
        "pico/cyw43_arch.h": "int cyw43_arch_init(void);\nvoid cyw43_arch_poll(void);\nunsigned get_core_num(void);\n",
        "pico/unique_id.h": "#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8\n",
    }.items():
        (tmp_path / name).parent.mkdir(exist_ok=True)
        (tmp_path / name).write_text(text)
    harness = tmp_path / "harness.c"
    harness.write_text(r"""
#include <assert.h>
#include <string.h>
#include "FreeRTOS.h"
#include "app/storage/storage_worker.h"
#include "app/usb/usb_hid.h"
#include "app/usb/usb_msc.h"
#include "app/wireless/bluetooth.h"
#include "app/wireless/wifi.h"
#include "app/wireless/wireless.h"
#include "platform/boot_config.h"
#include "platform/logging.h"
static unsigned saves, discards;
static int clock = -1, speed = -1, radios = -1;
static char ssid[33], saved_password[64];
static bool storage_busy;
static bool staged_test;
static unsigned cleared_tests, enabled_sta;
bool pf_boot_config_set_sysclk(pf_sysclk_profile_t p) { if ((unsigned)p > 3u) return false; clock = (int)p; return true; }
bool pf_boot_config_set_storage_speed(pf_storage_speed_t s) { if ((unsigned)s > 1u) return false; speed = (int)s; return true; }
bool pf_boot_config_set_radios(uint8_t r) { if (r > 3u) return false; radios = r; return true; }
bool pf_boot_config_set_wifi_ssid(const char *s) { strcpy(ssid, s); return true; }
bool pf_boot_config_set_wifi_password(const char *p) {
    if (p[0] != 0 && strlen(p) < 8u) return false;
    strcpy(saved_password, p);
    return true;
}
bool pf_boot_config_set_ble_key(const uint8_t key[32]) { (void)key; return true; }
bool pf_boot_config_save(void) { ++saves; return true; }
void pf_boot_config_discard(void) {
    ++discards; clock = speed = radios = -1; ssid[0] = 0; saved_password[0] = 0;
}
uint8_t pf_boot_config_radios(void) { return radios < 0 ? 0u : (uint8_t)radios; }
void pf_boot_config_wifi_ssid(char out[33]) { strcpy(out, ssid); }
void pf_boot_config_wifi_password(char out[64]) { strcpy(out, saved_password); }
void pf_boot_config_ble_key(uint8_t out[32]) { memset(out, 0, 32); }
bool pf_usb_msc_is_enabled(void) { return false; }
bool pf_usb_msc_transition_pending(void) { return false; }
bool pf_storage_worker_busy(void) { return storage_busy; }
bool pf_wifi_prepare(void) { return false; }
void pf_wifi_start(uint8_t *error) { (void)error; }
void pf_wifi_poll(uint8_t *error) { (void)error; }
uint8_t pf_wifi_state(void) { return 0; }
void pf_wifi_ip_address(uint8_t address[4]) { (void)address; }
void pf_wifi_enable(void) { ++enabled_sta; }
bool pf_wifi_command(const uint8_t report[101]) { (void)report; return true; }
void pf_wifi_reject_command(uint8_t transaction) { (void)transaction; }
void pf_wifi_result(uint8_t report[220]) { memset(report, 0, 220); }
bool pf_wifi_tested_credentials(char out_ssid[33], char out_password[64]) {
    if (!staged_test) return false;
    strcpy(out_ssid, "network");
    strcpy(out_password, "tested-pass");
    return true;
}
void pf_wifi_clear_tested_credentials(void) { staged_test = false; ++cleared_tests; }
bool pf_bluetooth_prepare(void) { return false; }
bool pf_bluetooth_start(void) { return false; }
int cyw43_arch_init(void) { return 0; }
void cyw43_arch_poll(void) {}
void vTaskCoreAffinitySet(void *task, UBaseType_t mask) { (void)task; (void)mask; }
unsigned get_core_num(void) { return 0; }
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; return true; }
static void request(uint8_t *report, uint16_t mhz, uint8_t profile)
{
    memset(report, 0, PF_WIRELESS_REQUEST_SIZE);
    report[0] = 2; report[1] = 7; report[2] = PF_WIRELESS_WIFI; report[3] = 3;
    strcpy((char *)report + 4, "network");
    report[133] = (uint8_t)mhz; report[134] = (uint8_t)(mhz >> 8); report[135] = profile;
}
int main(void)
{
    uint8_t report[PF_WIRELESS_REQUEST_SIZE], status[PF_WIRELESS_STATUS_SIZE];
    request(report, 250, 1);
    assert(pf_wireless_configure(report) && saves == 1 && discards == 0);
    assert(clock == PF_SYSCLK_250MHZ && speed == 1 && radios == PF_WIRELESS_WIFI && strcmp(ssid, "network") == 0);
    pf_wireless_status(status);
    assert(status[2] == PF_HID_COMMAND_COMPLETED && status[3] == 7);
    request(report, 175, 1);
    assert(!pf_wireless_configure(report) && saves == 1 && discards == 1 && ssid[0] == 0);
    request(report, 150, 2);
    assert(!pf_wireless_configure(report) && saves == 1 && discards == 2);
    request(report, 150, 0);
    report[3] = 2;
    assert(pf_wireless_configure(report) && saves == 2 && discards == 2 && saved_password[0] == 0);
    request(report, 150, 0);
    report[3] = 4;
    assert(!pf_wireless_configure(report) && saves == 2 && discards == 2);
    request(report, 150, 0);
    report[0] = 1;
    assert(!pf_wireless_configure(report) && saves == 2 && discards == 2);
    request(report, 150, 0);
    storage_busy = true;
    assert(!pf_wireless_configure(report) && saves == 2 && discards == 2);
    pf_wireless_status(status);
    assert(status[2] == PF_HID_COMMAND_BUSY);
    storage_busy = false;
    request(report, 150, 0);
    report[3] = 4;
    staged_test = true;
    assert(pf_wireless_configure(report) && saves == 3 && cleared_tests == 1);
    assert(strcmp(saved_password, "tested-pass") == 0);
    assert(!staged_test);

    uint8_t command[PF_WIRELESS_COMMAND_SIZE] = {1, 1, 9};
    pf_wireless_command(command);
    assert(enabled_sta == 1);
    uint8_t result[PF_WIRELESS_RESULT_SIZE];
    pf_wireless_result_report(result);
    return 0;
}
""")
    binary = tmp_path / "wireless"
    compiled = subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/wireless.c"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary)], check=True)
