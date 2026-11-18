/*
 * k32.c - KERNEL32/KERNELBASE, NTDLL e USER32 embutidos (W0: console).
 *
 * O minimo que o CRT do MinGW/zig e programas de console pedem. O que nao
 * esta aqui vira stub (pe.c) e so derruba o programa se for chamado.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"


#define H_STDIN ((HANDLE)(uintptr_t)0x10)
#define H_STDOUT ((HANDLE)(uintptr_t)0x14)
#define H_STDERR ((HANDLE)(uintptr_t)0x18)
#define H_PROCESS ((HANDLE)(intptr_t)-1)
#define H_THREAD ((HANDLE)(intptr_t)-2)
#define H_HEAP ((HANDLE)(uintptr_t)1)


/* ---------- processo / modulos ---------- */
static WINAPI __attribute__((noreturn)) void ExitProcess(DWORD code) { wexit((int)code); }
static WINAPI BOOL TerminateProcess(HANDLE h, DWORD code) {
  if (h == H_PROCESS) wexit((int)code);
  return 0;
}
static WINAPI HANDLE GetCurrentProcess(void) { return H_PROCESS; }
static WINAPI HANDLE GetCurrentThread(void) { return H_THREAD; }
static WINAPI DWORD GetCurrentProcessId(void) { return (DWORD) * (uint64_t*)(wteb() + TEB_PID); }
static WINAPI DWORD GetCurrentThreadId(void) { return (DWORD) * (uint64_t*)(wteb() + TEB_TID); }
static WINAPI DWORD GetLastError(void) { return wget_lasterr(); }
static WINAPI void SetLastError(DWORD e) { wset_lasterr(e); }
static WINAPI char* GetCommandLineA(void) { return wp()->cmdline; }
static WINAPI wchar16* GetCommandLineW(void) { return wp()->wcmdline; }

static WINAPI HANDLE GetModuleHandleA(const char* name) {
  struct wmod* m;
  if (!name) return wp()->exe ? wp()->exe->base : 0;
  m = pe_find(name);
  if (!m) {
    /* kernel32, ntdll... "sempre carregadas" no Windows: as embutidas sim */
    char n[WMOD_NAME];
    char* dot;
    wbase(n, name, (int)sizeof(n));
    dot = strrchr(n, '.');
    if (dot && wstreq(dot, ".dll")) *dot = 0;
    if (builtin_known(n)) m = pe_load(name, 0);
  }
  if (!m) wset_lasterr(ERROR_MOD_NOT_FOUND);
  return m ? pe_handle(m) : 0;
}
static WINAPI HANDLE GetModuleHandleW(const wchar16* name) {
  char* n;
  HANDLE h;
  if (!name) return GetModuleHandleA(0);
  n = wdup16to8(name);
  h = GetModuleHandleA(n);
  k_free(n);
  return h;
}
static WINAPI BOOL GetModuleHandleExW(DWORD flags, const wchar16* name, HANDLE* out) {
  if (flags & 4) { /* FROM_ADDRESS */
    struct wmod* m;
    uintptr_t a = (uintptr_t)name;
    for (m = wp()->mods; m; m = m->next)
      if (m->base && a >= (uintptr_t)m->base && a < (uintptr_t)m->base + m->size) {
        *out = m->base;
        return 1;
      }
    *out = 0;
    return 0;
  }
  *out = GetModuleHandleW(name);
  return *out != 0;
}
static WINAPI HANDLE LoadLibraryA(const char* name) {
  struct wmod* m = pe_load(name, 0);
  if (!m) {
    wset_lasterr(ERROR_MOD_NOT_FOUND);
    return 0;
  }
  return pe_handle(m);
}
static WINAPI HANDLE LoadLibraryW(const wchar16* name) {
  char* n = wdup16to8(name);
  HANDLE h = n ? LoadLibraryA(n) : 0;
  k_free(n);
  return h;
}
static WINAPI HANDLE LoadLibraryExA(const char* name, HANDLE f, DWORD fl) {
  (void)f;
  (void)fl;
  return LoadLibraryA(name);
}
static WINAPI HANDLE LoadLibraryExW(const wchar16* name, HANDLE f, DWORD fl) {
  (void)f;
  (void)fl;
  return LoadLibraryW(name);
}
static WINAPI BOOL FreeLibrary(HANDLE h) {
  (void)h;
  return 1; /* descarrega tudo no fim do processo */
}
static WINAPI void* GetProcAddress(HANDLE h, const char* name) {
  struct wmod* m = pe_from_handle(h);
  void* f = m ? pe_getproc(m, name) : 0;
  if (!f) {
    if (wine_debug && (uintptr_t)name > 0xFFFF) wlog("GetProcAddress: falta ", name);
    wset_lasterr(ERROR_PROC_NOT_FOUND);
  }
  return f;
}
static WINAPI DWORD GetModuleFileNameA(HANDLE h, char* buf, DWORD n) {
  struct wmod* m = pe_from_handle(h);
  const char* s = m ? (m->path[0] ? m->path : m->name) : "";
  DWORD len = (DWORD)strlen(s);
  if (!n) return 0;
  if (len >= n) {
    memcpy(buf, s, n - 1);
    buf[n - 1] = 0;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return n;
  }
  memcpy(buf, s, len + 1);
  return len;
}
static WINAPI DWORD GetModuleFileNameW(HANDLE h, wchar16* buf, DWORD n) {
  char tmp[260];
  DWORD len = GetModuleFileNameA(h, tmp, sizeof(tmp));
  int w;
  if (!n) return 0;
  w = wutf8_to_16(tmp, (int)len, buf, (int)n - 1);
  buf[w] = 0;
  return (DWORD)w;
}

