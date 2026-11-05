/**
 * @file wireless.h
 * @brief Owner-task radio lifecycle and USB-only RAM credential staging.
 */
#ifndef PF_WIRELESS_H
#define PF_WIRELESS_H

#include <stdbool.h>
#include <stdint.h>

/** USB command request, defined by the USB HID contract. */
typedef struct pf_hid_control_request pf_hid_control_request_t;
/** USB command result, defined by the USB HID contract. */
typedef struct pf_hid_control_result pf_hid_control_result_t;

/** Wireless status Feature report ID. */
#define PF_WIRELESS_REPORT_ID 12u
/** Wireless status payload bytes, excluding report ID. */
#define PF_WIRELESS_STATUS_SIZE 48u
/** Status layout version. */
#define PF_WIRELESS_STATUS_VERSION 1u
/** Report 12 Wi-Fi runtime/test state byte. */
#define PF_WIRELESS_STATUS_WIFI_STATE_OFFSET 1u
/** Report 12 IPv4 address byte offset. */
#define PF_WIRELESS_STATUS_IP_ADDRESS_OFFSET 5u
/** Report 12 Bluetooth state byte. */
#define PF_WIRELESS_STATUS_BLUETOOTH_STATE_OFFSET 3u
/** Unavailable saved-link RSSI sentinel. */
#define PF_WIFI_RSSI_UNAVAILABLE INT8_MAX

/** @brief Internal radio error category. */
typedef enum {
    PF_WIRELESS_ERROR_NONE = 0, /**< No error. */
    PF_WIRELESS_ERROR_INIT = 1, /**< SDK initialization failed. */
    PF_WIRELESS_ERROR_JOIN = 2, /**< Join failed. */
    PF_WIRELESS_ERROR_TIMEOUT = 3, /**< Join timed out. */
} pf_wireless_error_t;

/** @brief Initialize the shared SDK and start boot-enabled radios on the USB owner task. */
void pf_wireless_start(void);
/** @brief Write the radio-owned LED; return false before successful SDK initialization. */
bool pf_wireless_led_put(bool on);
/** @brief Poll radio work and expire staged secrets on the USB owner task. */
void pf_wireless_poll(void);
/** @brief Expire secrets synchronously and build the 48-byte Report 12 payload. */
void pf_wireless_status(uint8_t *report);
/** @brief Execute USB opcodes 6-13 and initialize the entire command result. */
void pf_wireless_command(const pf_hid_control_request_t *request, pf_hid_control_result_t *result);
/** @brief Expire staging and return the global save-precondition detail, or zero when valid. */
uint8_t pf_wireless_save_precondition(bool commit);
/**
 * @brief Apply staged credentials to boot configuration RAM, without writing flash.
 * @param commit_mask Receives SSID/password/key bits 0/1/2.
 * @return False unless wireless commit preconditions pass and all setters succeed.
 */
bool pf_wireless_apply_staged(uint8_t *commit_mask);
/** @brief Expire synchronously and return whether any candidate remains staged. */
bool pf_wireless_staged(void);
/** @brief Cancel the test, wipe all candidates and the expiry marker, and restore the saved link. */
void pf_wireless_discard(void);
/** @brief Wipe candidates and expiry marker after verified persistence, without restoring before reboot. */
void pf_wireless_saved(void);

#endif
