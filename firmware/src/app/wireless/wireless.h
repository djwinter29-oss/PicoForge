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
#define PF_WIRELESS_STATUS_SIZE 48u
/** Private USB configuration payload size. */
#define PF_WIRELESS_REQUEST_SIZE 136u
/** Scan/test command payload size. */
#define PF_WIRELESS_COMMAND_SIZE 101u
/** Scan/test result payload size, including six paged AP results. */
#define PF_WIRELESS_RESULT_SIZE 220u

/** @brief Start saved radios on the USB owner task; disabled radios are not initialized until requested. */
void pf_wireless_start(void);
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
