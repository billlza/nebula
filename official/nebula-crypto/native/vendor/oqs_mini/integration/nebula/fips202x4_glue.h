#ifndef NEBULA_CRYPTO_OQS_MINI_FIPS202X4_GLUE_H
#define NEBULA_CRYPTO_OQS_MINI_FIPS202X4_GLUE_H

#include "../../pqclean_shims/fips202x4.h"

#define mlk_shake128x4ctx shake128x4ctx
#define mlk_shake128x4_absorb_once shake128x4_absorb_once
#define mlk_shake128x4_squeezeblocks shake128x4_squeezeblocks
#define mlk_shake128x4_init shake128x4_init
#define mlk_shake128x4_release shake128x4_release
#define mlk_shake256x4 shake256x4

#endif
