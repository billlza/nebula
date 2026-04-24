#include "abi-demo.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  nebula_abi_demo_main_ping();
  printf("%lld %d %.1f\n",
         (long long)nebula_abi_demo_main_answer(),
         nebula_abi_demo_main_flip(false) ? 1 : 0,
         nebula_abi_demo_main_half(8.0));
  return 0;
}
