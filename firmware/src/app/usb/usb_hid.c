/**
 * @file
 * @brief Vendor HID reports and the Feature 20 control channel (LED, reboot, MSC, benchmark, boot settings).
 *
 * Runs on the USB task. Control requests are validated completely before any side effect, and every request ends in
 * exactly one published result.
 */
#include <limits.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_hid.h"
#include "config/config.h"
#include "platform/led.h"
#include "platform/logging.h"
#include "platform/storage/storage.h"
#include "platform/boot_config.h"
#include "platform/temperature.h"
#include "usb_msc.h"
#include "platform/storage/fatfs_volume.h"
#include "app/storage/storage_worker.h"
#include "app/runtime_stats.h"
#if PF_ENABLE_WIRELESS
#include "app/wireless/wireless.h"
#include "app/wireless/wireless_auth.h"
static bool ble_dispatch; /**< Current synchronous dispatcher targets the BLE-specific result slot. */
static pf_hid_control_result_t ble_result = { .version = PF_HID_REPORT_VERSION };
static uint64_t tcp_auth_proof_expires_us; /**< USB opcode 13 proof retention deadline; GET never consumes it. */
#endif

#ifndef PF_HID_BOARD_MODEL
#define PF_HID_BOARD_MODEL 0u
#endif

#define PF_HID_BOARD_FLAG_CARD_DETECT (1u << 0)
#define PF_HID_BOARD_FLAG_LED (1u << 1)
#define PF_HID_BOARD_FLAG_MONITOR_UART (1u << 2)
/** Bits 3-4 carry the next-boot Wi-Fi and BLE enables; PF_WIRELESS_* shifted left by 3. */
#define PF_HID_BOARD_RADIO_SHIFT 3u
#if PF_ENABLE_WIRELESS
#define PF_HID_RADIO_MASK (PF_WIRELESS_WIFI | PF_WIRELESS_BLE)
#else
#define PF_HID_RADIO_MASK 0u /**< Boards without a radio report and accept no radio enables. */
#endif

static bool reboot_requested; /**< A reboot or settings save awaits the USB task. */
static bool force_save_pending; /**< Confirmed save waits for Pending GET before USB detach. */
static bool force_save_result_read; /**< Host fetched the forced-save Pending result. */
static uint64_t force_save_at_us; /**< Earliest detach, or deadline if no Pending GET arrives. */
static bool force_save_reboot_waiting; /**< Completed forced save waits for result GET before reboot. */
static uint64_t force_save_reboot_deadline_us; /**< Bounded wait for reconnect and result GET. */
static pf_hid_control_request_t force_save_request; /**< Original transaction; never replayed. */
static uint8_t control_write[PF_HID_CONTROL_PAYLOAD_SIZE + 1u]; /**< USB SET envelope, including report ID. */
static bool control_write_valid; /**< DATA-stage validation passed before the USB status handshake. */
static bool control_write_receiving; /**< Count actual OUT bytes, including prematurely short transfers. */
static uint32_t control_write_bytes; /**< Received byte count for the current Feature 20 SET. */
/** Latest Feature 20 result returned by GET. */
static pf_hid_control_result_t control_result = {
    .version = PF_HID_REPORT_VERSION,
    .status = PF_HID_COMMAND_IDLE,
};
#if PF_ENABLE_STORAGE_BENCHMARK
static bool self_test_result_pending; /**< A benchmark start awaits its worker preflight result. */
static pf_hid_control_request_t pending_self_test_request; /**< Request to complete once preflight resolves. */
#endif

static bool trailing_arguments_zero(const uint8_t *arguments, size_t first)
{
    for (size_t index = first; index < 64u; ++index) {
        if (arguments[index] != 0u) return false;
    }
    return true;
}

static uint16_t read_u16_le(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8u);
}

#if PF_ENABLE_STORAGE_BENCHMARK
static uint32_t read_u32_le(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8u) |
        ((uint32_t)value[2] << 16u) | ((uint32_t)value[3] << 24u);
}
#endif

