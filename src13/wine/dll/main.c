/*
 * main.c - Entrada do WINE.DLL: monta o processo Windows e chama o EXE.
 *
 * O kernel (kernel/wine.c) so chega aqui com SET WINE=1 e um EXE do Windows.
 * start() roda na tarefa que o shell criou para o programa: carrega o EXE,
 * cria a thread principal do Windows (k_thread com TEB e pilha proprios) e
 * espera o ExitProcess, que pode vir de qualquer thread. Tudo que se aloca e
 * do programa e some quando start() devolve.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "../../kernel/wine.h"

void k_win_teb(void* teb);
void wgui_world_service(void);
void crt_set_process(struct wproc* p);
void wexplorer_stub(void);

int wine_debug;

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

/* RTL_USER_PROCESS_PARAMETERS: so o que programas leem direto do PEB. */
static void build_params(struct wproc* p) {
  uint8_t* pp = (uint8_t*)wzalloc(0x400);
  wchar16* img = wdup8to16(p->argv[0]);
  if (!pp || !img) return;
  *(uint32_t*)(pp + 0) = 0x400;
  *(uint32_t*)(pp + 4) = 0x400;
  *(uint32_t*)(pp + 8) = 1; /* normalizado */
  *(uint64_t*)(pp + 0x20) = 0x10;
  *(uint64_t*)(pp + 0x28) = 0x14;
  *(uint64_t*)(pp + 0x30) = 0x18;
  /* ImagePathName 0x60, CommandLine 0x70 (UNICODE_STRING: len, max, ptr) */
  *(uint16_t*)(pp + 0x60) = (uint16_t)(wstrlen16(img) * 2);
  *(uint16_t*)(pp + 0x62) = (uint16_t)(wstrlen16(img) * 2 + 2);
  *(uint64_t*)(pp + 0x68) = (uint64_t)(uintptr_t)img;
  *(uint16_t*)(pp + 0x70) = (uint16_t)(wstrlen16(p->wcmdline) * 2);
  *(uint16_t*)(pp + 0x72) = (uint16_t)(wstrlen16(p->wcmdline) * 2 + 2);
  *(uint64_t*)(pp + 0x78) = (uint64_t)(uintptr_t)p->wcmdline;
  *(uint64_t*)(p->peb + PEB_PARAMS) = (uint64_t)(uintptr_t)pp;
  /* OSMajorVersion 0x118, Minor 0x11C, Build 0x120 (Windows 10 22H2) */
  *(uint32_t*)(p->peb + 0x118) = 10;
  *(uint32_t*)(p->peb + 0x11C) = 0;
  *(uint16_t*)(p->peb + 0x120) = 19045;
  *(uint32_t*)(p->peb + 0x124) = 2; /* VER_PLATFORM_WIN32_NT */
  *(uint32_t*)(p->peb + 0xB8) = 1;  /* NumberOfProcessors */
}

/* Thread principal do Windows: callbacks de TLS do EXE e o ponto de entrada. */
static WINAPI DWORD main_thread(void* arg) {
  struct wproc* p = wp();
  typedef int (WINAPI * entry_fn)(void*);
  (void)arg;
  wexit(((entry_fn)(uintptr_t)p->exe->entry)(p->peb));
}

static int start(int argc, char** argv) {
  struct wproc* p;
  uint8_t* teb;
  void** tlsarr;
  char dbg[8];
  int i;
  struct wmod* exe;

  if (argc < 1 || !argv || !argv[0]) return 1;
  p = (struct wproc*)wzalloc(sizeof(*p));
  teb = (uint8_t*)k_malloc_aligned(TEB_SIZE, 64);
  tlsarr = (void**)wzalloc(sizeof(void*) * TLS_SLOTS);
  if (!p || !teb || !tlsarr) {
    k_puts("wine: sem memoria\n");
    return 1;
  }
  /* TEB de carga: desta tarefa (loader), nao da thread principal do EXE. */
  memset(teb, 0, TEB_SIZE);
  *(uint64_t*)(teb + TEB_SELF) = (uint64_t)(uintptr_t)teb;
  *(uint64_t*)(teb + TEB_PID) = 0x100;
  *(uint64_t*)(teb + TEB_TID) = 0x104;
  *(uint64_t*)(teb + TEB_TLSPTR) = (uint64_t)(uintptr_t)tlsarr;
  *(uint64_t*)(teb + TEB_PEB) = (uint64_t)(uintptr_t)p;
  *(uint64_t*)(p->peb + PEB_HEAP) = 1;   /* GetProcessHeap() */
  p->teb = teb;
  p->next_tid = 0x108;
  k_win_teb(teb);   /* daqui em diante gs:0x30/0x60 sao deste processo */
  wine_debug = k_getenv("WINEDEBUG", dbg, (int)sizeof(dbg)) > 0 && dbg[0] != '0';

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
  k_pwd(p->cwd, (int)sizeof(p->cwd));
  build_params(p);
  crt_set_process(p);
  wexplorer_stub();
  wlog("start ", p->cmdline);

  exe = pe_load(argv[0], 1);
  if (!exe) return 1;
  if (!exe->entry) {
    wsay(exe->name, ": sem ponto de entrada");
    return 1;
  }
  exc_arm();
  if (!wthread_create_main((void*)main_thread, 0, exe->stack_reserve)) {
    k_puts("wine: nao consegui criar a thread principal\n");
    return 1;
  }
  /* Espera o ExitProcess. O futex cede a CPU ao resto do SODOS. */
  while (!p->exit_req) {
    wgui_world_service();   /* abrir/fechar a janela so esta tarefa pode */
    wcon_flush();           /* console das threads */
    wspawn_world_service(); /* processo filho: k_exec so daqui */
    k_futex_wait(&p->exit_req, 0, 20);
  }
  wcon_flush();
  wlogx("ExitProcess ", (uint32_t)p->exit_code);
  wfile_proc_exit();
  return p->exit_code;
}

__attribute__((noreturn)) void wexit(int code) {
  struct wproc* p = wp();
  int first = !__sync_lock_test_and_set(&p->exiting, 1);
  if (first) {
    pe_detach_all();
    p->exit_code = code;
    __sync_lock_test_and_set(&p->exit_req, 1);
    k_futex_wake(&p->exit_req, -1);
    wsignal();
  }
  /* A tarefa do loader devolve o codigo; as threads so param. */
  if (!wthread()) {
    wfile_proc_exit();
    k_exit(code);
  }
  k_thread_exit(code);
  for (;;) {
  }
}

static const struct wine_ops ops = {WINE_ABI, start};

const struct wine_ops* wine_driver(void) { return &ops; }
