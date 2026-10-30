/**
 * @file wireless_auth.c
 * @brief Verify command-bound, UID-bound BLE proofs without exposing the USB-provisioned key.
 */
#include "wireless_auth.h"

#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

/** @copydoc pf_wireless_auth_verify */
bool pf_wireless_auth_verify(const uint8_t key[32], const uint8_t nonce[16], const uint8_t uid[8],
                             const uint8_t request[9], const uint8_t proof[16])
{
    uint8_t message[33];
    uint8_t expected[32];
    memcpy(message, nonce, 16u);
    memcpy(message + 16u, uid, 8u);
    memcpy(message + 24u, request, 9u);
    const mbedtls_md_info_t *algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = mbedtls_md_hmac(algorithm, key, 32u, message, sizeof(message), expected);
    uint8_t different = 0u;
    if (rc == 0) {
        for (size_t i = 0u; i < 16u; ++i) different |= expected[i] ^ proof[i];
    }
    mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(message, sizeof(message));
    return rc == 0 && different == 0u;
}
