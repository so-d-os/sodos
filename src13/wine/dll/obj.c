/*
 * obj.c - Handles, objetos de espera, threads, timers e portas de conclusao.
 *
 * Esperas: toda mudanca de estado (SetEvent, fim de thread, PostQueued...)
 * incrementa wproc.gen e acorda quem dorme no futex dessa palavra; quem
 * espera reconfere os seus objetos. Simples e sem perda de sinal.
 *
 * Threads: cada thread do Windows e uma k_thread com TEB proprio (base do GS)
 * e pilha propria registrada no vm.c (o Go acha os limites da pilha por
 * VirtualQuery). A thread principal do EXE tambem e uma k_thread: a tarefa do
 * SODOS que o shell criou fica so esperando o ExitProcess, que pode vir de
 * qualquer thread.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

void k_win_teb(void* teb);

/* uint64_t wine_call_on_stack(fn, arg, stack_top): fn(arg) na ABI MS. */
__asm__(
    ".text\n"
    ".globl wine_call_on_stack\n"
    ".type wine_call_on_stack,@function\n"
    "wine_call_on_stack:\n"
    "  pushq %rbp\n"
    "  pushq %rbx\n"
    "  movq %rsp, %rbx\n"
    "  movq %rdx, %rsp\n"
    "  andq $-16, %rsp\n"
    "  subq $32, %rsp\n"
    "  movq %rsi, %rcx\n"
    "  callq *%rdi\n"
    "  movq %rbx, %rsp\n"
    "  popq %rbx\n"
    "  popq %rbp\n"
    "  ret\n"
    ".size wine_call_on_stack, .-wine_call_on_stack\n");

uint64_t wnow_ms(void) { return k_ms(); }

/* ---------- trava do WINE ----------
 * Uma so para todos os processos do Windows: objetos (eventos, portas IOCP,
 * pipes) sao vistos por varios processos ao mesmo tempo. */
/* Trava global do WINE.DLL (todos os processos). 0 livre, 1 presa, 2 presa
 * com alguem dormindo. Gira um pouco e depois dorme no futex: com os 4
 * processos do Chromium a versao que so girava (com yield) ocupava 3 CPUs
 * esperando a trava e deixava o dono sem vez. */
/* wnokill_up/down (wine_int.h): o fim de um processo nao mata a thread que
 * esta com a trava (senao ela ficava presa e os outros processos paravam).
 * Dono (pid do SODOS e tid do Windows) para o [wine-dump] e para soltar a
 * trava de um processo que acabou com ela (wlock_pid_gone). */
static volatile int wine_lock;
static volatile uint32_t wine_lock_kpid, wine_lock_tid;
static inline void wlock_owner(void) {
  uint8_t* t = wteb();
  if (t && *(uint8_t**)(t + TEB_SELF) == t) { /* thread ou tarefa do loader (TEB de carga) */
    struct wproc* p = *(struct wproc**)(t + TEB_PEB);
    wine_lock_kpid = p ? p->kpid : 0;
    wine_lock_tid = (uint32_t)*(uint64_t*)(t + TEB_TID);
  } else {
    wine_lock_kpid = 0;
    wine_lock_tid = 0;
  }
}
void wlock(void) {
  int c, spin;
  wnokill_up();
  for (spin = 0; spin < 100; spin++) {
    if (__sync_val_compare_and_swap(&wine_lock, 0, 1) == 0) {
      wlock_owner();
      return;
    }
    __asm__ volatile("pause");
  }
  c = __sync_lock_test_and_set(&wine_lock, 2);
  while (c != 0) {
    wnokill_down(); /* dormindo sem a trava a thread pode morrer */
    k_futex_wait(&wine_lock, 2, 10);
    wnokill_up();
    c = __sync_lock_test_and_set(&wine_lock, 2);
  }
  wlock_owner();
}
void wunlock(void) {
  wine_lock_kpid = 0;
  wine_lock_tid = 0;
  if (__sync_fetch_and_sub(&wine_lock, 1) != 1) { /* era 2: alguem dorme */
    wine_lock = 0;
    __sync_synchronize();
    k_futex_wake(&wine_lock, 1);
  }
  wnokill_down();
}

/* [wine-dump]: valor da trava e dono (0 = livre). */
int wlock_state(uint32_t* kpid, uint32_t* tid) {
  if (kpid) *kpid = wine_lock_kpid;
  if (tid) *tid = wine_lock_tid;
  return wine_lock;
}

/* Fim do processo kpid (wine_pid_gone, depois que o kernel parou as threads
 * dele): se a trava ficou com ele, ninguem mais vai solta-la. Nao deveria
 * acontecer com o wnokill; se acontecer, o log diz e a trava volta. Quem
 * dorme nela acorda sozinho pelo prazo de 10 ms do wlock. */
void wlock_pid_gone(uint32_t kpid) {
  if (!kpid || !wine_lock || wine_lock_kpid != kpid) return;
  wlogx("trava global presa por um processo que acabou; solta. tid ", wine_lock_tid);
  wine_lock_kpid = 0;
  wine_lock_tid = 0;
  __sync_lock_release(&wine_lock);
}

/* ---------- acordar quem espera ----------
 * wine_gen: geracao global. Antes TODO sinal (SetEvent, post numa porta,
 * escrita num pipe) acordava todas as threads de todos os processos; com o
 * Claude (5 processos, ~150 threads, a maioria ociosa num evento ou numa porta
 * IOCP) cada mensagem do Mojo virava uma manada disputando wlock().
 *
 * Agora quem espera UM objeto (ou varios que caem no mesmo balde) dorme no
 * balde do endereco dele; wsignal_obj(o) acorda so esse balde e o global.
 * No global ficam as esperas de mensagens (user.c), pipes sincronos e
 * WaitForMultipleObjects com objetos em baldes diferentes.
 *
 * A geracao do balde sobe sempre; waiters so poupa a chamada de acordar.
 * Quem espera soma waiters ANTES de ler a geracao e quem sinaliza sobe a
 * geracao ANTES de ler waiters (operacoes __sync, barreira total): um dos
 * dois ve o outro, entao nao se perde acordar. */
volatile int wine_gen;
struct wbucket {
  volatile int gen;
  volatile int waiters;
  int pad[14]; /* uma linha de cache por balde */
};
static struct wbucket wbk[WBUCKETS];
int wbucket_of(const void* o) {
  uint64_t v = (uint64_t)(uintptr_t)o >> 4;
  return (int)((v * 0x9E3779B97F4A7C15ull) >> 58) & (WBUCKETS - 1);
}
static void wake_global(void) {
  __sync_fetch_and_add(&wine_gen, 1);
  k_futex_wake(&wine_gen, -1);
}
static void wake_bucket(int b) {
  __sync_fetch_and_add(&wbk[b].gen, 1);
  if (__sync_fetch_and_add(&wbk[b].waiters, 0)) k_futex_wake(&wbk[b].gen, -1);
}
/* sinal sem objeto conhecido: todos (global e baldes com alguem) */
void wsignal(void) {
  int b;
  wake_global();
  for (b = 0; b < WBUCKETS; b++) wake_bucket(b);
}
/* mudou o estado de o: quem espera o (balde) e as esperas globais */
void wsignal_obj(const void* o) {
  wake_bucket(wbucket_of(o));
  wake_global();
}
/* so as esperas globais (mensagens, pipes sincronos): ninguem num balde
 * espera por isso */
