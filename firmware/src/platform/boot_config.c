// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file boot_config.c
 * @brief Boot configuration: one RAM record, validation, and private flash persistence.
 */
#include "platform/boot_config.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "task.h"

#define MAGIC 0x46434650u /* "PFCF" in little-endian byte order */
#define FORMAT_VERSION 2u
#define SAVE_TIMEOUT_MS 1000u
#define LOCKOUT_RECLAIM_TIMEOUT_MS 1000u
#define TCP_CLI_DISABLED 4u /**< Private persisted service bit, independent of the two radio switches. */
#define CONTROL_KEY_PRESENT 1u /**< Byte 141 bit 0 distinguishes every raw key from absence. */

#define BOOT_CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

_Static_assert(BOOT_CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0u, "config sector must be aligned");

/** Private saved wireless settings; erased or invalid records contain no credentials. */
typedef struct {
    uint8_t wifi_auth_state; /**< pf_wifi_auth_state_t; zero means try WPA2. */
    uint8_t ssid_length; /**< Opaque SSID byte count, zero through 32. */
    uint8_t ssid[32]; /**< Opaque SSID, zero padded; embedded zero bytes are significant. */
    char password[PF_WIFI_PASSWORD_SIZE]; /**< NUL-terminated WPA2/WPA3 passphrase. */
    uint8_t ble_key[32]; /**< Raw Control HMAC-SHA256 key; presence is independent of its bytes. */
} wireless_settings_t;

/**
 * @brief v2 flash record: explicit SSID length, profiles, switches and credentials under one CRC.
 *
 * A torn or partial write fails the CRC, so the record loads all or nothing.
 *
 * ponytail: written and read as raw memory, which matches the flash byte order only on a little-endian CPU (RP2040,
 * RP2350 and the host tests). The static assert guards the unpadded layout. Upgrade path for a big-endian target:
 * serialize each field explicitly.
 */
typedef struct {
    uint32_t magic; /**< MAGIC. */
    uint16_t version; /**< FORMAT_VERSION. */
    uint16_t size; /**< sizeof(record_t). */
    uint8_t sysclk_profile; /**< pf_sysclk_profile_t value, independent of enum storage size. */
    uint8_t sdcard_profile; /**< Shared pf_storage_speed_t value for either transport. */
    uint8_t radios_disabled; /**< Disabled PF_WIRELESS_* radios and the TCP_CLI_DISABLED service bit. */
    wireless_settings_t wireless; /**< Private saved credentials. */
    uint8_t reserved[3]; /**< Byte 141 bit 0 is key presence; remaining bits and bytes 142-143 are zero. */
    uint32_t crc; /**< IEEE CRC-32 of all preceding bytes. */
} record_t;

_Static_assert(sizeof(wireless_settings_t) == 130u && offsetof(record_t, wireless) == 11u &&
               offsetof(record_t, reserved) == 141u && offsetof(record_t, crc) == 144u &&
               sizeof(record_t) == 148u, "v2 record layout");
_Static_assert(sizeof(record_t) <= FLASH_PAGE_SIZE, "settings fit one flash page");

static record_t config; /**< RAM record; zero profiles select defaults, and save refreshes metadata and CRC. */
static uint8_t boot_radios_disabled; /**< Wireless/radio service flags captured with the boot configuration. */
static bool boot_key_present; /**< Control-key presence captured at boot, including an all-zero raw key. */
static wireless_settings_t boot_wireless_settings; /**< Credential/settings snapshot used for restart diagnostics. */
static bool loaded; /**< The saved record has been read once. */
static bool boot_snapshot_valid; /**< Original saved values are available for restart comparison. */
/** Prevents access while flash_safe_execute is active. */
static bool save_in_progress;
/** Task count before flash_safe_execute; valid while lockout_pending. */
static UBaseType_t tasks_before_lockout;
/** True while the SDK's static lockout task may not have been reclaimed yet. */
static bool lockout_pending;

static const uint8_t *stored_data(void)
{
    return (const uint8_t *)(XIP_BASE + BOOT_CONFIG_FLASH_OFFSET);
}

