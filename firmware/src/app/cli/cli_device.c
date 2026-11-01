/**
 * @file
 * @brief Bounded device command handlers using the same owning services as HID control.
 */
#include "cli_device.h"
#include "cli.h"
#include "platform/boot_config.h"
#include "platform/led.h"
#include "platform/system.h"
#include "app/storage/storage_worker.h"
#include "app/usb/usb_msc.h"
#if PF_ENABLE_WIRELESS
#include "app/wireless/wireless.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/** Indented subcommands keep help readable within the transport's 1024-byte output reservation. */
#define BOARD_HELP \
    "get <clock | led | version | uptime>\r\n" \
    "\tset led <on | off>\r\n" \
    "\treboot"
/** Configuration values are saved targets, not running hardware state. */
#define CONFIG_HELP \
    "get [clock | storage | wifi | bluetooth]\r\n" \
    "\tset key=value[,key=value...] (saves and reboots)"
/** Radio diagnostics never expose credentials. */
#define RADIO_HELP "status"

static void (*reboot_callback)(void); /**< Borrowed USB owner task reset scheduler. */

/** @brief Refuse save/reset while storage operations or USB ownership prevent a safe restart. */
static bool restart_allowed(void)
{
    if (pf_storage_worker_busy() || pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
        pf_cli_print("ERR storage busy or USB-owned; safely eject and disable MSC before save/reboot");
        return false;
    }
    return true;
}

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
        pf_cli_print(pf_led_set(strcmp(value, "on") == 0) ? "OK" : "ERR LED unavailable or operation failed");
    } else if (count == 1u && strcmp(verb, "reboot") == 0) {
        if (!restart_allowed()) return;
        pf_cli_print("OK rebooting");
        reboot_callback();
    } else {
        pf_cli_print("ERR board " BOARD_HELP);
    }
}

/** @brief Trim ASCII whitespace in a bounded, mutable assignment buffer. */
static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

/** @brief Read one saved target; no private password/key getter is used. */
static bool print_config(const char *item)
{
    char line[64];
    if (strcmp(item, "clock") == 0) {
        const uint16_t mhz = pf_get_sysclk_mhz();
        if (mhz == 0u) pf_cli_print("clock=default (next boot)");
        else {
            snprintf(line, sizeof(line), "clock=%u MHz (next boot)", mhz);
            pf_cli_print(line);
        }
    } else if (strcmp(item, "storage") == 0) {
        snprintf(line, sizeof(line), "storage=%s (next boot)",
                 pf_boot_config_storage_speed() == PF_STORAGE_SPEED_HIGH ? "high" : "default");
        pf_cli_print(line);
    } else if (strcmp(item, "wifi") == 0 || strcmp(item, "bluetooth") == 0) {
#if PF_ENABLE_WIRELESS
        const uint8_t mask = strcmp(item, "wifi") == 0 ? PF_WIRELESS_WIFI : PF_WIRELESS_BLE;
        snprintf(line, sizeof(line), "%s=%s (next boot)", item,
                 (pf_boot_config_radios() & mask) != 0u ? "enabled" : "disabled");
        pf_cli_print(line);
#else
        pf_cli_print("ERR radios unavailable");
#endif
    } else return false;
    return true;
}

/** @brief Validate every assignment locally, then stage/save all targets or discard on failure. */
static void set_config(char *assignments)
{
    /* ponytail: four non-secret assignments fit 99 bytes; extend the editor limit and this buffer for more keys. */
    char buffer[100];
    if (assignments == NULL || strlen(assignments) >= sizeof(buffer)) {
        pf_cli_print("ERR configuration assignments must fit in 99 bytes");
        return;
    }
    strcpy(buffer, assignments);
    pf_sysclk_profile_t clock = pf_boot_config_sysclk();
    pf_storage_speed_t speed = pf_boot_config_storage_speed();
    uint8_t radios = pf_boot_config_radios();
    unsigned seen = 0u;
    char *next = buffer;
    while (next != NULL) {
        char *assignment = next;
        next = strchr(assignment, ',');
        if (next != NULL) *next++ = '\0';
        char *equals = strchr(assignment, '=');
        if (equals == NULL) goto invalid;
        *equals++ = '\0';
        const char *key = trim(assignment);
        const char *value = trim(equals);
        unsigned bit;
        if (strcmp(key, "clock") == 0) {
            bit = 1u;
            if (strcmp(value, "default") == 0) clock = PF_SYSCLK_DEFAULT;
            else if (strcmp(value, "150") == 0) clock = PF_SYSCLK_150MHZ;
            else if (strcmp(value, "200") == 0) clock = PF_SYSCLK_200MHZ;
            else if (strcmp(value, "250") == 0) clock = PF_SYSCLK_250MHZ;
            else goto invalid;
        } else if (strcmp(key, "storage") == 0) {
            bit = 2u;
            if (strcmp(value, "default") == 0) speed = PF_STORAGE_SPEED_DEFAULT;
            else if (strcmp(value, "high") == 0) speed = PF_STORAGE_SPEED_HIGH;
            else goto invalid;
#if PF_ENABLE_WIRELESS
        } else if (strcmp(key, "wifi") == 0 || strcmp(key, "bluetooth") == 0) {
            const uint8_t mask = strcmp(key, "wifi") == 0 ? PF_WIRELESS_WIFI : PF_WIRELESS_BLE;
            bit = strcmp(key, "wifi") == 0 ? 4u : 8u;
            if (strcmp(value, "enabled") == 0) radios |= mask;
            else if (strcmp(value, "disabled") == 0) radios &= (uint8_t)~mask;
            else goto invalid;
#endif
        } else goto invalid;
        if ((seen & bit) != 0u) goto invalid;
        seen |= bit;
    }
    if (!restart_allowed()) return;
    if (!pf_boot_config_set_sysclk(clock) || !pf_boot_config_set_storage_speed(speed) ||
        !pf_boot_config_set_radios(radios)) {
        pf_boot_config_discard();
        pf_cli_print("ERR cannot stage settings; no changes saved");
        return;
    }
    if (!pf_boot_config_save()) {
        pf_boot_config_discard();
        pf_cli_print("ERR save unconfirmed; no reboot; inspect config before retrying");
        return;
    }
    pf_cli_print("OK config saved; rebooting");
    reboot_callback();
    return;

invalid:
    pf_cli_print("ERR invalid/duplicate assignment; clock=default|150|200|250, storage=default|high");
#if PF_ENABLE_WIRELESS
    pf_cli_print("wifi=enabled|disabled, bluetooth=enabled|disabled; credentials unchanged");
#endif
}