void wsignal_global(void) { wake_global(); }
/* espera: b < 0 = global. wenter antes de ler a geracao, wleave depois. */
static void wenter(int b) {
  if (b >= 0) __sync_fetch_and_add(&wbk[b].waiters, 1);
}
static void wleave(int b) {
  if (b >= 0) __sync_fetch_and_sub(&wbk[b].waiters, 1);
}
static volatile int* wword(int b) { return b >= 0 ? &wbk[b].gen : &wine_gen; }

/* ---------- handles ---------- */
#define H_BASE 0x200u
/* Objetos em memoria sem dono: um handle herdado pode viver no filho depois
 * que o pai sai (o kernel solta a memoria de um processo quando ele termina).
 * k_free solta os dois tipos. */
struct wobj* wobj_new(int type) {
  struct wobj* o = (struct wobj*)k_malloc_shared((uint32_t)sizeof(*o));
  if (!o) {
    unsigned total = 0, freeb = 0;
    k_meminfo(&total, &freeb);
    wlogx("wobj_new: sem memoria; livre (KB) ", (uint64_t)(freeb >> 10));
  }
  if (o) {
    memset(o, 0, sizeof(*o));
    o->type = type;
    o->refs = 0;
  }
  return o;
}

HANDLE wh_new(struct wobj* o) {
  struct wproc* p = wp();
  int i;
  if (!o) return 0;
  wlock();
  for (i = 1; i < WHANDLE_MAX; i++)
    if (!p->handles[i]) {
      p->handles[i] = o;
      o->refs++;
      wunlock();
      return (HANDLE)(uintptr_t)(H_BASE + (unsigned)i * 4u);
    }
  wunlock();
  wlog("handles esgotados", 0);
  {
    /* uma vez: quantos de cada tipo, para achar o vazamento */
    static int dumped;
    if (!dumped) {
      unsigned cnt[16] = {0};
      dumped = 1;
      for (i = 1; i < WHANDLE_MAX; i++)
        if (p->handles[i]) cnt[(unsigned)p->handles[i]->type & 15]++;
      for (i = 0; i < 16; i++)
        if (cnt[i]) {
          wlogx("  handles do tipo ", (uint64_t)i);
          wlogx("    quantos ", (uint64_t)cnt[i]);
        }
    }
  }
  return 0;
}

struct wobj* wh_raw(HANDLE h) {
  uintptr_t v = (uintptr_t)h;
  if (h == (HANDLE)(intptr_t)-2) return wthread();
  if (v < H_BASE || (v & 3) || (v - H_BASE) / 4 >= WHANDLE_MAX) return 0;
  return wp()->handles[(v - H_BASE) / 4];
}
struct wobj* wh_get(HANDLE h, int type) {
  struct wobj* o = wh_raw(h);
  if (o && o->real) o = o->real; /* copia somente leitura: o arquivo de verdade */
  if (!o || (type && o->type != type)) return 0;
  return o;
}

/* Direitos de um handle de arquivo, como o NtQueryObject do Windows mostra:
 * GENERIC_* viram FILE_GENERIC_*, e o CreateFile sempre soma SYNCHRONIZE e
 * FILE_READ_ATTRIBUTES (por isso nunca da 0). */
DWORD wfile_access(DWORD a) {
  DWORD r = a & 0x00FFFFFFu & ~0x02000000u; /* sem MAXIMUM_ALLOWED */
  if (a & 0x80000000u) r |= 0x00120089u;     /* GENERIC_READ */
  if (a & 0x40000000u) r |= 0x00120116u;     /* GENERIC_WRITE */
  if (a & 0x20000000u) r |= 0x001200A0u;     /* GENERIC_EXECUTE */
  if (a & 0x10000000u) r |= 0x001F01FFu;     /* GENERIC_ALL */
  return r | 0x00100080u;
}
/* escrita, apagar, mudar dono ou ACL: o Chromium (ReadOnlyFile do Mojo)
 * confere que um arquivo "somente leitura" nao tem nenhum destes */
#define FILE_WRITE_RIGHTS (0x40000000u | 0x10000000u | 0x000D0116u)

/* Outro handle, sem escrita, para o arquivo o (DuplicateHandle sem
 * FILE_WRITE_DATA): um objeto pequeno que aponta o de verdade, porque os
 * direitos sao do handle e os dados, a posicao e o buffer sao do arquivo. */
static struct wobj* file_ro_alias(struct wobj* o, DWORD acc) {
  struct wobj* a = wobj_new(OBJ_FILE);
  if (!a) return 0;
  a->real = o;
  a->acc = wfile_access(acc) & ~0x000D0116u;
  wlock();
  o->refs++;
  wunlock();
  return a;
}

void wfile_close(struct wobj* o);   /* file.c */
void wsock_close(struct wobj* o);   /* net.c */
void wsection_close(struct wobj* o); /* map.c */

int wh_close(HANDLE h) {
  struct wproc* p = wp();
  uintptr_t v = (uintptr_t)h;
  struct wobj* o;
  if (v < H_BASE || (v & 3) || (v - H_BASE) / 4 >= WHANDLE_MAX) return 0;
  wlock();
  o = p->handles[(v - H_BASE) / 4];
  if (!o) {
    wunlock();
    return 0;
  }
  p->handles[(v - H_BASE) / 4] = 0;
  wunlock();
  wobj_unref(o);
  return 1;
}

/* Fim do processo: o Windows fecha todos os handles dele. Aqui os pipes, cujo
 * fim o outro lado espera (EOF/ERROR_BROKEN_PIPE): um filho que herdou a ponta
 * de escrita do stdout do pai (o Bun herda tudo) segurava o pipe aberto depois
 * de sair, e o pai nunca via o fim da saida (o ripgrep do Claude Code dava
 * "timed out after 20 seconds" em todo Glob/Grep). Os outros tipos ficam como
 * estavam (arquivos, secoes e eventos tem regras proprias no fim). */
void wh_proc_exit_pipes(void) {
  struct wproc* p = wp();
  int i, n = 0;
  for (i = 1; i < WHANDLE_MAX; i++) {
    struct wobj* o;
    wlock();
    o = p->handles[i];
    if (o && o->type == OBJ_PIPE) p->handles[i] = 0;
    else o = 0;
    wunlock();
    if (o) {
      wobj_unref(o);
      n++;
    }
  }
  if (n && wine_debug) wlogx("fim do processo: pipes fechados ", (uint64_t)n);
}

/* Solta uma referencia; a ultima fecha o objeto. */
void wobj_unref(struct wobj* o) {
  wlock();
  if (--o->refs > 0) o = 0;
  wunlock();
  if (o && o->real) { /* copia somente leitura: solta a do arquivo de verdade */
    struct wobj* r = o->real;
    k_free(o);
    wobj_unref(r);
    return;
  }
  if (o) {
    if (o->type == OBJ_FILE || o->type == OBJ_FIND) wfile_close(o);
    if (o->type == OBJ_SOCKET) wsock_close(o);
    if (o->type == OBJ_SECTION) wsection_close(o);
    if (o->type == OBJ_PIPE) wpipe_close(o);
    /* threads ficam: a propria thread ainda usa o objeto */
    if (o->type != OBJ_THREAD) {
      if (o->q) k_free(o->q);
      k_free(o);
    }
  }
}

static WINAPI BOOL CloseHandle(HANDLE h) {
  uintptr_t v = (uintptr_t)h;
  if (v == 0x10 || v == 0x14 || v == 0x18) return 1;
  if (!wh_close(h)) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  return 1;
}

