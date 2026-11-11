/* t_plug.c - vira libtplug.so: so e carregada por dlopen. Chama de volta um
 * simbolo do programa (exportado com -rdynamic) e tem thread_local, que cai na
 * folga do TLS estatico. */
#include <stdio.h>

int host_value(void);            /* definido no executavel */
int foo_add(int a, int b);       /* libtfoo, ja carregada pelo programa */

__thread int plug_tls = 1000;
static int g_inited;

__attribute__((constructor)) static void plug_init(void) { g_inited = 5; }

int plug_run(int x) {
  plug_tls += x;
  return foo_add(x, host_value()) + g_inited + plug_tls;
}

const char* plug_name(void) { return "tplug"; }