/* ---------- console / arquivos (W0: so console) ---------- */
static WINAPI HANDLE GetStdHandle(DWORD which) {
  switch (which) {
    case (DWORD)-10: return H_STDIN;
    case (DWORD)-11: return H_STDOUT;
    case (DWORD)-12: return H_STDERR;
  }
  wset_lasterr(ERROR_INVALID_HANDLE);
  return (HANDLE)(intptr_t)-1;
}
static WINAPI BOOL GetConsoleMode(HANDLE h, DWORD* mode) {
  if (h != H_STDIN && h != H_STDOUT && h != H_STDERR) return 0;
  if (mode) *mode = 3;
  return 1;
}
static WINAPI BOOL SetConsoleMode(HANDLE h, DWORD mode) {
  (void)h;
  (void)mode;
  return 1;
}
static WINAPI DWORD GetConsoleCP(void) { return 65001; }
static WINAPI DWORD GetConsoleOutputCP(void) { return 65001; }
static WINAPI BOOL SetConsoleOutputCP(DWORD cp) {
  (void)cp;
  return 1;
}
static WINAPI BOOL SetConsoleCtrlHandler(void* fn, BOOL add) {
  (void)fn;
  (void)add;
  return 1;
}
/* ---------- memoria ---------- */
static WINAPI HANDLE GetProcessHeap(void) { return H_HEAP; }
static WINAPI HANDLE HeapCreate(DWORD o, size_t a, size_t b) {
  (void)o;
  (void)a;
  (void)b;
  return H_HEAP;
}
static WINAPI BOOL HeapDestroy(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI void* HeapAlloc(HANDLE h, DWORD fl, size_t n) {
  void* p;
  (void)h;
  p = k_malloc((unsigned)(n ? n : 1));
  if (p && (fl & 8)) memset(p, 0, n);
  if (!p) wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
  return p;
}
static WINAPI BOOL HeapFree(HANDLE h, DWORD fl, void* p) {
  (void)h;
  (void)fl;
  if (p) k_free(p);
  return 1;
}
static WINAPI void* HeapReAlloc(HANDLE h, DWORD fl, void* p, size_t n) {
  size_t old = p ? k_msize(p) : 0;
  void* q;
  (void)h;
  q = k_realloc(p, (unsigned)(n ? n : 1));
  if (q && (fl & 8) && n > old) memset((char*)q + old, 0, n - old);
  return q;
}
static WINAPI size_t HeapSize(HANDLE h, DWORD fl, const void* p) {
  (void)h;
  (void)fl;
  return k_msize((void*)p);
}
static WINAPI void* LocalAlloc(DWORD fl, size_t n) { return HeapAlloc(H_HEAP, (fl & 0x40) ? 8 : 0, n); }
static WINAPI void* LocalFree(void* p) {
  if (p) k_free(p);
  return 0;
}

/* ---------- sincronizacao ---------- */
struct critsec {
  void* debug;
  volatile int lock;      /* LockCount: palavra do futex */
  int recursion;
  volatile uintptr_t owner;
  HANDLE sem;
  uintptr_t spin;
};
static WINAPI void InitializeCriticalSection(struct critsec* c) { memset(c, 0, sizeof(*c)); }
static WINAPI BOOL InitializeCriticalSectionAndSpinCount(struct critsec* c, DWORD s) {
  InitializeCriticalSection(c);
  c->spin = s;
  return 1;
}
static WINAPI BOOL InitializeCriticalSectionEx(struct critsec* c, DWORD s, DWORD f) {
  (void)f;
  return InitializeCriticalSectionAndSpinCount(c, s);
}
static WINAPI void DeleteCriticalSection(struct critsec* c) { (void)c; }
static WINAPI BOOL TryEnterCriticalSection(struct critsec* c) {
  uintptr_t me = (uintptr_t)wteb();
  if (c->owner == me) {
    c->recursion++;
    return 1;
  }
  if (__sync_val_compare_and_swap(&c->lock, 0, 1) != 0) return 0;
  c->owner = me;
  c->recursion = 1;
  return 1;
}
static WINAPI void EnterCriticalSection(struct critsec* c) {
  uintptr_t me = (uintptr_t)wteb();
  if (c->owner == me) {
    c->recursion++;
    return;
  }
  while (__sync_val_compare_and_swap(&c->lock, 0, 1) != 0) {
    __sync_val_compare_and_swap(&c->lock, 1, 2);
    k_futex_wait(&c->lock, 2, -1);
  }
  c->owner = me;
  c->recursion = 1;
}
static WINAPI void LeaveCriticalSection(struct critsec* c) {
  if (--c->recursion > 0) return;
  c->owner = 0;
  if (__sync_lock_test_and_set(&c->lock, 0) == 2) k_futex_wake(&c->lock, 1);
}
static WINAPI void InitializeSRWLock(void** l) { *l = 0; }
static WINAPI void AcquireSRWLockExclusive(void** l) {
  while (__sync_val_compare_and_swap((volatile intptr_t*)l, 0, 1) != 0) k_thread_yield();
}
static WINAPI void ReleaseSRWLockExclusive(void** l) { __sync_lock_release((volatile intptr_t*)l); }

/* TLS do Windows: TEB->TlsSlots[64]. */
static WINAPI DWORD TlsAlloc(void) {
  struct wproc* p = wp();
  DWORD i;
  wlock();
  for (i = 0; i < TLS_SLOTS; i++)
    if (!p->tls_used[i]) {
      p->tls_used[i] = 1;
      wunlock();
      ((void**)(wteb() + TEB_TLSSLOTS))[i] = 0;
      return i;
    }
  wunlock();
  return 0xFFFFFFFFu;
}
static WINAPI BOOL TlsFree(DWORD i) {
  if (i >= TLS_SLOTS) return 0;
  wp()->tls_used[i] = 0;
  return 1;
}
static WINAPI void* TlsGetValue(DWORD i) {
  wset_lasterr(0);
  return i < TLS_SLOTS ? ((void**)(wteb() + TEB_TLSSLOTS))[i] : 0;
}
static WINAPI BOOL TlsSetValue(DWORD i, void* v) {
  if (i >= TLS_SLOTS) return 0;
  ((void**)(wteb() + TEB_TLSSLOTS))[i] = v;
  return 1;
}

static WINAPI DWORD GetTickCount(void) { return k_ms(); }
static WINAPI uint64_t GetTickCount64(void) { return k_ms(); }
static WINAPI BOOL QueryPerformanceFrequency(int64_t* f) {
  *f = 1000;
  return 1;
}
static WINAPI BOOL QueryPerformanceCounter(int64_t* c) {
  *c = k_ms();
  return 1;
}
/* FILETIME: 100 ns desde 1601-01-01 (UTC). */
static uint64_t days_from_civil(int y, unsigned m, unsigned d) {
  int era;
  unsigned yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint64_t)(era * 146097 + (int)doe - 719468); /* dias desde 1970 */
}
uint64_t wfiletime_now(void);
static WINAPI void GetSystemTimeAsFileTime(uint64_t* ft) { *ft = wfiletime_now(); }
uint64_t wfiletime_now(void) {
  uint64_t v;
  uint64_t* ft = &v;
  int d = k_date(), t = k_time();
  uint64_t secs;
  if (d <= 0) d = 20260101;
  if (t < 0) t = 0;
  secs = days_from_civil(d / 10000, (unsigned)(d / 100 % 100), (unsigned)(d % 100)) * 86400u +
         (uint64_t)(t / 10000) * 3600u + (uint64_t)(t / 100 % 100) * 60u + (uint64_t)(t % 100);
  *ft = (secs + 11644473600ull) * 10000000ull + (uint64_t)(k_ms() % 1000) * 10000u;
  return v;
}
static WINAPI void GetSystemTimePreciseAsFileTime(uint64_t* ft) { GetSystemTimeAsFileTime(ft); }