/** @brief Keep raw assignment spacing while using the existing tokenizer for config get. */
static void config_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    (void)context;
    if (args != NULL && strncmp(args, "set", 3u) == 0 && isspace((unsigned char)args[3])) {
        args += 3u;
        while (isspace((unsigned char)*args)) ++args;
        set_config(args);
        return;
    }
    embeddedCliTokenizeArgs(args);
    const uint16_t count = embeddedCliGetTokenCount(args);
    if ((count == 1u || count == 2u) && strcmp(embeddedCliGetToken(args, 1u), "get") == 0) {
        if (count == 2u) {
            if (!print_config(embeddedCliGetToken(args, 2u))) pf_cli_print("ERR config " CONFIG_HELP);
        } else {
            print_config("clock");
            print_config("storage");
#if PF_ENABLE_WIRELESS
            print_config("wifi");
            print_config("bluetooth");
#endif
        }
    } else pf_cli_print("ERR config " CONFIG_HELP);
}

/** @brief Report USB mass-storage ownership; requests complete asynchronously. */
static void print_usb(void)
{
    char line[80];
    snprintf(line, sizeof(line), "usb=%s ejected=%u pending=%u rejected=%u",
             pf_usb_msc_is_enabled() ? "enabled" : "disabled", pf_usb_msc_is_ejected(),
             pf_usb_msc_transition_pending(), pf_usb_msc_request_rejected());
    pf_cli_print(line);
}

/** @brief Queue a normal MSC ownership request, never bypassing clean host eject. */
static void set_usb(bool enabled)
{
    if (pf_storage_worker_busy() || pf_usb_msc_transition_pending()) {
        pf_cli_print("ERR storage busy");
    } else if (!pf_usb_msc_request(enabled)) {
        pf_cli_print("ERR USB request refused");
    } else pf_cli_print("OK USB request queued; check storage get usb for completion/rejection");
}

#if PF_ENABLE_STORAGE_BENCHMARK
/** @brief Format the retained benchmark snapshot, including admission and cleanup failures. */
static void print_benchmark(void)
{
    static const char *const states[] = {
        "idle", "queued", "preparing", "writing", "reading", "cleanup", "passed", "failed", "cancelled"
    };
    pf_file_benchmark_status_t status;
    char line[160];
    pf_storage_worker_benchmark_status(&status);
    const char *state = (unsigned)status.state < sizeof(states) / sizeof(states[0]) ? states[status.state] : "unknown";
    snprintf(line, sizeof(line), "benchmark=%s target=%lu written=%lu read=%lu mismatches=%lu", state,
             (unsigned long)status.target_bytes, (unsigned long)status.written_bytes, (unsigned long)status.read_bytes,
             (unsigned long)status.mismatch_count);
    pf_cli_print(line);
    snprintf(line, sizeof(line), "write_ms=%lu read_ms=%lu error=%u filesystem_result=%u request_error=%u flags=%u",
             (unsigned long)status.write_elapsed_ms, (unsigned long)status.read_elapsed_ms,
             status.error, status.filesystem_result, status.request_error, status.flags);
    pf_cli_print(line);
}
#endif