static bool task_context(void)
{
    return !xPortIsInsideInterrupt() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/** Runs with interrupts disabled and the other core held out of flash. */
static void erase_and_program(void *param)
{
    flash_range_erase(BOOT_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(BOOT_CONFIG_FLASH_OFFSET, param, FLASH_PAGE_SIZE);
}

/** Wait for the idle task to reclaim the SDK's static lockout TCB. */
static bool lockout_reclaimed(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (uxTaskGetNumberOfTasks() != tasks_before_lockout) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/** @brief IEEE 802.3 CRC-32 (reflected, init and final XOR 0xFFFFFFFF). */
static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    while (size-- > 0u) {
        crc ^= *bytes++;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/** @brief Validate a bounded UTF-8 SSID without reading beyond its fixed-size storage. */
static bool wifi_ssid_valid(const char *ssid)
{
    if (ssid == NULL) return false;
    const unsigned char *text = (const unsigned char *)ssid;
    const unsigned char *terminator = memchr(text, '\0', PF_WIFI_SSID_SIZE);
    if (terminator == NULL) return false;
    size_t length = (size_t)(terminator - text);
    for (size_t i = 0u; text[i] != 0u;) {
        uint32_t code = text[i++];
        unsigned extra = code < 0x80u ? 0u : code >= 0xc2u && code <= 0xdfu ? 1u :
            code >= 0xe0u && code <= 0xefu ? 2u : code >= 0xf0u && code <= 0xf4u ? 3u : 4u;
        if (extra == 4u || i + extra > length) return false;
        const uint32_t minimum = extra == 0u ? 0u : extra == 1u ? 0x80u : extra == 2u ? 0x800u : 0x10000u;
        if (extra != 0u) code &= (1u << (6u - extra)) - 1u;
        for (unsigned j = 0u; j < extra; ++j) {
            if ((text[i] & 0xc0u) != 0x80u) return false;
            code = (code << 6u) | (text[i++] & 0x3fu);
        }
        if (code < minimum || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) return false;
    }
    return true;
}

/** @brief Validate an optional WPA2/WPA3 passphrase in its fixed-size storage. */
static bool wifi_password_valid(const char *password)
{
    if (password == NULL) return false;
    const unsigned char *text = (const unsigned char *)password;
    const unsigned char *terminator = memchr(text, '\0', PF_WIFI_PASSWORD_SIZE);
    if (terminator == NULL) return false;
    size_t length = (size_t)(terminator - text);
    if (length != 0u && (length < 8u || length > 63u)) return false;
    for (size_t i = 0u; i < length; ++i) {
        if (text[i] < 32u || text[i] > 126u) return false;
    }
    return true;
}

/** @brief Return whether any raw key byte is nonzero, for v1 presence inference and canonical absent records. */
static bool key_nonzero(const uint8_t key[32])
{
    uint8_t bits = 0u;
    for (size_t i = 0u; i < 32u; ++i) bits |= key[i];
    return bits != 0u;
}

/** @brief Reject malformed records before publishing their values to the RAM copy. */
static bool record_valid(const record_t *record)
{
    return record->magic == MAGIC && record->version == FORMAT_VERSION && record->size == sizeof(record_t) &&
           record->crc == crc32(record, offsetof(record_t, crc)) && record->sysclk_profile <= PF_SYSCLK_250MHZ &&
           record->sdcard_profile <= PF_STORAGE_SPEED_HIGH &&
           (record->radios_disabled & ~(PF_WIRELESS_WIFI | PF_WIRELESS_BLE | TCP_CLI_DISABLED)) == 0u &&
           record->wireless.wifi_auth_state <= PF_WIFI_AUTH_SAE &&
           (record->reserved[0] & ~CONTROL_KEY_PRESENT) == 0u &&
           record->reserved[1] == 0u && record->reserved[2] == 0u &&
           ((record->reserved[0] & CONTROL_KEY_PRESENT) != 0u || !key_nonzero(record->wireless.ble_key)) &&
           record->wireless.ssid_length <= 32u && wifi_password_valid(record->wireless.password);
}

/** @copydoc pf_boot_config_discard */
void pf_boot_config_discard(void)
{
    loaded = false;
    memset(&config, 0, sizeof(config));
}

/**
 * @brief Load the saved record into the RAM copy once.
 *
 * ponytail: lazy load on the first getter. main() loads settings before the scheduler, so in firmware the first
 * call reads flash directly; a running task can also load them if not already read. A
 * malformed record latches defaults until reboot; an unsafe context leaves loading pending for a safe caller.
 */
static void ensure_loaded(void)
{
    record_t record;
    if (loaded) {
        return;
    }
    if (save_in_progress || xPortIsInsideInterrupt() ||
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED && !task_context())) {
        return;
    }
    memcpy(&record, stored_data(), sizeof(record));
    /* v1 used the same 33 bytes for a UTF-8 C string. Validate its original CRC before converting in RAM. */
    if (record.version == 1u && record.crc == crc32(&record, offsetof(record_t, crc)) &&
        record.reserved[0] == 0u && record.reserved[1] == 0u && record.reserved[2] == 0u) {
        char legacy_ssid[PF_WIFI_SSID_SIZE];
        memcpy(legacy_ssid, &record.wireless.ssid_length, sizeof(legacy_ssid));
        if (wifi_ssid_valid(legacy_ssid)) {
            const uint8_t length = (uint8_t)strlen(legacy_ssid);
            record.wireless.ssid_length = length;
            memset(record.wireless.ssid, 0, sizeof(record.wireless.ssid));
            memcpy(record.wireless.ssid, legacy_ssid, length);
            record.reserved[0] = key_nonzero(record.wireless.ble_key) ? CONTROL_KEY_PRESENT : 0u;
            record.version = FORMAT_VERSION;
            record.crc = crc32(&record, offsetof(record_t, crc));
        }
    }
    memset(&config, 0, sizeof(config));
    if (record_valid(&record)) {
        config = record;
    }
    boot_radios_disabled = config.radios_disabled;
    boot_key_present = (config.reserved[0] & CONTROL_KEY_PRESENT) != 0u;
    boot_wireless_settings = config.wireless;
    boot_snapshot_valid = true;
    loaded = true;
}

/** @copydoc pf_boot_config_wifi_ssid */
void pf_boot_config_wifi_ssid(char ssid[PF_WIFI_SSID_SIZE])
{
    ensure_loaded();
    memset(ssid, 0, PF_WIFI_SSID_SIZE);
    memcpy(ssid, config.wireless.ssid, config.wireless.ssid_length);
}

/** @copydoc pf_boot_config_set_wifi_ssid */
bool pf_boot_config_set_wifi_ssid(const char *ssid)
{
    ensure_loaded();
    if (!loaded || !wifi_ssid_valid(ssid)) return false;
    return pf_boot_config_set_wifi_ssid_bytes((const uint8_t *)ssid, (uint8_t)strlen(ssid));
}

/** @copydoc pf_boot_config_wifi_ssid_bytes */
uint8_t pf_boot_config_wifi_ssid_bytes(uint8_t ssid[32])
{
    ensure_loaded();
    memcpy(ssid, config.wireless.ssid, sizeof(config.wireless.ssid));
    return config.wireless.ssid_length;
}

/** @copydoc pf_boot_config_set_wifi_ssid_bytes */
bool pf_boot_config_set_wifi_ssid_bytes(const uint8_t *ssid, uint8_t length)
{
    ensure_loaded();
    if (!loaded || length > 32u || (ssid == NULL && length != 0u)) return false;
    memset(config.wireless.ssid, 0, sizeof(config.wireless.ssid));
    if (length != 0u) memcpy(config.wireless.ssid, ssid, length);
    config.wireless.ssid_length = length;
    return true;
}

/** @copydoc pf_boot_config_wifi_password */
void pf_boot_config_wifi_password(char password[PF_WIFI_PASSWORD_SIZE])
{
    ensure_loaded();
    memcpy(password, config.wireless.password, sizeof(config.wireless.password));
}

/** @copydoc pf_boot_config_set_wifi_password */
bool pf_boot_config_set_wifi_password(const char *password)
{
    ensure_loaded();
    if (!loaded || !wifi_password_valid(password)) return false;
    size_t length = strlen(password);
    memset(config.wireless.password, 0, sizeof(config.wireless.password));
    memcpy(config.wireless.password, password, length + 1u);
    return true;
}

/** @copydoc pf_boot_config_wifi_password_configured */
bool pf_boot_config_wifi_password_configured(void)
{
    ensure_loaded();
    return config.wireless.password[0] != '\0';
}

/** @copydoc pf_boot_config_ble_key */
void pf_boot_config_ble_key(uint8_t key[32])
{
    ensure_loaded();
    memcpy(key, config.wireless.ble_key, sizeof(config.wireless.ble_key));
}

/** @copydoc pf_boot_config_set_ble_key */
bool pf_boot_config_set_ble_key(const uint8_t key[32])
{
    ensure_loaded();
    if (!loaded) return false;
    memset(config.wireless.ble_key, 0, sizeof(config.wireless.ble_key));
    if (key != NULL) memcpy(config.wireless.ble_key, key, sizeof(config.wireless.ble_key));
    config.reserved[0] = key != NULL ? CONTROL_KEY_PRESENT : 0u;
    return true;
}

/** @copydoc pf_boot_config_ble_key_configured */
bool pf_boot_config_ble_key_configured(void)
{
    ensure_loaded();
    return (config.reserved[0] & CONTROL_KEY_PRESENT) != 0u;
}

pf_sysclk_profile_t pf_boot_config_sysclk(void)
{
    ensure_loaded();
    return (pf_sysclk_profile_t)config.sysclk_profile;
}

bool pf_boot_config_set_sysclk(pf_sysclk_profile_t profile)
{
    ensure_loaded();
    if (!loaded || (unsigned)profile > PF_SYSCLK_250MHZ) {
        return false;
    }
    config.sysclk_profile = (uint8_t)profile;
    return true;
}

pf_storage_speed_t pf_boot_config_storage_speed(void)
{
    ensure_loaded();
    return (pf_storage_speed_t)config.sdcard_profile;
}

bool pf_boot_config_set_storage_speed(pf_storage_speed_t speed)
{
    ensure_loaded();
    if (!loaded || (speed != PF_STORAGE_SPEED_DEFAULT && speed != PF_STORAGE_SPEED_HIGH)) {
        return false;
    }
    config.sdcard_profile = (uint8_t)speed;
    return true;
}

/** @copydoc pf_boot_config_radios */
uint8_t pf_boot_config_radios(void)
{
    ensure_loaded();
    return (uint8_t)(~config.radios_disabled & (PF_WIRELESS_WIFI | PF_WIRELESS_BLE));
}

/** @copydoc pf_boot_config_set_radios */
bool pf_boot_config_set_radios(uint8_t radios)
{
    ensure_loaded();
    if (!loaded || (radios & ~(PF_WIRELESS_WIFI | PF_WIRELESS_BLE)) != 0u) return false;
    config.radios_disabled = (uint8_t)((config.radios_disabled & TCP_CLI_DISABLED) |
                                     (~radios & (PF_WIRELESS_WIFI | PF_WIRELESS_BLE)));
    return true;
}

/** @copydoc pf_boot_config_tcp_cli_enabled */
bool pf_boot_config_tcp_cli_enabled(void)
{
    ensure_loaded();
    return (config.radios_disabled & TCP_CLI_DISABLED) == 0u;
}

/** @copydoc pf_boot_config_set_tcp_cli_enabled */
bool pf_boot_config_set_tcp_cli_enabled(bool enabled)
{
    ensure_loaded();
    if (!loaded) return false;
    config.radios_disabled = (uint8_t)((config.radios_disabled & ~TCP_CLI_DISABLED) |
                                     (enabled ? 0u : TCP_CLI_DISABLED));
    return true;
}

/** @copydoc pf_boot_config_wifi_auth_state */
pf_wifi_auth_state_t pf_boot_config_wifi_auth_state(void)
{
    ensure_loaded();
    return (pf_wifi_auth_state_t)config.wireless.wifi_auth_state;
}

/** @copydoc pf_boot_config_wireless_restart_required */
bool pf_boot_config_wireless_restart_required(void)
{
    ensure_loaded();
    const uint8_t wireless_flags = PF_WIRELESS_WIFI | PF_WIRELESS_BLE | TCP_CLI_DISABLED;
    return loaded && boot_snapshot_valid &&
        (((config.radios_disabled ^ boot_radios_disabled) & wireless_flags) != 0u ||
         ((config.reserved[0] & CONTROL_KEY_PRESENT) != 0u) != boot_key_present ||
         memcmp(&config.wireless, &boot_wireless_settings, sizeof(config.wireless)) != 0);
}

/** @copydoc pf_boot_config_set_wifi_auth_state */
bool pf_boot_config_set_wifi_auth_state(pf_wifi_auth_state_t state)
{
    ensure_loaded();
    if (!loaded || state < PF_WIFI_AUTH_TRY_WPA2 || state > PF_WIFI_AUTH_SAE) return false;
    config.wireless.wifi_auth_state = (uint8_t)state;
    return true;
}

bool pf_boot_config_save(void)
{
    uint8_t page[FLASH_PAGE_SIZE];
    if (save_in_progress) return false;
    if (!task_context() || (lockout_pending && !lockout_reclaimed(0u))) {
        pf_boot_config_discard();
        return false;
    }
    lockout_pending = false;
    ensure_loaded();
    config.magic = MAGIC;
    config.version = FORMAT_VERSION;
    config.size = sizeof(config);
    config.reserved[0] &= CONTROL_KEY_PRESENT;
    config.reserved[1] = config.reserved[2] = 0u;
    config.crc = crc32(&config, offsetof(record_t, crc));
    memset(page, 0xff, sizeof(page));
    memcpy(page, &config, sizeof(config));
    if (memcmp(stored_data(), page, sizeof(config)) == 0) return true;
    save_in_progress = true;
    tasks_before_lockout = uxTaskGetNumberOfTasks();
    int rc = flash_safe_execute(erase_and_program, page, SAVE_TIMEOUT_MS);
    lockout_pending = !lockout_reclaimed(LOCKOUT_RECLAIM_TIMEOUT_MS);
    save_in_progress = false;
    bool saved = rc == PICO_OK && memcmp(stored_data(), page, sizeof(config)) == 0;
    if (!saved) pf_boot_config_discard();
    return saved;
}
