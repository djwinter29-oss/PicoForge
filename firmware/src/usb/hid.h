#ifndef PICOFORGE_USB_HID_H
#define PICOFORGE_USB_HID_H

#include <stdint.h>

#include "drivers/system.h"

#define PICOFORGE_HID_INFO_REPORT_ID 1u
#define PICOFORGE_HID_STATUS_REPORT_ID 2u
#define PICOFORGE_HID_BOARD_STATUS_REPORT_ID 3u
#define PICOFORGE_HID_LED_COMMAND_REPORT_ID 4u
#define PICOFORGE_HID_REPORT_VERSION 1u
#define PICOFORGE_HID_PRODUCT_NAME "PicoForge"
#define PICOFORGE_HID_PRODUCT_NAME_SIZE 20u
#define PICOFORGE_HID_INFO_PAYLOAD_SIZE 26u
#define PICOFORGE_HID_STATUS_PAYLOAD_SIZE 7u
#define PICOFORGE_HID_BOARD_STATUS_PAYLOAD_SIZE 4u
#define PICOFORGE_HID_LED_COMMAND_PAYLOAD_SIZE 2u

#define PICOFORGE_MCU_RP2040 PICOFORGE_SYSTEM_MCU_RP2040
#define PICOFORGE_MCU_RP2350 PICOFORGE_SYSTEM_MCU_RP2350

#define PICOFORGE_HID_STATUS_USB_MOUNTED (1u << 0)
#define PICOFORGE_HID_STATUS_CDC_OPEN (1u << 1)
#define PICOFORGE_HID_BOARD_TEMPERATURE_VALID (1u << 0)
#define PICOFORGE_HID_BOARD_LED_AVAILABLE (1u << 1)
#define PICOFORGE_HID_BOARD_LED_ON (1u << 2)

/** Hardware and product identity returned by HID Feature report 1. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t mcu_id;
    uint32_t system_clock_hz;
    char product_name[PICOFORGE_HID_PRODUCT_NAME_SIZE];
} picoforge_hid_info_report_t;

/** Runtime state returned by HID Input report 2. Integer fields are little-endian. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t sequence;
    uint32_t uptime_ms;
    uint8_t flags;
} picoforge_hid_status_report_t;

/** Board telemetry returned by HID Feature report 3; temperature is centidegrees C. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t flags;
    int16_t temperature_centidegrees;
} picoforge_hid_board_status_report_t;

/** LED set-state command sent through HID Feature report 4. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t on;
} picoforge_hid_led_command_report_t;

_Static_assert(sizeof(picoforge_hid_info_report_t) == PICOFORGE_HID_INFO_PAYLOAD_SIZE,
               "HID info report payload size changed");
_Static_assert(sizeof(picoforge_hid_status_report_t) == PICOFORGE_HID_STATUS_PAYLOAD_SIZE,
               "HID status report payload size changed");
_Static_assert(sizeof(picoforge_hid_board_status_report_t) == PICOFORGE_HID_BOARD_STATUS_PAYLOAD_SIZE,
               "HID board status report payload size changed");
_Static_assert(sizeof(picoforge_hid_led_command_report_t) == PICOFORGE_HID_LED_COMMAND_PAYLOAD_SIZE,
               "HID LED command report payload size changed");
_Static_assert((sizeof(PICOFORGE_HID_PRODUCT_NAME) - 1u) <= PICOFORGE_HID_PRODUCT_NAME_SIZE,
               "HID product name does not fit the report field");

void picoforge_hid_build_info_report(picoforge_hid_info_report_t *report);
void picoforge_hid_build_status_report(picoforge_hid_status_report_t *report, uint8_t sequence);
void picoforge_hid_build_board_status_report(picoforge_hid_board_status_report_t *report);

#endif