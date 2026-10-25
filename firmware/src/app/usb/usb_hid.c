#include <limits.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_hid.h"
#include "config/config.h"
#include "drivers/led.h"
#include "drivers/monitor_uart.h"
#include "middleware/storage/storage.h"
#include "drivers/boot_config.h"
#include "drivers/temperature.h"
#include "usb_msc.h"
#include "middleware/storage/fatfs_volume.h"
#include "app/storage/storage_worker.h"
#include "app/runtime_stats.h"

#ifndef PF_HID_BOARD_MODEL
#define PF_HID_BOARD_MODEL 0u
#endif

#define PF_HID_BOARD_FLAG_CARD_DETECT (1u << 0)
#define PF_HID_BOARD_FLAG_LED (1u << 1)
#define PF_HID_BOARD_FLAG_MONITOR_UART (1u << 2)

#define PF_HID_COMMAND_IDLE 0u
#define PF_HID_COMMAND_PENDING 1u
#define PF_HID_COMMAND_COMPLETED 2u
#define PF_HID_COMMAND_INVALID 3u
#define PF_HID_COMMAND_UNSUPPORTED 4u
#define PF_HID_COMMAND_BUSY 5u
#define PF_HID_COMMAND_FAILED 6u

#define PF_HID_ERROR_NONE 0u
#define PF_HID_ERROR_MALFORMED 1u
#define PF_HID_ERROR_CONFIRMATION 2u
#define PF_HID_ERROR_ARGUMENT 3u
#define PF_HID_ERROR_CAPABILITY 4u
#define PF_HID_ERROR_BUSY 5u
#define PF_HID_ERROR_OPERATION 6u
#define PF_HID_ERROR_OPCODE 7u

static bool reboot_requested;
static pf_hid_control_result_t control_result = {
    .version = PF_HID_REPORT_VERSION,
    .status = PF_HID_COMMAND_IDLE,
};
static bool self_test_result_pending;
static pf_hid_control_request_t pending_self_test_request;

static uint8_t gpio_value(uint32_t gpio)
{
    return (gpio > UINT8_MAX - 1u)
        ? UINT8_MAX : (uint8_t)gpio;
}

static bool trailing_arguments_zero(const uint8_t *arguments, size_t first)
{
    for (size_t index = first; index < 5u; ++index) {
        if (arguments[index] != 0u) return false;
    }
    return true;
}

static uint16_t read_u16_le(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8u);
}

static uint32_t read_u32_le(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8u) |
        ((uint32_t)value[2] << 16u) | ((uint32_t)value[3] << 24u);
}

static void set_result(const pf_hid_control_request_t *request, uint8_t status, uint8_t error,
                       const uint8_t result_data[4])
{
    control_result = (pf_hid_control_result_t){
        .version = PF_HID_REPORT_VERSION,
        .transaction_id = request->transaction_id,
        .opcode = request->opcode,
        .status = status,
        .error_reason = error,
    };
    if (result_data != NULL) memcpy(control_result.result_data, result_data, sizeof(control_result.result_data));
}

static uint8_t board_model(void)
{
    return (uint8_t)PF_HID_BOARD_MODEL;
}

bool pf_hid_take_reboot_request(void)
{
    const bool requested = reboot_requested;
    reboot_requested = false;
    return requested;
}

void pf_hid_poll_async_commands(void)
{
    if (!self_test_result_pending) return;

    bool accepted;
    pf_file_benchmark_error_t error;
    if (!pf_storage_worker_benchmark_admission_result(&accepted, &error)) return;

    self_test_result_pending = false;
    if ((control_result.transaction_id != pending_self_test_request.transaction_id) ||
        (control_result.opcode != pending_self_test_request.opcode)) {
        return;
    }
    bool busy = (error == PF_FILE_BENCHMARK_ERROR_BUSY) ||
                (error == PF_FILE_BENCHMARK_ERROR_USB_OWNER);
    set_result(&pending_self_test_request,
               accepted ? PF_HID_COMMAND_COMPLETED :
                   (busy ? PF_HID_COMMAND_BUSY : PF_HID_COMMAND_FAILED),
               accepted ? PF_HID_ERROR_NONE :
                   (busy ? PF_HID_ERROR_BUSY : PF_HID_ERROR_OPERATION), NULL);
}

