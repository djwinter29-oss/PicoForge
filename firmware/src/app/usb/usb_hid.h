/**
 * @file
 * @brief Vendor HID wire protocol: report IDs, payload layouts, command codes and report builders.
 * @note These values are shared with host tools; changing them breaks the wire protocol.
 */
#ifndef PF_USB_HID_H
#define PF_USB_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "pico/unique_id.h"
#include "platform/system.h"

/** @name Report IDs @{ */
#define PF_HID_IDENTITY_REPORT_ID 1u
#define PF_HID_CONFIGURATION_REPORT_ID 2u
#define PF_HID_TELEMETRY_REPORT_ID 3u
#define PF_HID_STORAGE_REPORT_ID 10u
#define PF_HID_SELF_TEST_REPORT_ID 11u
#define PF_HID_CONTROL_REPORT_ID 20u
/** @} */

/** @name Protocol constants @{ */
#define PF_HID_REPORT_VERSION 1u
#define PF_HID_PRODUCT_ID 2u
#define PF_HID_REBOOT_DELAY_MS 1000u /**< Grace period for the host to read the reboot result before reset. */
/** @} */

/** @name Payload sizes in bytes, excluding the report ID @{ */

#define PF_HID_IDENTITY_PAYLOAD_SIZE 26u
#define PF_HID_CONFIGURATION_PAYLOAD_SIZE 17u
#define PF_HID_TELEMETRY_PAYLOAD_SIZE 22u
#define PF_HID_STORAGE_PAYLOAD_SIZE 34u
#define PF_HID_SELF_TEST_PAYLOAD_SIZE 32u
#define PF_HID_CONTROL_PAYLOAD_SIZE 9u

/** @} */

/** @name Identity capability bits (PF_HID_CAP_*) @{ */
#define PF_HID_CAP_LED (1u << 0)
#define PF_HID_CAP_REBOOT (1u << 1)
#define PF_HID_CAP_SETTINGS (1u << 2)
#define PF_HID_CAP_SPI (1u << 3)
#define PF_HID_CAP_SDIO (1u << 4)
#define PF_HID_CAP_SELF_TEST (1u << 5)

/** @} */

/** @name Telemetry flag bits (PF_HID_TELEMETRY_*) @{ */
#define PF_HID_TELEMETRY_USB_MOUNTED (1u << 0)
#define PF_HID_TELEMETRY_CDC_OPEN (1u << 1)
#define PF_HID_TELEMETRY_TEMPERATURE_VALID (1u << 2)
#define PF_HID_TELEMETRY_LED_AVAILABLE (1u << 3)

/** @} */

/** @name Storage report flag bits (PF_HID_STORAGE_*) @{ */
#define PF_HID_STORAGE_CONFIGURED (1u << 0)
#define PF_HID_STORAGE_PRESENCE_KNOWN (1u << 1)
#define PF_HID_STORAGE_PRESENT (1u << 2)
#define PF_HID_STORAGE_INITIALIZED (1u << 3)
#define PF_HID_STORAGE_CID_VALID (1u << 4)
#define PF_HID_STORAGE_CAPACITY_VALID (1u << 5)
#define PF_HID_STORAGE_SAMPLE_VALID (1u << 6)

/** @} */

/** @name Storage report bus, speed and filesystem values @{ */
#define PF_HID_COMPONENT_NONE 0u /**< Storage component type: unavailable. */
#define PF_HID_COMPONENT_SDCARD 1u /**< Storage component type: SD card. */
#define PF_HID_BUS_MODE_UNKNOWN 0u
#define PF_HID_BUS_MODE_SDIO 1u
#define PF_HID_BUS_MODE_SPI 2u
#define PF_HID_SPEED_MODE_UNKNOWN 0u
#define PF_HID_SPEED_MODE_STANDARD 1u
#define PF_HID_SPEED_MODE_HIGH_SPEED 2u