/* Handle real do proprio processo (OpenProcess do proprio pid, DuplicateHandle
 * de GetCurrentProcess()): no Windows nao e o pseudo-handle -1, e o Chromium
 * confere isso (CHECK no ScopedHandle). Nunca sinaliza, STILL_ACTIVE. */
#define SELF_MARK 0x464C4553u /* "SELF" */
HANDLE wh_self_process(void) {
  struct wobj* o = wobj_new(OBJ_PROCESS);
  HANDLE h;
  if (!o) return 0;
  o->u1 = SELF_MARK;
  h = wh_new(o);
  if (!h) k_free(o);
  return h;
}
int wh_is_self(HANDLE h) {
  struct wobj* o;
  if (h == (HANDLE)(intptr_t)-1) return 1;
  o = wh_get(h, OBJ_PROCESS);
  return o && o->u1 == SELF_MARK;
}

static WINAPI BOOL DuplicateHandle(HANDLE sp, HANDLE h, HANDLE dp, HANDLE* out, DWORD acc, BOOL inh, DWORD opt);
/* ReOpenFile: outro handle para o mesmo arquivo (o Bun reabre stdio e logs).
 * O objeto e um so; sem escrita em access vira copia somente leitura. Pedir
 * escrita reabre o arquivo de verdade, como no Windows (abre pelo nome). */
