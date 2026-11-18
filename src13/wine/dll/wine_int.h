/*
 * wine_int.h - Estruturas internas do WINE.DLL (W0).
 *
 * Todo o estado de um processo Windows fica em struct wproc, que comeca com o
 * PEB: o codigo acha o processo corrente por gs:0x60 (TEB->Peb), e o kernel
 * troca a janela do gs a cada tarefa (kernel/wine.h). Assim dois EXEs do
 * Windows (duas janelas do WIN) nao dividem nada alem do codigo do DLL.
 *
 * As funcoes chamadas pelo codigo do Windows usam a ABI da Microsoft
 * (WINAPI = ms_abi): rcx, rdx, r8, r9 + 32 bytes de sombra.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define WINAPI __attribute__((ms_abi))

typedef uint16_t wchar16;

/* ---------- TEB / PEB (layout x64 do Windows, so os campos usados) ---------- */
#define TEB_SIZE 0x2000u
#define TEB_STACKBASE 0x08
#define TEB_STACKLIMIT 0x10
#define TEB_SELF 0x30
#define TEB_PID 0x40
#define TEB_TID 0x48
#define TEB_TLSPTR 0x58
#define TEB_PEB 0x60
#define TEB_LASTERR 0x68
#define TEB_TLSSLOTS 0x1480   /* 64 ponteiros */
#define TLS_SLOTS 64

#define PEB_IMAGEBASE 0x10
#define PEB_LDR 0x18
#define PEB_PARAMS 0x20
#define PEB_HEAP 0x30
#define PEB_SIZE 0x800u

/* ---------- modulos ---------- */
#define WMOD_MAGIC 0x444F4D57u  /* "WMOD": HMODULE de DLL embutida */
#define WMOD_NAME 64

struct wine_sym {
  const char* name;
  void* fn;
};

struct wine_builtin {
  const char* name;             /* "kernel32", "ucrtbase", ... */
  const struct wine_sym* syms;  /* termina com {0, 0} */
};

struct wmod {
  uint32_t magic;               /* WMOD_MAGIC (so para embutidas) */
  struct wmod* next;
  char name[WMOD_NAME];         /* minusculas, sem pasta, com .dll/.exe */
  char path[160];
  uint8_t* base;                /* imagem PE (0 nas embutidas) */
  uint32_t size;
  uint64_t entry;
  int is_dll;
  int tls_index;                /* -1 sem TLS */
  const struct wine_builtin* builtin;
};

#define WATEXIT_MAX 64
#define WFILE_MAX 32

struct wproc {
  uint8_t peb[PEB_SIZE];        /* TEM de ser o primeiro campo */
  uint8_t* teb;                 /* thread principal */
  struct wmod* mods;
  struct wmod* exe;
  int argc;
  char** argv;
  wchar16** wargv;
  char* cmdline;
  wchar16* wcmdline;
  char** environ;
  wchar16** wenviron;
  int tls_next;
  void (WINAPI* atexit_fn[WATEXIT_MAX])(void);
  int natexit;
  int crt_errno;
  int commode, fmode;
  int exiting;
};

/* Processo corrente: PEB = gs:0x60. */
static inline struct wproc* wp(void) {
  struct wproc* p;
  __asm__ volatile("mov %%gs:0x60, %0" : "=r"(p));
  return p;
}
static inline uint8_t* wteb(void) {
  uint8_t* t;
  __asm__ volatile("mov %%gs:0x30, %0" : "=r"(t));
  return t;
}
static inline void wset_lasterr(uint32_t e) { *(uint32_t*)(wteb() + TEB_LASTERR) = e; }
static inline uint32_t wget_lasterr(void) { return *(uint32_t*)(wteb() + TEB_LASTERR); }

/* ---------- pe.c ---------- */
/* Carrega um EXE ou DLL (PE32+) com as dependencias. 0 se falhou (ja disse). */
struct wmod* pe_load(const char* name, int is_exe);
/* Endereco exportado (nome ou, se name < 0x10000, ordinal). 0 se nao tem. */
void* pe_getproc(struct wmod* m, const char* name);
/* HMODULE (base da imagem ou wmod embutida) -> wmod; 0 se invalido. */
struct wmod* pe_from_handle(void* h);
void* pe_handle(struct wmod* m);
/* Ja carregado (nome sem pasta, com ou sem .dll). */
struct wmod* pe_find(const char* name);
/* TLS do modulo para a thread (teb) e callbacks com reason. */
void pe_tls_attach(struct wmod* m, uint8_t* teb);
void pe_call_tls_callbacks(struct wmod* m, uint32_t reason);
void pe_detach_all(void);

/* ---------- tabelas embutidas (k32.c, crt.c) ---------- */
extern const struct wine_builtin wine_kernel32;
extern const struct wine_builtin wine_ntdll;
extern const struct wine_builtin wine_ucrt;
extern const struct wine_builtin wine_user32;
const struct wine_builtin* builtin_for(const char* lname);

/* ---------- util.c ---------- */
void wlog(const char* a, const char* b);           /* serial */
void wsay(const char* a, const char* b);           /* console + serial */
void wcon_write(const char* s, size_t n);          /* console (tira o \r de \r\n) */
void wlower(char* d, const char* s, int max);
void wbase(char* d, const char* path, int max);    /* sem pasta, minusculas */
int wstreq(const char* a, const char* b);
size_t wstrlen16(const wchar16* s);
/* UTF-16 <-> UTF-8. Devolvem bytes/unidades escritos (sem o 0); n < 0 = ate o 0. */
int wutf16_to_8(const wchar16* s, int n, char* d, int max);
int wutf8_to_16(const char* s, int n, wchar16* d, int max);
char* wdup16to8(const wchar16* s);
wchar16* wdup8to16(const char* s);
void* wzalloc(size_t n);
__attribute__((noreturn)) void wexit(int code);    /* ExitProcess */

/* Chamado pelo stub de uma funcao que nao existe ("kernel32.dll!Foo"). */
void WINAPI wine_unimplemented(const char* what);
void* wine_make_stub(const char* dll, const char* name);
