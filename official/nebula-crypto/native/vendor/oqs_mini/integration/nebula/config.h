#ifndef NEBULA_CRYPTO_OQS_MINI_MLK_CONFIG_H
#define NEBULA_CRYPTO_OQS_MINI_MLK_CONFIG_H

#ifndef MLKEM_K
#define MLKEM_K 3
#endif

#if MLKEM_K == 3
#define MLK_NAMESPACE_PREFIX PQCP_MLKEM_NATIVE_MLKEM768_C
#else
#error "nebula-crypto oqs-mini only supports ML-KEM-768 in this slice"
#endif

#define MLK_FIPS202_CUSTOM_HEADER "../integration/nebula/fips202_glue.h"
#define MLK_FIPS202X4_CUSTOM_HEADER "../integration/nebula/fips202x4_glue.h"

#endif
