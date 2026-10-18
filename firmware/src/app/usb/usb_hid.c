#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_hid.h"
#include "board/board.h"
#include "drivers/sd_card/sd_block.h"
#include "drivers/led.h"
#include "drivers/temperature.h"
#include "usb_msc.h"
#include "app/storage/fatfs_volume.h"
#include "app/storage/storage_worker.h"
#include "integration/freertos/runtime_stats.h"

void pf_hid_build_info_report(pf_hid_info_report_t *report)
{
    char serial_number[PF_HID_SERIAL_NUMBER_SIZE + 1u];

    memset(report, 0, sizeof(*report));
    report->version = PF_HID_INFO_REPORT_VERSION;
    report->firmware_major = PF_FIRMWARE_VERSION_MAJOR;
    report->firmware_minor = PF_FIRMWARE_VERSION_MINOR;
    report->firmware_patch = PF_FIRMWARE_VERSION_PATCH;
    report->mcu_id = pf_system_mcu_id();
    memcpy(report->product_name, PF_HID_PRODUCT_NAME, sizeof(PF_HID_PRODUCT_NAME) - 1u);
    pico_get_unique_board_id_string(serial_number, sizeof(serial_number));
    memcpy(report->serial_number, serial_number, sizeof(report->serial_number));
}

void pf_hid_build_status_report(pf_hid_status_report_t *report, uint8_t sequence)
{
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_STATUS_REPORT_VERSION;
    report->sequence = sequence;
    report->uptime_ms = pf_system_uptime_ms();
    report->system_clock_hz = pf_system_clock_hz();
    pf_runtime_stats_t stats;
    pf_runtime_stats_read(&stats);
    report->cpu_usage_basis_points = stats.cpu_usage_basis_points;
    report->ram_used_bytes = stats.ram_used_bytes;
    report->ram_total_bytes = stats.ram_total_bytes;
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

bool pf_hid_build_memory_report(pf_hid_memory_report_t *report, uint8_t sequence)
{
#if PF_STORAGE_BACKEND_NOR
    /* The current HID memory report is SD-specific; never advertise NOR as an SD card. */
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    return true;
#else
    pf_sd_config_t config;
    pf_sd_info_t info;

    if (!pf_sd_try_lock()) {
        return false;
    }
    pf_sd_get_info(&info);
    pf_sd_unlock();
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->component_type = PF_HID_MEMORY_TYPE_SD_CARD;
    if (pf_board_get_sd_card_config(&config)) {
        report->flags |= PF_HID_MEMORY_CONFIGURED;
        report->mode = (config.transport.mode == PF_STORAGE_BUS_SPI)
            ? PF_HID_MEMORY_MODE_SPI
            : PF_HID_MEMORY_MODE_SDIO;
    }
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
    if (info.initialized && info.cid_valid) {
        report->flags |= PF_HID_MEMORY_CID_VALID;
        memcpy(report->cid, info.cid, sizeof(report->cid));
    }
    return true;
#endif
}

void pf_hid_build_filesystem_report(pf_hid_filesystem_report_t *report, uint8_t sequence)
{
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    switch (pf_fatfs_volume_filesystem_format()) {
    case PF_FATFS_VOLUME_FILESYSTEM_FAT12: report->format = PF_HID_FILESYSTEM_FAT12; break;
    case PF_FATFS_VOLUME_FILESYSTEM_FAT16: report->format = PF_HID_FILESYSTEM_FAT16; break;
    case PF_FATFS_VOLUME_FILESYSTEM_FAT32: report->format = PF_HID_FILESYSTEM_FAT32; break;
    case PF_FATFS_VOLUME_FILESYSTEM_EXFAT: report->format = PF_HID_FILESYSTEM_EXFAT; break;
    default: report->format = PF_HID_FILESYSTEM_UNKNOWN; break;
    }
}

void pf_hid_build_msc_status_report(pf_hid_msc_status_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_MSC_REPORT_VERSION;
    report->enabled = pf_usb_msc_is_enabled();
    report->transition_pending = pf_usb_msc_transition_pending();
    report->request_rejected = pf_usb_msc_request_rejected();
}

void pf_hid_build_self_test_report(pf_hid_self_test_status_t *report, uint8_t sequence)
{
    pf_file_benchmark_status_t status;
    pf_storage_worker_benchmark_status(&status);
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->state = (uint8_t)status.state;
    report->flags = status.flags;
    report->target_bytes = status.target_bytes;
    report->written_bytes = status.written_bytes;
    report->read_bytes = status.read_bytes;
    report->write_elapsed_ms = status.write_elapsed_ms;
    report->read_elapsed_ms = status.read_elapsed_ms;
    report->mismatch_count = status.mismatch_count;
    report->error = (uint8_t)status.error;
    report->filesystem_result = (uint8_t)status.filesystem_result;
    report->request_error = (uint8_t)status.request_error;
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
        if (!pf_hid_build_memory_report(&report, 0u)) {
            return 0u;
        }
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

    if ((report_id == PF_HID_SELF_TEST_REPORT_ID) && (report_type == HID_REPORT_TYPE_INPUT) &&
        (reqlen >= sizeof(pf_hid_self_test_status_t))) {
        pf_hid_self_test_status_t report;
        pf_hid_build_self_test_report(&report, 0u);
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
    } else if ((report_id == PF_HID_MSC_COMMAND_REPORT_ID) &&
               (bufsize == sizeof(pf_hid_msc_command_report_t)) &&
               (buffer[0] == PF_HID_MSC_REPORT_VERSION) && (buffer[1] == 2u)) {
        pf_usb_msc_request_force_return();
    } else if ((report_id == PF_HID_SELF_TEST_REPORT_ID) &&
               (bufsize == sizeof(pf_hid_self_test_command_t)) &&
               (buffer[0] == PF_HID_REPORT_VERSION) && (buffer[1] <= 1u)) {
        uint32_t size_mib = (uint32_t)buffer[2] | ((uint32_t)buffer[3] << 8u) |
                           ((uint32_t)buffer[4] << 16u) | ((uint32_t)buffer[5] << 24u);
        if (buffer[1] == 1u) {
            (void)pf_storage_worker_benchmark_start(size_mib, pf_usb_msc_is_enabled());
        } else if (size_mib == 0u) {
            (void)pf_storage_worker_benchmark_cancel();
        }
    }
}