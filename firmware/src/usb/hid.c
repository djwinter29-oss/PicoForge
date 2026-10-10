#include <string.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "hid.h"

void picoforge_hid_build_info_report(picoforge_hid_info_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->version = PICOFORGE_HID_REPORT_VERSION;
#if defined(PICO_RP2350) && PICO_RP2350
    report->mcu_id = PICOFORGE_MCU_RP2350;
#else
    report->mcu_id = PICOFORGE_MCU_RP2040;
#endif
    report->system_clock_hz = clock_get_hz(clk_sys);
    memcpy(report->product_name, PICOFORGE_HID_PRODUCT_NAME, sizeof(PICOFORGE_HID_PRODUCT_NAME) - 1u);
}

void picoforge_hid_build_status_report(picoforge_hid_status_report_t *report, uint8_t sequence)
{
    memset(report, 0, sizeof(*report));
    report->version = PICOFORGE_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->uptime_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
    if (tud_mounted()) {
        report->flags |= PICOFORGE_HID_STATUS_USB_MOUNTED;
    }
    if (tud_cdc_connected()) {
        report->flags |= PICOFORGE_HID_STATUS_CDC_OPEN;
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

    return 0u;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}