/* usemath.c - usa a MATHX.DLL ligada no link: cc usemath.c mathx.dll */
#include <stdio.h>
#include "mathx.h"

int main(void) {
  unsigned n;
  printf("mdc(84, 36) = %d\n", mx_gcd(84, 36));
  printf("20! = %llu\n", mx_fact(20));
  printf("primos ate 50:");
  for (n = 0; n <= 50; n++)
    if (mx_is_prime(n)) printf(" %u", n);
  printf("\n%d chamadas na DLL\n", mx_calls());
  return 0;
}
