"""Exercise production USB wireless provisioning without SDK or flash hardware."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_wireless_stack_credentials_use_non_elidable_cleanup():
    source = (ROOT / "firmware/src/app/wireless/wireless.c").read_text()
    startup = source.split("void pf_wireless_start(void)", 1)[1].split("/** @copydoc", 1)[0]
    configure = source.split("bool pf_wireless_configure(const uint8_t *report)", 1)[1]
    assert "mbedtls_platform_zeroize(tcp_key, sizeof(tcp_key))" in startup
    assert "mbedtls_platform_zeroize(&credentials, sizeof(credentials))" in configure
    assert "memset(" not in startup
    assert "memset(" not in configure


def test_board_and_wireless_configuration_saves_once_or_discards(tmp_path):
    for name, text in {
        "FreeRTOS.h": "typedef unsigned UBaseType_t;\n",
        "task.h": "void vTaskCoreAffinitySet(void *, UBaseType_t);\n",
        "pico/cyw43_arch.h": "#include <stdbool.h>\n#define CYW43_WL_GPIO_LED_PIN 0\n"
                             "#include <stdint.h>\nuint64_t time_us_64(void);\n"
                             "int cyw43_arch_init(void);\nvoid cyw43_arch_poll(void);\nunsigned get_core_num(void);\n"
                             "void cyw43_arch_gpio_put(unsigned wl_gpio, bool value);\n",
        "pico/unique_id.h": "#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8\n",
        "mbedtls/platform_util.h": "#include <stddef.h>\nvoid mbedtls_platform_zeroize(void *, size_t);\n",
    }.items():
        (tmp_path / name).parent.mkdir(exist_ok=True)
        (tmp_path / name).write_text(text)
    harness = tmp_path / "harness.c"
    harness.write_text(r"""
