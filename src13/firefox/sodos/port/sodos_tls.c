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

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8

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
