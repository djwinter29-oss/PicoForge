#ifndef PICOFORGE_USB_HID_H
#define PICOFORGE_USB_HID_H

#include <stdint.h>

#include "drivers/system.h"
#include "pico/unique_id.h"

#define PICOFORGE_HID_INFO_REPORT_ID 1u
#define PICOFORGE_HID_STATUS_REPORT_ID 2u
#define PICOFORGE_HID_LED_COMMAND_REPORT_ID 4u
#define PICOFORGE_HID_REPORT_VERSION 2u
#define PICOFORGE_HID_PRODUCT_NAME "PicoForge"
#define PICOFORGE_HID_PRODUCT_NAME_SIZE 20u
#define PICOFORGE_HID_SERIAL_NUMBER_SIZE (2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES)
#define PICOFORGE_HID_INFO_RESERVED_SIZE 4u
#define PICOFORGE_HID_INFO_PAYLOAD_SIZE (2u + PICOFORGE_HID_PRODUCT_NAME_SIZE + PICOFORGE_HID_SERIAL_NUMBER_SIZE + PICOFORGE_HID_INFO_RESERVED_SIZE)
#define PICOFORGE_HID_STATUS_PAYLOAD_SIZE 13u
#define PICOFORGE_HID_LED_COMMAND_PAYLOAD_SIZE 2u

#define PICOFORGE_MCU_RP2040 PICOFORGE_SYSTEM_MCU_RP2040
#define PICOFORGE_MCU_RP2350 PICOFORGE_SYSTEM_MCU_RP2350

#define PICOFORGE_HID_STATUS_USB_MOUNTED (1u << 0)
#define PICOFORGE_HID_STATUS_CDC_OPEN (1u << 1)
#define PICOFORGE_HID_STATUS_TEMPERATURE_VALID (1u << 2)
#define PICOFORGE_HID_STATUS_LED_AVAILABLE (1u << 3)
#define PICOFORGE_HID_STATUS_LED_ON (1u << 4)

/** Stable product/device identity returned by HID Feature report 1. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t mcu_id;
    char product_name[PICOFORGE_HID_PRODUCT_NAME_SIZE];
    char serial_number[PICOFORGE_HID_SERIAL_NUMBER_SIZE];
    uint8_t reserved[PICOFORGE_HID_INFO_RESERVED_SIZE];
} picoforge_hid_info_report_t;

/** Dynamic telemetry returned by HID Input report 2. Integer fields are little-endian. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t sequence;
    uint32_t uptime_ms;
    uint32_t system_clock_hz;
    uint8_t flags;
    int16_t temperature_centidegrees;
} picoforge_hid_status_report_t;

/** LED set-state command sent through HID Feature report 4. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t on;
} picoforge_hid_led_command_report_t;

_Static_assert(sizeof(picoforge_hid_info_report_t) == PICOFORGE_HID_INFO_PAYLOAD_SIZE,
               "HID info report payload size changed");
_Static_assert(sizeof(picoforge_hid_status_report_t) == PICOFORGE_HID_STATUS_PAYLOAD_SIZE,
               "HID status report payload size changed");
_Static_assert(sizeof(picoforge_hid_led_command_report_t) == PICOFORGE_HID_LED_COMMAND_PAYLOAD_SIZE,
               "HID LED command report payload size changed");
_Static_assert((sizeof(PICOFORGE_HID_PRODUCT_NAME) - 1u) <= PICOFORGE_HID_PRODUCT_NAME_SIZE,
               "HID product name does not fit the report field");

void picoforge_hid_build_info_report(picoforge_hid_info_report_t *report);
void picoforge_hid_build_status_report(picoforge_hid_status_report_t *report, uint8_t sequence);

#endif