#define PF_HID_FILESYSTEM_UNKNOWN 0u
#define PF_HID_FILESYSTEM_FAT12 1u
#define PF_HID_FILESYSTEM_FAT16 2u
#define PF_HID_FILESYSTEM_FAT32 3u
#define PF_HID_FILESYSTEM_EXFAT 4u

/** @} */

/** @name Feature 20 opcodes @{ */
#define PF_HID_OPCODE_LED 1u
#define PF_HID_OPCODE_REBOOT 2u
#define PF_HID_OPCODE_MSC 3u
#define PF_HID_OPCODE_SELF_TEST 4u
#define PF_HID_OPCODE_SAVE_SETTINGS 6u
/** @} */

/** @name Feature 20 result status (control result @c status) @{ */
#define PF_HID_COMMAND_IDLE 0u /**< No command has completed since startup. */
#define PF_HID_COMMAND_PENDING 1u /**< Accepted; the result is published later. */
#define PF_HID_COMMAND_COMPLETED 2u /**< Executed successfully. */
#define PF_HID_COMMAND_INVALID 3u /**< Malformed or out-of-range request. */
#define PF_HID_COMMAND_UNSUPPORTED 4u /**< Opcode or capability not available in this build. */
#define PF_HID_COMMAND_BUSY 5u /**< Device state does not allow the command now. */
#define PF_HID_COMMAND_FAILED 6u /**< Accepted but the operation failed. */
/** @} */

/** @name Feature 20 result error reason (control result @c error_reason) @{ */
#define PF_HID_ERROR_NONE 0u /**< Success. */
#define PF_HID_ERROR_MALFORMED 1u /**< Reserved bits, flags or trailing arguments are invalid. */
#define PF_HID_ERROR_CONFIRMATION 2u /**< A required confirmation flag is missing. */
#define PF_HID_ERROR_ARGUMENT 3u /**< An argument is out of range or not applicable. */
#define PF_HID_ERROR_CAPABILITY 4u /**< The device lacks the capability. */
#define PF_HID_ERROR_BUSY 5u /**< Storage, MSC or benchmark activity blocks the command. */
#define PF_HID_ERROR_OPERATION 6u /**< The device operation failed. */
#define PF_HID_ERROR_OPCODE 7u /**< Unknown opcode. */
/** @} */

/** @name MSC opcode actions (first argument byte) @{ */
#define PF_HID_MSC_ACTION_DISABLE 0u /**< Return the card to firmware after a clean eject. */
#define PF_HID_MSC_ACTION_ENABLE 1u /**< Give the card to the USB host. */
#define PF_HID_MSC_ACTION_FORCE_RETURN 2u /**< Confirmed unsafe return; requires the confirmation flag. */
/** @} */

/** @brief Feature 1 device identity, build capabilities, and SRAM reservation. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Report layout version. */
    uint16_t product_id; /**< Product-family identifier. */
    uint8_t device_uid[PICO_UNIQUE_BOARD_ID_SIZE_BYTES]; /**< SDK unique ID in USB serial order. */
    uint8_t firmware_major; /**< Firmware semantic version major. */
    uint8_t firmware_minor; /**< Firmware semantic version minor. */
    uint8_t firmware_patch; /**< Firmware semantic version patch. */
    uint8_t mcu; /**< PF_SYSTEM_MCU_* value. */
    uint8_t board_model; /**< Configured board target; zero means unspecified. */
    uint8_t reserved; /**< Must remain zero. */
    uint8_t capabilities; /**< PF_HID_CAP_* bits. */
    uint32_t ram_total_bytes; /**< Total on-chip SRAM. */
    uint32_t ram_reserved_bytes; /**< Linked and SDK-reserved SRAM. */
} pf_hid_identity_report_t;