static WINAPI HANDLE ReOpenFile(HANDLE h, DWORD access, DWORD share, DWORD flags) {
  HANDLE out = 0;
  struct wobj* o = wh_raw(h);
  (void)share; (void)flags;
  if (o && o->real && (access & FILE_WRITE_RIGHTS)) {
    out = wh_new(o->real);
    return out ? out : (HANDLE)(intptr_t)-1;
  }
  if (!DuplicateHandle((HANDLE)(intptr_t)-1, h, (HANDLE)(intptr_t)-1, &out, access, 0,
                       o && o->type == OBJ_FILE ? 0 : 2 /* SAME_ACCESS */)) {
    if (!wget_lasterr()) wset_lasterr(6);
    return (HANDLE)(intptr_t)-1;
  }
  return out;
}
/* copia somente leitura que nao chegou a nenhum handle */
static void alias_drop(struct wobj* a) {
  struct wobj* r;
  if (!a || a->refs) return;
  r = a->real;
  k_free(a);
  wobj_unref(r);
}
static WINAPI BOOL DuplicateHandle(HANDLE sp, HANDLE h, HANDLE dp, HANDLE* out, DWORD acc, BOOL inh, DWORD opt) {
  struct wobj* o;
  struct wobj* ro = 0; /* copia somente leitura feita aqui */
  uintptr_t v = (uintptr_t)h;
  if (sp && sp != (HANDLE)(intptr_t)-1 && !wh_is_self(sp) && wh_get(sp, OBJ_PROCESS)) {
    /* de outro processo (um filho) para este: o handle e da tabela dele */
    struct wobj* wdup_from_child(struct wobj* proc, HANDLE h, int close); /* proc.c */
    struct wobj* got = wdup_from_child(wh_get(sp, OBJ_PROCESS), h, opt & 1);
    if (wine_debug >= 2) wlogx("DuplicateHandle de outro processo: handle ", (uint64_t)v);
    if (!got) {
      wset_lasterr(ERROR_INVALID_HANDLE);
      return 0;
    }
    if (dp && dp != (HANDLE)(intptr_t)-1 && !wh_is_self(dp)) { /* de um filho para outro: nao */
      wobj_unref(got);
      wset_lasterr(ERROR_INVALID_HANDLE);
      return 0;
    }
    if (!out) {
      wobj_unref(got);
      return 1;
    }
    {
      HANDLE nh = wh_new(got); /* wh_new soma uma referencia: tira a que veio */
      wobj_unref(got);
      if (!nh) return 0;
      *out = nh;
    }
    return 1;
  }
  o = wh_raw(h);
  if (o && o->type == OBJ_FILE && !(opt & 2) && !(acc & 0x02000000u)) {
    /* arquivo sem DUPLICATE_SAME_ACCESS: o novo handle tem so os direitos
     * de acc. O Chromium duplica sem escrita e confere no NtQueryObject. */
    if (acc & FILE_WRITE_RIGHTS) {
      if (o->real) { /* de somente leitura nao se tira escrita */
        wset_lasterr(5); /* ERROR_ACCESS_DENIED */
        return 0;
      }
    } else if (!o->real) {
      ro = file_ro_alias(o, acc);
      if (!ro) {
        wset_lasterr(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
      }
      o = ro;
    }
  }
  if (o && dp && dp != (HANDLE)(intptr_t)-1 && !wh_is_self(dp)) {
    /* para outro processo: so os filhos que este processo abriu */
    HANDLE wdup_to_child(struct wobj* proc, struct wobj* o); /* proc.c */
    struct wobj* po = wh_get(dp, OBJ_PROCESS);
    HANDLE got = po ? wdup_to_child(po, o) : 0;
    if (wine_debug >= 2) {
      wlogx("DuplicateHandle para outro processo: handle ", (uint64_t)v);
      wlogx("  no outro processo vira ", (uint64_t)(uintptr_t)got);
    }
    if (!got) {
      alias_drop(ro);
      wset_lasterr(ERROR_INVALID_HANDLE);
      return 0;
    }
    if (out) *out = got;
    if (opt & 1) wh_close(h); /* DUPLICATE_CLOSE_SOURCE */
    return 1;
  }
  (void)inh;
  if (!o) {
    if (h == (HANDLE)(intptr_t)-1) { /* GetCurrentProcess() -> handle real */
      if (out) *out = wh_self_process();
      return !out || *out != 0;
    }
    if (h == (HANDLE)(intptr_t)-2 && wthread()) { /* GetCurrentThread() -> handle real */
      if (out) *out = wh_new(wthread());
      return !out || *out != 0;
    }
    if (v == 0x10 || v == 0x14 || v == 0x18) {
      if (out) *out = h;
      return 1;
    }
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (o->type == OBJ_SECTION && (o->u1 & 1) && !(opt & 2) && (acc & 2)) {
    /* de somente leitura nao se tira escrita (o Chromium testa isso) */
    wset_lasterr(5); /* ERROR_ACCESS_DENIED */
    return 0;
  }
  if (o->type == OBJ_SECTION && out) {
    /* sem DUPLICATE_SAME_ACCESS e sem FILE_MAP_WRITE: copia somente leitura */
    HANDLE wsection_dup(struct wobj* o, int readonly); /* map.c */
    *out = wsection_dup(o, !(opt & 2) && !(acc & 2));
  } else if (out) {
    *out = wh_new(o);
    if (!*out) {
      alias_drop(ro);
      return 0;
    }
  } else {
    alias_drop(ro);
  }
  if (opt & 1) wh_close(h); /* DUPLICATE_CLOSE_SOURCE */
  return 1;
}

static WINAPI BOOL GetHandleInformation(HANDLE h, DWORD* fl) {
  (void)h;
  if (fl) *fl = 0;
  return 1;
}
static WINAPI BOOL SetHandleInformation(HANDLE h, DWORD m, DWORD f) {
  (void)h;
  (void)m;
  (void)f;
  return 1;
}

/* ---------- esperas ---------- */
/* Com a trava: 1 se o objeto esta sinalizado; take consome. */
static int obj_ready(struct wobj* o, uintptr_t me, int take, uint64_t now) {
  switch (o->type) {
    case OBJ_TIMER:
      if (o->armed && now >= o->due) {
        o->signaled = 1;
        if (o->period > 0) o->due = now + (uint64_t)o->period;
        else o->armed = 0;
      }
      /* fallthrough */
    case OBJ_EVENT:
      if (!o->signaled) return 0;
      if (take && !o->manual) o->signaled = 0;
      return 1;
    case OBJ_THREAD:
    case OBJ_PROCESS:
      return o->exited;
    case OBJ_MUTEX:
      if (o->owner && o->owner != me) return 0;
      if (take) {
        o->owner = me;
        o->count++;
      }
      return 1;
    case OBJ_SEM:
      if (o->count <= 0) return 0;
      if (take) o->count--;
      return 1;
    default:
      return 1;
  }
}

static void park_if_exiting(void) {
  struct wproc* p = wp();
  /* O processo esta saindo: esta thread nao deve mais rodar codigo do EXE. */
  if (p->exit_req && wthread()) k_thread_exit(0);
}

static void* wait_caller; /* so para o registro de espera longa */
static DWORD wwait_in(int n, struct wobj** objs, int all, DWORD timeout_ms);
static const char* wobj_kind(const struct wobj* o) {
  switch (o ? o->type : 0) {
    case OBJ_EVENT: return "evento";
    case OBJ_THREAD: return "fim de thread";
    case OBJ_PROCESS: return "fim de processo";
    case OBJ_MUTEX: return "mutex";
    case OBJ_SEM: return "semaforo";
    case OBJ_TIMER: return "timer";
    case OBJ_IOCP: return "porta de conclusao (objeto)";
    case OBJ_FILE: return "arquivo";
    case OBJ_SOCKET: return "soquete";
    case OBJ_PIPE: return "pipe";
    default: return "objeto";
  }
}
static DWORD wwait_from(int n, struct wobj** objs, int all, DWORD timeout_ms, void* caller) {
  DWORD r;
  /* espera curta (0 ms) nao bloqueia: nao marca */
  if (!timeout_ms) return wwait_in(n, objs, all, timeout_ms);
  wwait_begin(n > 1 ? (all ? "varios objetos (todos)" : "varios objetos (qualquer)") : wobj_kind(objs[0]),
              objs[0], objs[0] && objs[0]->type == OBJ_MUTEX ? (uint64_t)objs[0]->owner : 0,
              objs[0] && objs[0]->type == OBJ_PROCESS ? objs[0]->path : 0, caller);
  r = wwait_in(n, objs, all, timeout_ms);
  wwait_end();
  return r;
}
DWORD wwait(int n, struct wobj** objs, int all, DWORD timeout_ms) {
  return wwait_from(n, objs, all, timeout_ms, __builtin_return_address(0));
}
static DWORD wwait_in(int n, struct wobj** objs, int all, DWORD timeout_ms) {
  struct wproc* p = wp();
  uintptr_t me = (uintptr_t)wteb();
  uint64_t t0 = wnow_ms();
  int noted = 0;
  void* caller = wait_caller;
  int b = wbucket_of(objs[0]), i0;
  for (i0 = 1; i0 < n; i0++)
    if (wbucket_of(objs[i0]) != b) b = -1; /* baldes diferentes: global */
  wenter(b);
  for (;;) {
    int g, i, ready = 0, first = -1;
    uint64_t now = wnow_ms(), next = ~0ull;
    int64_t w = -1;
    wlock();
    g = *wword(b);
    for (i = 0; i < n; i++) {
      if (obj_ready(objs[i], me, 0, now)) {
        ready++;
        if (first < 0) first = i;
      } else if (objs[i]->type == OBJ_TIMER && objs[i]->armed && objs[i]->due < next) {
        next = objs[i]->due;
      }
    }
    if (all ? ready == n : ready > 0) {
      if (all) {
        for (i = 0; i < n; i++) obj_ready(objs[i], me, 1, now);
        first = 0;
      } else {
        obj_ready(objs[first], me, 1, now);
      }
      wunlock();
      wleave(b);
      return (DWORD)first;
    }
    wunlock();
    if (p->exit_req) {
      wleave(b);
      park_if_exiting();
      wenter(b);
    }
    if (timeout_ms != INFINITE) {
      uint64_t el = now - t0;
      if (el >= timeout_ms) {
        wleave(b);
        return WAIT_TIMEOUT;
      }
      w = (int64_t)(timeout_ms - el);
    }
    if (next != ~0ull) {
      int64_t tw = (int64_t)(next - now);
      if (tw < 1) tw = 1;
      if (w < 0 || tw < w) w = tw;
    }
    if (w > 0x3FFFFFFF) w = 0x3FFFFFFF;
    if (wine_debug >= 2) {
      if (!noted && now - t0 > 5000) {
        noted = 1;
        wwait_note(objs[0]->type == OBJ_EVENT ? "evento" : objs[0]->type == OBJ_THREAD ? "thread"
                   : objs[0]->type == OBJ_PROCESS ? "processo" : "objeto", caller, now - t0);
        if (objs[0]->type == OBJ_EVENT && objs[0]->u2) wwait_note("  evento criado em", (void*)(uintptr_t)objs[0]->u2, 0);
        wstack_note("  pilha da espera:", (const uint64_t*)__builtin_frame_address(0));
      }
      if (w < 0 || w > 5000) w = 5000;
    }
    k_futex_wait(wword(b), g, w < 0 ? -1 : (int)(w ? w : 1));
  }
}

static DWORD wfmo(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, void* caller) {
  struct wobj* objs[64];
  DWORD i;
  if (wine_debug >= 2) wait_caller = caller;
  if (n > 64 || !n) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return WAIT_FAILED;
  }
  for (i = 0; i < n; i++) {
    objs[i] = wh_get(hs[i], 0);
    if (!objs[i]) {
      if (hs[i] == (HANDLE)(intptr_t)-1) {
        /* o proprio processo: nunca sinaliza */
        static struct wobj never = {OBJ_EVENT, 1};
        objs[i] = &never;
        continue;
      }
      wset_lasterr(ERROR_INVALID_HANDLE);
      return WAIT_FAILED;
    }
  }
  return wwait_from((int)n, objs, all, ms, caller);
}
static WINAPI DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alert) {
  (void)alert;
  return wfmo(n, hs, all, ms, __builtin_return_address(0));
}
static WINAPI DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
  return wfmo(n, hs, all, ms, __builtin_return_address(0));
}
static WINAPI DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alert) {
  (void)alert;
  return wfmo(1, &h, 0, ms, __builtin_return_address(0));
}
static WINAPI DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return wfmo(1, &h, 0, ms, __builtin_return_address(0)); }

static volatile int sleep_word;
static WINAPI void Sleep(DWORD ms) {
  uint64_t t0 = wnow_ms();
  if (!ms) {
    k_thread_yield();
    return;
  }
  if (ms >= 100) wwait_begin("Sleep", (void*)(uintptr_t)ms, 0, 0, __builtin_return_address(0));
  for (;;) {
    uint64_t el = wnow_ms() - t0;
    if (el >= ms) break;
    k_futex_wait(&sleep_word, 0, (int)(ms - el));
    if (wp()->exit_req) park_if_exiting();
  }
  if (ms >= 100) wwait_end();
}
static WINAPI DWORD SleepEx(DWORD ms, BOOL a) {
  (void)a;
  Sleep(ms);
  return 0;
}
static WINAPI BOOL SwitchToThread(void) {
  k_thread_yield();
  return 1;
}

