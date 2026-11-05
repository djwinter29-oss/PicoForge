/**
 * @file wireless_auth.h
 * @brief Shared key ownership and protocol-separated HMAC proofs for wireless control services.
 */
#ifndef PF_WIRELESS_AUTH_H
#define PF_WIRELESS_AUTH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Replace the shared Control key; only null clears it.
 * @param key Raw 32-byte key, including all-zero bytes, or null to clear.
 */
void pf_wireless_auth_set_key(const uint8_t key[32]);

/** @brief Return whether any raw Control key is loaded. @return False only when no key is loaded. */
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

/**
 * @brief Create the fixed TCP CLI admission proof for a challenge received by the USB host.
 * @param nonce 16-byte challenge from the active TCP CLI connection.
 * @param proof Output buffer for the first 16 HMAC-SHA256 bytes.
 * @return True when the configured key produced a proof.
 */
bool pf_wireless_auth_tcp_cli_proof(const uint8_t nonce[16], uint8_t proof[16]);

#endif
