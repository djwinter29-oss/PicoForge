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
#define FORMAT_VERSION 1u
#define SAVE_TIMEOUT_MS 1000u
#define LOCKOUT_RECLAIM_TIMEOUT_MS 1000u

#define BOOT_CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

_Static_assert(BOOT_CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0u, "config sector must be aligned");

/**
 * @brief v1 flash record, stored little-endian with one byte per profile.
 *
 * ponytail: written and read as raw memory, which matches the flash byte order only on a little-endian CPU (RP2040,
 * RP2350 and the host tests). Every field is naturally aligned, so there is no padding; the static assert guards the
 * layout. Upgrade path for a big-endian target: serialize each field explicitly.
 */
typedef struct {
    uint32_t magic; /**< MAGIC. */
    uint16_t version; /**< FORMAT_VERSION. */
    uint16_t size; /**< sizeof(record_t). */
    uint8_t sysclk_profile; /**< pf_sysclk_profile_t value, independent of enum storage size. */
    uint8_t sdcard_profile; /**< Shared pf_storage_speed_t value for either transport. */
    uint8_t radios_disabled; /**< PF_WIRELESS_* radios turned off; zero (erased or older record) enables both. */
    uint8_t wireless_required; /**< 1 when the record is only valid with its wireless extension; 0 in older records. */
    uint32_t crc; /**< IEEE CRC-32 of all preceding bytes. */
} record_t;

_Static_assert(sizeof(record_t) == 16u && offsetof(record_t, crc) == 12u, "v1 record layout");

static record_t config; /**< RAM record; zero profiles select defaults, and save refreshes metadata and CRC. */
/** Private saved wireless credentials; byte zero preserves the legacy extension layout. */
typedef struct {
    uint8_t reserved; /**< Former provisioning flags; ignored when loading older records. */
    char ssid[33]; /**< NUL-terminated Wi-Fi SSID. */
    char password[64]; /**< NUL-terminated WPA2 passphrase. */
    uint8_t ble_key[32]; /**< BLE control HMAC-SHA256 key. */
} wireless_settings_t;
/** Private extension after the unchanged v1 profile record, within the same flash page. */
typedef struct {
    uint32_t magic; /**< "PFWL", distinct from the profile record. */
    uint16_t version; /**< Extension version 1. */
    uint16_t size; /**< Complete extension size. */
    wireless_settings_t settings; /**< Private saved credentials. */
    uint16_t reserved; /**< Zero padding before the CRC. */
    uint32_t crc; /**< CRC-32 of all preceding extension bytes. */
} wireless_record_t;

_Static_assert(sizeof(wireless_settings_t) == 130u && sizeof(wireless_record_t) == 144u,
               "wireless extension layout");
_Static_assert(sizeof(record_t) + sizeof(wireless_record_t) <= FLASH_PAGE_SIZE, "settings fit one flash page");
static wireless_record_t wireless; /**< Private extension; erased/invalid records contain no credentials. */
static bool loaded; /**< The saved record has been read once. */
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

/** @brief Reject malformed records before publishing their profiles to the RAM copy. */
static bool record_valid(const record_t *record)
{
    return record->magic == MAGIC && record->version == FORMAT_VERSION && record->size == sizeof(record_t) &&
           record->crc == crc32(record, offsetof(record_t, crc)) && record->sysclk_profile <= PF_SYSCLK_250MHZ &&
           record->sdcard_profile <= PF_STORAGE_SPEED_HIGH &&
           (record->radios_disabled & ~(PF_WIRELESS_WIFI | PF_WIRELESS_BLE)) == 0u && record->wireless_required <= 1u;
}

