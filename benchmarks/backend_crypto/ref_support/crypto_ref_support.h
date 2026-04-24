#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nebula_crypto_native.h"

#ifdef __cplusplus
extern "C" {
#endif

int backend_crypto_ref_chacha20_poly1305_seal(
    uint8_t* out,
    size_t out_capacity,
    size_t* out_len,
    const uint8_t key[NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES],
    const uint8_t nonce[NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES],
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* plaintext,
    size_t plaintext_len);

int backend_crypto_ref_chacha20_poly1305_open(
    uint8_t* out,
    size_t out_capacity,
    size_t* out_len,
    const uint8_t key[NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES],
    const uint8_t nonce[NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES],
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* ciphertext,
    size_t ciphertext_len);

#ifdef __cplusplus
}
#endif
