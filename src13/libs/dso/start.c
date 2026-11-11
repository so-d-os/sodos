/* start.c - partida e saida de um programa dinamico. Faz parte da libc.so.6.
 *
 * O kernel entrega o programa com as bibliotecas ja mapeadas e relocadas, mas
 * sem rodar nada delas. __libc_start_main (chamada pelo crt1.o) monta o TLS da
 * thread principal, roda os inicializadores na ordem que o kernel informa
 * (dependencias antes de quem depende, o executavel por ultimo) e chama main.
 * exit() desfaz na ordem inversa: atexit/__cxa_atexit e depois os .fini_array. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <so.h>

extern char** environ;

/* Limites da pilha do programa para pthread_attr_getstack (sodos_extra.c). */
char* __sodos_stack_top;
unsigned long __sodos_stack_size_v;
/* argv[0] vira "/proc/self/exe" em readlink (misc.c). */
char** __sodos_argv;

void sodos__tls_init_main(void);

struct exit_fn {
  void (*fn)(void*);
  void* arg;
  void* dso;
};

static struct exit_fn* g_fns;
static int g_n, g_cap;

int __cxa_atexit(void (*fn)(void*), void* arg, void* dso) {
  if (g_n == g_cap) {
    int cap = g_cap ? g_cap * 2 : 64;
    struct exit_fn* p = (struct exit_fn*)realloc(g_fns, (size_t)cap * sizeof(*p));
    if (!p) return -1;
    g_fns = p;
    g_cap = cap;
  }
  g_fns[g_n].fn = fn;
  g_fns[g_n].arg = arg;
  g_fns[g_n].dso = dso;
  g_n++;
  return 0;
}

/* As threads sao cooperativas e nao morrem sozinhas: os destrutores de
 * thread_local rodam na saida do programa. */
int __cxa_thread_atexit(void (*fn)(void*), void* obj, void* dso) { return __cxa_atexit(fn, obj, dso); }
int __cxa_thread_atexit_impl(void (*fn)(void*), void* obj, void* dso) { return __cxa_atexit(fn, obj, dso); }

/* dso 0: fim do programa, roda tudo (inclusive o que um destrutor registrar).
 * Com dso, so os registros daquele modulo. */
void __cxa_finalize(void* dso) {
  int i;
  if (!dso) {
    while (g_n > 0) {
      struct exit_fn f = g_fns[--g_n];
      if (f.fn) f.fn(f.arg);
    }
    return;
  }
  for (i = g_n - 1; i >= 0; i--) {
    struct exit_fn f = g_fns[i];
    if (!f.fn || f.dso != dso) continue;
    g_fns[i].fn = 0;
    f.fn(f.arg);
  }
}

typedef void (*init_fn)(int, char**, char**);
typedef void (*fini_fn)(void);

static int g_argc;

/* Roda o que ainda nao foi inicializado: na partida, todos os modulos; depois
 * de um dlopen, so os que ele trouxe. */
void __so_dl_run_init(void) {
  struct k_dl_info di;
  while (k_dl_init_next(&di)) {
    init_fn* arr = (init_fn*)(uintptr_t)di.init_array;
    unsigned long long n = di.init_arraysz / sizeof(init_fn);
    unsigned long long i;
    if (di.init) ((init_fn)(uintptr_t)di.init)(g_argc, __sodos_argv, environ);
    for (i = 0; i < n; i++)
      if (arr[i] && arr[i] != (init_fn)(intptr_t)-1) arr[i](g_argc, __sodos_argv, environ);
  }
}

/* Chamado por exit() antes de fechar os arquivos. */
void __so_run_exit(void) {
  static int running;
  struct k_dl_info di;
  if (running) return;
  running = 1;
  __cxa_finalize(0);
  while (k_dl_fini_next(&di)) {
    fini_fn* arr = (fini_fn*)(uintptr_t)di.fini_array;
    unsigned long long n = di.fini_arraysz / sizeof(fini_fn);
    while (n--)
      if (arr[n] && arr[n] != (fini_fn)(intptr_t)-1) arr[n]();
    if (di.fini) ((fini_fn)(uintptr_t)di.fini)();
  }
}

int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv, void (*init)(void),
                      void (*fini)(void), void (*rtld_fini)(void), void* stack_end) {
  struct k_dl_info exe;
  (void)init;
  (void)fini;
  (void)rtld_fini;
  __sodos_stack_top = (char*)stack_end;
  __sodos_argv = argv;
  g_argc = argc;
  if (k_dl_info(k_dl_open(0, 0), &exe)) __sodos_stack_size_v = (unsigned long)exe.stack_size;
  sodos__tls_init_main();
  __so_dl_run_init();
  exit(main(argc, argv, environ));
  return 0;
}