/** @brief Start or cancel through worker admission; `storage get benchmark` reports asynchronous outcomes. */
static void benchmark_request(const char *value)
{
#if PF_ENABLE_STORAGE_BENCHMARK
    if (strcmp(value, "cancel") == 0) {
        pf_cli_print(pf_storage_worker_benchmark_cancel() ? "OK cancellation requested; wait for cleanup" :
                                                          "ERR no cancellable run or admission/cleanup busy");
        return;
    }
    uint32_t size = 0u;
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9' || size > PF_FILE_BENCHMARK_MAX_MIB) {
            pf_cli_print("ERR benchmark size must be 1..1024 MiB");
            return;
        }
        size = size * 10u + (unsigned)(*p - '0');
    }
    if (size == 0u || size > PF_FILE_BENCHMARK_MAX_MIB) {
        pf_cli_print("ERR benchmark size must be 1..1024 MiB");
    } else if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
        pf_cli_print("ERR USB owns storage or ownership transition pending");
    } else if (pf_storage_worker_benchmark_start(size, false)) {
        pf_cli_print("OK benchmark queued, not yet admitted; check storage get benchmark");
    } else {
        pf_cli_print("ERR benchmark request refused");
        print_benchmark();
    }
#else
    (void)value;
    pf_cli_print("ERR benchmark unavailable in this build");
#endif
}

bool pf_cli_device_storage(const char *args)
{
    char tokens[PF_CLI_DEVICE_ARGS_SIZE] = { 0 };
    const size_t length = args != NULL ? strlen(args) : 0u;
    if (length > sizeof(tokens) - 2u) {
        pf_cli_print("ERR storage arguments too long");
        return true;
    }
    if (length > 0u) memcpy(tokens, args, length);
    embeddedCliTokenizeArgs(tokens);
    const uint16_t count = embeddedCliGetTokenCount(tokens);
    const char *verb = count > 0u ? embeddedCliGetToken(tokens, 1u) : "";
    const char *item = count > 1u ? embeddedCliGetToken(tokens, 2u) : "";
    const char *value = count > 2u ? embeddedCliGetToken(tokens, 3u) : "";

    if ((strcmp(verb, "get") == 0 && (count == 1u || (count == 2u && strcmp(item, "status") == 0))) ||
        (count == 1u && (strcmp(verb, "mount") == 0 || strcmp(verb, "unmount") == 0)) ||
        (count == 2u && strcmp(verb, "format") == 0 && strcmp(item, "--confirm") == 0)) {
        return false; /* Status and volume maintenance belong to the storage worker. */
    }
    if (count == 2u && strcmp(verb, "get") == 0 && strcmp(item, "usb") == 0) {
        print_usb();
    } else if (count == 2u && strcmp(verb, "get") == 0 && strcmp(item, "benchmark") == 0) {
#if PF_ENABLE_STORAGE_BENCHMARK
        print_benchmark();
#else
        pf_cli_print("ERR benchmark unavailable in this build");
#endif
    } else if (count == 3u && strcmp(verb, "set") == 0 && strcmp(item, "usb") == 0 &&
               (strcmp(value, "on") == 0 || strcmp(value, "off") == 0)) {
        set_usb(strcmp(value, "on") == 0);
    } else if (count == 2u && strcmp(verb, "benchmark") == 0) {
        benchmark_request(item);
    } else if (count == 2u && strcmp(verb, "recover") == 0 && strcmp(item, "--confirm") == 0) {
        pf_usb_msc_request_recovery();
        pf_cli_print("Fault recovery requested, not clean eject. Data may be lost; inspect before storage mount. "
                     "Check UART recovery result and fresh storage get status.");
    } else {
        pf_cli_print("ERR storage " PF_CLI_STORAGE_HELP);
    }
    return true;
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
    static const char *const wifi_states[] = { "disabled", "connecting", "connected", "failed" };
    static const char *const ble_states[] = { "disabled", "advertising", "connected" };
    uint8_t report[PF_WIRELESS_STATUS_SIZE];
    char line[120];
    pf_wireless_status(report);
    snprintf(line, sizeof(line), "wifi=%s ip=%u.%u.%u.%u",
             report[4] < 4u ? wifi_states[report[4]] : "unknown", report[5], report[6], report[7], report[8]);
    pf_cli_print(line);
    snprintf(line, sizeof(line), "bluetooth=%s", report[45] < 3u ? ble_states[report[45]] : "unknown");
    pf_cli_print(line);
#else
    pf_cli_print("ERR radios unavailable in this build");
#endif
}

bool pf_cli_device_register(void (*request_reboot)(void))
{
    if (request_reboot == NULL) return false;
    reboot_callback = request_reboot;
    return pf_cli_add_binding((CliCommandBinding){ "board", BOARD_HELP, true, NULL, board_command }) &&
           pf_cli_add_binding((CliCommandBinding){ "config", CONFIG_HELP, false, NULL, config_command }) &&
           pf_cli_add_binding((CliCommandBinding){ "radio", RADIO_HELP, true, NULL, radio_command });
}