static void set_result(const pf_hid_control_request_t *request, uint8_t status, uint8_t error,
                       const uint8_t result_data[63])
{
    pf_hid_control_result_t *target = &control_result;
#if PF_ENABLE_WIRELESS
    if (ble_dispatch) target = &ble_result;
#endif
    *target = (pf_hid_control_result_t){
        .version = PF_HID_REPORT_VERSION,
        .transaction_id = request->transaction_id,
        .opcode = request->opcode,
        .status = status,
        .error_reason = error,
    };
    if (result_data != NULL) memcpy(target->result_data, result_data, sizeof(target->result_data));
#if PF_ENABLE_WIRELESS
    if (!ble_dispatch) {
        tcp_auth_proof_expires_us = request->opcode == PF_HID_OPCODE_TCP_PROOF &&
            status == PF_HID_COMMAND_COMPLETED ? time_us_64() + 10000000u : 0u;
    }
#endif
}

static uint8_t board_model(void)
{
    return (uint8_t)PF_HID_BOARD_MODEL;
}

/** Outcome of one control command before it is published as the Feature 20 result. */
typedef struct {
    uint8_t status; /**< PF_HID_COMMAND_* value. */
    uint8_t error; /**< PF_HID_ERROR_* value. */
    uint8_t data[63]; /**< Opcode-specific result bytes, zero otherwise. */
} outcome_t;

static outcome_t handle_save_settings(const pf_hid_control_request_t *request);

static outcome_t outcome(uint8_t status, uint8_t error)
{
    return (outcome_t){ .status = status, .error = error };
}

static outcome_t completed(void) { return outcome(PF_HID_COMMAND_COMPLETED, PF_HID_ERROR_NONE); }
static outcome_t busy_outcome(void)
{
    outcome_t result = outcome(PF_HID_COMMAND_BUSY, PF_HID_ERROR_BUSY);
    result.data[0] = pf_usb_msc_is_enabled() ? 2u : 1u;
    return result;
}
static outcome_t failed(void) { return outcome(PF_HID_COMMAND_FAILED, PF_HID_ERROR_OPERATION); }
static outcome_t invalid(uint8_t error) { return outcome(PF_HID_COMMAND_INVALID, error); }

#if PF_ENABLE_STORAGE_BENCHMARK
/** @brief Map a rejected benchmark admission to the busy or failed outcome. */
static outcome_t benchmark_rejection(pf_file_benchmark_error_t error)
{
    outcome_t result = (error == PF_FILE_BENCHMARK_ERROR_BUSY || error == PF_FILE_BENCHMARK_ERROR_USB_OWNER)
        ? busy_outcome() : failed();
    result.data[0] = (uint8_t)error;
    return result;
}
#endif

bool pf_hid_take_reboot_request(void)
{
    if (force_save_reboot_waiting) {
        if (time_us_64() < force_save_reboot_deadline_us) return false;
        force_save_reboot_waiting = false;
    }
    const bool requested = reboot_requested;
    reboot_requested = false;
    return requested;
}

bool pf_hid_control_pending(void)
{
    return reboot_requested || force_save_pending || force_save_reboot_waiting
#if PF_ENABLE_STORAGE_BENCHMARK
        || self_test_result_pending
#endif
        ;
}

void pf_hid_poll_async_commands(void)
{
    if (force_save_pending && time_us_64() >= force_save_at_us) {
        force_save_pending = false;
        force_save_result_read = true;
        outcome_t result = handle_save_settings(&force_save_request);
        set_result(&force_save_request, result.status, result.error, result.data);
        if (result.status == PF_HID_COMMAND_COMPLETED) {
            force_save_reboot_waiting = true;
            force_save_reboot_deadline_us = time_us_64() + 5000000u;
        }
    }
#if PF_ENABLE_STORAGE_BENCHMARK
    if (!self_test_result_pending) return;

    bool accepted;
    pf_file_benchmark_error_t error;
    if (!pf_storage_worker_benchmark_admission_result(&accepted, &error)) return;

    self_test_result_pending = false;
    pf_hid_control_result_t *target = &control_result;
    if ((target->transaction_id != pending_self_test_request.transaction_id) ||
        (target->opcode != pending_self_test_request.opcode)) {
        return;
    }
    const outcome_t result = accepted ? completed() : benchmark_rejection(error);
    set_result(&pending_self_test_request, result.status, result.error, result.data);
#endif
}

