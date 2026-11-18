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

/* ---------- trava do processo ---------- */
void wlock(void) {
  struct wproc* p = wp();
  int spin = 0;
  while (__sync_lock_test_and_set(&p->lock, 1)) {
    if (++spin > 200) {
      k_thread_yield();
      spin = 0;
    } else {
      __asm__ volatile("pause");
    }
  }
}
void wunlock(void) { __sync_lock_release(&wp()->lock); }

void wsignal(void) {
  struct wproc* p = wp();
  __sync_fetch_and_add(&p->gen, 1);
  k_futex_wake(&p->gen, -1);
}

/* ---------- handles ---------- */
#define H_BASE 0x200u
struct wobj* wobj_new(int type) {
  struct wobj* o = (struct wobj*)wzalloc(sizeof(*o));
  if (o) {
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
  return 0;
}

struct wobj* wh_get(HANDLE h, int type) {
  uintptr_t v = (uintptr_t)h;
  struct wobj* o;
  if (h == (HANDLE)(intptr_t)-2) o = wthread();
  else if (v < H_BASE || (v & 3) || (v - H_BASE) / 4 >= WHANDLE_MAX) return 0;
  else o = wp()->handles[(v - H_BASE) / 4];
  if (!o || (type && o->type != type)) return 0;
  return o;
}

void wfile_close(struct wobj* o);   /* file.c */
void wsock_close(struct wobj* o);   /* net.c */

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
  if (--o->refs > 0) o = 0;
  wunlock();
  if (o) {
    if (o->type == OBJ_FILE || o->type == OBJ_FIND) wfile_close(o);
    if (o->type == OBJ_SOCKET) wsock_close(o);
    /* threads ficam: a propria thread ainda usa o objeto */
    if (o->type != OBJ_THREAD) {
      if (o->q) k_free(o->q);
      k_free(o);
    }
  }
  return 1;
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

static WINAPI BOOL DuplicateHandle(HANDLE sp, HANDLE h, HANDLE dp, HANDLE* out, DWORD acc, BOOL inh, DWORD opt) {
  struct wobj* o = wh_get(h, 0);
  uintptr_t v = (uintptr_t)h;
  (void)sp;
  (void)dp;
  (void)acc;
  (void)inh;
  if (!o) {
    if (v == 0x10 || v == 0x14 || v == 0x18 || h == (HANDLE)(intptr_t)-1) {
      if (out) *out = h;
      return 1;
    }
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (out) *out = wh_new(o);
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

DWORD wwait(int n, struct wobj** objs, int all, DWORD timeout_ms) {
  struct wproc* p = wp();
  uintptr_t me = (uintptr_t)wteb();
  uint64_t t0 = wnow_ms();
  for (;;) {
    int g, i, ready = 0, first = -1;
    uint64_t now = wnow_ms(), next = ~0ull;
    int64_t w = -1;
    wlock();
    g = p->gen;
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
      return (DWORD)first;
    }
    wunlock();
    if (p->exit_req) park_if_exiting();
    if (timeout_ms != INFINITE) {
      uint64_t el = now - t0;
      if (el >= timeout_ms) return WAIT_TIMEOUT;
      w = (int64_t)(timeout_ms - el);
    }
    if (next != ~0ull) {
      int64_t tw = (int64_t)(next - now);
      if (tw < 1) tw = 1;
      if (w < 0 || tw < w) w = tw;
    }
    if (w > 0x3FFFFFFF) w = 0x3FFFFFFF;
    k_futex_wait(&p->gen, g, w < 0 ? -1 : (int)(w ? w : 1));
  }
}

static WINAPI DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alert) {
  struct wobj* objs[64];
  DWORD i;
  (void)alert;
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
  return wwait((int)n, objs, all, ms);
}
static WINAPI DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
  return WaitForMultipleObjectsEx(n, hs, all, ms, 0);
}
static WINAPI DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alert) {
  return WaitForMultipleObjectsEx(1, &h, 0, ms, alert);
}
static WINAPI DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjectsEx(1, &h, 0, ms, 0); }

