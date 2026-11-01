/**
 * @file wifi.h
 * @brief Internal Wi-Fi lifecycle and diagnostics, called only by the USB wireless owner task.
 */
#ifndef PF_WIFI_H
#define PF_WIFI_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Load boot credentials once before SDK initialization.
 * @return true when the Wi-Fi master switch and credentials allow startup.
 */
bool pf_wifi_prepare(void);

/** @brief Enable station mode and begin joining after successful shared SDK initialization.
 * @param error Shared diagnostic error byte, updated to none (0) or join failure (2).
 */
void pf_wifi_start(uint8_t *error);

/** @brief Check association/DHCP progress and retry failures without blocking the owner task.
 * @param error Shared diagnostic error byte, updated on link success, failure or timeout.
 */
void pf_wifi_poll(uint8_t *error);

/** @brief Return the diagnostic state: disabled (0), connecting (1), connected (2), or failed (3). */
uint8_t pf_wifi_state(void);

/** @brief Copy the four IPv4 address bytes; call only after SDK initialization and while connected.
 * @param address Output buffer of at least four bytes.
 */
void pf_wifi_ip_address(uint8_t address[4]);

/** @brief Enable station mode without connecting to saved credentials. */
void pf_wifi_enable(void);

/** @brief Accept a scan, page-selection, test-join, or test-cancel command.
 * @param report Feature 34 payload, excluding report ID.
 * @return True when the command is accepted.
 */
bool pf_wifi_command(const uint8_t report[101]);

/** @brief Report a command rejected by shared radio/USB admission.
 * @param transaction Nonzero host transaction.
 */
void pf_wifi_reject_command(uint8_t transaction);

/** @brief Copy sanitized scan and connection-test state into a Feature 35 payload.
 * @param report Output buffer of at least 220 bytes.
 */
void pf_wifi_result(uint8_t report[220]);

/** @brief Return true and copy tested credentials after a successful connection test.
 * @param ssid Output buffer of at least 33 bytes.
 * @param password Output buffer of at least 64 bytes.
 */
bool pf_wifi_tested_credentials(char ssid[33], char password[64]);

/** @brief Clear staged test credentials after save, cancellation, or failed persistence. */
void pf_wifi_clear_tested_credentials(void);

#endif
