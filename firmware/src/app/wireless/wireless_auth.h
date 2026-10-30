/**
 * @file wireless_auth.h
 * @brief Command-bound BLE proof verification using the SDK's pinned mbedTLS implementation.
 */
#ifndef PF_WIRELESS_AUTH_H
#define PF_WIRELESS_AUTH_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Verify a truncated HMAC-SHA256 of nonce[16] || UID[8] || control request[9].
 * @return false on a cryptographic error or mismatch; nonce lifetime and replay rejection belong to the caller.
 */
bool pf_wireless_auth_verify(const uint8_t key[32], const uint8_t nonce[16], const uint8_t uid[8],
                             const uint8_t request[9], const uint8_t proof[16]);

#endif
