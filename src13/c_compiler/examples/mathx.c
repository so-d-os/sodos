/* mathx.c - exemplo de DLL: cc -dll mathx.c  ->  MATHX.DLL
 * Somente o que tem __export entra na tabela de exports. */
#include <stdio.h>
#include <so.h>
#include "mathx.h"

static int calls;

/* chamada pelo loader: 1 ao carregar, 0 ao descarregar; devolver 0 recusa */
int DllMain(int reason) {
  if (reason == 1) calls = 0;
  return 1;
}

__export int mx_gcd(int a, int b) {
  calls++;
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  while (b) {
    int t = a % b;
    a = b;
    b = t;
  }
  return a;
}

__export unsigned long long mx_fact(int n) {
  unsigned long long r = 1;
  calls++;
  while (n > 1) r *= (unsigned)n--;
  return r;
}

__export int mx_is_prime(unsigned n) {
  unsigned d;
  calls++;
  if (n < 2) return 0;
  for (d = 2; d * d <= n; d++)
    if (n % d == 0) return 0;
  return 1;
}

__export int mx_calls(void) { return calls; }

/* sem __export: fica so dentro da DLL */
int mx_internal(void) { return 42; }
