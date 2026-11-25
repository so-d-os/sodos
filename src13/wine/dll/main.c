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
  /* PEB.LoaderLock: CRITICAL_SECTION sem dono (o Chromium confere OwningThread) */
  *(uint64_t*)(p->peb + 0x110) = (uint64_t)(uintptr_t)wzalloc(48);
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
  p->pid = wnew_pid();
  /* na tarefa do loader o k_getpid e o do processo; o registro de esperas
   * (util.c) usa este para todas as threads, criadas de onde forem */
  p->kpid = k_getpid();
  *(uint64_t*)(teb + TEB_PID) = p->pid;
  *(uint64_t*)(teb + TEB_TID) = 0x104;
  *(uint64_t*)(teb + TEB_TLSPTR) = (uint64_t)(uintptr_t)tlsarr;
  *(uint64_t*)(teb + TEB_PEB) = (uint64_t)(uintptr_t)p;
  *(uint64_t*)(p->peb + PEB_HEAP) = 1;   /* GetProcessHeap() */
  p->teb = teb;
  p->next_tid = 0x108;
  k_win_teb(teb);   /* daqui em diante gs:0x30/0x60 sao deste processo */
  wine_debug = k_getenv("WINEDEBUG", dbg, (int)sizeof(dbg)) > 0 && dbg[0] != '0';
  if (wine_debug && dbg[0] >= '2' && dbg[0] <= '9') wine_debug = dbg[0] - '0';

  /* filho iniciado em paralelo pelo CreateProcess (argv[1] = --wine-spawn=N):
   * troca pela linha de comando que o pai registrou */
  int is_child = wspawn_claim(&argc, &argv);
  wimg_cache_proc(1); /* antes de qualquer wexit, que desconta */
  if (is_child < 0) wexit(1); /* pedido orfao: nao roda como programa de cima */
  /* SET WINE_ARGS=...: opcoes acrescentadas a linha de comando (ex.: as do
   * Chromium). SET WINE_ARGS_EXE=claude.exe limita a um programa. */
  {
    static char extra[4096], only[64]; /* a lista de opcoes do Chromium passa de 600 */
    static char* nargv[160];
    int ne = k_getenv("WINE_ARGS", extra, (int)sizeof(extra));
    /* o SET guarda ate 259 caracteres: WINE_ARGS2 e WINE_ARGS3 continuam */
    {
      static const char* const more[2] = {"WINE_ARGS2", "WINE_ARGS3"};
      int k;
      for (k = 0; k < 2 && ne > 0; k++) {
        int used = (int)strlen(extra), m;
        if (used + 2 >= (int)sizeof(extra)) break;
        extra[used] = ' ';
        m = k_getenv(more[k], extra + used + 1, (int)sizeof(extra) - used - 1);
        if (m <= 0) extra[used] = 0;
      }
    }
    /* so no processo de cima: o Chromium repassa aos filhos o que precisa e o
     * crashpad-handler recusa opcoes que nao conhece */
    int apply = ne > 0 && !is_child;
    if (apply && k_getenv("WINE_ARGS_EXE", only, (int)sizeof(only)) > 0 && only[0]) {
      const char* b = argv[0];
      const char* q;
      for (q = argv[0]; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
      if (strchr(only, '\\')) {
        /* "app\claude.exe": casa o final do caminho (o instalador D:\CLAUDE.EXE
         * tem o mesmo nome e recusava as opcoes do Chromium) */
        size_t la = strlen(argv[0]), lo = strlen(only);
        apply = la >= lo && wstreq(argv[0] + la - lo, only) &&
                (la == lo || argv[0][la - lo - 1] == '\\' || argv[0][la - lo - 1] == '/' ||
                 argv[0][la - lo - 1] == ':');
      } else {
        apply = wstreq(b, only);
      }
    }
    if (apply && argc < 32) {
      char* s = extra;
      int n = 0;
      for (i = 0; i < argc; i++) nargv[n++] = argv[i];
      while (*s && n < 159) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        nargv[n++] = s;
        while (*s && *s != ' ') s++;
      }
      nargv[n] = 0;
      argc = n;
      argv = nargv;
    }
  }
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
  if (!p->cwd[0]) k_pwd(p->cwd, (int)sizeof(p->cwd)); /* o filho ja traz o do CreateProcess */
  build_params(p);
  crt_set_process(p);
  wexplorer_stub();
  wlog("start ", p->cmdline);
  /* na janela do programa: o comando que o abriu, com os argumentos (as
   * janelas dos filhos do Claude ficavam vazias ou so com o erro, sem dizer
   * o que tinha sido pedido). Linhas enormes do Chromium sao cortadas. */
  if (p->cmdline) {
    size_t n = strlen(p->cmdline);
    wcon_write("> ", 2);
    wcon_write(p->cmdline, n > 700 ? 700 : n);
    if (n > 700) wcon_write(" ...", 4);
    wcon_write("\r\n", 2);
  }

  /* perfil salvo de outro boot (profsave.c, SET WINEAPPSAVE e WINEUDSAVE) */
  if (!is_child) wprof_restore(argc, argv);
  /* depois do restore: resolver o APPDATA cria Z:\WINEAPP e o restore achava
   * que o perfil ja existia neste boot */
  wenv_publish();
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
    wurl_world_service();   /* ShellExecute de URL: o navegador (proc.c) */
    wpick_world_service();  /* Escolher pasta: o Explorer (picker.c) */
    if (!is_child) wprof_save(0); /* login etc. para o proximo boot, a cada minuto */
    k_futex_wait(&p->exit_req, 0, 20);
  }
  if (!is_child) wprof_save(1);
  wtrack_proc_exit(p); /* registro de esperas: as threads deste processo saem */
  wcon_flush();
  wgui_proc_exit();
  wlogx("ExitProcess ", (uint32_t)p->exit_code);
  wfile_proc_exit();
  return p->exit_code;
}