bool pf_hid_self_test_supported(void)
{
    return PF_ENABLE_STORAGE_BENCHMARK != 0;
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
    report->capabilities = PF_HID_CAP_LED | PF_HID_CAP_REBOOT | PF_HID_CAP_SETTINGS | PF_HID_CAP_FORCE_SAVE;
#if PF_ENABLE_WIRELESS
    report->capabilities |= PF_HID_CAP_WIRELESS;
#endif
    pf_storage_status_t storage;
    pf_storage_get_status(&storage);
    if (storage.bus == PF_STORAGE_BUS_SPI) report->capabilities |= PF_HID_CAP_SPI;
    if (storage.bus == PF_STORAGE_BUS_SDIO) report->capabilities |= PF_HID_CAP_SDIO;
    if (pf_hid_self_test_supported()) report->capabilities |= PF_HID_CAP_SELF_TEST;
    report->ram_total_bytes = stats.ram_total_bytes;
    report->ram_reserved_bytes = stats.ram_used_bytes;
}

void pf_hid_build_configuration_report(pf_hid_configuration_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->stored_sysclk_profile = pf_get_sysclk_mhz();
    report->stored_sdcard_profile = (uint8_t)pf_boot_config_storage_speed();
    report->monitor_uart_index = pf_logging_board.uart_index <= 1u
        ? (uint8_t)pf_logging_board.uart_index : UINT8_MAX;
    report->monitor_uart_tx_gpio = pf_logging_board.tx_gpio;
    report->monitor_uart_rx_gpio = pf_logging_board.rx_gpio;
    pf_storage_status_t storage;
    pf_storage_get_status(&storage);
    memcpy(report->storage_gpios, storage.bus_gpios, sizeof(report->storage_gpios));
    if (storage.detect_gpio != UINT8_MAX) report->board_flags |= PF_HID_BOARD_FLAG_CARD_DETECT;
    report->card_detect_gpio = storage.detect_gpio;
    if (pf_led_board.gpio != PF_LED_GPIO_NONE) {
        report->board_flags |= PF_HID_BOARD_FLAG_LED;
        report->led_gpio = pf_led_board.gpio;
    } else {
        report->led_gpio = UINT8_MAX;
    }
    /* The logging UART is always configured by the board, so unlike the LED and card detect it has no absent case. */
    report->board_flags |= PF_HID_BOARD_FLAG_MONITOR_UART;
    report->board_flags |= (uint8_t)((pf_boot_config_radios() & PF_HID_RADIO_MASK) << PF_HID_BOARD_RADIO_SHIFT);
#if PF_ENABLE_WIRELESS
    if (pf_boot_config_tcp_cli_enabled()) report->board_flags |= 1u << 5u;
#endif
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
    report->uptime_ms = pf_system_uptime_ms();
    report->system_clock_hz = pf_system_clock_hz();
    /* PF_RUNTIME_CPU_UNAVAILABLE (0xFFFF) is passed through as the wire value for "not sampled yet". */
    report->core0_service_load_basis_points = stats.cpu_usage_basis_points[0];
    report->core1_service_load_basis_points = stats.cpu_usage_basis_points[1];
    report->picoforge_flags |= PF_HID_TELEMETRY_LED_AVAILABLE;
    if (pf_led_is_on()) report->led_flags = 1u;
    if (tud_mounted()) report->picoforge_flags |= PF_HID_TELEMETRY_USB_MOUNTED;
    if (tud_cdc_connected()) report->picoforge_flags |= PF_HID_TELEMETRY_CDC_OPEN;
    if (pf_temperature_read_centidegrees(&temperature)) {
        report->picoforge_flags |= PF_HID_TELEMETRY_TEMPERATURE_VALID;
        report->temperature_centidegrees_celsius = temperature;
    }
}