#include <assert.h>
#include <string.h>
#include "FreeRTOS.h"
#include "pico/cyw43_arch.h"
#include "app/storage/storage_worker.h"
#include "app/usb/usb_hid.h"
#include "app/usb/usb_msc.h"
#include "app/wireless/bluetooth/bluetooth.h"
#include "app/wireless/wifi/tcp_cli.h"
#include "app/wireless/wifi/wifi.h"
#include "app/wireless/wireless.h"
#include "platform/boot_config.h"
#include "platform/led.h"
#include "platform/logging.h"
static unsigned saves, discards, zeroizes;
static bool save_result = true;
void mbedtls_platform_zeroize(void *buffer, size_t size) {
    volatile unsigned char *bytes = buffer;
    while (size-- != 0u) *bytes++ = 0u;
    ++zeroizes;
}
static int clock = -1, speed = -1, radios = -1;
static char ssid[33], saved_password[64];
static bool storage_busy;
static bool staged_test;
static unsigned cleared_tests;
static unsigned chip_inits;
static int chip_init_result;
static bool wifi_prepared;
static unsigned wifi_starts, tcp_cli_starts;
static uint64_t now;
static uint64_t tcp_retry_us;
static bool tcp_cli_enabled = true, tcp_listener, tcp_start_result = true;
static bool tcp_auth_required, shared_key_available, restart_required;
static int led_level = -1;
uint64_t time_us_64(void) { return now; }
bool pf_boot_config_tcp_cli_enabled(void) { return tcp_cli_enabled; }
bool pf_boot_config_set_tcp_cli_enabled(bool enabled) { tcp_cli_enabled = enabled; return true; }
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
void pf_boot_config_ble_key(uint8_t key[32]) { memset(key, 0, 32u); }
bool pf_boot_config_tcp_cli_auth_required(void) { return tcp_auth_required; }
bool pf_boot_config_set_tcp_cli_auth_required(bool required) { tcp_auth_required = required; return true; }
bool pf_boot_config_wireless_restart_required(void) { return restart_required; }
bool pf_boot_config_set_wifi_auth_state(pf_wifi_auth_state_t state) { (void)state; return true; }
pf_wifi_auth_state_t pf_boot_config_wifi_auth_state(void) { return PF_WIFI_AUTH_TRY_WPA2; }
bool pf_boot_config_save(void) { ++saves; return save_result; }
void pf_boot_config_discard(void) {
    ++discards; clock = speed = radios = -1; ssid[0] = 0; saved_password[0] = 0;
}
uint8_t pf_boot_config_radios(void) { return radios < 0 ? 0u : (uint8_t)radios; }
void pf_boot_config_wifi_ssid(char out[33]) { strcpy(out, ssid); }
void pf_boot_config_wifi_password(char out[64]) { strcpy(out, saved_password); }
bool pf_boot_config_wifi_password_configured(void) { return saved_password[0] != 0; }
bool pf_boot_config_ble_key_configured(void) { return false; }
bool pf_usb_msc_is_enabled(void) { return false; }
bool pf_usb_msc_transition_pending(void) { return false; }
bool pf_storage_worker_busy(void) { return storage_busy; }
bool pf_wifi_prepare(void) { return wifi_prepared; }
void pf_wifi_start(void) { ++wifi_starts; }
static bool saved_link_up = true, tcp_serving_set;
bool pf_tcp_cli_serving(void) { return tcp_listener && tcp_serving_set; }
bool pf_tcp_cli_set_auth_required(bool required) { return !required || shared_key_available; }
bool pf_tcp_cli_auth_required(void) { return tcp_auth_required; }
void pf_tcp_cli_poll(bool serving, uint64_t poll_now) {
    tcp_serving_set = serving;
    if (serving && !tcp_listener && poll_now >= tcp_retry_us) {
        ++tcp_cli_starts;
        tcp_listener = tcp_start_result;
        tcp_retry_us = tcp_listener ? 0u : poll_now + 5000000u;
    }
    if (!serving) { tcp_listener = false; tcp_retry_us = 0u; }
}
void pf_wireless_auth_set_key(const uint8_t key[32]) {
    shared_key_available = false;
    if (key) for (unsigned i = 0u; i < 32u; ++i) shared_key_available |= key[i] != 0u;
}
void pf_wifi_mdns_poll(bool saved_up, bool tcp_up, uint64_t poll_now) { (void)saved_up; (void)tcp_up; (void)poll_now; }
bool pf_wifi_saved_link_up(void) { return saved_link_up; }
static uint8_t wifi_error;
static bool ble_prepared, ble_start_result;
void pf_wifi_poll(uint64_t poll_now) { (void)poll_now; }
pf_wireless_error_t pf_wifi_error(void) { return (pf_wireless_error_t)wifi_error; }
pf_wifi_state_t pf_wifi_state(void) { return PF_WIFI_STATE_CONNECTED; }
void pf_wifi_ip_address(uint8_t address[4]) { memset(address, 42, 4u); }
static unsigned wifi_commands, wifi_rejects;
bool pf_wifi_command(const uint8_t report[101]) { ++wifi_commands; return report[0] == 1u; }
void pf_wifi_reject_command(uint8_t transaction) { assert(transaction != 0u); ++wifi_rejects; }
void pf_wifi_result(uint8_t report[PF_WIRELESS_RESULT_SIZE]) { memset(report, 0, PF_WIRELESS_RESULT_SIZE); }
bool pf_wifi_tested_credentials(char out_ssid[33], char out_password[64]) {
    if (!staged_test) return false;
    strcpy(out_ssid, "network");
    strcpy(out_password, "tested-pass");
    return true;
}
pf_wifi_auth_state_t pf_wifi_tested_auth_state(void) { return PF_WIFI_AUTH_WPA2; }
void pf_wifi_link_diagnostics(pf_wifi_auth_state_t *auth_state, uint16_t *reconnects, int8_t *rssi)
{ *auth_state = PF_WIFI_AUTH_TRY_WPA2; *reconnects = 0u; *rssi = PF_WIFI_RSSI_UNAVAILABLE; }
void pf_wifi_clear_tested_credentials(void) { staged_test = false; ++cleared_tests; }
bool pf_bluetooth_prepare(void) { return ble_prepared; }
bool pf_bluetooth_start(void) { return ble_start_result; }
uint8_t pf_bluetooth_state(void) { return 2; }
int cyw43_arch_init(void) { ++chip_inits; return chip_init_result; }
void cyw43_arch_poll(void) {}
void cyw43_arch_gpio_put(unsigned wl_gpio, bool value) { assert(wl_gpio == CYW43_WL_GPIO_LED_PIN); led_level = value; }
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
int main(int argc, char **argv)
{
    (void)argv;
    uint8_t report[PF_WIRELESS_REQUEST_SIZE], status[PF_WIRELESS_STATUS_SIZE];
    if (argc > 1) {
        tcp_auth_required = true;
        pf_wireless_start();
        pf_wireless_status(status);
        assert(status[PF_WIRELESS_STATUS_ERROR_OFFSET] == PF_WIRELESS_ERROR_INIT);
        assert(status[PF_WIRELESS_STATUS_VERSION_OFFSET] == PF_WIRELESS_STATUS_VERSION);
        assert((status[PF_WIRELESS_STATUS_SERVICES_OFFSET] &
                PF_WIRELESS_STATUS_SERVICE_TCP_CLI_AUTH_REQUIRED) != 0u);
        assert(status[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] == 0u);
        pf_wireless_poll();
        assert(tcp_cli_starts == 0u && zeroizes == 1u);
        return 0;
    }
    /* With both radios off, the chip still starts for the W-board LED, but no radio is enabled. */
    assert(!pf_wireless_led_put(true) && led_level == -1);
    tcp_auth_required = true;
    chip_init_result = -1;
    pf_wireless_start();
    assert(chip_inits == 1 && !pf_wireless_led_put(true) && led_level == -1);
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] == 0u);
    tcp_auth_required = false;
    chip_init_result = 0;
    pf_wireless_start();
    assert(chip_inits == 2 && led_level == 0);
    pf_wireless_status(status);
    assert(zeroizes == 2u);
    assert(wifi_starts == 0 && tcp_cli_starts == 0);
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] == 0u);
    assert(pf_wireless_led_put(true));
    assert(led_level == 1);
    request(report, 250, 1);
    const unsigned before_save_zeroizes = zeroizes;
    assert(pf_wireless_configure(report) && saves == 1 && discards == 0);
    assert(zeroizes == before_save_zeroizes + 1u);
    assert(clock == PF_SYSCLK_250MHZ && speed == 1 && radios == PF_WIRELESS_WIFI && strcmp(ssid, "network") == 0);
    pf_wireless_status(status);
    assert(status[2] == PF_HID_COMMAND_COMPLETED && status[3] == 7);
    assert(status[PF_WIRELESS_STATUS_VERSION_OFFSET] == PF_WIRELESS_STATUS_VERSION);
    assert(status[PF_WIRELESS_STATUS_RESTART_REQUIRED_OFFSET] == 0u);
    request(report, 175, 1);
    assert(!pf_wireless_configure(report) && saves == 1 && discards == 1 && ssid[0] == 0);
    request(report, 150, 2);
    assert(!pf_wireless_configure(report) && saves == 1 && discards == 2);
    request(report, 150, 0);
    report[3] = 2;
    assert(pf_wireless_configure(report) && saves == 2 && discards == 2 && saved_password[0] == 0);
    request(report, 150, 0);
    report[3] = 4;
    const unsigned before_rejected_zeroizes = zeroizes;
    assert(!pf_wireless_configure(report) && saves == 2 && discards == 2);
    assert(zeroizes == before_rejected_zeroizes + 1u);
    staged_test = true;
    report[4] = 'X';
    assert(!pf_wireless_configure(report) && saves == 2 && discards == 2);
    assert(zeroizes == before_rejected_zeroizes + 2u);
    staged_test = false;
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
    assert(wifi_commands == 1 && wifi_rejects == 0);
    command[0] = 2;
    pf_wireless_command(command);
    assert(wifi_commands == 2 && wifi_rejects == 1);
    storage_busy = true;
    pf_wireless_command(command);
    assert(wifi_commands == 2 && wifi_rejects == 2);
    storage_busy = false;
    command[2] = 0;
    pf_wireless_command(command);
    assert(wifi_commands == 2 && wifi_rejects == 2);
    uint8_t result[PF_WIRELESS_RESULT_SIZE];
    pf_wireless_result_report(result);
    wifi_prepared = true;
    pf_wireless_start();
    assert(chip_inits == 2 && wifi_starts == 1 && tcp_cli_starts == 0);
    /* The TCP CLI serves, and status reports it, only while the saved network is up. */
    pf_wireless_status(status);
    assert((status[PF_WIRELESS_STATUS_SERVICES_OFFSET] &
            PF_WIRELESS_STATUS_SERVICE_TCP_CLI_DISABLED) == 0u);
    tcp_start_result = false;
    pf_wireless_poll();
    assert(tcp_cli_starts == 1 && !pf_tcp_cli_serving());
    now = 4999999u;
    pf_wireless_poll();
    assert(tcp_cli_starts == 1);
    now = 5000000u;
    tcp_start_result = true;
    pf_wireless_poll();
    assert(tcp_cli_starts == 2);
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_VERSION_OFFSET] == PF_WIRELESS_STATUS_VERSION);
    assert(status[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] == 1u);
    assert(status[5] == 42u && status[PF_WIRELESS_STATUS_SERVICES_OFFSET] == 0u);
    saved_link_up = false;
    pf_wireless_poll();
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] == 0);
    assert(status[4] == 2 && status[5] == 0 && status[6] == 0 && status[7] == 0 && status[8] == 0);
    saved_link_up = true;
    pf_wireless_poll();

    /* Wi-Fi owns its error; a BLE start failure stays visible after Wi-Fi recovers. */
    wifi_error = PF_WIRELESS_ERROR_TIMEOUT;
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_ERROR_OFFSET] == PF_WIRELESS_ERROR_TIMEOUT);
    ble_prepared = true;
    pf_wireless_start();
    wifi_error = PF_WIRELESS_ERROR_NONE;
    pf_wireless_poll();
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_ERROR_OFFSET] == PF_WIRELESS_ERROR_INIT);

    /* Status never needs the secret values, only whether they are configured. */
    strcpy(saved_password, "configured");
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_PASSWORD_CONFIGURED_OFFSET] == 1 && strcmp((char *)status + 9, "network") == 0);

    /* A save can disable, preserve, or enable the service independently of the radio switches. */
    request(report, 150, 0);
    report[3] |= 8u;
    assert(pf_wireless_configure(report) && !tcp_cli_enabled);
    pf_wireless_start();
    pf_wireless_poll();
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_SERVICES_OFFSET] == PF_WIRELESS_STATUS_SERVICE_TCP_CLI_DISABLED);
    assert(status[PF_WIRELESS_STATUS_TCP_CLI_OFFSET] == 0u);
    request(report, 150, 0);
    report[3] |= 16u;
    assert(pf_wireless_configure(report) && !tcp_cli_enabled);
    request(report, 150, 0);
    report[3] |= 24u;
    const unsigned before_invalid = saves;
    assert(!pf_wireless_configure(report) && saves == before_invalid);
    request(report, 150, 0);
    report[3] |= 96u;
    assert(!pf_wireless_configure(report) && saves == before_invalid);
    request(report, 150, 0);
    assert(pf_wireless_configure(report) && tcp_cli_enabled);
    request(report, 150, 0);
    save_result = false;
    const unsigned before_failed_save_zeroizes = zeroizes;
    assert(!pf_wireless_configure(report));
    pf_wireless_status(status);
    assert(status[PF_WIRELESS_STATUS_SAVE_STATUS_OFFSET] == PF_HID_COMMAND_FAILED);
    assert(zeroizes == before_failed_save_zeroizes + 1u);
    return 0;
}
""")
    binary = tmp_path / "wireless"
    compiled = subprocess.run([
        "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/wireless.c"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary)], check=True)
    subprocess.run([str(binary), "missing-auth-key"], check=True)