__attribute__((noreturn)) void wexit(int code) {
  struct wproc* p = wp();
  int first = !__sync_lock_test_and_set(&p->exiting, 1);
  if (first && (uint32_t)code >= 0xC0000000u) {
    /* saida com codigo de erro (0xC0000409 do Claude Code sem mensagem):
     * quem chamou e a pilha */
    uint64_t here;
    wwait_note("saida com erro", __builtin_return_address(0), (uint32_t)code);
    wstack_note("saida com erro", &here);
  }
  if (first) {
    wspawn_exit(code);
    wgui_proc_exit(); /* user.c: HWNDs deste processo saem do registro */
    {
      void wmap_proc_exit(void); /* map.c */
      void wpipe_proc_exit(void); /* pipe.c */
      wmap_proc_exit();
      wpipe_proc_exit();
    }
    wimg_cache_proc(-1);
    if (!p->no_detach) pe_detach_all();
    p->exit_code = code;
    __sync_lock_test_and_set(&p->exit_req, 1);
    k_futex_wake(&p->exit_req, -1);
    wsignal();
  }
  /* A tarefa do loader devolve o codigo; as threads so param. */
  if (!wthread()) {
    void wpipe_proc_exit(void); /* pipe.c: de novo, as threads podem ter pedido leitura depois */
    wpipe_proc_exit();
    wfile_proc_exit();
    k_exit(code);
  }
  k_thread_exit(code);
  for (;;) {
  }
}

/* TerminateProcess do proprio processo, excecao nao tratada e fail fast: no
 * Windows o processo acaba sem DllMain(DLL_PROCESS_DETACH) (so o ExitProcess
 * roda o detach). O Chrome, depois de um CHECK, chamava TerminateProcess; o
 * detach do chrome.dll quebrado dava acesso invalido e o processo nao saia
 * (o .BAT ficava parado). */
__attribute__((noreturn)) void wterminate(int code) {
  wp()->no_detach = 1;
  wexit(code);
}

static const struct wine_ops ops = {WINE_ABI, start};

const struct wine_ops* wine_driver(void) { return &ops; }
