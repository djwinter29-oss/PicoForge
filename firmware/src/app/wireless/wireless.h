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
/** Sanitized status payload size; no credential readback. */
#define PF_WIRELESS_STATUS_SIZE 48u
/** Private USB configuration payload size. */
#define PF_WIRELESS_REQUEST_SIZE 136u

/** @brief Start saved radios on the USB owner task; disabled radios are not initialized. */
void pf_wireless_start(void);
/** @brief Poll SDK Wi-Fi/BLE work from the USB owner task; never call concurrently. */
void pf_wireless_poll(void);
/** @brief Copy sanitized wireless status into a PF_WIRELESS_STATUS_SIZE buffer. */
void pf_wireless_status(uint8_t *report);
/** @brief Save a validated USB configuration, retaining previous RAM settings on failure.
 * @return True only after verified persistence; the caller must queue the existing deferred reboot.
 */
bool pf_wireless_configure(const uint8_t *report);

#endif
