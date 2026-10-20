#ifndef PF_USB_HID_H
#define PF_USB_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/system.h"
#include "pico/unique_id.h"

#define PF_HID_INFO_REPORT_ID 1u
#define PF_HID_STATUS_REPORT_ID 2u
#define PF_HID_MEMORY_REPORT_ID 3u
#define PF_HID_LED_COMMAND_REPORT_ID 4u
#define PF_HID_MSC_COMMAND_REPORT_ID 5u
#define PF_HID_SELF_TEST_REPORT_ID 6u
#define PF_HID_FILESYSTEM_REPORT_ID 7u
#define PF_HID_SETTINGS_REPORT_ID 8u
#define PF_HID_REBOOT_REPORT_ID 9u
#define PF_HID_REPORT_VERSION 2u
/** @brief Identity layout with a firmware release version. */
#define PF_HID_INFO_REPORT_VERSION 3u
/** @brief Runtime layout with interval CPU usage and static SRAM reservations. */
#define PF_HID_STATUS_REPORT_VERSION 1u
#define PF_HID_PRODUCT_NAME "PicoForge"
#define PF_HID_PRODUCT_NAME_SIZE 20u
#define PF_HID_SERIAL_NUMBER_SIZE (2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES)
#define PF_HID_INFO_PAYLOAD_SIZE (2u + PF_HID_PRODUCT_NAME_SIZE + PF_HID_SERIAL_NUMBER_SIZE + 4u)
#define PF_HID_STATUS_PAYLOAD_SIZE 25u
#define PF_HID_MEMORY_PAYLOAD_SIZE 29u
#define PF_HID_LED_COMMAND_PAYLOAD_SIZE 2u
#define PF_HID_MSC_COMMAND_PAYLOAD_SIZE 2u
#define PF_HID_MSC_STATUS_PAYLOAD_SIZE 4u
/** @brief MSC status layout advertises confirmed force-return command support. */
#define PF_HID_MSC_REPORT_VERSION 3u
#define PF_HID_SELF_TEST_COMMAND_PAYLOAD_SIZE 6u
#define PF_HID_SELF_TEST_STATUS_PAYLOAD_SIZE 32u
#define PF_HID_FILESYSTEM_PAYLOAD_SIZE 3u
#define PF_HID_SETTINGS_REPORT_VERSION 1u
#define PF_HID_SETTINGS_PAYLOAD_SIZE 5u
#define PF_HID_REBOOT_REPORT_VERSION 1u
#define PF_HID_REBOOT_COMMAND_PAYLOAD_SIZE 2u
#define PF_HID_REBOOT_DELAY_MS 1000u

#define PF_MCU_RP2040 PF_SYSTEM_MCU_RP2040
#define PF_MCU_RP2350 PF_SYSTEM_MCU_RP2350

#define PF_HID_STATUS_USB_MOUNTED (1u << 0)
#define PF_HID_STATUS_CDC_OPEN (1u << 1)
#define PF_HID_STATUS_TEMPERATURE_VALID (1u << 2)
#define PF_HID_STATUS_LED_AVAILABLE (1u << 3)
#define PF_HID_STATUS_LED_ON (1u << 4)

#define PF_HID_MEMORY_CONFIGURED (1u << 0)
#define PF_HID_MEMORY_PRESENCE_KNOWN (1u << 1)
#define PF_HID_MEMORY_PRESENT (1u << 2)
#define PF_HID_MEMORY_INITIALIZED (1u << 3)
#define PF_HID_MEMORY_CID_VALID (1u << 4)
#define PF_HID_MEMORY_CAPACITY_VALID (1u << 5)
#define PF_HID_MEMORY_TYPE_SD_CARD 1u
#define PF_HID_MEMORY_MODE_UNKNOWN 0u
#define PF_HID_MEMORY_MODE_SDIO 1u
#define PF_HID_MEMORY_MODE_SPI 2u

#define PF_HID_FILESYSTEM_UNKNOWN 0u
#define PF_HID_FILESYSTEM_FAT12 1u
#define PF_HID_FILESYSTEM_FAT16 2u
#define PF_HID_FILESYSTEM_FAT32 3u
#define PF_HID_FILESYSTEM_EXFAT 4u

/** Stable product/device identity returned by HID Feature report 1. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Identity layout version, not the firmware version. */
    uint8_t mcu_id; /**< SDK-selected MCU family. */
    char product_name[PF_HID_PRODUCT_NAME_SIZE]; /**< Zero-padded product name. */
    char serial_number[PF_HID_SERIAL_NUMBER_SIZE]; /**< Hexadecimal unique board ID. */
    uint8_t firmware_major; /**< Firmware release major version. */
    uint8_t firmware_minor; /**< Firmware release minor version. */
    uint8_t firmware_patch; /**< Firmware release patch version. */
    uint8_t reserved; /**< Must remain zero. */
} pf_hid_info_report_t;

/** Dynamic telemetry returned by HID Input report 2. Integer fields are little-endian. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Runtime layout version. */
    uint8_t sequence; /**< Periodic report sequence. */
    uint32_t uptime_ms; /**< Milliseconds since boot, modulo 2^32. */
    uint32_t system_clock_hz; /**< Current system clock frequency. */
    uint8_t flags; /**< PF_HID_STATUS_* board and USB flags. */
    int16_t temperature_centidegrees; /**< Temperature when its validity flag is set. */
    uint16_t cpu_usage_core0_basis_points; /**< Core 0 non-idle time, 0..10000; UINT16_MAX until available. */
    uint16_t cpu_usage_core1_basis_points; /**< Core 1 non-idle time, 0..10000; UINT16_MAX until available. */
    uint32_t ram_used_bytes; /**< Static SRAM reservations including core and task stacks. */
    uint32_t ram_total_bytes; /**< SDK-selected on-chip SRAM capacity. */
} pf_hid_status_report_t;

