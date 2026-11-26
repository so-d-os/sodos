/*
 * wine_int.h - Estruturas internas do WINE.DLL.
 *
 * Todo o estado de um processo Windows fica em struct wproc, que comeca com o
 * PEB: o codigo acha o processo corrente por gs:0x60 (TEB->Peb). A base do GS
 * de uma thread Windows e o TEB dela (kernel/kthread.c). Assim dois EXEs do
 * Windows nao dividem nada alem do codigo do DLL.
 *
 * As funcoes chamadas pelo codigo do Windows usam a ABI da Microsoft
 * (WINAPI = ms_abi): rcx, rdx, r8, r9 + 32 bytes de sombra.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define WINAPI __attribute__((ms_abi))

typedef uint16_t wchar16;
typedef uint32_t DWORD;
typedef int BOOL;
typedef void* HANDLE;

/* ---------- TEB / PEB (layout x64 do Windows, so os campos usados) ---------- */
#define TEB_SIZE 0x2000u
#define TEB_STACKBASE 0x08    /* topo da pilha (o kernel usa TEB+0 e TEB+0x18) */
#define TEB_STACKLIMIT 0x10
#define TEB_SELF 0x30
#define TEB_PID 0x40
#define TEB_TID 0x48
#define TEB_TLSPTR 0x58
#define TEB_PEB 0x60
#define TEB_LASTERR 0x68
#define TEB_TLSSLOTS 0x1480   /* 64 ponteiros */
#define TEB_DEALLOC 0x1478    /* DeallocationStack */
#define TEB_OLE 0x1758        /* ReservedForOle: aponta o OleTlsData (o Chromium le direto) */
#define TEB_WTHREAD 0x1800    /* nosso: struct wobj da thread */
#define TEB_COINITS 0x1838    /* nosso: CoInitialize sem CoUninitialize nesta thread */
#define TEB_OLEDATA 0x1840    /* nosso: OleTlsData (64 bytes; flags do apartamento em +0x14) */
#define TEB_NOKILL 0x1900     /* nosso: travas globais seguras pela thread (o kernel le: KT_TEB_NOKILL) */
#define TLS_SLOTS 64

#define PEB_IMAGEBASE 0x10
#define PEB_LDR 0x18
#define PEB_PARAMS 0x20
#define PEB_HEAP 0x30
#define PEB_SIZE 0x800u

/* ---------- erros ---------- */
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_HANDLE_EOF 38
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_ALREADY_EXISTS 183
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_MORE_DATA 234
#define WAIT_TIMEOUT 258
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_INVALID_ADDRESS 487
#define ERROR_IO_PENDING 997
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* ---------- modulos ---------- */
#define WMOD_NAME 64

struct wine_sym {
  const char* name;
  void* fn;
};

struct wmod {
  struct wmod* next;
  char name[WMOD_NAME];         /* minusculas, sem pasta, com .dll/.exe */
  char path[160];
  uint8_t* base;                /* imagem PE (0 nas embutidas) */
  uint32_t size;
  uint64_t entry;
  int is_dll;
  int tls_index;                /* -1 sem TLS */
  int builtin;                  /* DLL do sistema: funcoes do WINE.DLL */
  uint32_t pdata_rva, pdata_size;
  uint32_t stack_reserve;
};

/* ---------- memoria virtual (vm.c) ---------- */
#define VM_PRIVATE 1
#define VM_IMAGE 2
#define VM_STACK 3
struct vmreg {
  struct vmreg* next;
  uint8_t* base;
  size_t size;
  void* raw;                    /* bloco do kernel */
  int high;                     /* 1 = k_hmalloc */
  int virt;                     /* 1 = memoria virtual do kernel (k_vm_*) */
  int kind;
  uint8_t* commit;              /* 1 bit por pagina; 0 = tudo commitado */
};

/* Arquivo temporario grande mantido em RAM (file.c). */
struct ramfile {
  struct ramfile* next;
  char* path;
  uint8_t* data;
  uint32_t size, cap;
  int users;                    /* handles de leitura abertos */
  int dead;                     /* apagado com handles abertos */
  struct wproc* owner;          /* processo dono da memoria */
};

/* ---------- objetos (obj.c) ---------- */
enum { OBJ_NONE, OBJ_EVENT, OBJ_THREAD, OBJ_MUTEX, OBJ_SEM, OBJ_TIMER, OBJ_IOCP, OBJ_FILE, OBJ_FIND,
       OBJ_KEY, OBJ_SOCKET, OBJ_MISC, OBJ_PROCESS, OBJ_SECTION, OBJ_PIPE };

