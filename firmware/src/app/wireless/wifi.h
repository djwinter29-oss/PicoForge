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

#endif
