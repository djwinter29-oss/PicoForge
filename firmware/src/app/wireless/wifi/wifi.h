/**
 * @file wifi.h
 * @brief Internal Wi-Fi lifecycle and diagnostics, called only by the USB wireless owner task.
 */
#ifndef PF_WIFI_H
#define PF_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#include "app/wireless/wireless.h"
#include "platform/boot_config.h"

/** @brief Station diagnostic state reported in Feature 30 byte 4 and Feature 35 byte 5. */
typedef enum {
    PF_WIFI_STATE_DISABLED = 0, /**< Station is not joining or joined. */
    PF_WIFI_STATE_CONNECTING = 1, /**< Association or DHCP is in progress. */
    PF_WIFI_STATE_CONNECTED = 2, /**< Station has an IPv4 address. */
    PF_WIFI_STATE_FAILED = 3, /**< Last join failed or the link was lost. */
} pf_wifi_state_t;

/** @brief Feature 34 byte 1 operations. */
typedef enum {
    PF_WIFI_OPERATION_SCAN = 1, /**< Start a network scan. */
    PF_WIFI_OPERATION_PAGE = 2, /**< Select a completed scan result page. */
    PF_WIFI_OPERATION_TEST = 3, /**< Join temporary credentials without saving them. */
    PF_WIFI_OPERATION_CLEAR = 4, /**< Clear a staged test and restore the saved network. */
} pf_wifi_operation_t;

/** @brief Load boot credentials once before SDK initialization.
 * @return true when the Wi-Fi master switch and credentials allow startup.
 */
bool pf_wifi_prepare(void);

/** @brief Enable station mode and begin joining after successful shared SDK initialization. */
void pf_wifi_start(void);

/** @brief Check scan, association and DHCP progress and pace retries without blocking the owner task.
 * @param now Owner-task poll timestamp from time_us_64().
 */
void pf_wifi_poll(uint64_t now);

/** @brief Return the current station diagnostic state. */
pf_wifi_state_t pf_wifi_state(void);

/** @brief Return whether the station has an address on the saved network, not on a temporary test network.
 *
 * The unauthenticated TCP CLI accepts clients only while this is true.
 */
bool pf_wifi_saved_link_up(void);

/** @brief Return the latest Wi-Fi error: none, join failure or connection timeout. */
pf_wireless_error_t pf_wifi_error(void);

/** @brief Copy the four IPv4 address bytes; call only after SDK initialization and while connected.
 * @param address Output buffer of at least four bytes.
 */
void pf_wifi_ip_address(uint8_t address[4]);

/** @brief Validate and accept a scan, page-selection, test-join, or test-cancel command.
 *
 * Station mode is enabled only for an accepted scan or test, and is dropped again when saved Wi-Fi is disabled and
 * the scan finishes, the test fails, or the staged test is cleared.
 * @param report Feature 34 payload, excluding report ID.
 * @return True when the command is accepted.
 */
bool pf_wifi_command(const uint8_t report[PF_WIRELESS_COMMAND_SIZE]);

/** @brief Report a command rejected by shared radio/USB admission.
 * The rejection is reported on the next Feature 35 read without changing accepted operation state.
 * @param transaction Nonzero host transaction.
 */
void pf_wifi_reject_command(uint8_t transaction);

/** @brief Copy sanitized scan and connection-test state into a Feature 35 payload.
 * @param report Output buffer of PF_WIRELESS_RESULT_SIZE bytes.
 */
void pf_wifi_result(uint8_t report[PF_WIRELESS_RESULT_SIZE]);

/** @brief Return true and copy tested credentials before their ten-minute successful-test deadline.
 * @param ssid Output buffer of at least 33 bytes.
 * @param password Output buffer of at least 64 bytes.
 */
bool pf_wifi_tested_credentials(char ssid[PF_WIFI_SSID_SIZE], char password[PF_WIFI_PASSWORD_SIZE]);

/** @brief Return the successful test authentication mode for saving alongside those credentials. */
pf_wifi_auth_state_t pf_wifi_tested_auth_state(void);

/** @brief Copy saved-link diagnostics; RSSI is 127 when unavailable. */
void pf_wifi_link_diagnostics(pf_wifi_auth_state_t *auth_state, uint16_t *reconnects, int8_t *rssi);

/** @brief Wipe staged test credentials after a successful save; the caller immediately schedules reboot. */
void pf_wifi_clear_tested_credentials(void);

#endif
