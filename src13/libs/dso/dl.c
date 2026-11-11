/* dl.c - dlopen/dlsym/dladdr/dl_iterate_phdr sobre o carregador ELF do kernel
 * (k_dl_*). Faz parte da libc.so.6; libdl.so.2 existe so pelo nome.
 *
 * dlclose nao descarrega: a biblioteca fica ate o programa terminar. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <link.h>
#include <so.h>

void __so_dl_run_init(void);
void sodos__tls_sync(void);
unsigned long long sodos__tls_offset(unsigned modid);

static char g_err[96];
static int g_err_set;

static void set_error(const char* what, const char* name) {
  const char* k = k_last_error();
  size_t n = 0;
  if (k && k[0]) what = k, name = 0;
  while (*what && n < sizeof(g_err) - 1) g_err[n++] = *what++;
  while (name && *name && n < sizeof(g_err) - 1) g_err[n++] = *name++;
  g_err[n] = 0;
  g_err_set = 1;
}

void* dlopen(const char* file, int mode) {
  int h = k_dl_open(file, mode & (RTLD_GLOBAL | RTLD_NOLOAD));
  if (!h) {
    set_error("dlopen: ", file ? file : "(programa)");
    return NULL;
  }
  /* thread_local da biblioteca nova, depois os construtores dela. */
  sodos__tls_sync();
  __so_dl_run_init();
  return (void*)(intptr_t)h;
}

void* dlsym(void* handle, const char* name) {
  void* p = k_dl_sym((int)(intptr_t)handle, name, __builtin_return_address(0));
  if (!p) set_error("dlsym: ", name);
  return p;
}

int dlclose(void* handle) {
  (void)handle;
  return 0;
}

char* dlerror(void) {
  if (!g_err_set) return NULL;
  g_err_set = 0;
  return g_err;
}

int dladdr(const void* addr, Dl_info* info) {
  struct k_dl_addr a;
  if (!info || !k_dl_addr(addr, &a)) return 0;
  info->dli_fname = a.fname;
  info->dli_fbase = (void*)(uintptr_t)a.fbase;
  info->dli_sname = a.sname;
  info->dli_saddr = (void*)(uintptr_t)a.saddr;
  return 1;
}

int dl_iterate_phdr(int (*cb)(struct dl_phdr_info*, size_t, void*), void* data) {
  struct k_dl_info di;
  int i, r;
  for (i = 0; k_dl_iter(i, &di); i++) {
    struct dl_phdr_info info;
    info.dlpi_addr = (ElfW(Addr))di.base;
    info.dlpi_name = (di.flags & K_DL_EXE) ? "" : di.path;
    info.dlpi_phdr = (const ElfW(Phdr)*)(uintptr_t)di.phdr;
    info.dlpi_phnum = (ElfW(Half))di.phnum;
    r = cb(&info, sizeof(info), data);
    if (r) return r;
  }
  return 0;
}

/* Modelo general-dynamic de TLS (codigo PIC sem -ftls-model=initial-exec).
 * Todo modulo tem lugar fixo no bloco estatico da thread. */
struct tls_index {
  unsigned long module;
  unsigned long offset;
};

void* __tls_get_addr(struct tls_index* ti) {
  char* tp;
  __asm__("movq %%fs:0, %0" : "=r"(tp));
  return tp - sodos__tls_offset((unsigned)ti->module) + ti->offset;
}
