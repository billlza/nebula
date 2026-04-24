#ifndef OQS_RANDOM_H
#define OQS_RANDOM_H

#include <stddef.h>
#include <stdint.h>

#include <oqs/common.h>

#if defined(__cplusplus)
extern "C" {
#endif

void OQS_randombytes(uint8_t *random_array, size_t bytes_to_read);

#if defined(__cplusplus)
}
#endif

#endif
