#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEBULA_CRYPTO_BLAKE3_BYTES 32
#define NEBULA_CRYPTO_SHA3_256_BYTES 32
#define NEBULA_CRYPTO_SHA3_512_BYTES 64
#define NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES 32
#define NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES 12
#define NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES 16

#define NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES 1184
#define NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES 2400
#define NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES 1088
#define NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES 32

#define NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES 1952
#define NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES 4032
#define NEBULA_CRYPTO_ML_DSA_65_SIGNATURE_BYTES 3309

void nebula_crypto_blake3_digest(const uint8_t* input, size_t input_len, uint8_t out[32]);
void nebula_crypto_sha3_256_digest(const uint8_t* input,
                                   size_t input_len,
                                   uint8_t out[NEBULA_CRYPTO_SHA3_256_BYTES]);
void nebula_crypto_sha3_512_digest(const uint8_t* input,
                                   size_t input_len,
                                   uint8_t out[NEBULA_CRYPTO_SHA3_512_BYTES]);

int nebula_crypto_ml_kem_768_keypair(uint8_t* public_key, uint8_t* secret_key);
int nebula_crypto_ml_kem_768_encapsulate(uint8_t* ciphertext,
                                         uint8_t* shared_secret,
                                         const uint8_t* public_key);
int nebula_crypto_ml_kem_768_decapsulate(uint8_t* shared_secret,
                                         const uint8_t* ciphertext,
                                         const uint8_t* secret_key);

int nebula_crypto_ml_dsa_65_keypair(uint8_t* public_key, uint8_t* secret_key);
int nebula_crypto_ml_dsa_65_sign(uint8_t* signature,
                                 size_t* signature_len,
                                 const uint8_t* message,
                                 size_t message_len,
                                 const uint8_t* secret_key);
int nebula_crypto_ml_dsa_65_verify(const uint8_t* message,
                                   size_t message_len,
                                   const uint8_t* signature,
                                   size_t signature_len,
                                   const uint8_t* public_key);

#ifdef __cplusplus
}
#endif
