/* t_dso.c - DSOTEST.EXE: executavel dinamico (libc.so.6 + libtfoo.so.1).
 * Confere DT_NEEDED, dados e funcoes entre modulos, TLS, dlopen/dlsym/dladdr,
 * dl_iterate_phdr, threads e a ordem de construtores/destrutores. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>

int foo_add(int a, int b);
int foo_bump(void);
int* foo_tls_addr(void);
extern int foo_counter;
extern __thread int foo_tls;

static int ok, fail;
__thread int exe_tls = 11;

static void check(const char* what, int cond) {
  printf("%s %s\n", cond ? "PASS" : "FALHA", what);
  if (cond) ok++; else fail++;
}

int host_value(void) { return 100; }

static int count_mods(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  if (info->dlpi_phdr && info->dlpi_phnum) (*(int*)data)++;
  return 0;
}

static void* worker(void* arg) {
  /* Cada thread tem a sua copia, com o valor inicial. */
  int a = exe_tls, b = foo_tls, c = *foo_tls_addr();
  exe_tls += 1;
  foo_tls += 1;
  *(int*)arg = (a == 11 && b == 7 && c == 7 && foo_tls == 8) ? 1 : 0;
  return NULL;
}

static void at_exit_fn(void) { printf("atexit do programa\n"); }

__attribute__((destructor)) static void exe_fini(void) { printf("destrutor do programa\n"); }

int main(int argc, char** argv) {
  void* h;
  int (*run)(int);
  const char* (*name)(void);
  Dl_info di;
  int mods = 0, tok = 0;
  pthread_t th;
  char* p;

  printf("DSOTEST argc=%d\n", argc);
  atexit(at_exit_fn);

  check("funcao de biblioteca (DT_NEEDED)", foo_add(2, 3) == 5);
  check("dado exportado pela biblioteca", foo_counter == 40 && foo_bump() == 41 && foo_counter == 41);
  check("TLS do programa e da biblioteca", exe_tls == 11 && foo_tls == 7 && foo_tls_addr() == &foo_tls);
  p = (char*)malloc(100000);
  check("malloc/strcpy da libc.so.6", p && strcmp(strcpy(p, "sodos"), "sodos") == 0);
  free(p);

  dl_iterate_phdr(count_mods, &mods);
  check("dl_iterate_phdr ve programa, libtfoo e libc", mods >= 3);

  check("dlopen de biblioteca que nao existe falha", dlopen("libnaoexiste.so", RTLD_NOW) == NULL && dlerror() != NULL);
  h = dlopen("libtplug.so", RTLD_NOW);
  check("dlopen(libtplug.so)", h != NULL);
  if (h) {
    run = (int (*)(int))dlsym(h, "plug_run");
    name = (const char* (*)(void))dlsym(h, "plug_name");
    check("dlsym acha as funcoes", run && name && strcmp(name(), "tplug") == 0);
    /* foo_add(1, 100) + 5 + (1000 + 1) */
    check("plugin chama o programa e a libtfoo, com TLS proprio", run && run(1) == 101 + 5 + 1001);
    check("dlsym de simbolo ausente devolve NULL", dlsym(h, "nao_existe") == NULL);
    check("dlopen repetido devolve o mesmo handle", dlopen("libtplug.so", RTLD_NOW) == h);
    check("dladdr acha a biblioteca do simbolo",
          run && dladdr((void*)run, &di) && di.dli_sname && strcmp(di.dli_sname, "plug_run") == 0);
  }
  check("dlsym(RTLD_DEFAULT) acha funcao da libc", dlsym(RTLD_DEFAULT, "printf") == (void*)printf);
  check("dlsym no handle do programa acha a libtfoo",
        dlsym(dlopen(NULL, RTLD_NOW), "foo_add") == (void*)foo_add);

  pthread_create(&th, NULL, worker, &tok);
  pthread_join(th, NULL);
  check("TLS por thread (valor inicial e copia propria)", tok == 1 && exe_tls == 11 && foo_tls == 7);

  printf("DSOTEST: %d ok, %d falhas\n", ok, fail);
  return fail ? 1 : 0;
}