void pf_hid_build_identity_report(pf_hid_identity_report_t *report)
{
    pico_unique_board_id_t unique_id;
    pf_runtime_stats_t stats;

    memset(report, 0, sizeof(*report));
    pico_get_unique_board_id(&unique_id);
    pf_runtime_stats_read(&stats);
    report->version = PF_HID_REPORT_VERSION;
    report->product_id = PF_HID_PRODUCT_ID;
    memcpy(report->device_uid, unique_id.id, sizeof(report->device_uid));
    report->firmware_major = PF_FIRMWARE_VERSION_MAJOR;
    report->firmware_minor = PF_FIRMWARE_VERSION_MINOR;
    report->firmware_patch = PF_FIRMWARE_VERSION_PATCH;
    report->mcu = pf_system_mcu_id();
    report->board_model = board_model();
    report->capabilities = PF_HID_CAP_REBOOT | PF_HID_CAP_SETTINGS;
    if (pf_led_is_available()) report->capabilities |= PF_HID_CAP_LED;
    pf_storage_status_t storage;
    pf_storage_get_status(&storage);
    if (storage.bus == PF_STORAGE_BUS_SPI) report->capabilities |= PF_HID_CAP_SPI;
    if (storage.bus == PF_STORAGE_BUS_SDIO) report->capabilities |= PF_HID_CAP_SDIO;
    report->capabilities |= PF_HID_CAP_SELF_TEST;
    report->ram_total_bytes = stats.ram_total_bytes;
    report->ram_reserved_bytes = stats.ram_used_bytes;
}

void pf_hid_build_configuration_report(pf_hid_configuration_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->stored_sysclk_profile = pf_get_sysclk_mhz();
    report->stored_sdcard_profile = (uint8_t)pf_boot_config_storage_speed();
    report->monitor_uart_index = pf_monitor_uart_board.uart_index <= 1u
        ? (uint8_t)pf_monitor_uart_board.uart_index : UINT8_MAX;
    report->monitor_uart_tx_gpio = gpio_value(pf_monitor_uart_board.tx_gpio);
    report->monitor_uart_rx_gpio = gpio_value(pf_monitor_uart_board.rx_gpio);
    pf_storage_status_t storage;
    pf_storage_get_status(&storage);
    memcpy(report->storage_gpios, storage.bus_gpios, sizeof(report->storage_gpios));
    if (storage.detect_gpio != UINT8_MAX) report->board_flags |= PF_HID_BOARD_FLAG_CARD_DETECT;
    report->card_detect_gpio = storage.detect_gpio;
    if (pf_led_board.gpio != PF_LED_GPIO_NONE) {
        report->board_flags |= PF_HID_BOARD_FLAG_LED;
        report->led_gpio = gpio_value(pf_led_board.gpio);
    } else {
        report->led_gpio = UINT8_MAX;
    }
    report->board_flags |= PF_HID_BOARD_FLAG_MONITOR_UART;
    report->reset_cause = pf_system_reset_cause();
}

void pf_hid_build_telemetry_report(pf_hid_telemetry_report_t *report, uint8_t sequence)
{
    pf_runtime_stats_t stats;
    int16_t temperature = 0;

    memset(report, 0, sizeof(*report));
    pf_runtime_stats_read(&stats);
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->uptime_ms = to_ms_since_boot(get_absolute_time());
    report->system_clock_hz = pf_system_clock_hz();
    report->core0_service_load_basis_points = stats.cpu_usage_basis_points[0];
    report->core1_service_load_basis_points = stats.cpu_usage_basis_points[1];
    if (pf_led_is_available()) {
        report->picoforge_flags |= PF_HID_TELEMETRY_LED_AVAILABLE;
        if (pf_led_is_on()) report->led_flags = 1u;
    }
    if (tud_mounted()) report->picoforge_flags |= PF_HID_TELEMETRY_USB_MOUNTED;
    if (tud_cdc_connected()) report->picoforge_flags |= PF_HID_TELEMETRY_CDC_OPEN;
    if (pf_temperature_read_centidegrees(&temperature)) {
        report->picoforge_flags |= PF_HID_TELEMETRY_TEMPERATURE_VALID;
        report->temperature_centidegrees_celsius = temperature;
    }
}

