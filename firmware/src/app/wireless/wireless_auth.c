// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file wireless_auth.c
 * @brief Own the single wireless control key and verify board-bound, protocol-separated HMAC proofs.
 */
#include "app/wireless/wireless_auth.h"

#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "pico/rand.h"
#include "pico/unique_id.h"

static uint8_t auth_key[32]; /**< Shared BLE/TCP control key, wiped when cleared. */
static bool key_available; /**< True after any raw 32-byte key is loaded, including all-zero bytes. */
/** @brief Protocol domain separating USB-assisted TCP CLI admission from all other proofs. */
static const uint8_t tcp_cli_domain[9] = {'P', 'F', 'T', 'C', 'P', 'C', 'L', 'I', '1'};
/** @brief Fixed request admitted by the TCP CLI; the USB interface cannot sign arbitrary messages. */
static const uint8_t tcp_cli_request[9] = {'T', 'C', 'P', 'C', 'L', 'I', '0', '0', '1'};

/** @copydoc pf_wireless_auth_set_key */
void pf_wireless_auth_set_key(const uint8_t key[32])
{
    if (key == NULL) {
        mbedtls_platform_zeroize(auth_key, sizeof(auth_key));
        key_available = false;
        return;
    }
    memcpy(auth_key, key, sizeof(auth_key));
    key_available = true;
}

/** @copydoc pf_wireless_auth_available */
bool pf_wireless_auth_available(void)
{
    return key_available;
}

/** @copydoc pf_wireless_auth_new_nonce */
bool pf_wireless_auth_new_nonce(uint8_t nonce[16])
{
    if (nonce == NULL) return false;
    for (size_t i = 0u; i < 16u; i += sizeof(uint64_t)) {
        uint64_t random = get_rand_64();
        memcpy(nonce + i, &random, sizeof(random));
    }
    return true;
}

/** @copydoc pf_wireless_auth_device_id */
void pf_wireless_auth_device_id(uint8_t uid[8])
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    memcpy(uid, id.id, sizeof(id.id));
}

/** @copydoc pf_wireless_auth_verify */
bool pf_wireless_auth_verify(const uint8_t nonce[16], const uint8_t domain[9], const uint8_t request[9],
                             const uint8_t proof[16])
{
    if (!key_available || nonce == NULL || domain == NULL || request == NULL || proof == NULL) return false;
    uint8_t message[42];
    uint8_t expected[32];
    memcpy(message, nonce, 16u);
    pf_wireless_auth_device_id(message + 16u);
    memcpy(message + 24u, domain, 9u);
    memcpy(message + 33u, request, 9u);
    const mbedtls_md_info_t *algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const int rc = mbedtls_md_hmac(algorithm, auth_key, sizeof(auth_key), message, sizeof(message), expected);
    uint8_t different = 0u;
    if (rc == 0) {
        for (size_t i = 0u; i < 16u; ++i) different |= expected[i] ^ proof[i];
    }
    mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(message, sizeof(message));
    return rc == 0 && different == 0u;
}

/** @copydoc pf_wireless_auth_tcp_cli_proof */
bool pf_wireless_auth_tcp_cli_proof(const uint8_t nonce[16], uint8_t proof[16])
{
    if (!key_available || nonce == NULL || proof == NULL) return false;
    uint8_t message[42];
    uint8_t digest[32];
    memcpy(message, nonce, 16u);
    pf_wireless_auth_device_id(message + 16u);
    memcpy(message + 24u, tcp_cli_domain, sizeof(tcp_cli_domain));
    memcpy(message + 33u, tcp_cli_request, sizeof(tcp_cli_request));
    const mbedtls_md_info_t *algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const int rc = mbedtls_md_hmac(algorithm, auth_key, sizeof(auth_key), message, sizeof(message), digest);
    if (rc == 0) memcpy(proof, digest, 16u);
    else mbedtls_platform_zeroize(proof, 16u);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_platform_zeroize(message, sizeof(message));
    return rc == 0;
}
