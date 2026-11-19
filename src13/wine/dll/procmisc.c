/*
 * procmisc.c - kernel32/psapi/winmm que o Claude.exe (Chromium/Electron)
 * importa e chama no inicio: informacao de processo e thread, politicas de
 * mitigacao, job objects, listas de atributos de CreateProcess, esperas
 * registradas, versao do Windows, pacotes (AppModel), heap e audio/midi sem
 * dispositivos. Cada uma responde como o Windows responde quando o recurso
 * nao existe ou nao muda nada, para o programa seguir pelo caminho normal.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define LOOK(t, n) ((t)builtin_lookup(n))

/* ---------- processo e thread ---------- */
static WINAPI BOOL GetProcessMitigationPolicy(HANDLE h, int pol, void* buf, size_t n) {
  (void)h;
  (void)pol;
  if (buf && n) memset(buf, 0, n); /* nenhuma politica ligada */
  return 1;
}
static WINAPI BOOL SetProcessMitigationPolicy(int pol, const void* buf, size_t n) {
  (void)pol; (void)buf; (void)n;
  return 1;
}
static WINAPI BOOL GetProcessInformation(HANDLE h, int cls, void* buf, DWORD n) {
  (void)h;
  (void)cls;
  if (buf && n) memset(buf, 0, n);
  return 1;
}
static WINAPI BOOL SetProcessInformation(HANDLE h, int cls, void* buf, DWORD n) {
  (void)h; (void)cls; (void)buf; (void)n;
  return 1;
}
static WINAPI BOOL SetThreadInformation(HANDLE h, int cls, void* buf, DWORD n) {
  (void)h; (void)cls; (void)buf; (void)n;
  return 1;
}
static WINAPI DWORD GetCurrentProcessorNumber(void) { return 0; }
static WINAPI void GetCurrentThreadStackLimits(uintptr_t* lo, uintptr_t* hi) {
  struct wobj* t = wthread();
  if (t && t->stack) {
    *lo = (uintptr_t)t->stack->base + t->stack->size - (t->stack_commit ? t->stack_commit : t->stack->size);
    *hi = (uintptr_t)t->stack->base + t->stack->size;
    return;
  }
  { /* tarefa do loader: 1 MB abaixo de onde estamos */
    uintptr_t sp = (uintptr_t)&t;
    *hi = (sp + 0x10000) & ~(uintptr_t)0xFFFF;
    *lo = *hi - 0x100000;
  }
}
static WINAPI BOOL GetThreadTimes(HANDLE h, uint64_t* c, uint64_t* e, uint64_t* k, uint64_t* u) {
  (void)h;
  if (c) *c = 0;
  if (e) *e = 0;
  if (k) *k = 0;
  if (u) *u = 0;
  return 1;
}
static WINAPI BOOL QueryThreadCycleTime(HANDLE h, uint64_t* t) {
  (void)h;
  if (t) *t = 0;
  return 1;
}
static WINAPI BOOL QueryProcessCycleTime(HANDLE h, uint64_t* t) {
  (void)h;
  if (t) *t = 0;
  return 1;
}
static WINAPI BOOL GetProcessIoCounters(HANDLE h, void* io) {
  (void)h;
  if (io) memset(io, 0, 48);
  return 1;
}
static WINAPI BOOL GetProcessHandleCount(HANDLE h, DWORD* n) {
  (void)h;
  if (n) *n = 64;
  return 1;
}
static WINAPI BOOL GetThreadPriorityBoost(HANDLE h, BOOL* b) {
  (void)h;
  if (b) *b = 0;
  return 1;
}
static WINAPI BOOL SetThreadPriorityBoost(HANDLE h, BOOL b) {
  (void)h;
  (void)b;
  return 1;
}
struct group_affinity {
  uint64_t mask;
  uint16_t group, res[3];
};
static WINAPI BOOL GetThreadGroupAffinity(HANDLE h, struct group_affinity* a) {
  (void)h;
  if (!a) return 0;
  memset(a, 0, sizeof(*a));
  a->mask = 1;
  return 1;
}
static WINAPI BOOL IsThreadAFiber(void) { return 0; }
static WINAPI BOOL CheckRemoteDebuggerPresent(HANDLE h, BOOL* b) {
  (void)h;
  if (b) *b = 0;
  return 1;
}
static WINAPI HANDLE OpenThread(DWORD acc, BOOL inh, DWORD tid) {
  (void)acc; (void)inh; (void)tid;
  wset_lasterr(5); /* ERROR_ACCESS_DENIED */
  return 0;
}
static WINAPI BOOL SetProcessShutdownParameters(DWORD lvl, DWORD fl) {
  (void)lvl;
  (void)fl;
  return 1;
}
static WINAPI long RegisterApplicationRestart(const wchar16* cmd, DWORD fl) {
  (void)cmd;
  (void)fl;
  return 0;
}
static WINAPI long WerRegisterRuntimeExceptionModule(const wchar16* dll, void* ctx) {
  (void)dll;
  (void)ctx;
  return 0;
}
static WINAPI DWORD RtlGetLastNtStatus(void) { return 0; }
static WINAPI uint16_t RtlCaptureStackBackTrace(DWORD skip, DWORD n, void** out, DWORD* hash) {
  (void)skip; (void)n; (void)out;
  if (hash) *hash = 0;
  return 0;
}
static WINAPI BOOL RtlDeleteFunctionTable(void* t) {
  (void)t;
  return 1;
}

/* ---------- versao: SODOS diz Windows 10 19045 ---------- */
static WINAPI uint64_t VerSetConditionMask(uint64_t cond, DWORD type, uint8_t op) {
  int i;
  for (i = 0; i < 8; i++)
    if (type & (1u << i)) cond |= (uint64_t)(op & 7) << (i * 3);
  return cond;
}
static WINAPI BOOL VerifyVersionInfoW(void* info, DWORD type, uint64_t cond) {
  (void)info; (void)type; (void)cond;
  return 1; /* os pedidos tipicos sao "pelo menos Windows 7/8/10" */
}

