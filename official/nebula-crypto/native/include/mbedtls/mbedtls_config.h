/**
 * Minimal mbedTLS config for nebula-crypto's communication-oriented AEAD slice.
 */
#ifndef NEBULA_CRYPTO_MBEDTLS_CONFIG_H
#define NEBULA_CRYPTO_MBEDTLS_CONFIG_H

#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

#endif
