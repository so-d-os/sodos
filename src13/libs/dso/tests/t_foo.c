/* t_foo.c - vira libtfoo.so.1: biblioteca que o programa de teste liga no
 * link (DT_NEEDED). Tem estado proprio, construtor/destrutor, uma variavel de
 * thread com valor inicial e usa a libm pelo nome padrao. */
#include <stdio.h>
#include <math.h>

int foo_counter = 40;
__thread int foo_tls = 7;
static int g_ready;

__attribute__((constructor)) static void foo_init(void) {
  g_ready = 1;
  printf("libtfoo: construtor\n");
}

__attribute__((destructor)) static void foo_fini(void) { printf("libtfoo: destrutor\n"); }

int foo_add(int a, int b) { return g_ready ? a + b + (int)sqrt(4.0) - 2 : -1; }

int foo_bump(void) { return ++foo_counter; }

int* foo_tls_addr(void) { return &foo_tls; }