/** @brief Map the mounted volume's filesystem to its wire code. */
static uint8_t filesystem_code(pf_fatfs_volume_filesystem_format_t format)
{
    switch (format) {
    case PF_FATFS_VOLUME_FILESYSTEM_FAT12: return PF_HID_FILESYSTEM_FAT12;
    case PF_FATFS_VOLUME_FILESYSTEM_FAT16: return PF_HID_FILESYSTEM_FAT16;
    case PF_FATFS_VOLUME_FILESYSTEM_FAT32: return PF_HID_FILESYSTEM_FAT32;
    case PF_FATFS_VOLUME_FILESYSTEM_EXFAT: return PF_HID_FILESYSTEM_EXFAT;
    default: return PF_HID_FILESYSTEM_UNKNOWN;
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

    /* Sampled separately from the status; across a remount one report may mix the two and the next corrects it. */
    report->filesystem_format = filesystem_code(pf_fatfs_volume_filesystem_format());

    if (storage.state < PF_STORAGE_STATE_ABSENT || storage.state == PF_STORAGE_STATE_USB) return;
    report->sd_flags |= PF_HID_STORAGE_SAMPLE_VALID;
    report->component_type = storage.medium == PF_STORAGE_MEDIUM_SDCARD ? PF_HID_COMPONENT_SDCARD
                                                                      : PF_HID_COMPONENT_NONE;
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
    memset(report, 0, sizeof(*report));
    report->version = PF_HID_REPORT_VERSION;
    report->sequence = sequence;
#if PF_ENABLE_STORAGE_BENCHMARK
    pf_file_benchmark_status_t status;
    pf_storage_worker_benchmark_status(&status);
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
#endif
}

static outcome_t handle_led(const pf_hid_control_request_t *request)
{
    if (request->flags != 0u || !trailing_arguments_zero(request->arguments, 1u)) {
        return invalid(PF_HID_ERROR_MALFORMED);
    }
    if (request->arguments[0] > 1u) {
        return invalid(PF_HID_ERROR_ARGUMENT);
    }
    return pf_led_set(request->arguments[0] != 0u) ? completed() : failed();
}

static outcome_t handle_reboot(const pf_hid_control_request_t *request)
{
    if ((request->flags & (uint8_t)~1u) != 0u || !trailing_arguments_zero(request->arguments, 0u)) {
        return invalid(PF_HID_ERROR_MALFORMED);
    }
    if ((request->flags & 1u) == 0u) {
        return invalid(PF_HID_ERROR_CONFIRMATION);
    }
#if PF_ENABLE_WIRELESS
    if (ble_dispatch && (force_save_pending || force_save_reboot_waiting)) {
        outcome_t result = busy_outcome();
        result.data[0] = 1u;
        return result;
    }
#endif
    if (pf_storage_worker_busy() || pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
        return busy_outcome();
    }
#if PF_ENABLE_WIRELESS
    if (ble_dispatch && pf_wireless_staged()) {
        outcome_t result = busy_outcome();
        result.data[0] = 28u;
        return result;
    }
    pf_wireless_discard();
#endif
    reboot_requested = true;
    return completed();
}

static outcome_t handle_msc(const pf_hid_control_request_t *request)
{
    const uint8_t action = request->arguments[0];
    const bool force = action == PF_HID_MSC_ACTION_FORCE_RETURN;

    if ((request->flags & (uint8_t)~1u) != 0u || !trailing_arguments_zero(request->arguments, 1u)) {
        return invalid(PF_HID_ERROR_MALFORMED);
    }
    if (action > PF_HID_MSC_ACTION_FORCE_RETURN) {
        return invalid(PF_HID_ERROR_ARGUMENT);
    }
    if (force && (request->flags & 1u) == 0u) {
        return invalid(PF_HID_ERROR_CONFIRMATION);
    }
    if (!force && request->flags != 0u) {
        return invalid(PF_HID_ERROR_MALFORMED);
    }
    if (pf_storage_worker_busy() || pf_usb_msc_transition_pending()) {
        return busy_outcome();
    }
    if (force && !pf_usb_msc_is_enabled()) return invalid(PF_HID_ERROR_ARGUMENT);
    const bool accepted = force ? pf_usb_msc_request_force_return()
                                : pf_usb_msc_request(action == PF_HID_MSC_ACTION_ENABLE);
    if (!accepted) {
        return busy_outcome();
    }
    pf_usb_msc_defer_for_hid_result(request->transaction_id);
    return completed();
}

#if PF_ENABLE_STORAGE_BENCHMARK
/** @brief Cancel a running benchmark; completes trivially when none is running. */
static outcome_t cancel_self_test(void)
{
    pf_file_benchmark_status_t benchmark;

    pf_storage_worker_benchmark_status(&benchmark);
    if (benchmark.state >= PF_FILE_BENCHMARK_QUEUED && benchmark.state <= PF_FILE_BENCHMARK_READING &&
        !pf_storage_worker_benchmark_cancel()) {
        return busy_outcome();
    }
    return completed();
}

/** @brief Start a benchmark; the result stays pending until the worker resolves its preflight. */
static outcome_t start_self_test(const pf_hid_control_request_t *request, uint32_t size_mib)
{
    pf_file_benchmark_status_t benchmark;

    if (pf_storage_worker_benchmark_start(size_mib, false)) {
        pending_self_test_request = *request;
        self_test_result_pending = true;
        return outcome(PF_HID_COMMAND_PENDING, PF_HID_ERROR_NONE);
    }
    pf_storage_worker_benchmark_status(&benchmark);
    return benchmark_rejection(benchmark.request_error);
}

static outcome_t handle_self_test(const pf_hid_control_request_t *request)
{
    const uint8_t action = request->arguments[0];
    const uint32_t size_mib = read_u32_le(&request->arguments[1]);

    if (request->flags != 0u || !trailing_arguments_zero(request->arguments, 5u)) {
        return invalid(PF_HID_ERROR_MALFORMED);
    }
    if (action > 1u || (action == 0u && size_mib != 0u)) {
        return invalid(PF_HID_ERROR_ARGUMENT);
    }
    if (action == 1u && (size_mib == 0u || size_mib > PF_FILE_BENCHMARK_MAX_MIB)) {
        outcome_t result = invalid(PF_HID_ERROR_ARGUMENT);
        result.data[0] = 3u;
        return result;
    }
    if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
        return busy_outcome();
    }
    return action == 0u ? cancel_self_test() : start_self_test(request, size_mib);
}
#endif