/* WaitOnAddress / WakeByAddress: o futex do kernel, direto. */
static BOOL woa_in(volatile void* addr, void* cmp, size_t size, DWORD ms);
static WINAPI BOOL WaitOnAddress(volatile void* addr, void* cmp, size_t size, DWORD ms) {
  BOOL r;
  if (!ms) return woa_in(addr, cmp, size, ms);
  wwait_begin("WaitOnAddress", (const void*)addr, 0, 0, __builtin_return_address(0));
  r = woa_in(addr, cmp, size, ms);
  wwait_end();
  return r;
}
static BOOL woa_in(volatile void* addr, void* cmp, size_t size, DWORD ms) {
  uint64_t t0 = wnow_ms();
  for (;;) {
    if (memcmp((const void*)addr, cmp, size) != 0) return 1;
    if (size == 4) {
      int r;
      if (wine_debug >= 2 && ms == INFINITE) {
        r = k_futex_wait((volatile int*)addr, *(int*)cmp, 5000);
        if (r == -110 && memcmp((const void*)addr, cmp, size) == 0) {
          wwait_note("WaitOnAddress", __builtin_return_address(0), wnow_ms() - t0);
          continue;
        }
        return 1;
      }
      r = k_futex_wait((volatile int*)addr, *(int*)cmp, ms == INFINITE ? -1 : (int)ms);
      if (r == -110) {
        wset_lasterr(1460); /* ERROR_TIMEOUT */
        return 0;
      }
      return 1;
    }
    if (ms != INFINITE && wnow_ms() - t0 >= ms) {
      wset_lasterr(1460);
      return 0;
    }
    k_futex_wait(&sleep_word, 0, 1);
  }
}
static WINAPI void WakeByAddressSingle(void* addr) { k_futex_wake((volatile int*)addr, 1); }
static WINAPI void WakeByAddressAll(void* addr) { k_futex_wake((volatile int*)addr, -1); }

/* As mesmas pela ntdll (o Claude Code, claude.exe/Bun, importa direto: sem
 * elas o stub encerrava o processo com 0xC0000139 na primeira espera).
 * timeout: NULL = infinito; negativo = relativo em 100 ns. */
static WINAPI long RtlWaitOnAddress(volatile void* addr, void* cmp, size_t size, const int64_t* timeout) {
  DWORD ms = INFINITE;
  if (timeout) {
    int64_t t = *timeout;
    if (t < 0) t = -t;
    t /= 10000;
    ms = t >= 0xFFFFFFFEll ? 0xFFFFFFFEu : (DWORD)t;
  }
  return WaitOnAddress(addr, cmp, size, ms) ? 0 : 0x102; /* STATUS_TIMEOUT */
}
static WINAPI void RtlWakeAddressSingle(void* addr) { WakeByAddressSingle(addr); }
static WINAPI void RtlWakeAddressAll(void* addr) { WakeByAddressAll(addr); }
static WINAPI void RtlFillMemory(void* d, size_t n, int c) { memset(d, c, n); }
static WINAPI void RtlExitUserProcess(DWORD code) { wexit((int)code); }

/* ---------- eventos, mutex, semaforo ---------- */
static HANDLE new_event(int manual, int initial) {
  struct wobj* o = wobj_new(OBJ_EVENT);
  if (!o) return 0;
  o->manual = manual;
  o->signaled = initial;
  return wh_new(o);
}
static WINAPI HANDLE CreateEventA(void* sa, BOOL manual, BOOL initial, const char* name) {
  (void)sa;
  (void)name;
  return new_event(manual, initial);
}
static WINAPI HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, const wchar16* name) {
  HANDLE h;
  struct wobj* o;
  (void)sa;
  (void)name;
  h = new_event(manual, initial);
  o = h ? wh_get(h, OBJ_EVENT) : 0;
  if (o) o->u2 = (uint64_t)(uintptr_t)__builtin_return_address(0); /* quem criou (log de espera longa) */
  return h;
}
static WINAPI HANDLE CreateEventExW(void* sa, const wchar16* name, DWORD fl, DWORD acc) {
  (void)sa;
  (void)name;
  (void)acc;
  return new_event(fl & 1, (fl & 2) != 0);
}
static WINAPI BOOL SetEvent(HANDLE h) {
  struct wobj* o = wh_get(h, OBJ_EVENT);
  if (!o) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  wlock();
  o->signaled = 1;
  wunlock();
  wsignal_obj(o);
  return 1;
}
static WINAPI BOOL ResetEvent(HANDLE h) {
  struct wobj* o = wh_get(h, OBJ_EVENT);
  if (!o) return 0;
  wlock();
  o->signaled = 0;
  wunlock();
  return 1;
}
static WINAPI HANDLE CreateMutexW(void* sa, BOOL owned, const wchar16* name) {
  struct wobj* o = wobj_new(OBJ_MUTEX);
  (void)sa;
  (void)name;
  if (!o) return 0;
  if (owned) {
    o->owner = (uintptr_t)wteb();
    o->count = 1;
  }
  wset_lasterr(0);
  return wh_new(o);
}
static WINAPI HANDLE CreateMutexA(void* sa, BOOL owned, const char* name) {
  (void)name;
  return CreateMutexW(sa, owned, 0);
}
static WINAPI HANDLE CreateMutexExW(void* sa, const wchar16* name, DWORD fl, DWORD acc) {
  (void)acc;
  return CreateMutexW(sa, fl & 1, name);
}
static WINAPI HANDLE OpenMutexW(DWORD acc, BOOL inh, const wchar16* name) {
  (void)acc;
  (void)inh;
  (void)name;
  wset_lasterr(ERROR_FILE_NOT_FOUND);
  return 0;
}
static WINAPI BOOL ReleaseMutex(HANDLE h) {
  struct wobj* o = wh_get(h, OBJ_MUTEX);
  if (!o || o->owner != (uintptr_t)wteb()) return 0;
  wlock();
  if (--o->count <= 0) {
    o->count = 0;
    o->owner = 0;
  }
  wunlock();
  wsignal_obj(o);
  return 1;
}
static WINAPI HANDLE CreateSemaphoreW(void* sa, int initial, int max, const wchar16* name) {
  struct wobj* o = wobj_new(OBJ_SEM);
  (void)sa;
  (void)name;
  if (!o) return 0;
  o->count = initial;
  o->max = max;
  return wh_new(o);
}
static WINAPI HANDLE CreateSemaphoreA(void* sa, int initial, int max, const char* name) {
  (void)name;
  return CreateSemaphoreW(sa, initial, max, 0);
}
static WINAPI HANDLE CreateSemaphoreExW(void* sa, int initial, int max, const wchar16* name, DWORD f, DWORD a) {
  (void)f;
  (void)a;
  return CreateSemaphoreW(sa, initial, max, name);
}
static WINAPI BOOL ReleaseSemaphore(HANDLE h, int n, int* prev) {
  struct wobj* o = wh_get(h, OBJ_SEM);
  if (!o) return 0;
  wlock();
  if (prev) *prev = o->count;
  o->count += n;
  wunlock();
  wsignal_obj(o);
  return 1;
}

