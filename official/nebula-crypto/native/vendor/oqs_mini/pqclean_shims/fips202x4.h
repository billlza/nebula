#ifndef FIPS202X4_H
#define FIPS202X4_H

#include <oqs/sha3.h>

#include <stddef.h>
#include <stdint.h>

typedef struct {
  OQS_SHA3_shake128_inc_ctx lanes[4];
} shake128x4ctx;

typedef struct {
  OQS_SHA3_shake256_inc_ctx lanes[4];
} shake256x4ctx;

#define shake128x4_init shake128x4_inc_init
#define shake128x4_release shake128x4_inc_ctx_release

#define SHAKE128_RATE OQS_SHA3_SHAKE128_RATE
#define SHAKE256_RATE OQS_SHA3_SHAKE256_RATE

static inline void shake128x4_inc_init(shake128x4ctx *state) {
  for (int i = 0; i < 4; ++i) {
    OQS_SHA3_shake128_inc_init(&state->lanes[i]);
  }
}

static inline void shake128x4_absorb_once(shake128x4ctx *state,
                                          const uint8_t *in0,
                                          const uint8_t *in1,
                                          const uint8_t *in2,
                                          const uint8_t *in3,
                                          size_t inlen) {
  const uint8_t *inputs[4] = {in0, in1, in2, in3};
  for (int i = 0; i < 4; ++i) {
    OQS_SHA3_shake128_inc_ctx_reset(&state->lanes[i]);
    OQS_SHA3_shake128_inc_absorb(&state->lanes[i], inputs[i], inlen);
    OQS_SHA3_shake128_inc_finalize(&state->lanes[i]);
  }
}

static inline void shake128x4_squeezeblocks(uint8_t *out0,
                                            uint8_t *out1,
                                            uint8_t *out2,
                                            uint8_t *out3,
                                            size_t nblocks,
                                            shake128x4ctx *state) {
  uint8_t *outputs[4] = {out0, out1, out2, out3};
  const size_t outlen = nblocks * OQS_SHA3_SHAKE128_RATE;
  for (int i = 0; i < 4; ++i) {
    OQS_SHA3_shake128_inc_squeeze(outputs[i], outlen, &state->lanes[i]);
  }
}

static inline void shake128x4_inc_ctx_release(shake128x4ctx *state) {
  for (int i = 0; i < 4; ++i) {
    OQS_SHA3_shake128_inc_ctx_release(&state->lanes[i]);
  }
}

static inline void shake256x4(uint8_t *out0,
                              uint8_t *out1,
                              uint8_t *out2,
                              uint8_t *out3,
                              size_t outlen,
                              const uint8_t *in0,
                              const uint8_t *in1,
                              const uint8_t *in2,
                              const uint8_t *in3,
                              size_t inlen) {
  OQS_SHA3_shake256(out0, outlen, in0, inlen);
  OQS_SHA3_shake256(out1, outlen, in1, inlen);
  OQS_SHA3_shake256(out2, outlen, in2, inlen);
  OQS_SHA3_shake256(out3, outlen, in3, inlen);
}

#endif