/* ---------- InitOnce em dois passos ---------- */
static WINAPI BOOL InitOnceBeginInitialize(volatile uintptr_t* once, DWORD fl, BOOL* pending, void** ctx) {
  (void)fl;
  for (;;) {
    uintptr_t v = *once;
    if ((v & 3) == 2) { /* pronto */
      if (pending) *pending = 0;
      if (ctx) *ctx = (void*)(v & ~(uintptr_t)3);
      return 1;
    }
    if (v == 0 && __sync_bool_compare_and_swap(once, 0, 1)) {
      if (pending) *pending = 1;
      return 1;
    }
    k_thread_yield(); /* outra thread esta inicializando */
  }
}
static WINAPI BOOL InitOnceComplete(volatile uintptr_t* once, DWORD fl, void* ctx) {
  if (fl & 4) { /* INIT_ONCE_INIT_FAILED */
    *once = 0;
    return 1;
  }
  *once = ((uintptr_t)ctx & ~(uintptr_t)3) | 2;
  return 1;
}
static WINAPI BOOL TryAcquireSRWLockShared(void* l) {
  volatile intptr_t* v = (volatile intptr_t*)l;
  intptr_t cur = *v;
  return cur >= 0 && __sync_val_compare_and_swap(v, cur, cur + 1) == cur;
}

/* ---------- jobs e atributos de processo ---------- */
static WINAPI HANDLE CreateJobObjectW(void* sa, const wchar16* name) {
  typedef HANDLE(WINAPI * ce)(void*, BOOL, BOOL, const wchar16*);
  ce f = LOOK(ce, "CreateEventW");
  (void)sa;
  (void)name;
  return f ? f(0, 1, 0, 0) : 0; /* um objeto qualquer: so precisa fechar */
}
static WINAPI BOOL AssignProcessToJobObject(HANDLE j, HANDLE p) {
  (void)j;
  (void)p;
  return 1;
}
static WINAPI BOOL SetInformationJobObject(HANDLE j, int cls, void* buf, DWORD n) {
  (void)j; (void)cls; (void)buf; (void)n;
  return 1;
}
static WINAPI BOOL QueryInformationJobObject(HANDLE j, int cls, void* buf, DWORD n, DWORD* ret) {
  (void)j;
  (void)cls;
  if (buf && n) memset(buf, 0, n);
  if (ret) *ret = n;
  return 1;
}
static WINAPI BOOL TerminateJobObject(HANDLE j, DWORD code) {
  (void)j;
  (void)code;
  return 1;
}
/* Lista de atributos: [0] quantos, [1] capacidade; a partir do byte 48,
 * {atributo, ponteiro, tamanho} de 24 bytes. O CreateProcess le a
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST (proc.c: wattr_handles). */
static WINAPI BOOL InitializeProcThreadAttributeList(void* list, DWORD n, DWORD fl, size_t* size) {
  size_t need = 48 + (size_t)n * 24;
  (void)fl;
  if (!list || !size || *size < need) {
    if (size) *size = need;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memset(list, 0, need);
  ((DWORD*)list)[1] = n;
  return 1;
}
static WINAPI BOOL UpdateProcThreadAttribute(void* list, DWORD fl, uintptr_t attr, void* v, size_t n, void* pv,
                                             size_t* rs) {
  DWORD* hdr = (DWORD*)list;
  uintptr_t* e;
  (void)fl; (void)pv; (void)rs;
  if (!list || hdr[0] >= hdr[1]) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  e = (uintptr_t*)((uint8_t*)list + 48 + (size_t)hdr[0] * 24);
  e[0] = attr;
  e[1] = (uintptr_t)v;
  e[2] = n;
  hdr[0]++;
  return 1;
}
/* PROC_THREAD_ATTRIBUTE_HANDLE_LIST de uma lista (0 = nao tem) */
const HANDLE* wattr_handles(const void* list, DWORD* count) {
  const DWORD* hdr = (const DWORD*)list;
  DWORD i;
  *count = 0;
  if (!list) return 0;
  for (i = 0; i < hdr[0] && i < hdr[1]; i++) {
    const uintptr_t* e = (const uintptr_t*)((const uint8_t*)list + 48 + (size_t)i * 24);
    if (e[0] == 0x20002u) { /* PROC_THREAD_ATTRIBUTE_HANDLE_LIST */
      *count = (DWORD)(e[2] / sizeof(HANDLE));
      return (const HANDLE*)e[1];
    }
  }
  return 0;
}
static WINAPI void DeleteProcThreadAttributeList(void* list) { (void)list; }

/* ---------- espera registrada: uma thread por espera ----------
 * UnregisterWaitEx(h, evento) volta na hora e sinaliza o evento quando nenhum
 * callback estiver mais rodando (o ObjectWatcher do Chromium espera por ele);
 * com INVALID_HANDLE_VALUE, espera o callback aqui mesmo. A thread confere o
 * pedido de parada a cada 100 ms, entao nao fica presa num objeto que nunca
 * sera sinalizado. */
struct regwait {
  HANDLE obj;
  void (WINAPI* cb)(void*, uint8_t);
  void* ctx;
  DWORD ms, flags;
  volatile int stop;
  volatile int in_cb;
  volatile int done;  /* a thread saiu: nao chama mais nada */
  volatile DWORD tid; /* thread da espera (UnregisterWaitEx de dentro do callback nao espera) */
};
static WINAPI DWORD regwait_thread(void* arg) {
  typedef DWORD(WINAPI * wf)(HANDLE, DWORD);
  struct regwait* r = (struct regwait*)arg;
  wf w = LOOK(wf, "WaitForSingleObject");
  uint64_t t0 = wnow_ms();
  {
    struct wobj* me = wthread();
    r->tid = me ? me->tid : 0;
  }
  while (!r->stop && w) {
    DWORD step = 100, rc;
    if (r->ms != INFINITE) {
      uint64_t el = wnow_ms() - t0;
      if (el >= r->ms) {
        step = 0;
      } else if (r->ms - el < step) {
        step = (DWORD)(r->ms - el);
      }
    }
    rc = w(r->obj, step);
    if (r->stop) break;
    if (rc == 0x102 && (r->ms == INFINITE || wnow_ms() - t0 < r->ms)) continue; /* so a volta de 100 ms */
    if (wine_debug >= 2) wlogx("espera registrada: disparou, objeto ", (uint64_t)(uintptr_t)r->obj);
    /* marca "no callback" ANTES de olhar o stop: quem cancela (stop = 1 e
     * depois espera in_cb = 0) nunca volta com um callback ainda por comecar */
    __atomic_store_n(&r->in_cb, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&r->stop, __ATOMIC_SEQ_CST)) {
      __atomic_store_n(&r->in_cb, 0, __ATOMIC_SEQ_CST);
      break;
    }
    r->cb(r->ctx, rc == 0x102); /* WAIT_TIMEOUT */
    __atomic_store_n(&r->in_cb, 0, __ATOMIC_SEQ_CST);
    wsignal();
    if (r->flags & 8) break;    /* WT_EXECUTEONLYONCE */
    t0 = wnow_ms();
  }
  r->done = 1;
  wsignal();
  return 0;
}
static WINAPI BOOL RegisterWaitForSingleObject(HANDLE* out, HANDLE obj, void* cb, void* ctx, DWORD ms, DWORD fl) {
  typedef HANDLE(WINAPI * ct)(void*, size_t, void*, void*, DWORD, DWORD*);
  ct f = LOOK(ct, "CreateThread");
  struct regwait* r = (struct regwait*)wzalloc(sizeof(*r));
  HANDLE h;
  if (!r || !f || !out) return 0;
  r->obj = obj;
  r->cb = (void (WINAPI*)(void*, uint8_t))cb;
  r->ctx = ctx;
  r->ms = ms;
  r->flags = fl;
  h = f(0, 0, (void*)regwait_thread, r, 0, 0);
  if (!h) {
    k_free(r);
    return 0;
  }
  if (wine_debug >= 2) wlogx("espera registrada: objeto ", (uint64_t)(uintptr_t)obj);
  *out = (HANDLE)r; /* o handle da espera e o proprio registro (fica: a thread ainda o le) */
  return 1;
}
static WINAPI BOOL UnregisterWaitEx(HANDLE h, HANDLE ev) {
  typedef BOOL(WINAPI * se_fn)(HANDLE);
  struct regwait* r = (struct regwait*)h;
  if (!r) {
    wset_lasterr(6);
    return 0;
  }
  __atomic_store_n(&r->stop, 1, __ATOMIC_SEQ_CST);
  if (ev) {
    /* um callback em andamento termina antes (a nao ser que seja esta thread chamando de dentro dele) */
    struct wobj* me = wthread();
    int inside = me && r->tid && me->tid == r->tid;
    while (!inside && __atomic_load_n(&r->in_cb, __ATOMIC_SEQ_CST) && !r->done) {
      volatile int z = 0;
      k_futex_wait(&z, 0, 2);
    }
    if (ev != (HANDLE)(intptr_t)-1) {
      se_fn se = LOOK(se_fn, "SetEvent");
      if (se) se(ev);
    }
  }
  return 1;
}
static WINAPI BOOL UnregisterWait(HANDLE h) { return UnregisterWaitEx(h, 0); }

