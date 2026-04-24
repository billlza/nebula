#include "nebula_crypto_native.h"

#include <oqs/sha3.h>

#include "mlkem-native_ml-kem-768_ref/mlkem/kem.h"
#include "mldsa65_ref/sign.h"

void nebula_crypto_sha3_256_digest(const uint8_t *input,
                                   size_t input_len,
                                   uint8_t out[NEBULA_CRYPTO_SHA3_256_BYTES]) {
  OQS_SHA3_sha3_256(out, input, input_len);
}

void nebula_crypto_sha3_512_digest(const uint8_t *input,
                                   size_t input_len,
                                   uint8_t out[NEBULA_CRYPTO_SHA3_512_BYTES]) {
  OQS_SHA3_sha3_512(out, input, input_len);
}

int nebula_crypto_ml_kem_768_keypair(uint8_t *public_key, uint8_t *secret_key) {
  return crypto_kem_keypair(public_key, secret_key);
}

int nebula_crypto_ml_kem_768_encapsulate(uint8_t *ciphertext,
                                         uint8_t *shared_secret,
                                         const uint8_t *public_key) {
  return crypto_kem_enc(ciphertext, shared_secret, public_key);
}

int nebula_crypto_ml_kem_768_decapsulate(uint8_t *shared_secret,
                                         const uint8_t *ciphertext,
                                         const uint8_t *secret_key) {
  return crypto_kem_dec(shared_secret, ciphertext, secret_key);
}

int nebula_crypto_ml_dsa_65_keypair(uint8_t *public_key, uint8_t *secret_key) {
  return crypto_sign_keypair(public_key, secret_key);
}

int nebula_crypto_ml_dsa_65_sign(uint8_t *signature,
                                 size_t *signature_len,
                                 const uint8_t *message,
                                 size_t message_len,
                                 const uint8_t *secret_key) {
  return crypto_sign_signature(signature, signature_len, message, message_len, NULL, 0, secret_key);
}

int nebula_crypto_ml_dsa_65_verify(const uint8_t *message,
                                   size_t message_len,
                                   const uint8_t *signature,
                                   size_t signature_len,
                                   const uint8_t *public_key) {
  return crypto_sign_verify(signature, signature_len, message, message_len, NULL, 0, public_key);
}
