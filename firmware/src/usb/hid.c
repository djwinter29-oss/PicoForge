#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "hid.h"
#include "board/board.h"
#include "drivers/sd_card/sd_card_block.h"
#include "drivers/led.h"
#include "drivers/temperature.h"
#include "msc.h"

void pf_hid_build_info_report(pf_hid_info_report_t *report)
{
    char serial_number[PF_HID_SERIAL_NUMBER_SIZE + 1u];

    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->mcu_id = pf_system_mcu_id();
    memcpy(report->product_name, PF_HID_PRODUCT_NAME, sizeof(PF_HID_PRODUCT_NAME) - 1u);
    pico_get_unique_board_id_string(serial_number, sizeof(serial_number));
    memcpy(report->serial_number, serial_number, sizeof(report->serial_number));
}

void pf_hid_build_status_report(pf_hid_status_report_t *report, uint8_t sequence)
{
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->uptime_ms = pf_system_uptime_ms();
    report->system_clock_hz = pf_system_clock_hz();
    if (tud_mounted()) {
        report->flags |= PF_HID_STATUS_USB_MOUNTED;
    }
    if (tud_cdc_connected()) {
        report->flags |= PF_HID_STATUS_CDC_OPEN;
    }
    int16_t temperature;

    if (pf_temperature_read_centidegrees(&temperature)) {
        report->flags |= PF_HID_STATUS_TEMPERATURE_VALID;
        report->temperature_centidegrees = temperature;
    }
    if (pf_led_is_available()) {
        report->flags |= PF_HID_STATUS_LED_AVAILABLE;
        if (pf_led_is_on()) {
            report->flags |= PF_HID_STATUS_LED_ON;
        }
    }
}

void pf_hid_build_memory_report(pf_hid_memory_report_t *report, uint8_t sequence)
{
    pf_sd_card_config_t config;
    pf_sd_card_info_t info;

    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->component_type = PF_HID_MEMORY_TYPE_SD_CARD;
    if (pf_board_get_sd_card_config(&config)) {
        report->flags |= PF_HID_MEMORY_CONFIGURED;
        report->mode = (config.transport == PF_SD_CARD_TRANSPORT_SPI)
            ? PF_HID_MEMORY_MODE_SPI
            : PF_HID_MEMORY_MODE_SDIO;
    }
    pf_sd_card_get_info(&info);
    if (info.presence_known) {
        report->flags |= PF_HID_MEMORY_PRESENCE_KNOWN;
    }
    if (info.present) {
        report->flags |= PF_HID_MEMORY_PRESENT;
    }
    if (info.initialized) {
        report->flags |= PF_HID_MEMORY_INITIALIZED | PF_HID_MEMORY_CAPACITY_VALID;
        report->bus_clock_hz = info.bus_clock_hz;
        report->block_count = info.block_count;
    }
    if (info.cid_valid) {
        report->flags |= PF_HID_MEMORY_CID_VALID;
        memcpy(report->cid, info.cid, sizeof(report->cid));
    }
}

void pf_hid_build_msc_status_report(pf_hid_msc_status_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->enabled = pf_usb_msc_is_enabled();
    report->transition_pending = pf_usb_msc_transition_pending();
    report->request_rejected = pf_usb_msc_request_rejected();
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    if (instance != 0u) {
        return 0u;
    }

    if ((report_id == PF_HID_INFO_REPORT_ID) && (report_type == HID_REPORT_TYPE_FEATURE) &&
        (reqlen >= sizeof(pf_hid_info_report_t))) {
        pf_hid_info_report_t report;
        pf_hid_build_info_report(&report);
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    if ((report_id == PF_HID_STATUS_REPORT_ID) && (report_type == HID_REPORT_TYPE_INPUT) &&
        (reqlen >= sizeof(pf_hid_status_report_t))) {
        pf_hid_status_report_t report;
        pf_hid_build_status_report(&report, 0u);
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    if ((report_id == PF_HID_MEMORY_REPORT_ID) && (report_type == HID_REPORT_TYPE_INPUT) &&
        (reqlen >= sizeof(pf_hid_memory_report_t))) {
        pf_hid_memory_report_t report;
        pf_hid_build_memory_report(&report, 0u);
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    if ((report_id == PF_HID_MSC_COMMAND_REPORT_ID) &&
        (report_type == HID_REPORT_TYPE_INPUT) &&
        (reqlen >= sizeof(pf_hid_msc_status_report_t))) {
        pf_hid_msc_status_report_t report;
        pf_hid_build_msc_status_report(&report);
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    return 0u;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    if ((instance != 0u) || (report_type != HID_REPORT_TYPE_FEATURE) ||
        (buffer == NULL)) {
        return;
    }

    if ((report_id == PF_HID_LED_COMMAND_REPORT_ID) &&
        (bufsize >= sizeof(pf_hid_led_command_report_t)) &&
        (buffer[0] == PF_HID_REPORT_VERSION) && (buffer[1] <= 1u)) {
        (void)pf_led_set(buffer[1] != 0u);
    } else if ((report_id == PF_HID_MSC_COMMAND_REPORT_ID) &&
               (bufsize >= sizeof(pf_hid_msc_command_report_t)) &&
               (buffer[0] == PF_HID_REPORT_VERSION) && (buffer[1] <= 1u)) {
        pf_usb_msc_request(buffer[1] != 0u);
    }
}