#if PF_ENABLE_WIRELESS
/** @copydoc pf_hid_ble_control */
bool pf_hid_ble_control(const pf_hid_control_request_t *request)
{
    if (request->version != PF_HID_REPORT_VERSION || request->transaction_id == 0u) return false;
    ble_dispatch = true;
    outcome_t result;
    switch (request->opcode) {
    case PF_HID_OPCODE_LED: result = handle_led(request); break;
    case PF_HID_OPCODE_REBOOT: result = handle_reboot(request); break;
    case PF_HID_OPCODE_SAVE_SETTINGS: result = handle_save_settings(request); break;
    default:
        result = outcome(PF_HID_COMMAND_UNSUPPORTED,
            request->opcode >= 1u && request->opcode <= 13u ? PF_HID_ERROR_CAPABILITY : PF_HID_ERROR_OPCODE);
        break;
    }
    set_result(request, result.status, result.error, result.data);
    ble_dispatch = false;
    return true;
}

/** @copydoc pf_hid_ble_result */
void pf_hid_ble_result(pf_hid_control_result_t *report)
{
    *report = ble_result;
}
#endif

/** @brief Validate, stage and save new boot settings; the caller discards the staged values unless this completes. */
static outcome_t save_settings(const pf_hid_control_request_t *request)
{
    const uint16_t system_clock_mhz = read_u16_le(request->arguments);
    const uint8_t sdcard_profile = request->arguments[2];
    const uint8_t switches = request->arguments[3];
    const uint8_t radios = switches & 3u;
    const bool force = (request->flags & PF_HID_SAVE_SETTINGS_FORCE_MSC) != 0u;
    const bool commit = (request->flags & PF_HID_SAVE_SETTINGS_COMMIT_WIRELESS) != 0u;
    outcome_t result;

    if ((request->flags & (uint8_t)~3u) != 0u || !trailing_arguments_zero(request->arguments, 4u)) {
        return invalid(PF_HID_ERROR_MALFORMED);
    }
#if PF_ENABLE_WIRELESS
    if (ble_dispatch && request->flags != 0u) return invalid(PF_HID_ERROR_MALFORMED);
#endif
    if ((switches & (uint8_t)~(PF_HID_RADIO_MASK != 0u ? 7u : 0u)) != 0u ||
        (system_clock_mhz != 0u && system_clock_mhz != 150u &&
         system_clock_mhz != 200u && system_clock_mhz != 250u) || sdcard_profile > 1u) {
        return invalid(PF_HID_ERROR_ARGUMENT);
    }
#if PF_ENABLE_WIRELESS
    if (ble_dispatch && (force_save_pending || force_save_reboot_waiting)) {
        result = busy_outcome();
        result.data[0] = 1u;
        return result;
    }
#endif
    if (pf_usb_msc_transition_pending() || pf_storage_worker_busy() ||
        (pf_usb_msc_is_enabled() && !force)) {
        return busy_outcome();
    }
#if PF_ENABLE_WIRELESS
    if (ble_dispatch && pf_wireless_staged()) {
        result = busy_outcome();
        result.data[0] = 28u;
        return result;
    }
    const uint8_t detail = pf_wireless_save_precondition(commit);
    if (detail != 0u) {
        result = detail == 25u || detail == 26u ? busy_outcome() : invalid(PF_HID_ERROR_ARGUMENT);
        result.data[0] = detail;
        return result;
    }
    if (ble_dispatch && radios != pf_boot_config_radios()) return invalid(PF_HID_ERROR_ARGUMENT);
#else
    if (commit) {
        result = invalid(PF_HID_ERROR_ARGUMENT);
        result.data[0] = 21u;
        return result;
    }
#endif
    if (force && !pf_usb_msc_is_enabled()) return invalid(PF_HID_ERROR_ARGUMENT);
    if (force && !force_save_pending && !force_save_result_read) {
        force_save_request = *request;
        force_save_pending = true;
        force_save_at_us = time_us_64() + 500000u;
        return outcome(PF_HID_COMMAND_PENDING, PF_HID_ERROR_NONE);
    }
    if (force && !pf_usb_msc_prepare_save()) {
        PF_LOGGING_ERROR("settings", "Forced save refused: storage sync or USB detach failed");
        return failed();
    }
    uint8_t commit_mask = 0u;
    bool staged = pf_boot_config_set_sysclk(pf_sysclk_profile_from_mhz(system_clock_mhz)) &&
        pf_boot_config_set_storage_speed((pf_storage_speed_t)sdcard_profile);
#if PF_ENABLE_WIRELESS
    staged = staged && pf_boot_config_set_radios(radios) &&
        pf_boot_config_set_tcp_cli_enabled((switches & 4u) != 0u) &&
        (!commit || pf_wireless_apply_staged(&commit_mask));
#else
    (void)radios;
#endif
    bool saved = staged && pf_boot_config_save();
    if (force) pf_usb_msc_finish_save(saved);
    if (!saved) {
        PF_LOGGING_ERROR("settings", "Boot settings save failed; reboot not scheduled");
        return failed();
    }
    result = completed();
    result.data[0] = (uint8_t)system_clock_mhz;
    result.data[1] = (uint8_t)(system_clock_mhz >> 8u);
    result.data[2] = sdcard_profile;
    result.data[3] = switches;
    result.data[4] = commit_mask;
#if PF_ENABLE_WIRELESS
    pf_wireless_saved();
#endif
    reboot_requested = true;
    return result;
}