static volatile int sleep_word;
static WINAPI void Sleep(DWORD ms) {
  uint64_t t0 = wnow_ms();
  if (!ms) {
    k_thread_yield();
    return;
  }
  for (;;) {
    uint64_t el = wnow_ms() - t0;
    if (el >= ms) break;
    k_futex_wait(&sleep_word, 0, (int)(ms - el));
    if (wp()->exit_req) park_if_exiting();
  }
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
static WINAPI BOOL WaitOnAddress(volatile void* addr, void* cmp, size_t size, DWORD ms) {
  uint64_t t0 = wnow_ms();
  for (;;) {
    if (memcmp((const void*)addr, cmp, size) != 0) return 1;
    if (size == 4) {
      int r = k_futex_wait((volatile int*)addr, *(int*)cmp, ms == INFINITE ? -1 : (int)ms);
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
  (void)sa;
  (void)name;
  return new_event(manual, initial);
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
  wsignal();
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
  wsignal();
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
  wsignal();
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
  wsignal();
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
int wiocp_post(struct wobj* port, DWORD bytes, uintptr_t key, void* ov) {
  wlock();
  if (port->qn == port->qcap) {
    int ncap = port->qcap ? port->qcap * 2 : 32, i;
    struct iocp_ent* nq = (struct iocp_ent*)k_malloc((unsigned)(sizeof(*nq) * (size_t)ncap));
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
  wsignal();
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
  }
  return existing;
}
static WINAPI BOOL PostQueuedCompletionStatus(HANDLE h, DWORD bytes, uintptr_t key, void* ov) {
  struct wobj* port = wh_get(h, OBJ_IOCP);
  if (!port) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  return wiocp_post(port, bytes, key, ov);
}
struct ov_entry {
  uintptr_t key;
  void* ov;
  uintptr_t internal;
  DWORD bytes;
};
static WINAPI BOOL GetQueuedCompletionStatusEx(HANDLE h, struct ov_entry* ents, DWORD max, DWORD* got, DWORD ms,
                                               BOOL alert) {
  struct wproc* p = wp();
  struct wobj* port = wh_get(h, OBJ_IOCP);
  uint64_t t0 = wnow_ms();
  (void)alert;
  if (!port) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  for (;;) {
    int g;
    DWORD n = 0;
    int64_t w = -1;
    if (p->net) wnet_poll();
    wlock();
    g = p->gen;
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
      *got = n;
      return 1;
    }
    if (p->exit_req) park_if_exiting();
    if (ms != INFINITE) {
      uint64_t el = wnow_ms() - t0;
      if (el >= ms) {
        *got = 0;
        wset_lasterr(WAIT_TIMEOUT);
        return 0;
      }
      w = (int64_t)(ms - el);
    }
    /* com soquetes abertos, reconfere a rede a cada poucos ms */
    if (p->net && (w < 0 || w > 5)) w = 5;
    k_futex_wait(&p->gen, g, w < 0 ? -1 : (int)(w ? w : 1));
  }
}
static WINAPI BOOL GetQueuedCompletionStatus(HANDLE h, DWORD* bytes, uintptr_t* key, void** ov, DWORD ms) {
  struct ov_entry e;
  DWORD got = 0;
  if (!GetQueuedCompletionStatusEx(h, &e, 1, &got, ms, 0) || !got) {
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
  wlock();
  t->exited = 1;
  t->exit_code = code;
  wunlock();
  wsignal();
}

static int thread_tramp(void* arg) {
  struct wobj* t = (struct wobj*)arg;
  uint64_t rc;
  k_win_teb(t->teb);
  while (t->suspend > 0) k_futex_wait(&t->suspend, t->suspend, 50);
  pe_tls_attach_all(t->teb);
  /* a thread principal recebe DLL_PROCESS_ATTACH nos callbacks de TLS do EXE */
  if (t->manual) pe_call_tls_callbacks(wp()->exe, 1);
  else pe_thread_notify(2); /* DLL_THREAD_ATTACH */
  rc = wine_call_on_stack(t->start, t->param, t->stack->base + t->stack->size - 64);
  pe_thread_notify(3);
  thread_finish(t, (DWORD)rc);
  return (int)rc;
}

static struct wobj* thread_new(void* start, void* param, size_t stack, int suspended, int is_main) {
  struct wproc* p = wp();
  struct wobj* t = wobj_new(OBJ_THREAD);
  uint8_t* teb = (uint8_t*)k_malloc_aligned(TEB_SIZE, 64);
  void** tlsarr = (void**)wzalloc(sizeof(void*) * TLS_SLOTS);
  if (!t || !teb || !tlsarr) return 0;
  if (!stack) stack = p->exe && p->exe->stack_reserve ? p->exe->stack_reserve : 0x100000;
  if (stack < 0x40000) stack = 0x40000;
  t->stack = vm_reserve(stack, VM_STACK, 2);
  if (!t->stack) return 0;
  memset(teb, 0, TEB_SIZE);
  wlock();
  t->tid = p->next_tid;
  p->next_tid += 4;
  wunlock();
  *(uint64_t*)(teb + TEB_STACKLIMIT) = (uint64_t)(uintptr_t)t->stack->base;
  *(uint64_t*)(teb + TEB_DEALLOC) = (uint64_t)(uintptr_t)t->stack->base;
  *(uint64_t*)(teb + TEB_SELF) = (uint64_t)(uintptr_t)teb;
  *(uint64_t*)(teb + TEB_PID) = 0x100;
  *(uint64_t*)(teb + TEB_TID) = t->tid;
  *(uint64_t*)(teb + TEB_TLSPTR) = (uint64_t)(uintptr_t)tlsarr;
  *(uint64_t*)(teb + TEB_PEB) = (uint64_t)(uintptr_t)p;
  *(struct wobj**)(teb + TEB_WTHREAD) = t;
  t->teb = teb;
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
  return t;
}

struct wobj* wthread_create_main(void* entry, void* arg, uint32_t stack_reserve) {
  return thread_new(entry, arg, stack_reserve, 0, 1);
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
  if (!t) return (DWORD)-1;
  prev = t->suspend;
  if (prev > 0) {
    t->suspend = prev - 1;
    k_futex_wake(&t->suspend, -1);
  }
  return (DWORD)prev;
}
/* Sem parar thread alheia: o Go trata -1 como "nao deu para preemptar". */
static WINAPI DWORD SuspendThread(HANDLE h) {
  (void)h;
  wset_lasterr(ERROR_ACCESS_DENIED);
  return (DWORD)-1;
}
static WINAPI BOOL GetThreadContext(HANDLE h, void* ctx) {
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
    S(CloseHandle), S(DuplicateHandle), S(GetHandleInformation), S(SetHandleInformation),
    S(WaitForMultipleObjectsEx), S(WaitForMultipleObjects), S(WaitForSingleObjectEx),
    S(WaitForSingleObject), S(Sleep), S(SleepEx), S(SwitchToThread), S(WaitOnAddress),
    S(WakeByAddressSingle), S(WakeByAddressAll), S(CreateEventA), S(CreateEventW), S(CreateEventExW),
    S(SetEvent), S(ResetEvent), S(CreateMutexW), S(CreateMutexA), S(CreateMutexExW), S(OpenMutexW),
    S(ReleaseMutex), S(CreateSemaphoreW), S(CreateSemaphoreA), S(CreateSemaphoreExW),
    S(ReleaseSemaphore), S(CreateWaitableTimerExW), S(CreateWaitableTimerW), S(SetWaitableTimer),
    S(CancelWaitableTimer), S(CreateIoCompletionPort), S(PostQueuedCompletionStatus),
    S(GetQueuedCompletionStatusEx), S(GetQueuedCompletionStatus), S(CreateThread), S(ExitThread),
    S(GetExitCodeThread), S(ResumeThread), S(SuspendThread), S(GetThreadContext), S(SetThreadContext),
    S(TerminateThread), S(SetThreadPriority), S(GetThreadPriority), S(SetThreadAffinityMask),
    S(GetThreadId), S(SetThreadDescription), S(SetThreadStackGuarantee), S(GetProcessAffinityMask),
    S(SetProcessPriorityBoost), S(SetPriorityClass), S(GetExitCodeProcess),
    {0, 0}};
