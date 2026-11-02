/**
 * @file wireless_auth.h
 * @brief Shared key ownership and protocol-separated HMAC proofs for wireless control services.
 */
#ifndef PF_WIRELESS_AUTH_H
#define PF_WIRELESS_AUTH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Replace the shared control key; null or all-zero input clears it.
 * @param key 32-byte key, or null/all-zero to clear.
 */
void pf_wireless_auth_set_key(const uint8_t key[32]);

/** @brief Return whether a nonzero shared control key is available. @return True when a control key is loaded. */
bool pf_wireless_auth_available(void);

/**
 * @brief Generate a fresh 16-byte challenge with the Pico SDK random source.
 * @param nonce Output buffer of at least 16 bytes.
 * @return False when nonce is null; true after generating the challenge.
 */
bool pf_wireless_auth_new_nonce(uint8_t nonce[16]);

/**
 * @brief Copy the board's eight-byte unique ID for an authentication challenge.
 * @param uid Output buffer of at least eight bytes.
 */
void pf_wireless_auth_device_id(uint8_t uid[8]);

/**
 * @brief Verify a truncated HMAC-SHA256 bound to nonce, board UID, protocol domain and request bytes.
 * @param nonce 16-byte challenge.
 * @param domain Nine-byte protocol identifier.
 * @param request Nine-byte command representation.
 * @param proof First 16 bytes of HMAC-SHA256.
 * @return True only when the loaded key verifies the proof.
 */
bool pf_wireless_auth_verify(const uint8_t nonce[16], const uint8_t domain[9], const uint8_t request[9],
                             const uint8_t proof[16]);

#endif