/** @brief Roll back boot-setting mutations on rejection; wireless candidates remain available for retry. */
static outcome_t handle_save_settings(const pf_hid_control_request_t *request)
{
    outcome_t result = save_settings(request);
    if (result.status != PF_HID_COMMAND_COMPLETED) pf_boot_config_discard();
    return result;
}

static void process_control_request(const pf_hid_control_request_t *request)
{
    outcome_t result;
    if (force_save_pending || force_save_reboot_waiting) {
        /* Preserve the outstanding transaction for reconnect result verification. */
        PF_LOGGING_WARNING("settings", "Control write ignored while forced-save result is outstanding");
        return;
    }
    force_save_result_read = false;

    switch (request->opcode) {
    case PF_HID_OPCODE_LED: result = handle_led(request); break;
    case PF_HID_OPCODE_REBOOT: result = handle_reboot(request); break;
    case PF_HID_OPCODE_MSC: result = handle_msc(request); break;
    case PF_HID_OPCODE_SELF_TEST:
#if PF_ENABLE_STORAGE_BENCHMARK
        result = handle_self_test(request);
#else
        result = outcome(PF_HID_COMMAND_UNSUPPORTED, PF_HID_ERROR_CAPABILITY);
#endif
        break;
    case PF_HID_OPCODE_SAVE_SETTINGS: result = handle_save_settings(request); break;
#if PF_ENABLE_WIRELESS
    case PF_HID_OPCODE_STAGE_SSID:
    case PF_HID_OPCODE_STAGE_PASSWORD:
    case PF_HID_OPCODE_STAGE_KEY:
    case PF_HID_OPCODE_WIFI_TEST:
    case PF_HID_OPCODE_WIFI_CANCEL:
    case PF_HID_OPCODE_WIFI_SCAN:
    case PF_HID_OPCODE_WIFI_ENTRY:
    case PF_HID_OPCODE_TCP_PROOF: {
        pf_hid_control_result_t wireless_result;
        pf_wireless_command(request, &wireless_result);
        set_result(request, wireless_result.status, wireless_result.error_reason, wireless_result.result_data);
        return;
    }
#else
    case PF_HID_OPCODE_STAGE_SSID:
    case PF_HID_OPCODE_STAGE_PASSWORD:
    case PF_HID_OPCODE_STAGE_KEY:
    case PF_HID_OPCODE_WIFI_TEST:
    case PF_HID_OPCODE_WIFI_CANCEL:
    case PF_HID_OPCODE_WIFI_SCAN:
    case PF_HID_OPCODE_WIFI_ENTRY:
    case PF_HID_OPCODE_TCP_PROOF:
        result = outcome(PF_HID_COMMAND_UNSUPPORTED, PF_HID_ERROR_CAPABILITY);
        break;
#endif
    default: result = outcome(PF_HID_COMMAND_UNSUPPORTED, PF_HID_ERROR_OPCODE); break;
    }
    set_result(request, result.status, result.error, result.data);
}

