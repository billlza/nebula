#include <oqs/common.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <sys/random.h>
#else
#error "nebula-crypto oqs-mini supports only macOS and Linux hosts"
#endif

void *OQS_MEM_aligned_alloc(size_t alignment, size_t size) {
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  void *ptr = NULL;
  if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
  return ptr;
}

void OQS_MEM_aligned_free(void *ptr) {
  free(ptr);
}

void OQS_randombytes(uint8_t *random_array, size_t bytes_to_read) {
  if (bytes_to_read == 0) return;

#if defined(__APPLE__)
  arc4random_buf(random_array, bytes_to_read);
#else
  size_t filled = 0;
  while (filled < bytes_to_read) {
    const ssize_t rc = getrandom(random_array + filled, bytes_to_read - filled, 0);
    if (rc > 0) {
      filled += (size_t)rc;
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    abort();
  }
#endif
}