/* ---------- ambiente / sistema ---------- */
/* Ambiente do processo Windows: um conjunto fixo (pastas dentro de D:\WINE)
 * e, antes dele, as variaveis do SODOS (SET), para GODEBUG, TEMP proprio... */
const char* wine_dir(const char* rel);
struct env_fix {
  const char* name;
  const char* val;
  int dir;   /* 1 = val e uma pasta relativa a D:\WINE */
};
static const struct env_fix env_fixed[] = {
    {"SystemDrive", "D:", 0},
    {"SystemRoot", "", 1},
    {"windir", "", 1},
    {"TEMP", "TEMP", 1},
    {"TMP", "TEMP", 1},
    {"USERPROFILE", "USERS\\SODOS", 1},
    {"HOMEDRIVE", "D:", 0},
    {"HOMEPATH", "\\WINE\\USERS\\SODOS", 0},
    {"APPDATA", "USERS\\SODOS\\APPDATA\\ROAMING", 1},
    {"LOCALAPPDATA", "USERS\\SODOS\\APPDATA\\LOCAL", 1},
    {"ProgramData", "PROGRAMDATA", 1},
    {"ProgramFiles", "PROGRAM FILES", 1},
    {"USERNAME", "sodos", 0},
    {"COMPUTERNAME", "SODOS", 0},
    {"OS", "Windows_NT", 0},
    {"PROCESSOR_ARCHITECTURE", "AMD64", 0},
    {"NUMBER_OF_PROCESSORS", "1", 0},
    {"PATHEXT", ".COM;.EXE;.BAT;.CMD", 0},
    {"PATH", "SYSTEM32", 1},
    {0, 0, 0}};