/* ---------- timers ---------- */
static HANDLE new_timer(int manual) {
  struct wobj* o = wobj_new(OBJ_TIMER);
  if (!o) return 0;
  o->manual = manual;
  return wh_new(o);
}
static WINAPI HANDLE CreateWaitableTimerExW(void* sa, const wchar16* name, DWORD fl, DWORD acc) {
  (void)sa;
  (void)name;
  (void)acc;
  return new_timer(fl & 1);
}
static WINAPI HANDLE CreateWaitableTimerW(void* sa, BOOL manual, const wchar16* name) {
  (void)sa;
  (void)name;
  return new_timer(manual);
}
uint64_t wfiletime_now(void); /* k32.c */
static WINAPI BOOL SetWaitableTimer(HANDLE h, const int64_t* due, int period, void* fn, void* arg, BOOL res) {
  struct wobj* o = wh_get(h, OBJ_TIMER);
  int64_t d = *due, ms;
  (void)fn;
  (void)arg;
  (void)res;
  if (!o) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (d > 0) d = -(d - (int64_t)wfiletime_now()); /* absoluto -> relativo */
  ms = (-d + 9999) / 10000;                       /* 100 ns -> ms, para cima */
  if (ms < 0) ms = 0;
  wlock();
  o->signaled = 0;
  o->due = wnow_ms() + (uint64_t)ms;
  o->period = period;
  o->armed = 1;
  wunlock();
  wsignal_obj(o);
  return 1;
}
static WINAPI BOOL CancelWaitableTimer(HANDLE h) {
  struct wobj* o = wh_get(h, OBJ_TIMER);
  if (!o) return 0;
  wlock();
  o->armed = 0;
  wunlock();
  return 1;
}

/* ---------- portas de conclusao ---------- */
void* k_malloc_shared(uint32_t n);
int wiocp_post(struct wobj* port, DWORD bytes, uintptr_t key, void* ov) {
  wlock();
  if (port->qn == port->qcap) {
    int ncap = port->qcap ? port->qcap * 2 : 32, i;
    /* memoria sem dono: quem posta pode ser outro processo (pipes) */
    struct iocp_ent* nq = (struct iocp_ent*)k_malloc_shared((unsigned)(sizeof(*nq) * (size_t)ncap));
    if (!nq) {
      wunlock();
      return 0;
    }
    for (i = 0; i < port->qn; i++) nq[i] = port->q[(port->qhead + i) % port->qcap];
    if (port->q) k_free(port->q);
    port->q = nq;
    port->qhead = 0;
    port->qcap = ncap;
  }
  {
    struct iocp_ent* e = &port->q[(port->qhead + port->qn) % port->qcap];
    e->bytes = bytes;
    e->key = key;
    e->ov = ov;
    port->qn++;
  }
  wunlock();
  wsignal_obj(port);
  return 1;
}

void wnet_poll(void); /* net.c: transforma soquetes prontos em conclusoes */

