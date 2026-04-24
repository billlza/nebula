#ifndef NEBULA_CRYPTO_OQS_MINI_FIPS202_GLUE_H
#define NEBULA_CRYPTO_OQS_MINI_FIPS202_GLUE_H

#include "../../pqclean_shims/fips202.h"

#define mlk_shake128ctx shake128incctx
#define mlk_shake128_absorb_once shake128_absorb_once
#define mlk_shake128_squeezeblocks shake128_squeezeblocks
#define mlk_shake128_init shake128_init
#define mlk_shake128_release shake128_inc_ctx_release
#define mlk_shake256 shake256
#define mlk_sha3_256 sha3_256
#define mlk_sha3_512 sha3_512

#endif