void pf_hid_build_storage_report(pf_hid_storage_report_t *report, uint8_t sequence)
{
    pf_storage_status_t storage;
    const bool msc_enabled = pf_usb_msc_is_enabled();
    const bool transition_pending = pf_usb_msc_transition_pending();

    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
    report->msc_enabled = msc_enabled;
    report->msc_transition_pending = transition_pending;
    report->msc_request_rejected = pf_usb_msc_request_rejected();
    if (!msc_enabled && !transition_pending) {
        pf_storage_get_status(&storage);
    } else {
        storage = (pf_storage_status_t){.state = PF_STORAGE_STATE_USB};
    }
    if (storage.state != PF_STORAGE_STATE_NOT_CONFIGURED) report->sd_flags |= PF_HID_STORAGE_CONFIGURED;

    switch (pf_fatfs_volume_filesystem_format()) {
    case PF_FATFS_VOLUME_FILESYSTEM_FAT12: report->filesystem_format = PF_HID_FILESYSTEM_FAT12; break;
    case PF_FATFS_VOLUME_FILESYSTEM_FAT16: report->filesystem_format = PF_HID_FILESYSTEM_FAT16; break;
    case PF_FATFS_VOLUME_FILESYSTEM_FAT32: report->filesystem_format = PF_HID_FILESYSTEM_FAT32; break;
    case PF_FATFS_VOLUME_FILESYSTEM_EXFAT: report->filesystem_format = PF_HID_FILESYSTEM_EXFAT; break;
    default: report->filesystem_format = PF_HID_FILESYSTEM_UNKNOWN; break;
    }

    if (storage.state < PF_STORAGE_STATE_ABSENT || storage.state == PF_STORAGE_STATE_USB) return;
    report->sd_flags |= PF_HID_STORAGE_SAMPLE_VALID;
    report->component_type = storage.medium == PF_STORAGE_MEDIUM_SDCARD ? 1u : 0u;
    report->bus_mode = storage.bus == PF_STORAGE_BUS_SPI ? PF_HID_BUS_MODE_SPI : PF_HID_BUS_MODE_SDIO;
    if (storage.state == PF_STORAGE_STATE_ABSENT) {
        report->sd_flags |= PF_HID_STORAGE_PRESENCE_KNOWN;
        return;
    }
    if (storage.state == PF_STORAGE_STATE_UNINITIALIZED) {
        if (storage.presence_known) report->sd_flags |= PF_HID_STORAGE_PRESENCE_KNOWN | PF_HID_STORAGE_PRESENT;
        return;
    }
    report->sd_flags |= PF_HID_STORAGE_PRESENCE_KNOWN | PF_HID_STORAGE_PRESENT | PF_HID_STORAGE_INITIALIZED;
    report->bus_clock_hz = storage.bus_clock_hz;
    report->speed_mode = storage.high_speed ? PF_HID_SPEED_MODE_HIGH_SPEED : PF_HID_SPEED_MODE_STANDARD;
    if (storage.id_valid) {
        report->sd_flags |= PF_HID_STORAGE_CID_VALID;
        memcpy(report->cid, storage.id, sizeof(report->cid));
    }
    if (storage.sector_count != 0u) {
        report->sd_flags |= PF_HID_STORAGE_CAPACITY_VALID;
        report->block_count = storage.sector_count;
    }
}

void pf_hid_build_self_test_report(pf_hid_self_test_report_t *report, uint8_t sequence)
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