/** @brief Feature 2 persisted boot profiles and board-owned GPIO configuration. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Report layout version. */
    uint16_t stored_sysclk_profile; /**< Persisted next-boot clock in MHz, zero selects build default. */
    uint8_t stored_sdcard_profile; /**< Zero Standard; one High-Speed. */
    uint8_t monitor_uart_index; /**< Hardware UART index, or UINT8_MAX. */
    uint8_t monitor_uart_tx_gpio; /**< GPIO number, or UINT8_MAX. */
    uint8_t monitor_uart_rx_gpio; /**< GPIO number, or UINT8_MAX. */
    uint8_t storage_gpios[6]; /**< Selected transport pin order. */
    uint8_t board_flags; /**< Card detect, LED, and monitor UART configuration bits. */
    uint8_t reset_cause; /**< Reset cause; zero when unknown. */
    uint8_t led_gpio; /**< GPIO number, or UINT8_MAX. */
    uint8_t card_detect_gpio; /**< GPIO number, or UINT8_MAX. */
} pf_hid_configuration_report_t;

/** @brief Input 3 live telemetry using the PicoUART-compatible 20-byte prefix. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Report layout version. */
    uint8_t led_flags; /**< Bit zero means the available identification LED is on. */
    int16_t temperature_centidegrees_celsius; /**< Estimated die temperature, valid when flagged. */
    uint64_t uptime_ms; /**< Monotonic uptime in milliseconds. */
    uint16_t core0_service_load_basis_points; /**< 0-10000, or UINT16_MAX when unavailable. */
    uint16_t core1_service_load_basis_points; /**< 0-10000, or UINT16_MAX when unavailable. */
    uint32_t system_clock_hz; /**< Current system clock. */
    uint8_t sequence; /**< Accepted publication sequence. */
    uint8_t picoforge_flags; /**< PF_HID_TELEMETRY_* bits. */
} pf_hid_telemetry_report_t;

/** @brief Input 10 SD, filesystem, and MSC state. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Report layout version. */
    uint8_t sequence; /**< Accepted publication sequence. */
    uint8_t sd_flags; /**< PF_HID_STORAGE_* state and validity bits. */
    uint8_t component_type; /**< PF_HID_COMPONENT_* value. */
    uint8_t bus_mode; /**< PF_HID_BUS_MODE_* value. */
    uint32_t bus_clock_hz; /**< Effective bus clock when sample is valid and initialized. */
    uint8_t speed_mode; /**< PF_HID_SPEED_MODE_* active card timing. */
    uint32_t block_count; /**< 512-byte sectors when capacity is valid. */
    uint8_t cid[16]; /**< Raw SD CID when valid. */
    uint8_t filesystem_format; /**< PF_HID_FILESYSTEM_* last successful mount format. */
    uint8_t msc_enabled; /**< MSC owns storage. */
    uint8_t msc_transition_pending; /**< Ownership transition is active. */
    uint8_t msc_request_rejected; /**< Most recent valid MSC request failed. */
} pf_hid_storage_report_t;

/** @brief Input 11 benchmark lifecycle and progress; fields retain the file-benchmark encoding. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Report layout version. */
    uint8_t sequence; /**< Accepted publication sequence. */
    uint8_t state; /**< pf_file_benchmark_state_t value. */
    uint8_t flags; /**< File-remains and volume-mounted bits. */
    uint32_t target_bytes; /**< Requested file size. */
    uint32_t written_bytes; /**< Bytes accepted by writes. */
    uint32_t read_bytes; /**< Bytes verified by reads. */
    uint32_t write_elapsed_ms; /**< Write and sync duration. */
    uint32_t read_elapsed_ms; /**< Read and verification duration. */
    uint32_t mismatch_count; /**< Detected byte mismatches. */
    uint8_t error; /**< Accepted run error. */
    uint8_t filesystem_result; /**< FatFs result associated with the run. */
    uint8_t request_error; /**< Most recent request rejection. */
    uint8_t reserved; /**< Must remain zero. */
} pf_hid_self_test_report_t;