struct iocp_ent {
  DWORD bytes;
  uintptr_t key;
  void* ov;
};

struct wobj {
  int type;
  int refs;
  /* evento / semaforo / mutex / timer */
  int manual, signaled;
  int skip_ok;              /* FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (pipes) */
  int count, max;
  uintptr_t owner;
  uint64_t due;                 /* timer: k_ms de disparo (64 bits) */
  int armed;
  int period;
  /* thread */
  int ktid;
  int exited;
  DWORD exit_code;
  DWORD tid;
  uint8_t* teb;
  struct vmreg* stack;
  size_t stack_commit;      /* parte da pilha com paginas (o topo) */
  void* start;
  void* param;
  volatile int suspend;
  int wslot;                    /* vaga no registro de esperas (util.c), 1.. */
  /* iocp */
  struct iocp_ent* q;
  int qhead, qn, qcap;
  /* arquivo */
  char* path;
  uint8_t* data;                /* conteudo em memoria */
  uint32_t size, cap;
  uint64_t pos;
  int writable, dirty, is_dir, append;
  struct ramfile* ram;          /* leitura de um arquivo em RAM (data nao e do handle) */
  DWORD acc;                    /* arquivo: direitos do handle (GrantedAccess); 0 = pelo writable */
  struct wobj* real;            /* copia somente leitura de um arquivo (DuplicateHandle sem
                                 * escrita): o objeto de verdade; wh_get devolve ele */
  /* generico */
  void* p1;
  void* p2;
  uint64_t u1, u2;
  void* watch_ov;               /* pasta: ReadDirectoryChangesW pendente */
};
int wdir_watch_cancel(struct wobj* f); /* fill.c: 1 se havia vigia pendente */

#define WHANDLE_MAX 4096 /* o renderer do Claude passa de 1000 (threads, secoes, eventos) */
#define WATEXIT_MAX 64
#define WVEH_MAX 16