/** @brief Validate a bounded UTF-8 SSID without reading beyond its fixed-size storage. */
static bool wifi_ssid_valid(const char *ssid)
{
    if (ssid == NULL) return false;
    const unsigned char *text = (const unsigned char *)ssid;
    const unsigned char *terminator = memchr(text, '\0', 33u);
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

/** @brief Validate an optional WPA2 passphrase in its fixed-size storage. */
static bool wifi_password_valid(const char *password)
{
    if (password == NULL) return false;
    const unsigned char *text = (const unsigned char *)password;
    const unsigned char *terminator = memchr(text, '\0', 64u);
    if (terminator == NULL) return false;
    size_t length = (size_t)(terminator - text);
    if (length != 0u && (length < 8u || length > 63u)) return false;
    for (size_t i = 0u; i < length; ++i) {
        if (text[i] < 32u || text[i] > 126u) return false;
    }
    return true;
}

/** @brief Validate persisted credentials independently of the radio master switches. */
static bool wireless_settings_valid(const wireless_record_t *settings)
{
    return wifi_ssid_valid(settings->settings.ssid) && wifi_password_valid(settings->settings.password);
}

/** @copydoc pf_boot_config_discard */
void pf_boot_config_discard(void)
{
    loaded = false;
    memset(&config, 0, sizeof(config));
    memset(&wireless, 0, sizeof(wireless));
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
    wireless_record_t extension;
    memcpy(&record, stored_data(), sizeof(record));
    memcpy(&extension, stored_data() + sizeof(record), sizeof(extension));
    const bool extension_valid = extension.magic == 0x4c574650u && extension.version == 1u &&
        extension.size == sizeof(extension) && extension.reserved == 0u &&
        extension.crc == crc32(&extension, offsetof(wireless_record_t, crc)) && wireless_settings_valid(&extension);
    memset(&config, 0, sizeof(config));
    memset(&wireless, 0, sizeof(wireless));
    /* A save torn after the profile record loads defaults, never new profiles without their credentials. */
    if (record_valid(&record) && (extension_valid || record.wireless_required == 0u)) {
        config = record;
        if (extension_valid) {
            extension.settings.reserved = 0u;
            wireless = extension;
        }
    }
    loaded = true;
}

/** @copydoc pf_boot_config_wifi_ssid */
void pf_boot_config_wifi_ssid(char ssid[33])
{
    ensure_loaded();
    memcpy(ssid, wireless.settings.ssid, sizeof(wireless.settings.ssid));
}

/** @copydoc pf_boot_config_set_wifi_ssid */
bool pf_boot_config_set_wifi_ssid(const char *ssid)
{
    ensure_loaded();
    if (!loaded || !wifi_ssid_valid(ssid)) return false;
    size_t length = strlen(ssid);
    memset(wireless.settings.ssid, 0, sizeof(wireless.settings.ssid));
    memcpy(wireless.settings.ssid, ssid, length + 1u);
    return true;
}

/** @copydoc pf_boot_config_wifi_password */
void pf_boot_config_wifi_password(char password[64])
{
    ensure_loaded();
    memcpy(password, wireless.settings.password, sizeof(wireless.settings.password));
}

/** @copydoc pf_boot_config_set_wifi_password */
bool pf_boot_config_set_wifi_password(const char *password)
{
    ensure_loaded();
    if (!loaded || !wifi_password_valid(password)) return false;
    size_t length = strlen(password);
    memset(wireless.settings.password, 0, sizeof(wireless.settings.password));
    memcpy(wireless.settings.password, password, length + 1u);
    return true;
}

/** @copydoc pf_boot_config_ble_key */
void pf_boot_config_ble_key(uint8_t key[32])
{
    ensure_loaded();
    memcpy(key, wireless.settings.ble_key, sizeof(wireless.settings.ble_key));
}

/** @copydoc pf_boot_config_set_ble_key */
bool pf_boot_config_set_ble_key(const uint8_t key[32])
{
    ensure_loaded();
    if (!loaded || key == NULL) return false;
    memcpy(wireless.settings.ble_key, key, sizeof(wireless.settings.ble_key));
    return true;
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
    config.radios_disabled = (uint8_t)(~radios & (PF_WIRELESS_WIFI | PF_WIRELESS_BLE));
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
    config.wireless_required = 1u;
    config.crc = crc32(&config, offsetof(record_t, crc));
    wireless.magic = 0x4c574650u;
    wireless.version = 1u;
    wireless.size = sizeof(wireless);
    wireless.settings.reserved = 0u;
    wireless.reserved = 0u;
    wireless.crc = crc32(&wireless, offsetof(wireless_record_t, crc));
    memset(page, 0xff, sizeof(page));
    memcpy(page, &config, sizeof(config));
    memcpy(page + sizeof(config), &wireless, sizeof(wireless));
    if (memcmp(stored_data(), page, sizeof(config) + sizeof(wireless)) == 0) return true;
    save_in_progress = true;
    tasks_before_lockout = uxTaskGetNumberOfTasks();
    int rc = flash_safe_execute(erase_and_program, page, SAVE_TIMEOUT_MS);
    lockout_pending = !lockout_reclaimed(LOCKOUT_RECLAIM_TIMEOUT_MS);
    save_in_progress = false;
    bool saved = rc == PICO_OK && memcmp(stored_data(), page, sizeof(config) + sizeof(wireless)) == 0;
    if (!saved) pf_boot_config_discard();
    return saved;
}