static void process_control_request(const pf_hid_control_request_t *request)
{
    uint8_t result_data[4] = {0};
    uint8_t status = PF_HID_COMMAND_INVALID;
    uint8_t error = PF_HID_ERROR_MALFORMED;

    if (request->opcode == PF_HID_OPCODE_LED) {
        if (request->flags != 0u || !trailing_arguments_zero(request->arguments, 1u)) {
            error = PF_HID_ERROR_MALFORMED;
        } else if (request->arguments[0] > 1u) {
            error = PF_HID_ERROR_ARGUMENT;
        } else if (!pf_led_is_available()) {
            status = PF_HID_COMMAND_UNSUPPORTED;
            error = PF_HID_ERROR_CAPABILITY;
        } else if (!pf_led_set(request->arguments[0] != 0u)) {
            status = PF_HID_COMMAND_FAILED;
            error = PF_HID_ERROR_OPERATION;
        } else {
            status = PF_HID_COMMAND_COMPLETED;
            error = PF_HID_ERROR_NONE;
        }
    } else if (request->opcode == PF_HID_OPCODE_REBOOT) {
        if ((request->flags & (uint8_t)~1u) != 0u || !trailing_arguments_zero(request->arguments, 0u)) {
            error = PF_HID_ERROR_MALFORMED;
        } else if ((request->flags & 1u) == 0u) {
            error = PF_HID_ERROR_CONFIRMATION;
        } else if (pf_storage_worker_busy() || pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
            status = PF_HID_COMMAND_BUSY;
            error = PF_HID_ERROR_BUSY;
        } else {
            reboot_requested = true;
            status = PF_HID_COMMAND_COMPLETED;
            error = PF_HID_ERROR_NONE;
        }
    } else if (request->opcode == PF_HID_OPCODE_MSC) {
        const uint8_t action = request->arguments[0];
        if ((request->flags & (uint8_t)~1u) != 0u || !trailing_arguments_zero(request->arguments, 1u)) {
            error = PF_HID_ERROR_MALFORMED;
        } else if (action > 2u) {
            error = PF_HID_ERROR_ARGUMENT;
        } else if (action == 2u && (request->flags & 1u) == 0u) {
            error = PF_HID_ERROR_CONFIRMATION;
        } else if ((action != 2u) && request->flags != 0u) {
            error = PF_HID_ERROR_MALFORMED;
        } else if (action == 2u && !pf_usb_msc_is_enabled()) {
            error = PF_HID_ERROR_ARGUMENT;
        } else if (pf_storage_worker_busy()) {
            status = PF_HID_COMMAND_BUSY;
            error = PF_HID_ERROR_BUSY;
        } else {
            bool accepted = (action == 2u) ? pf_usb_msc_request_force_return()
                                            : pf_usb_msc_request(action == 1u);
            if (!accepted) {
                status = PF_HID_COMMAND_BUSY;
                error = PF_HID_ERROR_BUSY;
            } else {
                pf_usb_msc_defer_for_hid_result(request->transaction_id);
                status = PF_HID_COMMAND_COMPLETED;
                error = PF_HID_ERROR_NONE;
            }
        }
    } else if (request->opcode == PF_HID_OPCODE_SELF_TEST) {
        const uint8_t action = request->arguments[0];
        const uint32_t size_mib = read_u32_le(&request->arguments[1]);
        pf_file_benchmark_status_t benchmark;
        if (request->flags != 0u) {
            error = PF_HID_ERROR_MALFORMED;
        } else if (action > 1u || (action == 0u && size_mib != 0u)) {
            error = PF_HID_ERROR_ARGUMENT;
        } else if (action == 1u && (size_mib == 0u || size_mib > 1024u)) {
            (void)pf_storage_worker_benchmark_start(size_mib, false);
            status = PF_HID_COMMAND_INVALID;
            error = PF_HID_ERROR_ARGUMENT;
        } else if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
            status = PF_HID_COMMAND_BUSY;
            error = PF_HID_ERROR_BUSY;
        } else if (action == 0u) {
            pf_storage_worker_benchmark_status(&benchmark);
            if (benchmark.state >= PF_FILE_BENCHMARK_QUEUED &&
                benchmark.state <= PF_FILE_BENCHMARK_READING) {
                if (!pf_storage_worker_benchmark_cancel()) {
                    status = PF_HID_COMMAND_BUSY;
                    error = PF_HID_ERROR_BUSY;
                } else {
                    status = PF_HID_COMMAND_COMPLETED;
                    error = PF_HID_ERROR_NONE;
                }
            } else {
                status = PF_HID_COMMAND_COMPLETED;
                error = PF_HID_ERROR_NONE;
            }
        } else if (pf_storage_worker_benchmark_start(size_mib, false)) {
            pending_self_test_request = *request;
            self_test_result_pending = true;
            status = PF_HID_COMMAND_PENDING;
            error = PF_HID_ERROR_NONE;
        } else {
            pf_storage_worker_benchmark_status(&benchmark);
            if (benchmark.request_error == PF_FILE_BENCHMARK_ERROR_BUSY ||
                benchmark.request_error == PF_FILE_BENCHMARK_ERROR_USB_OWNER) {
                status = PF_HID_COMMAND_BUSY;
                error = PF_HID_ERROR_BUSY;
            } else {
                status = PF_HID_COMMAND_FAILED;
                error = PF_HID_ERROR_OPERATION;
            }
        }
    } else if (request->opcode == PF_HID_OPCODE_SAVE_SETTINGS) {
        const uint16_t system_clock_mhz = read_u16_le(request->arguments);
        const uint8_t sdcard_profile = request->arguments[2];
        const pf_sysclk_profile_t old_sysclk = pf_boot_config_sysclk();
        const pf_storage_speed_t old_sd_speed = pf_boot_config_storage_speed();
        pf_sysclk_profile_t sysclk = PF_SYSCLK_DEFAULT;
        while (sysclk < PF_SYSCLK_250MHZ && pf_sysclk_profile_mhz(sysclk) != system_clock_mhz) {
            ++sysclk;
        }
        if (request->flags != 0u || !trailing_arguments_zero(request->arguments, 3u)) {
            error = PF_HID_ERROR_MALFORMED;
        } else if (pf_sysclk_profile_mhz(sysclk) != system_clock_mhz || !pf_boot_config_set_sysclk(sysclk) ||
                   !pf_boot_config_set_storage_speed((pf_storage_speed_t)sdcard_profile)) {
            error = PF_HID_ERROR_ARGUMENT;
        } else if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending() || pf_storage_worker_busy()) {
            status = PF_HID_COMMAND_BUSY;
            error = PF_HID_ERROR_BUSY;
        } else if (!pf_boot_config_save()) {
            status = PF_HID_COMMAND_FAILED;
            error = PF_HID_ERROR_OPERATION;
        } else {
            result_data[0] = (uint8_t)system_clock_mhz;
            result_data[1] = (uint8_t)(system_clock_mhz >> 8u);
            result_data[2] = sdcard_profile;
            status = PF_HID_COMMAND_COMPLETED;
            error = PF_HID_ERROR_NONE;
            reboot_requested = true;
        }
        if (status != PF_HID_COMMAND_COMPLETED) {
            /* Keep the RAM copy equal to flash so reports never show unsaved values. */
            (void)pf_boot_config_set_sysclk(old_sysclk);
            (void)pf_boot_config_set_storage_speed(old_sd_speed);
        }
    } else {
        status = PF_HID_COMMAND_UNSUPPORTED;
        error = PF_HID_ERROR_OPCODE;
    }

    set_result(request, status, error, result_data);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    if (instance != 0u || buffer == NULL) return 0u;

    if (report_type == HID_REPORT_TYPE_FEATURE) {
        if (report_id == PF_HID_IDENTITY_REPORT_ID && reqlen >= sizeof(pf_hid_identity_report_t)) {
            pf_hid_identity_report_t report;
            pf_hid_build_identity_report(&report);
            memcpy(buffer, &report, sizeof(report));
            return sizeof(report);
        }
        if (report_id == PF_HID_CONFIGURATION_REPORT_ID &&
            reqlen >= sizeof(pf_hid_configuration_report_t)) {
            pf_hid_configuration_report_t report;
            pf_hid_build_configuration_report(&report);
            memcpy(buffer, &report, sizeof(report));
            return sizeof(report);
        }
        if (report_id == PF_HID_CONTROL_REPORT_ID && reqlen >= sizeof(control_result)) {
            pf_usb_msc_note_hid_result_read(control_result.transaction_id);
            memcpy(buffer, &control_result, sizeof(control_result));
            return sizeof(control_result);
        }
    } else if (report_type == HID_REPORT_TYPE_INPUT) {
        if (report_id == PF_HID_TELEMETRY_REPORT_ID && reqlen >= sizeof(pf_hid_telemetry_report_t)) {
            pf_hid_telemetry_report_t report;
            pf_hid_build_telemetry_report(&report, 0u);
            memcpy(buffer, &report, sizeof(report));
            return sizeof(report);
        }
        if (report_id == PF_HID_STORAGE_REPORT_ID && reqlen >= sizeof(pf_hid_storage_report_t)) {
            pf_hid_storage_report_t report;
            pf_hid_build_storage_report(&report, 0u);
            memcpy(buffer, &report, sizeof(report));
            return sizeof(report);
        }
        if (report_id == PF_HID_SELF_TEST_REPORT_ID && reqlen >= sizeof(pf_hid_self_test_report_t)) {
            pf_hid_identity_report_t identity;
            pf_hid_build_identity_report(&identity);
            if ((identity.capabilities & PF_HID_CAP_SELF_TEST) == 0u) return 0u;
            pf_hid_self_test_report_t report;
            pf_hid_build_self_test_report(&report, 0u);
            memcpy(buffer, &report, sizeof(report));
            return sizeof(report);
        }
    }
    return 0u;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    if (instance != 0u || report_id != PF_HID_CONTROL_REPORT_ID ||
        report_type != HID_REPORT_TYPE_FEATURE || buffer == NULL ||
        bufsize != sizeof(pf_hid_control_request_t)) return;

    pf_hid_control_request_t request;
    memcpy(&request, buffer, sizeof(request));
    if (request.version != PF_HID_REPORT_VERSION || request.transaction_id == 0u) return;
    process_control_request(&request);
}
