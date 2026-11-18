/*
 * main.c - Entrada do WINE.DLL: monta o processo Windows e chama o EXE.
 *
 * O kernel (kernel/wine.c) so chega aqui com SET WINE=1. start() roda como a
 * thread principal de um programa comum do SODOS (pid, pilha e argv do
 * mod_exec); tudo que se aloca aqui e do programa e some quando ele termina.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "../../kernel/wine.h"

void k_win_teb(void* teb);

static int quote_needed(const char* s) {
  if (!*s) return 1;
  for (; *s; s++)
    if (*s == ' ' || *s == '\t' || *s == '"') return 1;
  return 0;
}

/* argv -> "linha de comando" do Windows (aspas so onde precisa). */
static char* build_cmdline(int argc, char** argv) {
  size_t n = 1;
  int i;
  char* c;
  for (i = 0; i < argc; i++) n += strlen(argv[i]) * 2 + 3;
  c = (char*)wzalloc(n);
  if (!c) return 0;
  for (i = 0; i < argc; i++) {
    const char* s = argv[i];
    if (i) strcat(c, " ");
    if (quote_needed(s)) {
      char* d = c + strlen(c);
      *d++ = '"';
      for (; *s; s++) {
        if (*s == '"') *d++ = '\\';
        *d++ = *s;
      }
      *d++ = '"';
      *d = 0;
    } else {
      strcat(c, s);
    }
  }
  return c;
}

static int start(int argc, char** argv) {
  struct wproc* p;
  uint8_t* teb;
  void** tlsarr;
  uint64_t rsp;
  int i;
  typedef int (WINAPI * entry_fn)(void*);
  struct wmod* exe;

  if (argc < 1 || !argv || !argv[0]) return 1;
  p = (struct wproc*)wzalloc(sizeof(*p));
  teb = (uint8_t*)k_malloc_aligned(TEB_SIZE, 16);
  tlsarr = (void**)wzalloc(sizeof(void*) * TLS_SLOTS);
  if (!p || !teb || !tlsarr) {
    k_puts("wine: sem memoria\n");
    return 1;
  }
  memset(teb, 0, TEB_SIZE);
  __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
  *(uint64_t*)(teb + TEB_STACKBASE) = (rsp + 0xFFF) & ~0xFFFull;
  *(uint64_t*)(teb + TEB_STACKLIMIT) = (rsp & ~0xFFFull) - (WINE_STACK - 0x20000u);
  *(uint64_t*)(teb + TEB_SELF) = (uint64_t)(uintptr_t)teb;
  *(uint64_t*)(teb + TEB_PID) = 0x100;
  *(uint64_t*)(teb + TEB_TID) = 0x104;
  *(uint64_t*)(teb + TEB_TLSPTR) = (uint64_t)(uintptr_t)tlsarr;
  *(uint64_t*)(teb + TEB_PEB) = (uint64_t)(uintptr_t)p;
  *(uint64_t*)(p->peb + PEB_HEAP) = 1;   /* GetProcessHeap() */
  p->teb = teb;
  k_win_teb(teb);   /* daqui em diante gs:0x30/0x58/0x60 sao deste processo */

  p->argc = argc;
  p->argv = (char**)wzalloc(sizeof(char*) * ((size_t)argc + 1));
  p->wargv = (wchar16**)wzalloc(sizeof(wchar16*) * ((size_t)argc + 1));
  p->environ = (char**)wzalloc(sizeof(char*) * 2);
  p->wenviron = (wchar16**)wzalloc(sizeof(wchar16*) * 2);
  if (!p->argv || !p->wargv || !p->environ || !p->wenviron) {
    k_puts("wine: sem memoria\n");
    return 1;
  }
  for (i = 0; i < argc; i++) {
    p->argv[i] = argv[i];
    p->wargv[i] = wdup8to16(argv[i]);
  }
  p->cmdline = build_cmdline(argc, argv);
  p->wcmdline = wdup8to16(p->cmdline ? p->cmdline : "");
  wlog("start ", p->cmdline);

  exe = pe_load(argv[0], 1);
  if (!exe) return 1;
  if (!exe->entry) {
    wsay(exe->name, ": sem ponto de entrada");
    return 1;
  }
  {
    struct wmod* m;
    for (m = p->mods; m; m = m->next)
      if (m == exe) pe_call_tls_callbacks(m, 1);
  }
  k_win_teb(teb);
  /* O EXE do Windows recebe o PEB em rcx e quase sempre termina por
   * ExitProcess (wexit -> k_exit), sem voltar aqui. */
  wexit(((entry_fn)(uintptr_t)exe->entry)(p->peb));
}

__attribute__((noreturn)) void wexit(int code) {
  struct wproc* p = wp();
  if (p && !p->exiting) {
    p->exiting = 1;
    pe_detach_all();
  }
  wlog("ExitProcess", 0);
  k_exit(code);
  for (;;) {
  }
}

static const struct wine_ops ops = {WINE_ABI, start};

const struct wine_ops* wine_driver(void) { return &ops; }
