#ifndef OQS_COMMON_H
#define OQS_COMMON_H

#include <oqs/oqsconfig.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define OQS_API

#define OQS_EXIT_IF_NULLPTR(x, loc)                                            \
  do {                                                                         \
    if ((x) == (void *)0) {                                                    \
      fprintf(stderr, "Unexpected NULL returned from %s API. Exiting.\n",      \
              loc);                                                            \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

typedef enum {
  OQS_SUCCESS = 0,
  OQS_ERROR = -1,
} OQS_STATUS;

void *OQS_MEM_aligned_alloc(size_t alignment, size_t size);
void OQS_MEM_aligned_free(void *ptr);
void OQS_randombytes(uint8_t *random_array, size_t bytes_to_read);

#if defined(__cplusplus)
}
#endif

#endif
