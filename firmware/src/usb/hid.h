#ifndef PICOFORGE_USB_HID_H
#define PICOFORGE_USB_HID_H

#include <stdint.h>

#define PICOFORGE_HID_INFO_REPORT_ID 1u
#define PICOFORGE_HID_STATUS_REPORT_ID 2u
#define PICOFORGE_HID_REPORT_VERSION 1u
#define PICOFORGE_HID_PRODUCT_NAME "PicoForge"
#define PICOFORGE_HID_PRODUCT_NAME_SIZE 20u
#define PICOFORGE_HID_INFO_PAYLOAD_SIZE 26u
#define PICOFORGE_HID_STATUS_PAYLOAD_SIZE 7u

#define PICOFORGE_MCU_RP2040 1u
#define PICOFORGE_MCU_RP2350 2u

#define PICOFORGE_HID_STATUS_USB_MOUNTED (1u << 0)
#define PICOFORGE_HID_STATUS_CDC_OPEN (1u << 1)

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

_Static_assert(sizeof(picoforge_hid_info_report_t) == PICOFORGE_HID_INFO_PAYLOAD_SIZE,
               "HID info report payload size changed");
_Static_assert(sizeof(picoforge_hid_status_report_t) == PICOFORGE_HID_STATUS_PAYLOAD_SIZE,
               "HID status report payload size changed");
_Static_assert((sizeof(PICOFORGE_HID_PRODUCT_NAME) - 1u) <= PICOFORGE_HID_PRODUCT_NAME_SIZE,
               "HID product name does not fit the report field");

void picoforge_hid_build_info_report(picoforge_hid_info_report_t *report);
void picoforge_hid_build_status_report(picoforge_hid_status_report_t *report, uint8_t sequence);

#endif