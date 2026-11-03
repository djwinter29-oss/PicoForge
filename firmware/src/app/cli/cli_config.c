/**
 * @file
 * @brief Boot configuration command handlers.
 */
#include "cli_config.h"
#include "cli.h"
#include "cli_internal.h"
#include "platform/boot_config.h"
#if PF_ENABLE_WIRELESS
#include "app/wireless/wireless.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/** Configuration values are saved targets, not running hardware state. */
#define CONFIG_HELP \
    "get [clock | storage | wifi | bluetooth]\r\n" \
    "\tset key=value[,key=value...] (saves and reboots)"

static void (*reboot_callback)(void); /**< Borrowed USB owner task reset scheduler. */

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
    if (!pf_cli_restart_allowed()) return;
    if (!pf_boot_config_set_sysclk(clock) || !pf_boot_config_set_storage_speed(speed) ||
        !pf_boot_config_set_radios(radios)) {
        pf_boot_config_discard();
        pf_cli_print("ERR cannot stage settings; no changes saved");
        return;
    }
    if (!pf_boot_config_save()) {
        pf_boot_config_discard();
        pf_cli_print("WARN Save unconfirmed; no reboot; inspect config before retrying");
        return;
    }
    pf_cli_print("OK config saved");
    pf_cli_print("QUEUED reboot; waiting for CLI output to flush");
    reboot_callback();
    return;

invalid:
    pf_cli_print("ERR invalid/duplicate assignment; clock=default|150|200|250, storage=default|high");
#if PF_ENABLE_WIRELESS
    pf_cli_print("wifi=enabled|disabled, bluetooth=enabled|disabled; credentials unchanged");
#endif
}

/** @brief Keep raw assignment spacing while using the existing tokenizer for config get. */
void pf_cli_config_command(EmbeddedCli *instance, char *args, void *context)
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

bool pf_cli_config_register(void (*request_reboot)(void))
{
    if (request_reboot == NULL) return false;
    reboot_callback = request_reboot;
    return pf_cli_add_binding((CliCommandBinding){ "config", CONFIG_HELP, false, NULL, pf_cli_config_command });
}
