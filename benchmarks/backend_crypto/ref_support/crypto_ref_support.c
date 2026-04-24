#include "crypto_ref_support.h"

#include "mbedtls/chachapoly.h"

#include <string.h>

int backend_crypto_ref_chacha20_poly1305_seal(
    uint8_t* out,
    size_t out_capacity,
    size_t* out_len,
    const uint8_t key[NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES],
    const uint8_t nonce[NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES],
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* plaintext,
    size_t plaintext_len) {
  if (out == NULL || out_len == NULL || key == NULL || nonce == NULL || plaintext == NULL) {
    return -1;
  }
  if (out_capacity < plaintext_len + NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES) {
    return -2;
  }

  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int rc = mbedtls_chachapoly_setkey(&ctx, key);
  if (rc != 0) {
    mbedtls_chachapoly_free(&ctx);
    return rc;
  }

  rc = mbedtls_chachapoly_encrypt_and_tag(&ctx,
                                          plaintext_len,
                                          nonce,
                                          aad,
                                          aad_len,
                                          plaintext,
                                          out,
                                          out + plaintext_len);
  mbedtls_chachapoly_free(&ctx);
  if (rc != 0) {
    return rc;
  }
  *out_len = plaintext_len + NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES;
  return 0;
}

int backend_crypto_ref_chacha20_poly1305_open(
    uint8_t* out,
    size_t out_capacity,
    size_t* out_len,
    const uint8_t key[NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES],
    const uint8_t nonce[NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES],
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* ciphertext,
    size_t ciphertext_len) {
  if (out == NULL || out_len == NULL || key == NULL || nonce == NULL || ciphertext == NULL) {
    return -1;
  }
  if (ciphertext_len < NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES) {
    return -2;
  }

  size_t plaintext_len = ciphertext_len - NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES;
  if (out_capacity < plaintext_len) {
    return -3;
  }

  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int rc = mbedtls_chachapoly_setkey(&ctx, key);
  if (rc != 0) {
    mbedtls_chachapoly_free(&ctx);
    return rc;
  }

  rc = mbedtls_chachapoly_auth_decrypt(&ctx,
                                       plaintext_len,
                                       nonce,
                                       aad,
                                       aad_len,
                                       ciphertext + plaintext_len,
                                       ciphertext,
                                       out);
  mbedtls_chachapoly_free(&ctx);
  if (rc != 0) {
    memset(out, 0, plaintext_len);
    return rc;
  }
  *out_len = plaintext_len;
  return 0;
}
