#include <string.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "hid.h"
#include "drivers/led.h"
#include "drivers/temperature.h"

void picoforge_hid_build_info_report(picoforge_hid_info_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->version = PICOFORGE_HID_REPORT_VERSION;
    report->mcu_id = picoforge_system_mcu_id();
    report->system_clock_hz = picoforge_system_clock_hz();
    memcpy(report->product_name, PICOFORGE_HID_PRODUCT_NAME, sizeof(PICOFORGE_HID_PRODUCT_NAME) - 1u);
}

void picoforge_hid_build_status_report(picoforge_hid_status_report_t *report, uint8_t sequence)
{
    memset(report, 0, sizeof(*report));
    report->version = PICOFORGE_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->uptime_ms = picoforge_system_uptime_ms();
    if (tud_mounted()) {
        report->flags |= PICOFORGE_HID_STATUS_USB_MOUNTED;
    }
    if (tud_cdc_connected()) {
        report->flags |= PICOFORGE_HID_STATUS_CDC_OPEN;
    }
}

void picoforge_hid_build_board_status_report(picoforge_hid_board_status_report_t *report)
{
    int16_t temperature;

    memset(report, 0, sizeof(*report));
    report->version = PICOFORGE_HID_REPORT_VERSION;
    if (picoforge_temperature_read_centidegrees(&temperature)) {
        report->flags |= PICOFORGE_HID_BOARD_TEMPERATURE_VALID;
        report->temperature_centidegrees = temperature;
    }
    if (picoforge_led_is_available()) {
        report->flags |= PICOFORGE_HID_BOARD_LED_AVAILABLE;
        if (picoforge_led_is_on()) {
            report->flags |= PICOFORGE_HID_BOARD_LED_ON;
        }
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    if (instance != 0u) {
        return 0u;
    }

    if ((report_id == PICOFORGE_HID_INFO_REPORT_ID) && (report_type == HID_REPORT_TYPE_FEATURE) &&
        (reqlen >= sizeof(picoforge_hid_info_report_t))) {
        picoforge_hid_info_report_t report;
        picoforge_hid_build_info_report(&report);
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    if ((report_id == PICOFORGE_HID_STATUS_REPORT_ID) && (report_type == HID_REPORT_TYPE_INPUT) &&
        (reqlen >= sizeof(picoforge_hid_status_report_t))) {
        picoforge_hid_status_report_t report;
        picoforge_hid_build_status_report(&report, 0u);
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    if ((report_id == PICOFORGE_HID_BOARD_STATUS_REPORT_ID) && (report_type == HID_REPORT_TYPE_FEATURE) &&
        (reqlen >= sizeof(picoforge_hid_board_status_report_t))) {
        picoforge_hid_board_status_report_t report;
        picoforge_hid_build_board_status_report(&report);
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    return 0u;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    if ((instance != 0u) || (report_id != PICOFORGE_HID_LED_COMMAND_REPORT_ID) ||
        (report_type != HID_REPORT_TYPE_FEATURE) ||
        (buffer == NULL) || (bufsize < sizeof(picoforge_hid_led_command_report_t))) {
        return;
    }

    if ((buffer[0] == PICOFORGE_HID_REPORT_VERSION) && (buffer[1] <= 1u)) {
        (void)picoforge_led_set(buffer[1] != 0u);
    }
}