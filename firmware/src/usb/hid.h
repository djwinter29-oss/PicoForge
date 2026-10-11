#ifndef PF_USB_HID_H
#define PF_USB_HID_H

#include <stdint.h>

#include "drivers/system.h"
#include "pico/unique_id.h"

#define PF_HID_INFO_REPORT_ID 1u
#define PF_HID_STATUS_REPORT_ID 2u
#define PF_HID_MEMORY_REPORT_ID 3u
#define PF_HID_LED_COMMAND_REPORT_ID 4u
#define PF_HID_MSC_COMMAND_REPORT_ID 5u
#define PF_HID_REPORT_VERSION 2u
#define PF_HID_PRODUCT_NAME "PicoForge"
#define PF_HID_PRODUCT_NAME_SIZE 20u
#define PF_HID_SERIAL_NUMBER_SIZE (2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES)
#define PF_HID_INFO_RESERVED_SIZE 4u
#define PF_HID_INFO_PAYLOAD_SIZE (2u + PF_HID_PRODUCT_NAME_SIZE + PF_HID_SERIAL_NUMBER_SIZE + PF_HID_INFO_RESERVED_SIZE)
#define PF_HID_STATUS_PAYLOAD_SIZE 13u
#define PF_HID_MEMORY_PAYLOAD_SIZE 29u
#define PF_HID_LED_COMMAND_PAYLOAD_SIZE 2u
#define PF_HID_MSC_COMMAND_PAYLOAD_SIZE 2u
#define PF_HID_MSC_STATUS_PAYLOAD_SIZE 4u

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

/** Stable product/device identity returned by HID Feature report 1. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t mcu_id;
    char product_name[PF_HID_PRODUCT_NAME_SIZE];
    char serial_number[PF_HID_SERIAL_NUMBER_SIZE];
    uint8_t reserved[PF_HID_INFO_RESERVED_SIZE];
} pf_hid_info_report_t;

/** Dynamic telemetry returned by HID Input report 2. Integer fields are little-endian. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t sequence;
    uint32_t uptime_ms;
    uint32_t system_clock_hz;
    uint8_t flags;
    int16_t temperature_centidegrees;
} pf_hid_status_report_t;

/** Dynamic SD-card state, bus settings, capacity, and raw SD CID bytes. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t sequence;
    uint8_t flags;
    uint8_t component_type;
    uint8_t mode;
    uint32_t bus_clock_hz;
    uint32_t block_count;
    uint8_t cid[16];
} pf_hid_memory_report_t;

/** LED set-state command sent through HID Feature report 4. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t on;
} pf_hid_led_command_report_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t enable;
} pf_hid_msc_command_report_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t enabled;
    uint8_t transition_pending;
    uint8_t request_rejected;
} pf_hid_msc_status_report_t;

_Static_assert(sizeof(pf_hid_info_report_t) == PF_HID_INFO_PAYLOAD_SIZE,
               "HID info report payload size changed");
_Static_assert(sizeof(pf_hid_status_report_t) == PF_HID_STATUS_PAYLOAD_SIZE,
               "HID status report payload size changed");
_Static_assert(sizeof(pf_hid_memory_report_t) == PF_HID_MEMORY_PAYLOAD_SIZE,
               "HID memory report payload size changed");
_Static_assert(sizeof(pf_hid_led_command_report_t) == PF_HID_LED_COMMAND_PAYLOAD_SIZE,
               "HID LED command report payload size changed");
_Static_assert((sizeof(PF_HID_PRODUCT_NAME) - 1u) <= PF_HID_PRODUCT_NAME_SIZE,
               "HID product name does not fit the report field");

void pf_hid_build_info_report(pf_hid_info_report_t *report);
void pf_hid_build_status_report(pf_hid_status_report_t *report, uint8_t sequence);
void pf_hid_build_memory_report(pf_hid_memory_report_t *report, uint8_t sequence);
void pf_hid_build_msc_status_report(pf_hid_msc_status_report_t *report);

#endif