/** @brief Feature 20 request envelope; SET never contains its report ID. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Report layout version. */
    uint8_t transaction_id; /**< Host-selected nonzero request identifier. */
    uint8_t opcode; /**< PF_HID_OPCODE_* operation. */
    uint8_t flags; /**< Opcode-specific flags. */
    uint8_t arguments[5]; /**< Opcode-specific arguments. */
} pf_hid_control_request_t;

/** @brief Feature 20 result slot returned by GET. */
typedef struct __attribute__((packed)) {
    uint8_t version; /**< Report layout version. */
    uint8_t transaction_id; /**< ID associated with the result. */
    uint8_t opcode; /**< Operation associated with the result. */
    uint8_t status; /**< Idle, Pending, Completed, Invalid, Unsupported, Busy, or Failed. */
    uint8_t error_reason; /**< Zero on success; otherwise command error enum. */
    uint8_t result_data[4]; /**< Saved-setting result for opcode six, zero otherwise. */
} pf_hid_control_result_t;

_Static_assert(sizeof(pf_hid_identity_report_t) == PF_HID_IDENTITY_PAYLOAD_SIZE, "identity report size changed");
_Static_assert(sizeof(pf_hid_configuration_report_t) == PF_HID_CONFIGURATION_PAYLOAD_SIZE,
               "configuration report size changed");
_Static_assert(sizeof(pf_hid_telemetry_report_t) == PF_HID_TELEMETRY_PAYLOAD_SIZE,
               "telemetry report size changed");
_Static_assert(sizeof(pf_hid_storage_report_t) == PF_HID_STORAGE_PAYLOAD_SIZE, "storage report size changed");
_Static_assert(sizeof(pf_hid_self_test_report_t) == PF_HID_SELF_TEST_PAYLOAD_SIZE, "self-test report size changed");
_Static_assert(sizeof(pf_hid_control_request_t) == PF_HID_CONTROL_PAYLOAD_SIZE, "control request size changed");
_Static_assert(sizeof(pf_hid_control_result_t) == PF_HID_CONTROL_PAYLOAD_SIZE, "control result size changed");

/**
 * @brief Build the Feature 1 identity report.
 * @param report Receives the payload; fully overwritten.
 */
void pf_hid_build_identity_report(pf_hid_identity_report_t *report);

/**
 * @brief Build the Feature 2 configuration report.
 * @param report Receives the payload; fully overwritten.
 */
void pf_hid_build_configuration_report(pf_hid_configuration_report_t *report);

/**
 * @brief Build the Input 3 telemetry report.
 * @param report Receives the payload; fully overwritten.
 * @param sequence Publication sequence number.
 */
void pf_hid_build_telemetry_report(pf_hid_telemetry_report_t *report, uint8_t sequence);

/**
 * @brief Build the Input 10 storage and MSC report.
 * @param report Receives the payload; fully overwritten.
 * @param sequence Publication sequence number.
 */
void pf_hid_build_storage_report(pf_hid_storage_report_t *report, uint8_t sequence);

/**
 * @brief Build the Input 11 benchmark report; all zero when the benchmark is not built in.
 * @param report Receives the payload; fully overwritten.
 * @param sequence Publication sequence number.
 */
void pf_hid_build_self_test_report(pf_hid_self_test_report_t *report, uint8_t sequence);

/** @return True when this build includes the file benchmark and reports PF_HID_CAP_SELF_TEST. */
bool pf_hid_self_test_supported(void);

/**
 * @brief Consume a validated reboot request.
 * @return True once per accepted reboot or settings-save command.
 * @note USB task only.
 */
bool pf_hid_take_reboot_request(void);

/**
 * @brief Publish completion of an asynchronous command preflight when the worker resolves it.
 * @note USB task only; call every cycle.
 */
void pf_hid_poll_async_commands(void);

#endif