/* ---------- memoria ---------- */
static WINAPI DWORD DiscardVirtualMemory(void* a, size_t n) {
  (void)a;
  (void)n;
  return 0;
}
static WINAPI BOOL PrefetchVirtualMemory(HANDLE h, size_t n, void* e, DWORD fl) {
  (void)h; (void)n; (void)e; (void)fl;
  return 1;
}
static WINAPI BOOL VirtualUnlock(void* a, size_t n) {
  (void)a;
  (void)n;
  return 1;
}
static WINAPI BOOL VirtualFreeEx(HANDLE p, void* a, size_t n, DWORD t) {
  typedef BOOL(WINAPI * vf)(void*, size_t, DWORD);
  vf f = LOOK(vf, "VirtualFree");
  (void)p;
  return f ? f(a, n, t) : 0;
}
static WINAPI BOOL VirtualProtectEx(HANDLE p, void* a, size_t n, DWORD pr, DWORD* old) {
  typedef BOOL(WINAPI * vp)(void*, size_t, DWORD, DWORD*);
  vp f = LOOK(vp, "VirtualProtect");
  (void)p;
  return f ? f(a, n, pr, old) : 0;
}
static WINAPI BOOL HeapSetInformation(HANDLE h, int cls, void* v, size_t n) {
  (void)h; (void)cls; (void)v; (void)n;
  return 1;
}
static WINAPI size_t HeapCompact(HANDLE h, DWORD fl) {
  (void)h;
  (void)fl;
  return 0x100000;
}
static WINAPI BOOL HeapLock(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI BOOL HeapUnlock(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI BOOL HeapValidate(HANDLE h, DWORD fl, const void* p) {
  (void)h; (void)fl; (void)p;
  return 1;
}
static WINAPI BOOL HeapWalk(HANDLE h, void* e) {
  (void)h;
  (void)e;
  wset_lasterr(259); /* ERROR_NO_MORE_ITEMS */
  return 0;
}
static WINAPI DWORD GetProcessHeaps(DWORD n, HANDLE* out) {
  typedef HANDLE(WINAPI * gph)(void);
  gph f = LOOK(gph, "GetProcessHeap");
  if (n && out && f) out[0] = f();
  return 1;
}
struct procmem {
  DWORD cb, faults;
  size_t peak_ws, ws, peak_qpp, qpp, peak_qnp, qnp, pagefile, peak_pagefile, priv;
};
static WINAPI BOOL K32GetProcessMemoryInfo(HANDLE h, struct procmem* m, DWORD cb) {
  (void)h;
  if (!m || cb < 72) return 0;
  memset(m, 0, cb);
  m->cb = cb;
  m->ws = m->peak_ws = 64u << 20;
  m->pagefile = m->priv = 64u << 20;
  return 1;
}
static WINAPI BOOL K32GetPerformanceInfo(void* pi, DWORD cb) {
  if (!pi || cb < 4) return 0;
  memset(pi, 0, cb);
  *(DWORD*)pi = cb;
  return 1;
}
static WINAPI BOOL K32QueryWorkingSetEx(HANDLE h, void* pv, DWORD cb) {
  (void)h;
  if (pv && cb) memset(pv, 0, cb);
  return 1;
}

/* ---------- modulos (psapi) ---------- */
static WINAPI BOOL K32EnumProcessModules(HANDLE h, HANDLE* mods, DWORD cb, DWORD* need) {
  typedef HANDLE(WINAPI * gmh)(const wchar16*);
  gmh f = LOOK(gmh, "GetModuleHandleW");
  (void)h;
  if (need) *need = sizeof(HANDLE);
  if (mods && cb >= sizeof(HANDLE) && f) mods[0] = f(0);
  return 1;
}
struct modinfo {
  void* base;
  DWORD size;
  void* entry;
};
static WINAPI BOOL K32GetModuleInformation(HANDLE p, HANDLE m, struct modinfo* mi, DWORD cb) {
  struct wmod* w = pe_from_handle(m);
  (void)p;
  if (!w || !mi || cb < sizeof(*mi)) return 0;
  mi->base = w->base;
  mi->size = w->size;
  mi->entry = (void*)(uintptr_t)w->entry;
  return 1;
}
static WINAPI DWORD K32GetModuleBaseNameW(HANDLE p, HANDLE m, wchar16* buf, DWORD n) {
  struct wmod* w = pe_from_handle(m);
  int len;
  (void)p;
  if (!w || !buf || !n) return 0;
  len = wutf8_to_16(w->name, -1, 0, 0);
  if ((DWORD)len >= n) len = (int)n - 1;
  wutf8_to_16(w->name, -1, buf, len);
  buf[len] = 0;
  return (DWORD)len;
}
static WINAPI BOOL GetModuleHandleExA(DWORD fl, const char* name, HANDLE* out) {
  typedef BOOL(WINAPI * gme)(DWORD, const wchar16*, HANDLE*);
  gme f = LOOK(gme, "GetModuleHandleExW");
  wchar16* w = 0;
  BOOL r;
  if (!f) return 0;
  if (fl & 4) return f(fl, (const wchar16*)name, out); /* FROM_ADDRESS: e um endereco */
  w = name ? wdup8to16(name) : 0;
  r = f(fl, w, out);
  k_free(w);
  return r;
}

/* ---------- pacotes (AppModel): o processo nao roda "empacotado" ---------- */
#define APPMODEL_ERROR_NO_PACKAGE 15700
static WINAPI long GetCurrentPackageFullName(DWORD* n, wchar16* buf) {
  (void)n;
  (void)buf;
  return APPMODEL_ERROR_NO_PACKAGE;
}
static WINAPI long GetPackageFamilyName(HANDLE p, DWORD* n, wchar16* buf) {
  (void)p; (void)n; (void)buf;
  return APPMODEL_ERROR_NO_PACKAGE;
}
static WINAPI long GetPackagePathByFullName(const wchar16* full, DWORD* n, wchar16* buf) {
  (void)full; (void)n; (void)buf;
  return 1168; /* ERROR_NOT_FOUND */
}
static WINAPI long GetPackagesByPackageFamily(const wchar16* fam, DWORD* count, wchar16** names, DWORD* len,
                                              wchar16* buf) {
  (void)fam; (void)names; (void)buf;
  if (count) *count = 0;
  if (len) *len = 0;
  return 0;
}

/* ---------- idioma e regiao ---------- */
static WINAPI BOOL GetThreadPreferredUILanguages(DWORD fl, DWORD* num, wchar16* buf, DWORD* size) {
  static const wchar16 en[] = {'e', 'n', '-', 'U', 'S', 0, 0};
  (void)fl;
  if (num) *num = 1;
  if (!size) return 0;
  if (!buf || *size < 7) {
    *size = 7;
    if (buf) {
      wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    return 1;
  }
  memcpy(buf, en, sizeof(en));
  *size = 7;
  return 1;
}
static WINAPI int ResolveLocaleName(const wchar16* name, wchar16* out, int n) {
  static const wchar16 en[] = {'e', 'n', '-', 'U', 'S', 0};
  (void)name;
  if (!out || n < 6) return 6;
  memcpy(out, en, sizeof(en));
  return 6;
}
static WINAPI DWORD GetUserGeoID(DWORD cls) {
  (void)cls;
  return 244; /* Estados Unidos */
}
static WINAPI int GetGeoInfoW(DWORD geo, DWORD type, wchar16* buf, int n, uint16_t lang) {
  static const wchar16 us[] = {'U', 'S', 0};
  (void)geo; (void)type; (void)lang;
  if (!buf || n < 3) return 3;
  memcpy(buf, us, sizeof(us));
  return 3;
}
/* numero/moeda: devolve o valor como veio (sem separadores de milhar) */
static int copy_num(const wchar16* v, wchar16* out, int n) {
  int len = v ? (int)wstrlen16(v) : 0;
  if (!out || n <= 0) return len + 1;
  if (len + 1 > n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(out, v, (size_t)len * 2);
  out[len] = 0;
  return len + 1;
}
static WINAPI int GetNumberFormatEx(const wchar16* loc, DWORD fl, const wchar16* v, const void* f, wchar16* out,
                                    int n) {
  (void)loc; (void)fl; (void)f;
  return copy_num(v, out, n);
}
static WINAPI int GetCurrencyFormatEx(const wchar16* loc, DWORD fl, const wchar16* v, const void* f, wchar16* out,
                                      int n) {
  (void)loc; (void)fl; (void)f;
  return copy_num(v, out, n);
}
typedef BOOL(WINAPI* locw_fn)(wchar16*);
static WINAPI BOOL EnumSystemLocalesW(locw_fn fn, DWORD fl) {
  wchar16 id[] = {'0', '0', '0', '0', '0', '4', '0', '9', 0};
  (void)fl;
  if (fn) fn(id);
  return 1;
}

/* ---------- energia ---------- */
static WINAPI HANDLE PowerCreateRequest(void* ctx) {
  typedef HANDLE(WINAPI * ce)(void*, BOOL, BOOL, const wchar16*);
  ce f = LOOK(ce, "CreateEventW");
  (void)ctx;
  return f ? f(0, 1, 0, 0) : 0;
}
static WINAPI BOOL PowerSetRequest(HANDLE h, int t) {
  (void)h;
  (void)t;
  return 1;
}
static WINAPI BOOL PowerClearRequest(HANDLE h, int t) {
  (void)h;
  (void)t;
  return 1;
}

/* ---------- caminhos e cadeias ---------- */
static WINAPI BOOL NeedCurrentDirectoryForExePathW(const wchar16* exe) {
  (void)exe;
  return 1;
}
static WINAPI int lstrcmpiA(const char* a, const char* b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
}
const char* wine_dir(const char* rel);
static WINAPI DWORD GetWindowsDirectoryA(char* buf, DWORD n) {
  const char* d = wine_dir("");
  DWORD len = (DWORD)strlen(d);
  while (len && d[len - 1] == '\\') len--;
  if (!buf || len + 1 > n) return len + 1;
  memcpy(buf, d, len);
  buf[len] = 0;
  return len;
}

/* ---------- winmm: sem dispositivos de audio/midi ---------- */
#define MMSYSERR_NODRIVER 6
static WINAPI DWORD mm_zero(void) { return 0; }
static WINAPI DWORD mm_nodriver(void) { return MMSYSERR_NODRIVER; }


/* ---------- window station e desktop: um so de cada ("WinSta0\Default") ----
 * Os do processo sao valores fixos; os criados/abertos pelo programa (o
 * sandbox do Chromium cria um desktop alternativo) sao blocos com o nome. */
#define WS_PROC ((HANDLE)(uintptr_t)0x10040)
#define DESK_THREAD ((HANDLE)(uintptr_t)0x10044)
#define UOBJ_MAGIC 0x4A424F55u
struct uobj {
  DWORD magic;
  int desk; /* 0 window station, 1 desktop */
  wchar16 name[64];
};
static HANDLE uobj_new(int desk, const wchar16* name) {
  struct uobj* u = (struct uobj*)wzalloc(sizeof(*u));
  int i;
  if (!u) return 0;
  u->magic = UOBJ_MAGIC;
  u->desk = desk;
  for (i = 0; name && name[i] && i < 63; i++) u->name[i] = name[i];
  return (HANDLE)u;
}
static struct uobj* uobj_of(HANDLE h) {
  struct uobj* u = (struct uobj*)h;
  return (uintptr_t)h > 0x20000 && wptr_ok(u) && u->magic == UOBJ_MAGIC ? u : 0;
}
static WINAPI HANDLE GetProcessWindowStation(void) { return WS_PROC; }
static WINAPI HANDLE GetThreadDesktop(DWORD tid) {
  (void)tid;
  return DESK_THREAD;
}
static WINAPI BOOL SetProcessWindowStation(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI BOOL SetThreadDesktop(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI HANDLE CreateWindowStationW(const wchar16* name, DWORD fl, DWORD acc, void* sa) {
  (void)fl; (void)acc; (void)sa;
  return uobj_new(0, name);
}
static WINAPI HANDLE OpenWindowStationW(const wchar16* name, BOOL inh, DWORD acc) {
  (void)inh; (void)acc;
  return uobj_new(0, name);
}
static WINAPI HANDLE CreateDesktopW(const wchar16* name, const wchar16* dev, void* mode, DWORD fl, DWORD acc,
                                    void* sa) {
  (void)dev; (void)mode; (void)fl; (void)acc; (void)sa;
  return uobj_new(1, name);
}
static WINAPI HANDLE OpenDesktopW(const wchar16* name, DWORD fl, BOOL inh, DWORD acc) {
  (void)fl; (void)inh; (void)acc;
  return uobj_new(1, name);
}
static WINAPI HANDLE OpenInputDesktop(DWORD fl, BOOL inh, DWORD acc) {
  static const wchar16 def[] = {'D', 'e', 'f', 'a', 'u', 'l', 't', 0};
  (void)fl; (void)inh; (void)acc;
  return uobj_new(1, def);
}
static BOOL uobj_close(HANDLE h) {
  struct uobj* u = uobj_of(h);
  if (u) {
    u->magic = 0;
    k_free(u);
  }
  return 1;
}
static WINAPI BOOL CloseWindowStation(HANDLE h) { return uobj_close(h); }
static WINAPI BOOL CloseDesktop(HANDLE h) { return uobj_close(h); }
static WINAPI BOOL SwitchDesktop(HANDLE h) {
  (void)h;
  return 1;
}
static DWORD put_w(const wchar16* s, void* buf, DWORD len, DWORD* need) {
  DWORD n = ((DWORD)wstrlen16(s) + 1) * 2;
  if (need) *need = n;
  if (!buf || len < n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(buf, s, n);
  return 1;
}
static WINAPI BOOL GetUserObjectInformationW(HANDLE h, int idx, void* buf, DWORD len, DWORD* need) {
  static const wchar16 winsta[] = {'W', 'i', 'n', 'S', 't', 'a', '0', 0};
  static const wchar16 deflt[] = {'D', 'e', 'f', 'a', 'u', 'l', 't', 0};
  static const wchar16 t_ws[] = {'W', 'i', 'n', 'd', 'o', 'w', 'S', 't', 'a', 't', 'i', 'o', 'n', 0};
  static const wchar16 t_dk[] = {'D', 'e', 's', 'k', 't', 'o', 'p', 0};
  struct uobj* u = uobj_of(h);
  int desk = u ? u->desk : h == DESK_THREAD;
  switch (idx) {
    case 1: /* UOI_FLAGS {fInherit, fReserved, dwFlags} */
      if (need) *need = 12;
      if (!buf || len < 12) {
        wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
        return 0;
      }
      memset(buf, 0, 12);
      ((DWORD*)buf)[2] = 1; /* WSF_VISIBLE / DF_ALLOWOTHERACCOUNTHOOK */
      return 1;
    case 2: /* UOI_NAME */
      return put_w(u && u->name[0] ? u->name : desk ? deflt : winsta, buf, len, need) != 0;
    case 3: /* UOI_TYPE */
      return put_w(desk ? t_dk : t_ws, buf, len, need) != 0;
    case 4: /* UOI_USER_SID: nenhum */
      if (need) *need = 0;
      return 1;
    case 6: /* UOI_IO: e o desktop de entrada */
      if (need) *need = 4;
      if (!buf || len < 4) {
        wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
        return 0;
      }
      *(DWORD*)buf = 1;
      return 1;
  }
  wset_lasterr(ERROR_INVALID_PARAMETER);
  return 0;
}
static WINAPI BOOL GetUserObjectSecurity(HANDLE h, DWORD* what, void* sd, DWORD len, DWORD* need) {
  typedef BOOL(WINAPI * gks)(HANDLE, DWORD, void*, DWORD, DWORD*);
  gks f = LOOK(gks, "GetKernelObjectSecurity");
  return f ? f(h, what ? *what : 0, sd, len, need) : 0;
}
static WINAPI BOOL SetUserObjectSecurity(HANDLE h, DWORD* what, void* sd) {
  (void)h; (void)what; (void)sd;
  return 1;
}


/* ---------- ETW: provedores registrados, nenhum consumidor ligado ---------- */
static uint64_t etw_seq = 0x100;
static WINAPI DWORD EventRegister(const void* guid, void* cb, void* ctx, uint64_t* h) {
  (void)guid; (void)cb; (void)ctx;
  if (h) *h = __sync_add_and_fetch(&etw_seq, 1);
  return 0;
}
static WINAPI DWORD EventUnregister(uint64_t h) {
  (void)h;
  return 0;
}
static WINAPI BOOL EventEnabled(uint64_t h, const void* desc) {
  (void)h;
  (void)desc;
  return 0;
}
static WINAPI BOOL EventProviderEnabled(uint64_t h, uint8_t lvl, uint64_t kw) {
  (void)h; (void)lvl; (void)kw;
  return 0;
}
static WINAPI DWORD EventWrite(uint64_t h, const void* d, DWORD n, void* data) {
  (void)h; (void)d; (void)n; (void)data;
  return 0;
}
static WINAPI DWORD EventWriteTransfer(uint64_t h, const void* d, const void* a, const void* r, DWORD n, void* data) {
  (void)h; (void)d; (void)a; (void)r; (void)n; (void)data;
  return 0;
}
static WINAPI DWORD EventWriteEx(uint64_t h, const void* d, uint64_t f, DWORD fl, const void* a, const void* r,
                                 DWORD n, void* data) {
  (void)h; (void)d; (void)f; (void)fl; (void)a; (void)r; (void)n; (void)data;
  return 0;
}
static WINAPI DWORD EventWriteString(uint64_t h, uint8_t lvl, uint64_t kw, const wchar16* s) {
  (void)h; (void)lvl; (void)kw; (void)s;
  return 0;
}
static WINAPI DWORD EventSetInformation(uint64_t h, int cls, void* info, DWORD n) {
  (void)h; (void)cls; (void)info; (void)n;
  return 0;
}
static WINAPI DWORD EventActivityIdControl(DWORD code, void* guid) {
  (void)code;
  if (guid) memset(guid, 0, 16);
  return 0;
}

/* ---------- nome da thread e do computador ---------- */
static WINAPI long GetThreadDescription(HANDLE h, wchar16** out) {
  typedef void*(WINAPI * la_fn)(DWORD, size_t);
  la_fn la = LOOK(la_fn, "LocalAlloc");
  (void)h;
  if (!out) return (long)0x80070057;
  *out = la ? (wchar16*)la(0x40, 2) : 0; /* "" (o programa solta com LocalFree) */
  return *out ? 0 : (long)0x8007000E;
}
static WINAPI long SetThreadDescription(HANDLE h, const wchar16* d) {
  (void)h;
  (void)d;
  return 0;
}
static WINAPI int GetHostNameW(wchar16* name, int n) {
  static const wchar16 nm[] = {'s', 'o', 'd', 'o', 's', 0};
  if (!name || n < 6) return -1;
  memcpy(name, nm, sizeof(nm));
  return 0;
}
static WINAPI BOOL GetFileInformationByName(const wchar16* f, int cls, void* buf, DWORD n) {
  (void)f; (void)cls; (void)buf; (void)n;
  wset_lasterr(ERROR_NOT_SUPPORTED); /* o Chromium volta para GetFileAttributesEx */
  return 0;
}


/* ---------- topologia: N nucleos (1 thread cada), 1 pacote, 1 no, 1 grupo ---------- */
static int ncpu(void) {
  typedef void(WINAPI * gsi)(void*);
  uint8_t si[64];
  gsi f = LOOK(gsi, "GetSystemInfo");
  int n = 1;
  if (f) {
    f(si);
    n = (int)*(DWORD*)(si + 32);
  }
  return n < 1 ? 1 : n > 64 ? 64 : n;
}
static uint64_t all_mask(int n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }

/* um registro do formato Ex em out (devolve o tamanho) */
static DWORD lpi_ex_one(uint8_t* out, int rel, int i, int n) {
  DWORD size = rel == 2 ? 56 : rel == 4 ? 80 : 48;
  if (!out) return size;
  memset(out, 0, size);
  *(DWORD*)out = (DWORD)rel;
  *(DWORD*)(out + 4) = size;
  switch (rel) {
    case 0: /* nucleo */
    case 3: /* pacote */
      *(uint16_t*)(out + 8 + 22) = 1;
      *(uint64_t*)(out + 8 + 24) = rel == 0 ? (1ull << i) : all_mask(n);
      break;
    case 1: /* no NUMA 0 */
      *(uint16_t*)(out + 8 + 22) = 1;
      *(uint64_t*)(out + 8 + 24) = all_mask(n);
      break;
    case 2: { /* cache: i = 0 L1d, 1 L1i, 2 L2, 3 L3 */
      static const uint8_t lvl[] = {1, 1, 2, 3};
      static const DWORD sz[] = {32u << 10, 32u << 10, 512u << 10, 8u << 20};
      static const int type[] = {2, 1, 0, 0}; /* Data, Instruction, Unified */
      out[8] = lvl[i];
      out[9] = 8;
      *(uint16_t*)(out + 10) = 64;
      *(DWORD*)(out + 12) = sz[i];
      *(int*)(out + 16) = type[i];
      *(uint16_t*)(out + 8 + 38) = 1;
      *(uint64_t*)(out + 8 + 40) = all_mask(n);
      break;
    }
    case 4: /* grupo 0 */
      *(uint16_t*)(out + 8) = 1;
      *(uint16_t*)(out + 10) = 1;
      out[8 + 24] = (uint8_t)n;
      out[8 + 25] = (uint8_t)n;
      *(uint64_t*)(out + 8 + 64) = all_mask(n);
      break;
  }
  return size;
}
static WINAPI BOOL GetLogicalProcessorInformationEx(int want, uint8_t* buf, DWORD* len) {
  int n = ncpu(), rel, i, cnt;
  DWORD need = 0, at = 0;
  if (!len) return 0;
  for (rel = 0; rel <= 4; rel++) {
    if (want != 0xFFFF && want != rel) continue;
    cnt = rel == 0 ? n : rel == 2 ? 4 : 1;
    for (i = 0; i < cnt; i++) need += lpi_ex_one(0, rel, i, n);
  }
  if (!buf || *len < need) {
    *len = need;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  for (rel = 0; rel <= 4; rel++) {
    if (want != 0xFFFF && want != rel) continue;
    cnt = rel == 0 ? n : rel == 2 ? 4 : 1;
    for (i = 0; i < cnt; i++) at += lpi_ex_one(buf + at, rel, i, n);
  }
  *len = need;
  return 1;
}
/* formato antigo: {ULONG_PTR mask; int rel; union de 16 bytes} = 32 bytes */
static WINAPI BOOL GetLogicalProcessorInformation(uint8_t* buf, DWORD* len) {
  int n = ncpu(), i, k = 0;
  DWORD need = (DWORD)(n + 2 + 4) * 32;
  if (!len) return 0;
  if (!buf || *len < need) {
    *len = need;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memset(buf, 0, need);
  for (i = 0; i < n; i++, k++) { /* nucleos */
    *(uint64_t*)(buf + k * 32) = 1ull << i;
    *(int*)(buf + k * 32 + 8) = 0;
  }
  *(uint64_t*)(buf + k * 32) = all_mask(n); /* pacote */
  *(int*)(buf + k * 32 + 8) = 3;
  k++;
  *(uint64_t*)(buf + k * 32) = all_mask(n); /* no NUMA */
  *(int*)(buf + k * 32 + 8) = 1;
  k++;
  for (i = 0; i < 4; i++, k++) { /* caches */
    static const uint8_t lvl[] = {1, 1, 2, 3};
    static const DWORD sz[] = {32u << 10, 32u << 10, 512u << 10, 8u << 20};
    static const int type[] = {2, 1, 0, 0};
    uint8_t* e = buf + k * 32;
    *(uint64_t*)e = all_mask(n);
    *(int*)(e + 8) = 2;
    e[16] = lvl[i];
    e[17] = 8;
    *(uint16_t*)(e + 18) = 64;
    *(DWORD*)(e + 20) = sz[i];
    *(int*)(e + 24) = type[i];
  }
  *len = need;
  return 1;
}

/* suspender/retomar: o SODOS nao suspende; o registro so precisa existir */
static WINAPI HANDLE RegisterSuspendResumeNotification(HANDLE recipient, DWORD fl) {
  (void)recipient;
  (void)fl;
  return (HANDLE)(uintptr_t)0x10050;
}
static WINAPI BOOL UnregisterSuspendResumeNotification(HANDLE h) {
  (void)h;
  return 1;
}

/* tabelas de desenrolar para codigo gerado (JIT do V8): aceitas; o SEH por
 * tabela ainda nao as consulta */
static WINAPI DWORD RtlAddGrowableFunctionTable(void** table, void* list, DWORD n, DWORD max, uintptr_t lo,
                                                uintptr_t hi) {
  (void)list; (void)n; (void)max; (void)lo; (void)hi;
  if (table) *table = (void*)(uintptr_t)0x10060;
  return 0;
}
static WINAPI void RtlGrowFunctionTable(void* t, DWORD n) {
  (void)t;
  (void)n;
}
static WINAPI void RtlDeleteGrowableFunctionTable(void* t) { (void)t; }
static WINAPI long AppPolicyGetThreadInitializationType(HANDLE tok, int* type) {
  (void)tok;
  if (type) *type = 0; /* AppPolicyThreadInitializationType_None */
  return 0;
}


/* ---------- console: o que o libuv (Node) usa no stdin/stdout ----------
 * Entrada do console sem eventos pendentes; cursor, cores e titulo aceitos
 * (o console do SODOS e texto corrido). */
static WINAPI BOOL GetNumberOfConsoleInputEvents(HANDLE h, DWORD* n) {
  (void)h;
  if (n) *n = 0;
  return 1;
}
static WINAPI BOOL ReadConsoleInputW(HANDLE h, void* recs, DWORD n, DWORD* got) {
  (void)h; (void)recs; (void)n;
  if (got) *got = 0;
  return 1;
}
static WINAPI BOOL PeekConsoleInputW(HANDLE h, void* recs, DWORD n, DWORD* got) {
  (void)h; (void)recs; (void)n;
  if (got) *got = 0;
  return 1;
}
static WINAPI BOOL WriteConsoleInputW(HANDLE h, const void* recs, DWORD n, DWORD* done) {
  (void)h; (void)recs;
  if (done) *done = n;
  return 1;
}
static WINAPI BOOL FlushConsoleInputBuffer(HANDLE h) {
  (void)h;
  return 1;
}
struct cursor_info {
  DWORD size;
  BOOL visible;
};
static WINAPI BOOL GetConsoleCursorInfo(HANDLE h, struct cursor_info* c) {
  (void)h;
  if (c) {
    c->size = 25;
    c->visible = 1;
  }
  return 1;
}
static WINAPI BOOL SetConsoleCursorInfo(HANDLE h, const struct cursor_info* c) {
  (void)h;
  (void)c;
  return 1;
}
static WINAPI BOOL SetConsoleCursorPosition(HANDLE h, DWORD pos) {
  (void)h;
  (void)pos;
  return 1;
}
static WINAPI BOOL SetConsoleTextAttribute(HANDLE h, uint16_t a) {
  (void)h;
  (void)a;
  return 1;
}
static WINAPI BOOL FillConsoleOutputAttribute(HANDLE h, uint16_t a, DWORD n, DWORD pos, DWORD* done) {
  (void)h; (void)a; (void)pos;
  if (done) *done = n;
  return 1;
}
static WINAPI BOOL FillConsoleOutputCharacterW(HANDLE h, wchar16 c, DWORD n, DWORD pos, DWORD* done) {
  (void)h; (void)c; (void)pos;
  if (done) *done = n;
  return 1;
}
static WINAPI BOOL SetConsoleTitleW(const wchar16* t) {
  (void)t;
  return 1;
}
static WINAPI DWORD GetConsoleTitleW(wchar16* buf, DWORD n) {
  if (buf && n) buf[0] = 0;
  return 0;
}
static WINAPI BOOL ScrollConsoleScreenBufferW(HANDLE h, const void* a, const void* b, DWORD c, const void* d) {
  (void)h; (void)a; (void)b; (void)c; (void)d;
  return 1;
}
static WINAPI BOOL SetConsoleWindowInfo(HANDLE h, BOOL abs, const void* r) {
  (void)h; (void)abs; (void)r;
  return 1;
}

#define S(n) {#n, (void*)n}
const struct wine_sym procmisc_syms[] = {
    S(GetNumberOfConsoleInputEvents), S(ReadConsoleInputW), S(PeekConsoleInputW), S(WriteConsoleInputW),
    S(FlushConsoleInputBuffer), S(GetConsoleCursorInfo), S(SetConsoleCursorInfo), S(SetConsoleCursorPosition),
    S(SetConsoleTextAttribute), S(FillConsoleOutputAttribute), S(FillConsoleOutputCharacterW),
    S(SetConsoleTitleW), S(GetConsoleTitleW), S(ScrollConsoleScreenBufferW), S(SetConsoleWindowInfo),
    S(RtlAddGrowableFunctionTable), S(RtlGrowFunctionTable), S(RtlDeleteGrowableFunctionTable),
    S(AppPolicyGetThreadInitializationType),

    S(RegisterSuspendResumeNotification), S(UnregisterSuspendResumeNotification),
    S(GetLogicalProcessorInformationEx), S(GetLogicalProcessorInformation),
    S(EventRegister), S(EventUnregister), S(EventEnabled), S(EventProviderEnabled), S(EventWrite),
    S(EventWriteTransfer), S(EventWriteEx), S(EventWriteString), S(EventSetInformation), S(EventActivityIdControl),
    S(GetThreadDescription), S(SetThreadDescription), S(GetHostNameW), S(GetFileInformationByName),
    S(GetProcessWindowStation), S(GetThreadDesktop), S(SetProcessWindowStation), S(SetThreadDesktop),
    S(CreateWindowStationW), S(OpenWindowStationW), S(CreateDesktopW), S(OpenDesktopW), S(OpenInputDesktop),
    S(CloseWindowStation), S(CloseDesktop), S(SwitchDesktop), S(GetUserObjectInformationW),
    S(GetUserObjectSecurity), S(SetUserObjectSecurity),

    S(GetProcessMitigationPolicy), S(SetProcessMitigationPolicy), S(GetProcessInformation),
    S(SetProcessInformation), S(SetThreadInformation), S(GetCurrentProcessorNumber), S(GetCurrentThreadStackLimits),
    S(GetThreadTimes), S(QueryThreadCycleTime), S(QueryProcessCycleTime), S(GetProcessIoCounters),
    S(GetProcessHandleCount), S(GetThreadPriorityBoost), S(SetThreadPriorityBoost), S(GetThreadGroupAffinity),
    S(IsThreadAFiber), S(CheckRemoteDebuggerPresent), S(OpenThread), S(SetProcessShutdownParameters),
    S(RegisterApplicationRestart), S(WerRegisterRuntimeExceptionModule), S(RtlGetLastNtStatus),
    S(RtlCaptureStackBackTrace), S(RtlDeleteFunctionTable), S(VerSetConditionMask), S(VerifyVersionInfoW),
    S(InitOnceBeginInitialize), S(InitOnceComplete), S(TryAcquireSRWLockShared), S(CreateJobObjectW),
    S(AssignProcessToJobObject), S(SetInformationJobObject), S(QueryInformationJobObject), S(TerminateJobObject),
    S(InitializeProcThreadAttributeList), S(UpdateProcThreadAttribute), S(DeleteProcThreadAttributeList),
    S(RegisterWaitForSingleObject), S(UnregisterWaitEx), S(UnregisterWait), S(DiscardVirtualMemory),
    S(PrefetchVirtualMemory), S(VirtualUnlock), S(VirtualFreeEx), S(VirtualProtectEx), S(HeapSetInformation),
    S(HeapCompact), S(HeapLock), S(HeapUnlock), S(HeapValidate), S(HeapWalk), S(GetProcessHeaps),
    S(K32GetProcessMemoryInfo), S(K32GetPerformanceInfo), S(K32QueryWorkingSetEx), S(K32EnumProcessModules),
    S(K32GetModuleInformation), S(K32GetModuleBaseNameW), S(GetModuleHandleExA), S(GetCurrentPackageFullName),
    S(GetPackageFamilyName), S(GetPackagePathByFullName), S(GetPackagesByPackageFamily),
    S(GetThreadPreferredUILanguages), S(ResolveLocaleName), S(GetUserGeoID), S(GetGeoInfoW), S(GetNumberFormatEx),
    S(GetCurrencyFormatEx), S(EnumSystemLocalesW), S(PowerCreateRequest), S(PowerSetRequest), S(PowerClearRequest),
    S(NeedCurrentDirectoryForExePathW), S(lstrcmpiA), S(GetWindowsDirectoryA),
    {"GetProcessMemoryInfo", (void*)K32GetProcessMemoryInfo}, {"EnumProcessModules", (void*)K32EnumProcessModules},
    {"GetModuleInformation", (void*)K32GetModuleInformation}, {"GetModuleBaseNameW", (void*)K32GetModuleBaseNameW},
    {"waveOutGetNumDevs", (void*)mm_zero}, {"waveInGetNumDevs", (void*)mm_zero}, {"midiInGetNumDevs", (void*)mm_zero},
    {"midiOutGetNumDevs", (void*)mm_zero}, {"waveOutOpen", (void*)mm_nodriver}, {"midiInOpen", (void*)mm_nodriver},
    {"midiOutOpen", (void*)mm_nodriver}, {"midiInGetDevCapsW", (void*)mm_nodriver},
    {"midiOutGetDevCapsW", (void*)mm_nodriver}, {0, 0}};