struct wproc {
  uint8_t peb[PEB_SIZE];        /* TEM de ser o primeiro campo */
  DWORD pid;                    /* pid do Windows (unico entre os processos do WINE) */
  struct spawnrec* spawn_rec;   /* processo filho iniciado em paralelo (proc.c) */
  void* dwrite;                 /* estado do DirectWrite deste processo (dwrite.c) */
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
  uint8_t tls_used[TLS_SLOTS];  /* TlsAlloc */
  void (WINAPI* atexit_fn[WATEXIT_MAX])(void);
  int natexit;
  int commode, fmode;
  int exiting;
  int no_detach;                /* wterminate: sai sem DllMain(DLL_PROCESS_DETACH) */
  /* saida do processo: a tarefa principal do SODOS espera aqui */
  volatile int exit_req;
  int unh_reports;              /* exc.c: excecoes ja relatadas sem WINEDEBUG */
  int exit_code;
  /* sincronizacao */
  volatile int lock;            /* protege objetos, handles e regioes */
  volatile int gen;             /* muda a cada sinal: futex das esperas */
  struct wobj* handles[WHANDLE_MAX];
  DWORD next_tid;
  struct vmreg* regs;
  /* um bloco liberado ha pouco: o Go libera e pede de novo alinhado */
  struct vmreg* spare;
  void* veh[WVEH_MAX];
  void* vch[WVEH_MAX];
  void* unhandled_filter;
  int fault_armed;
  struct wobj* main_thread;
  char cwd[200];
  void* user;                   /* estado do user32/gdi32 (gui.c) */
  void* reg;                    /* registro em memoria (reg.c) */
  void* net;                    /* soquetes (net.c) */
  /* console: as threads do Windows nao escrevem direto (no WIN a saida de
   * uma k_thread de outro programa vai para a serial); a tarefa principal
   * escreve por elas (util.c) */
  char* cbuf;
  uint32_t cn, ccap;
  volatile int clk;
  volatile int in_req;          /* 1 = linha pedida, 2 = pronta; 3/4 = tecla */
  int in_len;
  char in_line[260];
  /* processo filho pedido a tarefa principal (proc.c) */
  volatile int spawn_req;
  char* spawn_cmd;
  struct wobj** spawn_inh;      /* handles herdados, tirados no CreateProcess */
  struct wobj* spawn_obj;
  /* ambiente do processo (bloco UTF-8 "NOME=valor\0...\0"): o que o pai passou
   * no CreateProcess (lpEnvironment) ou herdou dele; 0 = o conjunto fixo +
   * variaveis do SODOS (processo de cima) */
  char* envb;
  uint32_t envlen;
  char* spawn_env;              /* para o filho (k_malloc_shared) */
  char* spawn_dir;              /* lpCurrentDirectory do filho (k_malloc_shared) */
  HANDLE spawn_std[3];          /* STARTUPINFO hStdInput/Output/Error para o filho */
  HANDLE std[3];                /* GetStdHandle: 0 = console */
  uint32_t kpid;                /* pid do SODOS (k_getpid na tarefa do loader) */
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
static inline struct wobj* wthread(void) { return *(struct wobj**)(wteb() + TEB_WTHREAD); }

/* Travas globais do WINE.DLL (wlock, map_lock, spawn_lock...). Uma copia so
 * deste DLL serve todos os processos do Claude, entao essas palavras sao de
 * todos. O fim de um processo mata as threads dele em qualquer ponto; uma
 * thread morta com a trava na mao parava os outros processos para sempre (o
 * Claude na tela de carregamento logo depois de um renderer sair, 26/11/2026).
 * wnokill_up antes de pegar, wnokill_down depois de soltar: com o contador do
 * TEB > 0 o kernel (kernel/kthread.c, may_die) adia a morte da thread ate ela
 * soltar. Fora de thread do Windows (a tarefa principal, que nao e morta
 * assim) nao faz nada. Nao e atomico: so a propria thread grava, e o kernel
 * le na mesma CPU (tique) ou na propria thread. */
static inline volatile int* wnokill_word(void) {
  uint8_t* t = wteb();
  if (!t || *(uint8_t**)(t + TEB_SELF) != t) return 0;
  return (volatile int*)(t + TEB_NOKILL);
}
static inline void wnokill_up(void) {
  volatile int* n = wnokill_word();
  if (n) ++*n;
  __asm__ volatile("" ::: "memory"); /* antes de pegar a trava */
}
static inline void wnokill_down(void) {
  volatile int* n;
  __asm__ volatile("" ::: "memory"); /* depois de soltar a trava */
  n = wnokill_word();
  if (n && *n > 0) --*n;
}
/* Trava global simples (0 livre, 1 presa) com o contador acima. Ao ceder a
 * vez a thread fica sem o contador: sem a trava ela pode morrer ali. */
void k_thread_yield(void);
static inline void wglock(volatile int* l) {
  wnokill_up();
  while (__sync_lock_test_and_set(l, 1)) {
    wnokill_down();
    k_thread_yield();
    wnokill_up();
  }
}
static inline int wgtrylock(volatile int* l) {
  wnokill_up();
  if (!__sync_lock_test_and_set(l, 1)) return 1;
  wnokill_down();
  return 0;
}
static inline void wgunlock(volatile int* l) {
  __sync_lock_release(l);
  wnokill_down();
}

/* ---------- pe.c ---------- */
struct wmod* pe_load(const char* name, int is_exe);
void* pe_getproc(struct wmod* m, const char* name);
struct wmod* pe_from_handle(void* h);
void* pe_handle(struct wmod* m);
struct wmod* pe_find(const char* name);
struct wmod* pe_from_addr(uint64_t addr);
int pe_delay_has(struct wmod* m, const char* dll, const char* name); /* m importa dll!name com carga atrasada */
int pe_delay_dll(struct wmod* m, const char* dll);                   /* m importa algo de dll com carga atrasada */
int pe_delay_name_is(struct wmod* m, const char* p);                 /* p e o nome de DLL de um descritor atrasado de m */
struct wmod* pe_load_phantom(const char* name);                     /* DLL ausente: modulo embutido vazio */
void pe_tls_attach(struct wmod* m, uint8_t* teb);
void pe_tls_attach_all(uint8_t* teb);
void pe_call_tls_callbacks(struct wmod* m, uint32_t reason);
void pe_thread_notify(uint32_t reason);
void pe_detach_all(void);
/* Recurso (.rsrc) por tipo/nome/idioma: ponteiro e tamanho, 0 se nao tem. */
const uint8_t* pe_find_resource(struct wmod* m, uintptr_t type, uintptr_t name, uint32_t* size);

/* ---------- tabelas embutidas ---------- */
extern const struct wine_sym k32_syms[], crt_syms[], vm_syms[], obj_syms[], exc_syms[], file_syms[],
    sys_syms[], gui_syms[], reg_syms[], net_syms[], misc_syms[], crypt_syms[], winrt_syms[], proc_syms[], sig_syms[], compat_syms[], gdi_syms[], dlg_syms[], shell_syms[], uimisc_syms[], procmisc_syms[], sec_syms[], map_syms[], safearray_syms[], dwrite_syms[], nt_syms[], pipe_syms[], fill_syms[], picker_syms[];
/* pipe.c */
HANDLE wpipe_open(const wchar16* name, DWORD flags);
int wpipe_read(struct wobj* o, void* buf, DWORD len, DWORD* done, void* ov);
int wpipe_write(struct wobj* o, const void* buf, DWORD len, DWORD* done, void* ov);
void wpipe_close(struct wobj* o);
int wpipe_flush(HANDLE h); /* -1 = nao e pipe */
int wpipe_fileinfo(HANDLE h, int cls, void* buf, DWORD len);
int wpipe_mode(HANDLE h);
void wobj_unref(struct wobj* o); /* obj.c */
/* Handle que e ponteiro (GDI, menu, HSTRING...): so le o "magic" se o
 * endereco pode ser nosso. O Chromium passa valores invalidos de proposito
 * (ou por engano) e o Windows so devolve erro; aqui o 0xfffffffc00000000 de
 * um ponteiro limpo do V8 derrubava o processo dentro do WINE.DLL. */
static inline int wptr_ok(const void* p) {
  uintptr_t v = (uintptr_t)p;
  return v >= 0x10000 && v < 0x0000800000000000ull && !(v & 3);
}
void* k_malloc_shared(uint32_t n); /* memoria sem dono, vista por todos os processos */
void k_free_shared(void* p);
void* wshared_high_alloc(uint64_t n, uint64_t align); /* util.c: sem dono acima de 4 GB; 0 = use a arena baixa */
void wshared_free(void* p);                            /* solta wshared_high_alloc ou k_malloc_shared */
void* k_hmalloc(unsigned long long n, unsigned long long align); /* RAM acima de 4 GB (cota SET HIMEM) */
void k_hfree(void* p);
int wpipe_cancel(struct wobj* o, void* ov);
void* safearray_ordinal(unsigned ord); /* safearray.c */
const char* ordinal_name(const char* dll, unsigned ord); /* ordtab.c */
void* uimisc_ordinal(const char* dll, unsigned ord); /* uimisc.c */
int builtin_known(const char* lname);              /* nome sem .dll */
void* builtin_lookup(const char* name);
void* builtin_ordinal(const char* dll, unsigned ord);   /* compat.c */

/* ---------- vm.c ---------- */
struct vmreg* vm_reserve(size_t n, int kind, int committed);
void vm_register_image(uint8_t* base, size_t size);
struct vmreg* vm_find(const void* addr);
void* WINAPI wVirtualAlloc(void* addr, size_t n, DWORD type, DWORD prot);

/* ---------- obj.c ---------- */
void wlock(void);
void wunlock(void);
int wlock_state(uint32_t* kpid, uint32_t* tid);   /* [wine-dump]: 0 livre, 1/2 presa */
void wlock_pid_gone(uint32_t kpid);                /* solta a trava de quem acabou */
void wsignal(void);                               /* gen++ e acorda as esperas */
HANDLE wh_new(struct wobj* o);
HANDLE wh_self_process(void); /* obj.c: handle real do proprio processo */
int wh_is_self(HANDLE h);
/* proc.c: processos filhos em paralelo (tarefa do WIN) */
int wspawn_claim(int* argc, char*** argv);
char* wenv_snapshot(uint32_t* len);
void wenv_publish(void); /* k32.c: ProcessParameters->Environment no PEB */
char* wenv_snapshot(uint32_t* len); /* k32.c: bloco do ambiente atual (k_malloc_shared) */   /* no inicio do filho */
void wspawn_exit(int code);                   /* na saida de qualquer processo */
DWORD wnew_pid(void);
void wimg_cache_proc(int delta); /* pe.c */
struct wobj* wh_get(HANDLE h, int type);           /* type 0 = qualquer */
struct wobj* wh_raw(HANDLE h);                      /* sem seguir a copia somente leitura */
DWORD wfile_access(DWORD access);                   /* GENERIC_* -> direitos de arquivo */
struct wobj* wobj_new(int type);
int wh_close(HANDLE h);
DWORD wwait(int n, struct wobj** objs, int all, DWORD timeout_ms);
struct wobj* wthread_create_main(void* entry, void* arg, uint32_t stack_reserve);
uint64_t wnow_ms(void);                            /* k_ms em 64 bits */
/* Chama fn(arg) (ms_abi) com a pilha em stack_top. */
uint64_t wine_call_on_stack(void* fn, void* arg, void* stack_top);

/* ---------- proc.c ---------- */
void wspawn_world_service(void);
/* ---------- file.c ---------- */
struct overlapped;
BOOL wfile_write(struct wobj* f, const void* buf, DWORD n, DWORD* done, struct overlapped* ov); /* file.c */
void wfile_proc_exit(void);                        /* solta os temporarios em RAM do processo */

/* ---------- exc.c ---------- */
void exc_arm(void);

/* ---------- file.c ---------- */
/* Caminho do Windows -> caminho do SODOS (D:\X). Devolve out. */
char* wpath(const char* in, char* out, int max);
char* wpath16(const wchar16* in, char* out, int max);

/* ---------- util.c ---------- */
void wlog(const char* a, const char* b);           /* serial */
void wlogx(const char* a, uint64_t v);             /* serial, com hexa */
void wsay(const char* a, const char* b);           /* console + serial */
void wcon_write(const char* s, size_t n);          /* console (tira o \r de \r\n) */
void wcon_flush(void);                             /* tarefa principal: escreve o que as threads deixaram */
int wcon_readline(char* buf, int max);
int wcon_getc(void);
void wlower(char* d, const char* s, int max);
void wbase(char* d, const char* path, int max);    /* sem pasta, minusculas */
int wstreq(const char* a, const char* b);
size_t wstrlen16(const wchar16* s);
int wutf16_to_8(const wchar16* s, int n, char* d, int max);
int wutf8_to_16(const char* s, int n, wchar16* d, int max);
char* wdup16to8(const wchar16* s);
wchar16* wdup8to16(const char* s);
void* wzalloc(size_t n);
/* heap do programa (util.c): blocos grandes acima de 4 GB */
void* wheap_alloc(size_t n);
void wprof_save(int force);   /* profsave.c: WINEAPPDATA -> WINEAPPSAVE, --user-data-dir -> WINEUDSAVE */
void wprof_restore(int argc, char** argv); /* na partida do processo de cima */
void* wheap_realloc(void* p, size_t n);
void wheap_free(void* p);
size_t wheap_size(void* p);
__attribute__((noreturn)) void wexit(int code);    /* ExitProcess, de qualquer thread */
__attribute__((noreturn)) void wterminate(int code); /* TerminateProcess: sem o detach das DLLs */

void WINAPI wine_unimplemented(const char* what);
void* wine_make_stub(const char* dll, const char* name);
void* wine_api_stub(const char* dll, const char* name); /* k32.c */
/* 1 = registra na serial cada GetProcAddress/import que faltou (SET WINEDEBUG=1). */
extern int wine_debug;
/* geracao global: objetos (eventos, processos, pipes) sao vistos por varios
 * processos do WINE; quem espera dorme aqui e confere o seu objeto ao acordar */
extern volatile int wine_gen;
/* baldes de espera (obj.c): quem espera um objeto dorme no balde dele */
#define WBUCKETS 64
int wbucket_of(const void* o);
void wsignal_obj(const void* o); /* mudou o estado de o */
void wsignal_global(void);       /* so mensagens/pipes sincronos (esperas globais) */
void wvm_note(uint64_t a); /* vm.c: estado da pagina (relatorio de excecao) */
void wgui_proc_exit(void); /* user.c: HWNDs do processo saem do registro global */
void wpick_world_service(void); /* picker.c: abre o Explorer no modo selecionar pasta */
int wpick_folder(const char* title, const char* init, char* out, int max);
int wpicker_create(const void* clsid, void** out); /* CLSID_FileOpenDialog */
void wurl_world_service(void); /* proc.c: abre o navegador pedido por ShellExecute */
void wwait_note(const char* what, void* caller, uint64_t waited_ms); /* util.c */
/* Registro de esperas (util.c): cada thread do Windows tem uma vaga com o que
 * esta esperando agora; o REBOOT do SODOS chama wine_dump e tudo vai para a
 * serial ("[wine-dump]"), para achar travamentos sem depurador. */
void wtrack_thread(struct wobj* t);
void wtrack_exit(struct wobj* t);
void wtrack_proc_exit(struct wproc* p);
uint32_t k_getpid(void);              /* kernel: pid do SODOS do processo */
void wine_pid_gone(uint32_t kpid);     /* kernel: processo pid acabou (util.c) */
void wwait_begin(const char* what, const void* obj, uint64_t owner_teb, const char* detail, void* caller);
void wwait_begin_w(const char* what, const void* obj, const wchar16* detail, void* caller);
void wwait_end(void);
void wstack_note(const char* what, const uint64_t* sp);
