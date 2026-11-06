// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file wifi.h
 * @brief Owner-task Wi-Fi radio operations, independent test and scan diagnostics.
 */
#ifndef PF_WIFI_H
#define PF_WIFI_H
#include <stdbool.h>
#include <stdint.h>
#include "app/wireless/wireless.h"
#include "platform/boot_config.h"

/** @brief Saved station state. */
typedef enum {
    PF_WIFI_STATE_DISABLED = 0, /**< Off. */
    PF_WIFI_STATE_CONNECTING = 1, /**< Joining or waiting for DHCP. */
    PF_WIFI_STATE_CONNECTED = 2, /**< Address assigned. */
    PF_WIFI_STATE_FAILED = 3, /**< Join failed or link lost. */
} pf_wifi_state_t;

/** @brief Load boot credentials; return whether saved Wi-Fi may start. */
bool pf_wifi_prepare(void);
/** @brief Enable station mode and join saved credentials. */
void pf_wifi_start(void);
/** @brief Advance radio operations and paced retries at the supplied microsecond timestamp. */
void pf_wifi_poll(uint64_t now);
/** @brief Return saved station state. */
pf_wifi_state_t pf_wifi_state(void);
/** @brief Return whether the current link belongs to saved credentials. */
bool pf_wifi_saved_link_up(void);
/** @brief Return the last internal join error. */
pf_wireless_error_t pf_wifi_error(void);
/** @brief Copy four current IPv4 address bytes. */
void pf_wifi_ip_address(uint8_t address[4]);
/** @brief Return whether a test is joining. */
bool pf_wifi_test_active(void);
/** @brief Return whether a logical or draining driver scan is active. */
bool pf_wifi_scan_active(void);
/** @brief Start a test with an explicitly sized SSID; accepted failures still advance its generation. */
bool pf_wifi_test(const uint8_t *ssid, uint8_t length, const char *password);
/** @brief Cancel and wipe test state, optionally restoring the saved network. */
void pf_wifi_cancel_test(bool restore);
/** @brief Return whether the retained pair successfully tested. */
bool pf_wifi_test_succeeded(void);
/** @brief Return the authentication mode proven by the retained test. */
pf_wifi_auth_state_t pf_wifi_tested_auth_state(void);
/** @brief Start a scan; accepted driver failures still advance its generation. */
void pf_wifi_scan(void);
/** @brief Read an entry into result data; return global detail 22/23, or zero on success. */
uint8_t pf_wifi_scan_entry(uint8_t index, uint16_t generation, uint8_t data[63]);
/** @brief Fill Report 12 radio/test/scan fields without changing unrelated bytes. */
void pf_wifi_status(uint8_t report[PF_WIRELESS_STATUS_SIZE]);
/** @brief Copy saved-link diagnostics; RSSI is 127 when unavailable. */
void pf_wifi_link_diagnostics(pf_wifi_auth_state_t *auth_state, uint16_t *reconnects, int8_t *rssi);
#endif
