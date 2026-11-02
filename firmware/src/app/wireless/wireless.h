/**
 * @file wireless.h
 * @brief W-board radios, USB-only provisioning and restricted authenticated BLE diagnostics/control.
 */
#ifndef PF_WIRELESS_H
#define PF_WIRELESS_H

#include <stdbool.h>
#include <stdint.h>

/** USB provisioning/status Feature report ID. */
#define PF_WIRELESS_REPORT_ID 30u
/** USB-only private provisioning Feature report ID. */
#define PF_WIRELESS_CONFIG_REPORT_ID 31u
/** USB-only scan and connection-test command Feature report ID. */
#define PF_WIRELESS_COMMAND_REPORT_ID 34u
/** Sanitized scan and connection-test result Feature report ID. */
#define PF_WIRELESS_RESULT_REPORT_ID 35u
/** Sanitized status payload size; no credential readback. */
#define PF_WIRELESS_STATUS_SIZE 53u
/**
 * @brief Byte offsets in the sanitized wireless status payload.
 *
 * The payload is also exposed as a USB Feature report; keep these offsets stable for that protocol.
 */
typedef enum {
    PF_WIRELESS_STATUS_VERSION_OFFSET = 0u, /**< Status payload version. */
    PF_WIRELESS_STATUS_RADIOS_OFFSET = 1u, /**< Enabled radio mask. */
    PF_WIRELESS_STATUS_SAVE_STATUS_OFFSET = 2u, /**< Last configuration-save status. */
    PF_WIRELESS_STATUS_SAVE_TRANSACTION_OFFSET = 3u, /**< Last configuration-save transaction. */
    PF_WIRELESS_STATUS_WIFI_STATE_OFFSET = 4u, /**< Wi-Fi connection state. */
    PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET = 5u, /**< Four-byte IPv4 address. */
    PF_WIRELESS_STATUS_SSID_OFFSET = 9u, /**< Configured SSID bytes, never used for CLI output. */
    PF_WIRELESS_STATUS_PASSWORD_CONFIGURED_OFFSET = 42u, /**< Whether a password is configured. */
    PF_WIRELESS_STATUS_BLE_KEY_CONFIGURED_OFFSET = 43u, /**< Whether a BLE key is configured. */
    PF_WIRELESS_STATUS_ERROR_OFFSET = 44u, /**< Last wireless error code. */
    PF_WIRELESS_STATUS_BLUETOOTH_STATE_OFFSET = 45u, /**< Bluetooth connection state. */
    PF_WIRELESS_STATUS_TCP_CLI_OFFSET = 46u, /**< Whether the TCP CLI accepts clients on the saved network. */
    PF_WIRELESS_STATUS_SERVICES_OFFSET = 47u, /**< Version 3: bit 0 disables CLI; bit 1 requires its HMAC proof. */
    PF_WIRELESS_STATUS_AUTH_STATE_OFFSET = 48u, /**< pf_wifi_auth_state_t for the saved network. */
    PF_WIRELESS_STATUS_RECONNECT_COUNT_OFFSET = 49u, /**< Little-endian saved-link loss count. */
    PF_WIRELESS_STATUS_RSSI_OFFSET = 51u, /**< Signed dBm; 127 when unavailable. */
    PF_WIRELESS_STATUS_RESTART_REQUIRED_OFFSET = 52u, /**< Version 4: saved wireless settings differ from boot state. */
} pf_wireless_status_offset_t;
/** @brief Feature 30 byte 44 error codes; an initialization failure takes precedence over Wi-Fi errors. */
typedef enum {
    PF_WIRELESS_ERROR_NONE = 0, /**< No error. */
    PF_WIRELESS_ERROR_INIT = 1, /**< SDK radio or BLE initialization failed. */
    PF_WIRELESS_ERROR_JOIN = 2, /**< Wi-Fi join failed or was refused by the network. */
    PF_WIRELESS_ERROR_TIMEOUT = 3, /**< Wi-Fi association or DHCP timed out. */
} pf_wireless_error_t;
/** Private USB configuration payload size. */
#define PF_WIRELESS_REQUEST_SIZE 136u
/** Scan/test command payload size. */
#define PF_WIRELESS_COMMAND_SIZE 101u
/** Byte offset of the nonzero host transaction in the scan/test command payload. */
#define PF_WIRELESS_COMMAND_TRANSACTION_OFFSET 2u
/** RSSI sentinel used when the saved link is down or the driver cannot report it. */
#define PF_WIFI_RSSI_UNAVAILABLE INT8_MAX
/** Scan/test result payload size, including six paged AP results. */
#define PF_WIRELESS_RESULT_SIZE 222u

/** @brief Initialize the W-board CYW43 for its LED, then start only enabled radios on the USB owner task. */
void pf_wireless_start(void);
/**
 * @brief Drive the CYW43 LED from the USB owner task after SDK initialization.
 * @param on true to light the LED.
 * @return false when the SDK is not initialized; true when the write is issued.
 */
bool pf_wireless_led_put(bool on);
/** @brief Poll SDK Wi-Fi/BLE work from the USB owner task; never call concurrently. */
void pf_wireless_poll(void);
/** @brief Copy sanitized wireless status into a PF_WIRELESS_STATUS_SIZE buffer. */
void pf_wireless_status(uint8_t *report);
/** @brief Start a scan, select a scan page, test a network, or clear a staged test. */
void pf_wireless_command(const uint8_t *report);
/** @brief Mark an admitted report as rejected by an outer USB save/reboot gate. */
void pf_wireless_reject_command(uint8_t transaction);
/** @brief Copy sanitized scan/test results into a PF_WIRELESS_RESULT_SIZE buffer. */
void pf_wireless_result_report(uint8_t *report);
/** @brief Validate and save a version-2 USB board configuration and credential update.
 *
 * Clock, SD profile, radio switches and credentials are saved together or none are: a validation or save failure
 * discards the staged values, and the next access reloads flash. The caller queues the existing deferred reboot after
 * true.
 * @return True only after verified persistence.
 */
bool pf_wireless_configure(const uint8_t *report);

#endif