static WINAPI HANDLE CreateIoCompletionPort(HANDLE file, HANDLE existing, uintptr_t key, DWORD n) {
  struct wobj* port;
  (void)n;
  if (existing) {
    port = wh_get(existing, OBJ_IOCP);
    if (!port) {
      wset_lasterr(ERROR_INVALID_HANDLE);
      return 0;
    }
  } else {
    port = wobj_new(OBJ_IOCP);
    if (!port) return 0;
    existing = wh_new(port);
  }
  if (file != INVALID_HANDLE_VALUE) {
    struct wobj* f = wh_get(file, 0);
    if (!f) {
      wset_lasterr(ERROR_INVALID_HANDLE);
      return 0;
    }
    f->p1 = port;
    f->u1 = key;
    if (wine_debug >= 2 && f->type == OBJ_PIPE) wlogx("pipe ligado a porta ", (uint64_t)(uintptr_t)port);
  }
  return existing;
}
static WINAPI BOOL PostQueuedCompletionStatus(HANDLE h, DWORD bytes, uintptr_t key, void* ov) {
  struct wobj* port = wh_get(h, OBJ_IOCP);
  static volatile int nlog;
  if (!port) {
    if (wine_debug) wlogx("PostQueuedCompletionStatus: handle invalido ", (uint64_t)(uintptr_t)h);
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (wine_debug >= 2 && wp()->net && __sync_add_and_fetch(&nlog, 1) < 400)
    wlogx("porta: post (porta) ", (uint64_t)(uintptr_t)port);
  return wiocp_post(port, bytes, key, ov);
}
struct ov_entry {
  uintptr_t key;
  void* ov;
  uintptr_t internal;
  DWORD bytes;
};
static BOOL gqcs_in(HANDLE h, struct ov_entry* ents, DWORD max, DWORD* got, DWORD ms, BOOL alert, void* caller);
static BOOL gqcs(HANDLE h, struct ov_entry* ents, DWORD max, DWORD* got, DWORD ms, BOOL alert, void* caller) {
  BOOL r;
  if (!ms) return gqcs_in(h, ents, max, got, ms, alert, caller);
  wwait_begin("porta de conclusao (IOCP)", h, 0, 0, caller);
  r = gqcs_in(h, ents, max, got, ms, alert, caller);
  wwait_end();
  return r;
}
static WINAPI BOOL GetQueuedCompletionStatusEx(HANDLE h, struct ov_entry* ents, DWORD max, DWORD* got, DWORD ms,
                                               BOOL alert) {
  return gqcs(h, ents, max, got, ms, alert, __builtin_return_address(0));
}
static BOOL gqcs_in(HANDLE h, struct ov_entry* ents, DWORD max, DWORD* got, DWORD ms, BOOL alert, void* caller) {
  struct wproc* p = wp();
  struct wobj* port = wh_get(h, OBJ_IOCP);
  uint64_t t0 = wnow_ms();
  int noted = 0;
  (void)alert;
  int b;
  if (!port) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  b = wbucket_of(port);
  wenter(b);
  for (;;) {
    int g;
    DWORD n = 0;
    int64_t w = -1;
    if (p->net) wnet_poll();
    wlock();
    g = *wword(b);
    while (port->qn > 0 && n < max) {
      struct iocp_ent* e = &port->q[port->qhead];
      ents[n].key = e->key;
      ents[n].ov = e->ov;
      ents[n].internal = 0;
      ents[n].bytes = e->bytes;
      n++;
      port->qhead = (port->qhead + 1) % port->qcap;
      port->qn--;
    }
    wunlock();
    if (n) {
      if (wine_debug >= 2) wlogx("porta: retirou de ", (uint64_t)(uintptr_t)port);
      wleave(b);
      *got = n;
      return 1;
    }
    if (p->exit_req) {
      wleave(b);
      park_if_exiting();
      wenter(b);
    }
    if (ms != INFINITE) {
      uint64_t el = wnow_ms() - t0;
      if (el >= ms) {
        wleave(b);
        *got = 0;
        wset_lasterr(WAIT_TIMEOUT);
        return 0;
      }
      w = (int64_t)(ms - el);
    }
    /* com soquetes abertos, reconfere a rede a cada poucos ms */
    if (p->net && (w < 0 || w > 5)) w = 5;
    if (wine_debug >= 2 && !noted && wnow_ms() - t0 > 5000) {
      noted = 1;
      wwait_note("porta de conclusao", caller, wnow_ms() - t0);
    }
    if (wine_debug >= 2 && (w < 0 || w > 5000)) w = 5000;
    k_futex_wait(wword(b), g, w < 0 ? -1 : (int)(w ? w : 1));
  }
}
static WINAPI BOOL GetQueuedCompletionStatus(HANDLE h, DWORD* bytes, uintptr_t* key, void** ov, DWORD ms) {
  struct ov_entry e;
  DWORD got = 0;
  if (!gqcs(h, &e, 1, &got, ms, 0, __builtin_return_address(0)) || !got) {
    if (ov) *ov = 0;
    return 0;
  }
  if (bytes) *bytes = e.bytes;
  if (key) *key = e.key;
  if (ov) *ov = e.ov;
  return 1;
}

/* ---------- threads ---------- */
static void thread_finish(struct wobj* t, DWORD code) {
  wtrack_exit(t);
  wlock();
  t->exited = 1;
  t->exit_code = code;
  wunlock();
  wsignal_obj(t);
}

/* Tudo o que e codigo do Windows roda na pilha do Windows: callbacks de TLS e
 * DllMain(THREAD_ATTACH/DETACH) tambem. Na pilha do kernel (64 KB) o __chkstk
 * deles le o StackLimit do TEB (a pilha do Windows, em outro lugar) e "sonda"
 * toda a memoria entre as duas, zerando um byte por pagina (codigo do WINE.DLL
 * inclusive). */
static WINAPI uint64_t thread_body(void* arg) {
  struct wobj* t = (struct wobj*)arg;
  uint64_t rc;
  /* a thread principal recebe DLL_PROCESS_ATTACH nos callbacks de TLS do EXE */
  if (t->manual) pe_call_tls_callbacks(wp()->exe, 1);
  else pe_thread_notify(2); /* DLL_THREAD_ATTACH */
  rc = ((DWORD(WINAPI*)(void*))t->start)(t->param);
  pe_thread_notify(3);
  return rc;
}
static int thread_tramp(void* arg) {
  struct wobj* t = (struct wobj*)arg;
  uint64_t rc;
  k_win_teb(t->teb);
  while (t->suspend > 0) k_futex_wait(&t->suspend, t->suspend, 50);
  pe_tls_attach_all(t->teb);
  rc = wine_call_on_stack((void*)thread_body, t, t->stack->base + t->stack->size - 64);
  /* ja de volta na pilha do kernel: a pilha do Windows (1 MB commitado) volta.
   * Antes ficava ate o processo sair; o servico de rede do Chromium cria e
   * solta threads o tempo todo e acumulava centenas (erro 8 no Rust/tokio). */
  if (!t->manual && t->stack) {
    void vm_stack_free(struct vmreg* r); /* vm.c */
    struct vmreg* st = t->stack;
    t->stack = 0;
    vm_stack_free(st);
  }
  thread_finish(t, (DWORD)rc);
  return (int)rc;
}

static struct wobj* thread_new(void* start, void* param, size_t stack, int suspended, int is_main) {
  struct wproc* p = wp();
  struct wobj* t = wobj_new(OBJ_THREAD);
  uint8_t* teb = (uint8_t*)k_malloc_aligned(TEB_SIZE, 64);
  void** tlsarr = (void**)wzalloc(sizeof(void*) * TLS_SLOTS);
  if (!t || !teb || !tlsarr) {
    /* arena baixa (k_malloc) sem memoria: diz qual faltou e quanto ha */
    unsigned lt = 0, lf = 0;
    k_meminfo(&lt, &lf);
    wlogx("thread: sem memoria baixa (1 obj, 2 TEB, 4 TLS) ", (uint64_t)((t ? 0 : 1) | (teb ? 0 : 2) | (tlsarr ? 0 : 4)));
    wlogx("  arena baixa livre (KB) ", (uint64_t)(lf >> 10));
    return 0;
  }
  if (!stack) stack = p->exe && p->exe->stack_reserve ? p->exe->stack_reserve : 0x100000;
  if (stack < 0x40000) stack = 0x40000;
  /* commitado: 1 MB nas threads de trabalho (o padrao do Windows), tudo na
   * principal (o JS do renderer roda nela) */
  {
    size_t commit = is_main ? stack : (stack > 0x100000 ? 0x100000 : stack);
    struct vmreg* vm_stack_alloc(size_t reserve, size_t commit); /* vm.c */
    t->stack = vm_stack_alloc(stack, commit);
    if (!t->stack) {
      void k_hminfo(uint64_t* quota, uint64_t* used, uint64_t* total_ram);
      uint64_t q = 0, u = 0, tr = 0;
      k_hminfo(&q, &u, &tr);
      wlogx("thread: sem memoria para a pilha, reserva ", (uint64_t)stack);
      wlogx("  commit ", (uint64_t)commit);
      wlogx("  HIMEM cota (MB) ", q >> 20);
      wlogx("  HIMEM usado (MB) ", u >> 20);
      return 0;
    }
    t->stack_commit = commit;
  }
  memset(teb, 0, TEB_SIZE);
  wlock();
  t->tid = p->next_tid;
  p->next_tid += 4;
  wunlock();
  *(uint64_t*)(teb + TEB_STACKBASE) = (uint64_t)(uintptr_t)(t->stack->base + t->stack->size);
  /* a thread so ve a parte commitada (o resto da reserva nao tem paginas) */
  *(uint64_t*)(teb + TEB_STACKLIMIT) = (uint64_t)(uintptr_t)(t->stack->base + t->stack->size - t->stack_commit);
  *(uint64_t*)(teb + TEB_DEALLOC) = (uint64_t)(uintptr_t)(t->stack->base + t->stack->size - t->stack_commit);
  *(uint64_t*)(teb + TEB_SELF) = (uint64_t)(uintptr_t)teb;
  *(uint64_t*)(teb + TEB_PID) = p->pid;
  *(uint64_t*)(teb + TEB_TID) = t->tid;
  *(uint64_t*)(teb + TEB_TLSPTR) = (uint64_t)(uintptr_t)tlsarr;
  *(uint64_t*)(teb + TEB_PEB) = (uint64_t)(uintptr_t)p;
  *(struct wobj**)(teb + TEB_WTHREAD) = t;
  t->teb = teb;
  wtrack_thread(t); /* registro de esperas (util.c) */
  t->start = start;
  t->param = param;
  t->suspend = suspended ? 1 : 0;
  t->refs = 1; /* a propria thread */
  t->manual = is_main;
  if (is_main) p->main_thread = t;
  t->ktid = k_thread_create(thread_tramp, t, 0x10000, 0);
  if (t->ktid <= 0) {
    wlogx("k_thread_create falhou: ", (uint64_t)(int64_t)t->ktid);
    return 0;
  }
  /* ninguem faz join: sem detach a vaga no kernel (KT_MAX 1024 no sistema
   * todo) ficava morta ate o processo sair */
  k_thread_detach(t->ktid);
  return t;
}

struct wobj* wthread_create_main(void* entry, void* arg, uint32_t stack_reserve) {
  return thread_new(entry, arg, stack_reserve, 0, 1);
}

/* Thread do proprio WINE.DLL (bashbox.c): um Windows de verdade por baixo
 * (TEB, wp()), sem handle para o EXE. entry: DWORD WINAPI f(void*). */
struct wobj* wthread_internal(void* entry, void* arg) {
  return thread_new(entry, arg, 0x40000, 0, 0);
}

static WINAPI HANDLE CreateThread(void* sa, size_t stack, void* start, void* param, DWORD flags, DWORD* tid) {
  struct wobj* t;
  (void)sa;
  t = thread_new(start, param, stack, (flags & 4) != 0, 0);
  if (!t) {
    wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  if (tid) *tid = t->tid;
  return wh_new(t);
}
/* CreateRemoteThread: no proprio processo e um CreateThread. Em outro
 * processo nao da (o Chromium usa para forcar o dump de um renderer que nao
 * responde, DumpAndCrashTargetProcess): falha com ERROR_ACCESS_DENIED e o
 * Chromium segue. Antes o stub de funcao ausente encerrava o browser inteiro
 * (0xC0000139) e todas as janelas do Claude fechavam (notebook lento). */
static WINAPI HANDLE CreateRemoteThread(HANDLE proc, void* sa, size_t stack, void* start, void* param,
                                        DWORD flags, DWORD* tid) {
  if (wh_is_self(proc)) return CreateThread(sa, stack, start, param, flags, tid);
  if (wine_debug) wlog("CreateRemoteThread em outro processo: recusado", 0);
  wset_lasterr(5); /* ERROR_ACCESS_DENIED */
  return 0;
}
static WINAPI HANDLE CreateRemoteThreadEx(HANDLE proc, void* sa, size_t stack, void* start, void* param,
                                          DWORD flags, void* attrs, DWORD* tid) {
  (void)attrs;
  return CreateRemoteThread(proc, sa, stack, start, param, flags, tid);
}
static WINAPI __attribute__((noreturn)) void ExitThread(DWORD code) {
  struct wobj* t = wthread();
  if (t) {
    pe_thread_notify(3);
    thread_finish(t, code);
    if (t == wp()->main_thread) {
      /* a thread principal saiu: o processo segue ate o ExitProcess */
    }
  }
  k_thread_exit((int)code);
  for (;;) {
  }
}
static WINAPI BOOL GetExitCodeThread(HANDLE h, DWORD* code) {
  struct wobj* t = wh_get(h, OBJ_THREAD);
  if (!t) return 0;
  *code = t->exited ? t->exit_code : 259; /* STILL_ACTIVE */
  return 1;
}
static WINAPI DWORD ResumeThread(HANDLE h) {
  struct wobj* t = wh_get(h, OBJ_THREAD);
  int prev;
  if (!t) {
    /* hThread do CreateProcess (proc.c) e um objeto de processo: o filho ja
     * roda, mas o libuv (Bun/Claude Code) cria com CREATE_SUSPENDED e trata
     * -1 aqui como falha do uv_spawn (EUNKNOWN): diz "estava suspensa 1 vez" */
    struct wobj* o = wh_get(h, 0);
    if (o && o->type == OBJ_PROCESS) return 1;
    wset_lasterr(ERROR_INVALID_HANDLE);
    return (DWORD)-1;
  }
  prev = t->suspend;
  if (prev > 0) {
    t->suspend = prev - 1;
    k_futex_wake(&t->suspend, -1);
  }
  return (DWORD)prev;
}
/* Sem parar thread alheia: o Go trata -1 como "nao deu para preemptar". */
static WINAPI DWORD SuspendThread(HANDLE h) {
  static int n;
  if (wine_debug >= 2 && n++ < 5) wwait_note("SuspendThread (recusado)", __builtin_return_address(0), 0);
  (void)h;
  wset_lasterr(ERROR_ACCESS_DENIED);
  return (DWORD)-1;
}
static WINAPI BOOL GetThreadContext(HANDLE h, void* ctx) {
  static int n;
  if (wine_debug >= 2 && n++ < 5) wwait_note("GetThreadContext (recusado)", __builtin_return_address(0), 0);
  (void)h;
  (void)ctx;
  wset_lasterr(ERROR_ACCESS_DENIED);
  return 0;
}
static WINAPI BOOL SetThreadContext(HANDLE h, const void* ctx) {
  (void)h;
  (void)ctx;
  wset_lasterr(ERROR_ACCESS_DENIED);
  return 0;
}
static WINAPI BOOL TerminateThread(HANDLE h, DWORD code) {
  (void)h;
  (void)code;
  return 0;
}
static WINAPI BOOL SetThreadPriority(HANDLE h, int pr) {
  (void)h;
  (void)pr;
  return 1;
}
static WINAPI int GetThreadPriority(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI uintptr_t SetThreadAffinityMask(HANDLE h, uintptr_t m) {
  (void)h;
  (void)m;
  return 1;
}
static WINAPI DWORD GetThreadId(HANDLE h) {
  struct wobj* t = wh_get(h, OBJ_THREAD);
  return t ? t->tid : 0;
}
static WINAPI long SetThreadDescription(HANDLE h, const wchar16* d) {
  (void)h;
  (void)d;
  return 0;
}
static WINAPI BOOL SetThreadStackGuarantee(DWORD* sz) {
  if (sz) *sz = 0;
  return 1;
}
static WINAPI BOOL GetProcessAffinityMask(HANDLE h, uintptr_t* pm, uintptr_t* sm) {
  (void)h;
  if (pm) *pm = 1;
  if (sm) *sm = 1;
  return 1;
}
static WINAPI BOOL SetProcessPriorityBoost(HANDLE h, BOOL off) {
  (void)h;
  (void)off;
  return 1;
}
static WINAPI BOOL SetPriorityClass(HANDLE h, DWORD c) {
  (void)h;
  (void)c;
  return 1;
}
static WINAPI BOOL GetExitCodeProcess(HANDLE h, DWORD* code) {
  struct wobj* o = wh_get(h, OBJ_PROCESS);
  *code = o && o->exited ? o->exit_code : 259; /* STILL_ACTIVE */
  return 1;
}

#define S(n) {#n, (void*)n}
const struct wine_sym obj_syms[] = {
    S(CloseHandle), S(DuplicateHandle), S(ReOpenFile), S(GetHandleInformation), S(SetHandleInformation),
    S(WaitForMultipleObjectsEx), S(WaitForMultipleObjects), S(WaitForSingleObjectEx),
    S(WaitForSingleObject), S(Sleep), S(SleepEx), S(SwitchToThread), S(WaitOnAddress),
    S(WakeByAddressSingle), S(WakeByAddressAll), S(RtlWaitOnAddress), S(RtlWakeAddressSingle),
    S(RtlWakeAddressAll), S(RtlFillMemory), S(RtlExitUserProcess), S(CreateEventA), S(CreateEventW), S(CreateEventExW),
    S(SetEvent), S(ResetEvent), S(CreateMutexW), S(CreateMutexA), S(CreateMutexExW), S(OpenMutexW),
    S(ReleaseMutex), S(CreateSemaphoreW), S(CreateSemaphoreA), S(CreateSemaphoreExW),
    S(ReleaseSemaphore), S(CreateWaitableTimerExW), S(CreateWaitableTimerW), S(SetWaitableTimer),
    S(CancelWaitableTimer), S(CreateIoCompletionPort), S(PostQueuedCompletionStatus),
    S(GetQueuedCompletionStatusEx), S(GetQueuedCompletionStatus), S(CreateThread), S(CreateRemoteThread), S(CreateRemoteThreadEx), S(ExitThread),
    S(GetExitCodeThread), S(ResumeThread), S(SuspendThread), S(GetThreadContext), S(SetThreadContext),
    S(TerminateThread), S(SetThreadPriority), S(GetThreadPriority), S(SetThreadAffinityMask),
    S(GetThreadId), S(SetThreadDescription), S(SetThreadStackGuarantee), S(GetProcessAffinityMask),
    S(SetProcessPriorityBoost), S(SetPriorityClass), S(GetExitCodeProcess),
    {0, 0}};