static int env_get(const char* name, char* out, int max) {
  int i, len;
  /* PATH do SODOS tem outro formato: fica o do conjunto fixo */
  if (!wstreq(name, "PATH")) {
    len = k_getenv(name, out, max);
    if (len >= 0) return len;
  }
  for (i = 0; env_fixed[i].name; i++) {
    const char* v;
    if (!wstreq(name, env_fixed[i].name)) continue;
    v = env_fixed[i].dir ? wine_dir(env_fixed[i].val) : env_fixed[i].val;
    len = (int)strlen(v);
    if (len >= max) len = max - 1;
    memcpy(out, v, (size_t)len);
    out[len] = 0;
    return len;
  }
  return -1;
}
static WINAPI DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD n) {
  char tmp[300];
  int len = env_get(name, tmp, (int)sizeof(tmp));
  if (len < 0) {
    wset_lasterr(ERROR_ENVVAR_NOT_FOUND);
    return 0;
  }
  if ((DWORD)len + 1 > n) return (DWORD)len + 1;
  memcpy(buf, tmp, (size_t)len + 1);
  return (DWORD)len;
}
static WINAPI DWORD GetEnvironmentVariableW(const wchar16* name, wchar16* buf, DWORD n) {
  char* nm = wdup16to8(name);
  char tmp[300];
  int len = nm ? env_get(nm, tmp, (int)sizeof(tmp)) : -1;
  int w;
  k_free(nm);
  if (len < 0) {
    wset_lasterr(ERROR_ENVVAR_NOT_FOUND);
    return 0;
  }
  w = wutf8_to_16(tmp, len, 0, 0);
  if ((DWORD)w + 1 > n) return (DWORD)w + 1;
  wutf8_to_16(tmp, len, buf, w);
  buf[w] = 0;
  return (DWORD)w;
}
static WINAPI BOOL SetEnvironmentVariableA(const char* name, const char* v) {
  return k_setenv(name, v ? v : "") == 0;
}
static WINAPI BOOL SetEnvironmentVariableW(const wchar16* name, const wchar16* v) {
  char* n8 = wdup16to8(name);
  char* v8 = v ? wdup16to8(v) : 0;
  BOOL r = n8 ? k_setenv(n8, v8 ? v8 : "") == 0 : 0;
  k_free(n8);
  k_free(v8);
  return r;
}
/* Bloco "NOME=valor\0...\0\0": o conjunto fixo e as variaveis do Go do SODOS. */
static size_t env_add(char* b, size_t o, const char* name) {
  char tmp[300];
  int len = env_get(name, tmp, (int)sizeof(tmp));
  size_t nl = strlen(name);
  if (len < 0 || o + nl + (size_t)len + 4 > 4096) return o;
  memcpy(b + o, name, nl);
  o += nl;
  b[o++] = '=';
  memcpy(b + o, tmp, (size_t)len + 1);
  return o + (size_t)len + 1;
}
static char* env_block(size_t* size) {
  char* b = (char*)wzalloc(4096);
  size_t o = 0;
  int i;
  static const char* const extra[] = {"GODEBUG", "GOMAXPROCS", "GOTRACEBACK", 0};
  if (!b) return 0;
  for (i = 0; env_fixed[i].name; i++) o = env_add(b, o, env_fixed[i].name);
  for (i = 0; extra[i]; i++) o = env_add(b, o, extra[i]);
  b[o++] = 0;
  *size = o;
  return b;
}
static WINAPI char* GetEnvironmentStringsA(void) {
  size_t n;
  return env_block(&n);
}
static WINAPI wchar16* GetEnvironmentStringsW(void) {
  size_t n = 0, i;
  char* b = env_block(&n);
  wchar16* w = (wchar16*)wzalloc((n + 2) * 2);
  if (b && w)
    for (i = 0; i < n; i++) w[i] = (uint8_t)b[i];
  k_free(b);
  return w;
}
/* %NOME% -> valor */
static WINAPI DWORD ExpandEnvironmentStringsW(const wchar16* src, wchar16* dst, DWORD n) {
  char* s8 = wdup16to8(src);
  char out[900];
  size_t o = 0;
  const char* s;
  int w;
  if (!s8) return 0;
  for (s = s8; *s && o < sizeof(out) - 301;) {
    const char* e = *s == '%' ? strchr(s + 1, '%') : 0;
    if (e && e - s < 64) {
      char name[64], val[300];
      int len;
      memcpy(name, s + 1, (size_t)(e - s - 1));
      name[e - s - 1] = 0;
      len = env_get(name, val, (int)sizeof(val));
      if (len >= 0) {
        memcpy(out + o, val, (size_t)len);
        o += (size_t)len;
        s = e + 1;
        continue;
      }
    }
    out[o++] = *s++;
  }
  out[o] = 0;
  k_free(s8);
  w = wutf8_to_16(out, -1, 0, 0);
  if ((DWORD)w + 1 <= n) {
    wutf8_to_16(out, -1, dst, w);
    dst[w] = 0;
  }
  return (DWORD)w + 1;
}
static WINAPI BOOL FreeEnvironmentStringsA(char* p) {
  k_free(p);
  return 1;
}
static WINAPI BOOL FreeEnvironmentStringsW(wchar16* p) {
  k_free(p);
  return 1;
}
struct startupinfo {
  DWORD cb;
  uint8_t rest[100];
};
static WINAPI void GetStartupInfoA(struct startupinfo* s) {
  memset(s, 0, sizeof(*s));
  s->cb = 104;
}
static WINAPI void GetStartupInfoW(struct startupinfo* s) { GetStartupInfoA(s); }
struct sysinfo {
  uint16_t arch, res;
  DWORD page;
  void* minaddr;
  void* maxaddr;
  uintptr_t mask;
  DWORD ncpu, type, gran;
  uint16_t level, rev;
};
static WINAPI void GetSystemInfo(struct sysinfo* s) {
  memset(s, 0, sizeof(*s));
  s->arch = 9; /* AMD64 */
  s->page = 0x1000;
  s->minaddr = (void*)0x10000;
  s->maxaddr = (void*)0x7FFFFFFEFFFFull;
  s->mask = 1;
  s->ncpu = 1;
  s->type = 8664;
  s->gran = 0x10000;
  s->level = 6;
}
static WINAPI void GetNativeSystemInfo(struct sysinfo* s) { GetSystemInfo(s); }
static WINAPI BOOL IsProcessorFeaturePresent(DWORD f) {
  /* 10 SSE2, 6 SSE, 17 NX, 23 FASTFAIL... diz sim so para SSE/SSE2/SSE3 */
  return f == 6 || f == 10 || f == 13;
}
static WINAPI BOOL IsDebuggerPresent(void) { return 0; }
static WINAPI DWORD GetVersion(void) { return 0x23F00206u; } /* 6.2 build 9200 */
static WINAPI uint16_t GetACP(void) { return 65001; }
static WINAPI uint16_t GetOEMCP(void) { return 437; }
static WINAPI BOOL IsValidCodePage(DWORD cp) { return cp == 65001 || cp == 437 || cp == 1252 || cp == 0; }

