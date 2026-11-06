// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file
 * @brief Board and radio command handlers.
 */
#include "cli_board.h"
#include "cli.h"
#include "cli_internal.h"
#include "platform/led.h"
#include "platform/system.h"
#if PF_ENABLE_WIRELESS
#include "app/wireless/wireless.h"
#endif

#include <stdio.h>
#include <string.h>

/** Indented subcommands keep help readable within the transport's 1024-byte output reservation. */
#define BOARD_HELP \
    "get <clock | led | version | uptime>\r\n" \
    "\tset led <on | off>\r\n" \
    "\treboot"
/** Radio diagnostics never expose credentials. */
#define RADIO_HELP "status"

static void (*reboot_callback)(void); /**< Borrowed USB owner task reset scheduler. */

/** @brief Emit board values through platform services; validate the whole request before mutating. */
static void board_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    (void)context;
    const uint16_t count = embeddedCliGetTokenCount(args);
    const char *verb = embeddedCliGetToken(args, 1u);
    const char *item = embeddedCliGetToken(args, 2u);
    char line[80];

    if (count == 2u && strcmp(verb, "get") == 0) {
        if (strcmp(item, "clock") == 0) {
            snprintf(line, sizeof(line), "clock=%lu Hz", (unsigned long)pf_system_clock_hz());
        } else if (strcmp(item, "uptime") == 0) {
            snprintf(line, sizeof(line), "uptime=%lu ms", (unsigned long)pf_system_uptime_ms());
        } else if (strcmp(item, "version") == 0) {
            snprintf(line, sizeof(line), "version=%u.%u.%u", PF_FIRMWARE_VERSION_MAJOR,
                     PF_FIRMWARE_VERSION_MINOR, PF_FIRMWARE_VERSION_PATCH);
        } else if (strcmp(item, "led") == 0) {
            snprintf(line, sizeof(line), "led=%s", pf_led_is_on() ? "on" : "off");
        } else {
            pf_cli_print("ERR board " BOARD_HELP);
            return;
        }
        pf_cli_print(line);
    } else if (count == 3u && strcmp(verb, "set") == 0 && strcmp(item, "led") == 0) {
        const char *value = embeddedCliGetToken(args, 3u);
        if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) {
            pf_cli_print("ERR board set led on|off");
            return;
        }
        pf_cli_print(pf_led_set(strcmp(value, "on") == 0) ?
                     (strcmp(value, "on") == 0 ? "OK LED on" : "OK LED off") :
                     "ERR LED write failed; verify the board LED driver is initialized");
    } else if (count == 1u && strcmp(verb, "reboot") == 0) {
        if (!pf_cli_restart_allowed()) return;
        pf_cli_print("QUEUED reboot; waiting for CLI output to flush");
        reboot_callback();
    } else {
        pf_cli_print("ERR board " BOARD_HELP);
    }
}

/** @brief Print live Wi-Fi and Bluetooth state; never the SSID or credential information. */
static void radio_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    (void)context;
    if (embeddedCliGetTokenCount(args) != 1u || strcmp(embeddedCliGetToken(args, 1u), "status") != 0) {
        pf_cli_print("ERR radio " RADIO_HELP);
        return;
    }
#if PF_ENABLE_WIRELESS
    static const char *const wifi_states[] = {
        "disabled", "connecting", "connected", "failed", "testing", "test-connected", "test-failed"
    };
    static const char *const ble_states[] = { "disabled", "advertising", "connected" };
    uint8_t report[PF_WIRELESS_STATUS_SIZE];
    char line[120];
    pf_wireless_status(report);
    snprintf(line, sizeof(line), "wifi=%s ip=%u.%u.%u.%u",
             report[PF_WIRELESS_STATUS_WIFI_STATE_OFFSET] < sizeof(wifi_states) / sizeof(wifi_states[0]) ?
                 wifi_states[report[PF_WIRELESS_STATUS_WIFI_STATE_OFFSET]] : "unknown",
             report[PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET], report[PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET + 1u],
             report[PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET + 2u], report[PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET + 3u]);
    pf_cli_print(line);
    snprintf(line, sizeof(line), "bluetooth=%s",
             report[PF_WIRELESS_STATUS_BLUETOOTH_STATE_OFFSET] < 3u ?
                 ble_states[report[PF_WIRELESS_STATUS_BLUETOOTH_STATE_OFFSET]] : "unknown");
    pf_cli_print(line);
#else
    pf_cli_print("ERR radios unavailable in this build");
#endif
}

bool pf_cli_board_register(void (*request_reboot)(void))
{
    if (request_reboot == NULL) return false;
    reboot_callback = request_reboot;
    return pf_cli_add_binding((CliCommandBinding){ "board", BOARD_HELP, true, NULL, board_command }) &&
           pf_cli_add_binding((CliCommandBinding){ "radio", RADIO_HELP, true, NULL, radio_command });
}