bool __real_hidd_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request);
bool __real_usbd_control_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t bytes);
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize);

/** @brief Count SET data before TinyUSB invokes DATA validation; its class callback exposes only requested length. */
bool __wrap_usbd_control_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t bytes)
{
    if (control_write_receiving && ep_addr == 0u && result == XFER_RESULT_SUCCESS) control_write_bytes += bytes;
    return __real_usbd_control_xfer_cb(rhport, ep_addr, result, bytes);
}

/**
 * @brief Reject invalid USB SET envelopes before the status handshake, without changing the result slot.
 *
 * The pinned TinyUSB SET callback runs at ACK, too late to reject an invalid envelope. Interpose only SET_REPORT;
 * descriptors, GET_REPORT and the remaining HID class requests keep the SDK implementation.
 */
bool __wrap_hidd_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    if (stage == CONTROL_STAGE_SETUP) control_write_receiving = false;
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS || request->bRequest != HID_REQ_CONTROL_SET_REPORT) {
        return __real_hidd_control_xfer_cb(rhport, stage, request);
    }
    if (request->bmRequestType_bit.direction != TUSB_DIR_OUT ||
        request->wValue != ((uint16_t)HID_REPORT_TYPE_FEATURE << 8u | PF_HID_CONTROL_REPORT_ID) ||
        request->wLength != sizeof(control_write)) return false;
    if (stage == CONTROL_STAGE_SETUP) {
        control_write_valid = false;
        control_write_receiving = true;
        control_write_bytes = 0u;
        memset(control_write, 0, sizeof(control_write));
        return tud_control_xfer(rhport, request, control_write, sizeof(control_write));
    }
    if (stage == CONTROL_STAGE_DATA) {
        control_write_receiving = false;
        control_write_valid = control_write_bytes == sizeof(control_write) &&
            control_write[0] == PF_HID_CONTROL_REPORT_ID &&
            control_write[1] == PF_HID_REPORT_VERSION && control_write[2] != 0u;
        return control_write_valid;
    }
    if (stage != CONTROL_STAGE_ACK || !control_write_valid) return false;
    control_write_valid = false;
    tud_hid_set_report_cb(0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE,
                          control_write + 1u, PF_HID_CONTROL_PAYLOAD_SIZE);
    return true;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    if (instance != 0u || buffer == NULL) return 0u;

    if (report_type == HID_REPORT_TYPE_FEATURE) {
#if PF_ENABLE_WIRELESS
        if (report_id == PF_WIRELESS_REPORT_ID && reqlen >= PF_WIRELESS_STATUS_SIZE) {
            pf_wireless_status(buffer);
            return PF_WIRELESS_STATUS_SIZE;
        }
#endif
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
#if PF_ENABLE_WIRELESS
            if (tcp_auth_proof_expires_us != 0u && time_us_64() >= tcp_auth_proof_expires_us) {
                memset(control_result.result_data, 0, sizeof(control_result.result_data));
                control_result.status = PF_HID_COMMAND_FAILED;
                control_result.error_reason = PF_HID_ERROR_OPERATION;
                tcp_auth_proof_expires_us = 0u;
            }
#endif
            if (force_save_pending && !force_save_result_read) {
                force_save_result_read = true;
                force_save_at_us = time_us_64() + 10000u;
            }
            if (force_save_reboot_waiting) {
                force_save_reboot_deadline_us = time_us_64();
            }
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
            if (!pf_hid_self_test_supported()) return 0u;
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
    /* Intentionally silent: an unversioned or zero-transaction envelope has no transaction to answer, so the last
     * result stays readable and is never overwritten by an unattributable request. */
    if (request.version != PF_HID_REPORT_VERSION || request.transaction_id == 0u) return;
    process_control_request(&request);
}