/* ---------- strings ---------- */
static WINAPI int MultiByteToWideChar(DWORD cp, DWORD fl, const char* s, int n, wchar16* d, int max) {
  int len;
  (void)cp;
  (void)fl;
  if (n < 0) n = (int)strlen(s) + 1;
  len = wutf8_to_16(s, n, 0, 0);
  if (!max) return len;
  if (len > max) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  return wutf8_to_16(s, n, d, max);
}
static WINAPI int WideCharToMultiByte(DWORD cp, DWORD fl, const wchar16* s, int n, char* d, int max,
                                      const char* defch, BOOL* used) {
  int len;
  (void)cp;
  (void)fl;
  (void)defch;
  if (used) *used = 0;
  if (n < 0) n = (int)wstrlen16(s) + 1;
  len = wutf16_to_8(s, n, 0, 0);
  if (!max) return len;
  if (len > max) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  return wutf16_to_8(s, n, d, max);
}
/* FormatMessageW: so mensagens do sistema, texto generico com o numero. */
static WINAPI DWORD FormatMessageW(DWORD fl, const void* src, DWORD id, DWORD lang, wchar16* buf, DWORD n,
                                   void* args) {
  char txt[64];
  static const char hex[] = "0123456789";
  int i = 0, k, w;
  char num[12];
  DWORD v = id;
  const char* pre = "Erro do Windows ";
  (void)src;
  (void)lang;
  (void)args;
  for (k = 0; pre[k]; k++) txt[i++] = pre[k];
  k = 0;
  do {
    num[k++] = hex[v % 10];
    v /= 10;
  } while (v);
  while (k) txt[i++] = num[--k];
  txt[i++] = '.';
  txt[i] = 0;
  w = wutf8_to_16(txt, -1, 0, 0);
  if (fl & 0x100) { /* ALLOCATE_BUFFER */
    wchar16* out = wdup8to16(txt);
    *(wchar16**)buf = out;
    return out ? (DWORD)w : 0;
  }
  if ((DWORD)w + 1 > n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  wutf8_to_16(txt, -1, buf, w);
  buf[w] = 0;
  return (DWORD)w;
}
static WINAPI wchar16* lstrcpyW(wchar16* d, const wchar16* s) {
  wchar16* r = d;
  while ((*d++ = *s++)) {
  }
  return r;
}
static WINAPI wchar16* lstrcatW(wchar16* d, const wchar16* s) {
  lstrcpyW(d + wstrlen16(d), s);
  return d;
}
static WINAPI wchar16* lstrcpynW(wchar16* d, const wchar16* s, int n) {
  int i = 0;
  if (n <= 0) return d;
  while (i < n - 1 && s[i]) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
  return d;
}
static WINAPI int lstrcmpiW(const wchar16* a, const wchar16* b) {
  for (;; a++, b++) {
    wchar16 x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (wchar16)(x - 32);
    if (y >= 'a' && y <= 'z') y = (wchar16)(y - 32);
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
}
static WINAPI int lstrcmpW(const wchar16* a, const wchar16* b) {
  for (;; a++, b++) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (!*a) return 0;
  }
}
static WINAPI int lstrlenA(const char* s) { return s ? (int)strlen(s) : 0; }
static WINAPI int lstrlenW(const wchar16* s) { return s ? (int)wstrlen16(s) : 0; }

static WINAPI void InitializeSListHead(void* h) { memset(h, 0, 16); }
static WINAPI DWORD FlsAlloc(void* cb) {
  (void)cb;
  return TlsAlloc();
}
static WINAPI void* FlsGetValue(DWORD i) { return TlsGetValue(i); }
static WINAPI BOOL FlsSetValue(DWORD i, void* v) { return TlsSetValue(i, v); }
static WINAPI BOOL FlsFree(DWORD i) { return TlsFree(i); }
static WINAPI uint64_t EncodePointer(uint64_t p) { return p; }
static WINAPI uint64_t DecodePointer(uint64_t p) { return p; }

#define S(n) {#n, (void*)n}

const struct wine_sym k32_syms[] = {
    S(ExitProcess), S(TerminateProcess), S(GetCurrentProcess), S(GetCurrentThread),
    S(GetCurrentProcessId), S(GetCurrentThreadId), S(GetLastError), S(SetLastError),
    S(GetCommandLineA), S(GetCommandLineW), S(GetModuleHandleA), S(GetModuleHandleW),
    S(GetModuleHandleExW), S(LoadLibraryA), S(LoadLibraryW), S(LoadLibraryExA), S(LoadLibraryExW),
    S(FreeLibrary), S(GetProcAddress), S(GetModuleFileNameA), S(GetModuleFileNameW),
    S(GetStdHandle), 
    S(GetConsoleMode), S(SetConsoleMode), S(GetConsoleCP), S(GetConsoleOutputCP),
    S(SetConsoleOutputCP), S(SetConsoleCtrlHandler),
    S(GetProcessHeap), S(HeapCreate), S(HeapDestroy), S(HeapAlloc), S(HeapFree), S(HeapReAlloc),
    S(HeapSize), S(LocalAlloc), S(LocalFree),  S(InitializeCriticalSection), S(InitializeCriticalSectionAndSpinCount),
    S(InitializeCriticalSectionEx), S(DeleteCriticalSection), S(TryEnterCriticalSection),
    S(EnterCriticalSection), S(LeaveCriticalSection), S(InitializeSRWLock),
    S(AcquireSRWLockExclusive), S(ReleaseSRWLockExclusive), S(TlsAlloc), S(TlsFree),
    S(TlsGetValue), S(TlsSetValue),  S(GetTickCount), S(GetTickCount64),
    S(QueryPerformanceFrequency), S(QueryPerformanceCounter), S(GetSystemTimeAsFileTime),
    S(GetSystemTimePreciseAsFileTime), S(GetEnvironmentVariableA), S(GetEnvironmentVariableW),
    S(SetEnvironmentVariableA), S(SetEnvironmentVariableW), S(ExpandEnvironmentStringsW), S(GetEnvironmentStringsA), S(GetEnvironmentStringsW),
    S(FreeEnvironmentStringsA), S(FreeEnvironmentStringsW), S(GetStartupInfoA),
    S(GetStartupInfoW), S(GetSystemInfo), S(GetNativeSystemInfo), S(IsProcessorFeaturePresent),
    S(IsDebuggerPresent),  S(GetVersion), S(GetACP), S(GetOEMCP), S(IsValidCodePage),
    S(MultiByteToWideChar), S(FormatMessageW), S(lstrcpyW), S(lstrcatW), S(lstrcpynW), S(lstrcmpiW), S(lstrcmpW), S(WideCharToMultiByte), S(lstrlenA), S(lstrlenW),
     S(InitializeSListHead), S(FlsAlloc), S(FlsGetValue), S(FlsSetValue),
    S(FlsFree), S(EncodePointer), S(DecodePointer),
    {0, 0}};

