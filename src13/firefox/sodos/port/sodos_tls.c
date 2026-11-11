/* sodos_tls.c - TLS estatico (variante II do x86_64) para programas SOX2.
 *
 * O GNU ld resolve thread_local para acessos relativos a %fs. Aqui montamos o
 * bloco de cada thread e apontamos o FS para ele. Os programas rodam em ring 0,
 * entao o FS e trocado direto pelo MSR. As variaveis __sodos_tls_* sao preenchidas
 * por toolchain/elf2sox2.py; com filesz e memsz em 0, o programa nao tem TLS.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(SODOS_DYNAMIC)

/* libc.so.6: o programa e as bibliotecas sao ELF e quem conhece o TLS de cada
 * modulo e o carregador do kernel (k_dl_iter). O bloco de toda thread tem o
 * tamanho que o kernel fixou na partida (o executavel, as bibliotecas dele e
 * uma folga); um dlopen posterior so ocupa um pedaco da folga. Cada bloco
 * guarda quantos modulos ja recebeu, e a troca de thread copia os que faltam. */
#include <so.h>

#define TCB_SIZE 256
#define TCB_INSTALLED(tp) (*(uint64_t*)((char*)(tp) + 0x30))
#define TLS_MODS_MAX 128

struct tls_mod {
  uint64_t image, filesz, offset;
};

static struct tls_mod g_mods[TLS_MODS_MAX];
static uint64_t g_off_by_id[TLS_MODS_MAX]; /* indice = tls_modid */
static unsigned g_nmods;
static uint64_t g_static;
static void* g_main_tp;

static void set_fs_base(void* tp) {
  uint64_t v = (uint64_t)(uintptr_t)tp;
  __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Acrescenta a g_mods os modulos com TLS que o kernel carregou desde a ultima vez. */
static void tls_scan(void) {
  struct k_dl_info di;
  int i;
  for (i = 0; k_dl_iter(i, &di); i++) {
    if (di.flags & K_DL_EXE) g_static = di.tls_static;
    if (!di.tls_modid || di.tls_modid >= TLS_MODS_MAX || g_off_by_id[di.tls_modid]) continue;
    if (g_nmods >= TLS_MODS_MAX) break;
    g_off_by_id[di.tls_modid] = di.tls_offset;
    g_mods[g_nmods].image = di.tls_image;
    g_mods[g_nmods].filesz = di.tls_filesz;
    g_mods[g_nmods].offset = di.tls_offset;
    g_nmods++;
  }
}

/* O resto do bloco (.tbss) ja nasce zerado. */
static void tls_install(char* tp) {
  unsigned k;
  for (k = (unsigned)TCB_INSTALLED(tp); k < g_nmods; k++)
    if (g_mods[k].filesz)
      memcpy(tp - g_mods[k].offset, (const void*)(uintptr_t)g_mods[k].image, g_mods[k].filesz);
  TCB_INSTALLED(tp) = g_nmods;
}

void* sodos__tls_new(void** raw) {
  char* p;
  char* tp;
  *raw = NULL;
  if (!g_nmods && !g_static) tls_scan();
  p = (char*)calloc(1, g_static + TCB_SIZE + 64);
  if (!p) return NULL;
  tp = (char*)(((uintptr_t)p + g_static + 63) & ~(uintptr_t)63);
  *(void**)tp = tp;
  *(uint64_t*)(tp + 0x28) = 0x9e3779b97f4a7c15ull; /* mesmo guarda de pilha do kernel */
  tls_install(tp);
  *raw = p;
  return tp;
}

void sodos__tls_switch(void* tp) {
  if (!tp) tp = g_main_tp;
  if (!tp) return;
  if (TCB_INSTALLED(tp) != g_nmods) tls_install((char*)tp);
  set_fs_base(tp);
}

/* Depois de um dlopen: a thread atual recebe ja o TLS da biblioteca nova. */
void sodos__tls_sync(void) {
  char* tp;
  tls_scan();
  __asm__("movq %%fs:0, %0" : "=r"(tp));
  if (tp) tls_install(tp);
}

unsigned long long sodos__tls_offset(unsigned modid) {
  if (modid >= TLS_MODS_MAX) return 0;
  if (!g_off_by_id[modid]) tls_scan();
  return g_off_by_id[modid];
}

/* Chamada por __libc_start_main antes de qualquer construtor. */
void sodos__tls_init_main(void) {
  void* raw;
  tls_scan();
  g_main_tp = sodos__tls_new(&raw);
  if (g_main_tp) set_fs_base(g_main_tp);
}

#elif defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8

long __sodos_tls_rel = 1;        /* dados fora da .bss: o conversor os grava */
unsigned long __sodos_tls_filesz = 1;
unsigned long __sodos_tls_memsz = 1;
unsigned long __sodos_tls_align = 1;

/* link64 nao chama o conversor: os valores iniciais acima significam "sem TLS". */
#define TLS_ABSENT() (__sodos_tls_rel == 1 && __sodos_tls_memsz == 1)

#define TCB_SIZE 256 /* %fs:0 aponta para si mesmo; %fs:0x28 e o guarda de pilha */

static void* g_main_tp;

static void set_fs_base(void* tp) {
  uint64_t v = (uint64_t)(uintptr_t)tp;
  __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Devolve o ponteiro de thread; *raw e o bloco a liberar. */
void* sodos__tls_new(void** raw) {
  unsigned long align, msize;
  char* p;
  char* tp;

  *raw = NULL;
  if (TLS_ABSENT()) return NULL;
  /* O ld calcula os deslocamentos com o alinhamento do proprio segmento TLS. */
  align = __sodos_tls_align ? __sodos_tls_align : 1;
  msize = (__sodos_tls_memsz + align - 1) & ~(align - 1);
  if (align < 16) align = 16;
  p = (char*)calloc(1, msize + TCB_SIZE + align);
  if (!p) return NULL;
  tp = (char*)(((uintptr_t)p + msize + align - 1) & ~(uintptr_t)(align - 1));
  if (__sodos_tls_filesz)
    memcpy(tp - msize, (const char*)&__sodos_tls_rel + __sodos_tls_rel, __sodos_tls_filesz);
  *(void**)tp = tp;
  *(uint64_t*)(tp + 0x28) = 0x9e3779b97f4a7c15ull; /* mesmo guarda de pilha do kernel */
  *raw = p;
  return tp;
}

void sodos__tls_switch(void* tp) {
  if (!tp) tp = g_main_tp;
  if (tp) set_fs_base(tp);
}

/* Roda antes de qualquer outro construtor. */
__attribute__((constructor(101))) static void sodos_tls_init_main(void) {
  void* raw;
  g_main_tp = sodos__tls_new(&raw);
  if (g_main_tp) set_fs_base(g_main_tp);
}

#else

void* sodos__tls_new(void** raw) {
  *raw = NULL;
  return NULL;
}

void sodos__tls_switch(void* tp) { (void)tp; }

#endif
