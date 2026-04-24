#define BLAKE3_NO_SSE2 1
#define BLAKE3_NO_SSE41 1
#define BLAKE3_NO_AVX2 1
#define BLAKE3_NO_AVX512 1

#include "blake3.c"
#include "blake3_dispatch.c"
#include "blake3_portable.c"

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#include "blake3_neon.c"
#endif

#include "nebula_crypto_native.h"

void nebula_crypto_blake3_digest(const uint8_t* input, size_t input_len, uint8_t out[32]) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, input, input_len);
  blake3_hasher_finalize(&hasher, out, 32);
}