/** Dynamic SD card state: layout 2 with the card CID. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< SD memory layout version: 2. */
    uint8_t sequence; /**< Periodic sample sequence. */
    uint8_t flags; /**< Presence, initialization, identity and capacity validity. */
    uint8_t component_type; /**< 0 unavailable, 1 SD card. */
    uint8_t mode; /**< 0 unknown, 1 SDIO, 2 SPI. */
    uint32_t bus_clock_hz; /**< Effective physical symbol clock. */
    uint32_t block_count; /**< Usable logical capacity in 512-byte sectors. */
    uint8_t cid[16]; /**< SD card identification register. */
} pf_hid_memory_report_t;

/** LED set-state command sent through HID Feature report 4. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t on;
} pf_hid_led_command_report_t;

/** @brief MSC command; layout 2 keeps safe toggles, layout 3 action 2 explicitly forces return. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Command layout: 2 for safe toggles, 3 for force return. */
    uint8_t enable; /**< 0 disables safely, 1 enables, 2 forces return only with layout 3. */
} pf_hid_msc_command_report_t;

/** @brief Ownership transition status; layout 3 advertises force-return support. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< MSC status layout version. */
    uint8_t enabled; /**< MSC is exposed and owns storage. */
    uint8_t transition_pending; /**< Includes the forced-return remount attempt. */
    uint8_t request_rejected; /**< Request or forced remount failed; inspect enabled and logs. */
} pf_hid_msc_status_report_t;

/** Start (action=1, size_mib=1..1024) or cancel (action=0, size_mib=0). */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t action;
    uint32_t size_mib;
} pf_hid_self_test_command_t;

/** Benchmark progress/timings; integer fields are little-endian. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t sequence;
    uint8_t state;
    uint8_t flags;
    uint32_t target_bytes;
    uint32_t written_bytes;
    uint32_t read_bytes;
    uint32_t write_elapsed_ms;
    uint32_t read_elapsed_ms;
    uint32_t mismatch_count;
    uint8_t error;
    uint8_t filesystem_result;
    uint8_t request_error;
    uint8_t reserved;
} pf_hid_self_test_status_t;

/** Last successfully mounted filesystem format; unknown until firmware mounts the volume. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t sequence;
    uint8_t format;
} pf_hid_filesystem_report_t;

/** Persisted boot settings returned by and written through HID Feature report 8. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Settings report layout version. */
    uint16_t system_clock_mhz; /**< 0 (build default), 150, 200 or 250. */
    uint8_t sdio_bus_mhz; /**< 0, 25 or 50. */
    uint8_t spi_bus_mhz; /**< 0, 25 or 50. */
} pf_hid_settings_report_t;

/** Request a delayed MCU reboot after the host has received the HID acknowledgement. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Reboot command layout version. */
    uint8_t action; /**< Must be 1 to request reboot. */
} pf_hid_reboot_command_t;

_Static_assert(sizeof(pf_hid_self_test_command_t) == PF_HID_SELF_TEST_COMMAND_PAYLOAD_SIZE,
               "Self-test command size changed");
_Static_assert(sizeof(pf_hid_self_test_status_t) == PF_HID_SELF_TEST_STATUS_PAYLOAD_SIZE,
               "Self-test status size changed");

_Static_assert(sizeof(pf_hid_info_report_t) == PF_HID_INFO_PAYLOAD_SIZE,
               "HID info report payload size changed");
_Static_assert(sizeof(pf_hid_status_report_t) == PF_HID_STATUS_PAYLOAD_SIZE,
               "HID status report payload size changed");
_Static_assert(sizeof(pf_hid_memory_report_t) == PF_HID_MEMORY_PAYLOAD_SIZE,
               "HID memory report payload size changed");
_Static_assert(sizeof(pf_hid_settings_report_t) == PF_HID_SETTINGS_PAYLOAD_SIZE,
               "HID settings report payload size changed");
_Static_assert(sizeof(pf_hid_reboot_command_t) == PF_HID_REBOOT_COMMAND_PAYLOAD_SIZE,
               "HID reboot command size changed");
_Static_assert(sizeof(pf_hid_led_command_report_t) == PF_HID_LED_COMMAND_PAYLOAD_SIZE,
               "HID LED command report payload size changed");
_Static_assert((sizeof(PF_HID_PRODUCT_NAME) - 1u) <= PF_HID_PRODUCT_NAME_SIZE,
               "HID product name does not fit the report field");

void pf_hid_build_info_report(pf_hid_info_report_t *report);
void pf_hid_build_status_report(pf_hid_status_report_t *report, uint8_t sequence);
/** Return false without modifying the report if card state is currently busy. */
bool pf_hid_build_memory_report(pf_hid_memory_report_t *report, uint8_t sequence);
void pf_hid_build_filesystem_report(pf_hid_filesystem_report_t *report, uint8_t sequence);
void pf_hid_build_settings_report(pf_hid_settings_report_t *report);
/** Consume a validated reboot request from the HID callback. */
bool pf_hid_take_reboot_request(void);
void pf_hid_build_msc_status_report(pf_hid_msc_status_report_t *report);
void pf_hid_build_self_test_report(pf_hid_self_test_status_t *report, uint8_t sequence);

#endif