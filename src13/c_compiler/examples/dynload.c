/* dynload.c - carrega a MATHX.DLL so na hora de usar (k_dll_load/k_dll_sym).
 * cc dynload.c  (nao precisa da DLL no link) */
#include <stdio.h>
#include <so.h>

typedef int (*gcd_fn)(int, int);
typedef int (*count_fn)(void);

int main(int argc, char** argv) {
  const char* name = argc > 1 ? argv[1] : "MATHX.DLL";
  int h = k_dll_load(name);
  gcd_fn gcd;
  count_fn calls;
  if (!h) {
    printf("nao carregou %s: %s\n", name, k_last_error());
    return 1;
  }
  gcd = (gcd_fn)k_dll_sym(h, "mx_gcd");
  calls = (count_fn)k_dll_sym(h, "mx_calls");
  if (!gcd || !calls) {
    printf("%s nao tem mx_gcd/mx_calls\n", name);
    return 1;
  }
  printf("mdc(1071, 462) = %d via %s\n", gcd(1071, 462), name);
  printf("mx_internal exportada? %s\n", k_dll_sym(h, "mx_internal") ? "sim" : "nao");
  printf("%d chamada(s)\n", calls());
  k_dll_free(h);
  return 0;
}
