/*
 * k32.c - KERNEL32/KERNELBASE, NTDLL e USER32 embutidos (W0: console).
 *
 * O minimo que o CRT do MinGW/zig e programas de console pedem. O que nao
 * esta aqui vira stub (pe.c) e so derruba o programa se for chamado.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

typedef uint32_t DWORD;
typedef int BOOL;
typedef void* HANDLE;

#define H_STDIN ((HANDLE)(uintptr_t)0x10)
#define H_STDOUT ((HANDLE)(uintptr_t)0x14)
#define H_STDERR ((HANDLE)(uintptr_t)0x18)
#define H_PROCESS ((HANDLE)(intptr_t)-1)
#define H_THREAD ((HANDLE)(intptr_t)-2)
#define H_HEAP ((HANDLE)(uintptr_t)1)

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_INSUFFICIENT_BUFFER 122

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
    if (builtin_for(n)) m = pe_load(name, 0);
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
  if (!f) wset_lasterr(ERROR_PROC_NOT_FOUND);
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
static WINAPI BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* done, void* ov) {
  (void)ov;
  if (h == H_STDOUT || h == H_STDERR) {
    wcon_write((const char*)buf, n);
    if (done) *done = n;
    return 1;
  }
  if (done) *done = 0;
  wset_lasterr(ERROR_INVALID_HANDLE);
  return 0;
}
static WINAPI BOOL WriteConsoleA(HANDLE h, const void* buf, DWORD n, DWORD* done, void* r) {
  (void)r;
  return WriteFile(h, buf, n, done, 0);
}
static WINAPI BOOL WriteConsoleW(HANDLE h, const wchar16* buf, DWORD n, DWORD* done, void* r) {
  char tmp[512];
  DWORD i = 0;
  (void)r;
  (void)h;
  while (i < n) {
    DWORD k = n - i > 120 ? 120 : n - i;
    int b = wutf16_to_8(buf + i, (int)k, tmp, (int)sizeof(tmp));
    wcon_write(tmp, (size_t)b);
    i += k;
  }
  if (done) *done = n;
  return 1;
}
static WINAPI BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* done, void* ov) {
  (void)ov;
  if (h == H_STDIN && n > 0) {
    char line[256];
    int len = k_readline(line, (int)sizeof(line) - 2);
    if (len < 0) len = 0;
    line[len++] = '\r';
    line[len++] = '\n';
    if ((DWORD)len > n) len = (int)n;
    memcpy(buf, line, (size_t)len);
    if (done) *done = (DWORD)len;
    return 1;
  }
  if (done) *done = 0;
  wset_lasterr(ERROR_INVALID_HANDLE);
  return 0;
}
static WINAPI DWORD GetFileType(HANDLE h) {
  if (h == H_STDIN || h == H_STDOUT || h == H_STDERR) return 2; /* FILE_TYPE_CHAR */
  return 0;
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
static WINAPI BOOL FlushFileBuffers(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI BOOL CloseHandle(HANDLE h) {
  (void)h;
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

/* VirtualAlloc sem paginas: reserva = bloco zerado do programa (alinhado a
 * 64 KB); commit dentro dele so devolve o endereco. */
static WINAPI void* VirtualAlloc(void* addr, size_t n, DWORD type, DWORD prot) {
  void* p;
  (void)prot;
  if (addr && !(type & 0x2000)) return addr; /* MEM_COMMIT de algo reservado */
  if (addr) {
    wset_lasterr(487); /* ERROR_INVALID_ADDRESS: sem MAP_FIXED */
    return 0;
  }
  if (n > 0xFFF00000u) {
    wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  p = k_malloc_aligned((unsigned)n, 0x10000);
  if (!p) {
    wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  memset(p, 0, n);
  return p;
}
static WINAPI BOOL VirtualFree(void* p, size_t n, DWORD type) {
  (void)n;
  if (type & 0x8000) k_free(p); /* MEM_RELEASE */
  return 1;
}
static WINAPI BOOL VirtualProtect(void* p, size_t n, DWORD prot, DWORD* old) {
  (void)p;
  (void)n;
  (void)prot;
  if (old) *old = 0x40; /* PAGE_EXECUTE_READWRITE: o SODOS nao protege */
  return 1;
}
struct mbi {
  void* base;
  void* alloc_base;
  DWORD alloc_protect;
  uint16_t part;
  size_t region;
  DWORD state, protect, type;
};
static WINAPI size_t VirtualQuery(const void* p, struct mbi* m, size_t len) {
  uintptr_t pg = (uintptr_t)p & ~(uintptr_t)0xFFF;
  if (len < sizeof(*m)) return 0;
  memset(m, 0, sizeof(*m));
  m->base = (void*)pg;
  m->alloc_base = (void*)pg;
  m->alloc_protect = 0x40;
  m->region = 0x1000;
  m->state = 0x1000;      /* MEM_COMMIT */
  m->protect = 0x40;
  m->type = 0x1000000;    /* MEM_IMAGE */
  return sizeof(*m);
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
  void** slots = (void**)(p->teb + TEB_TLSSLOTS);
  static const char used_marker = 0;
  DWORD i;
  /* slots 0..tls_next-1 sao dos modulos com .tls (array em TEB+0x58), os
   * TlsAlloc usam TEB+0x1480; o mapa de ocupados fica no fim do TEB. */
  uint8_t* map = p->teb + TEB_SIZE - TLS_SLOTS;
  (void)used_marker;
  for (i = 0; i < TLS_SLOTS; i++)
    if (!map[i]) {
      map[i] = 1;
      slots[i] = 0;
      return i;
    }
  return 0xFFFFFFFFu;
}
static WINAPI BOOL TlsFree(DWORD i) {
  if (i >= TLS_SLOTS) return 0;
  (wp()->teb + TEB_SIZE - TLS_SLOTS)[i] = 0;
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

/* ---------- tempo ---------- */
static WINAPI void Sleep(DWORD ms) {
  unsigned t0 = k_ms();
  if (!ms) {
    k_yield();
    return;
  }
  while (k_ms() - t0 < ms) k_yield();
}
static WINAPI DWORD SleepEx(DWORD ms, BOOL a) {
  (void)a;
  Sleep(ms);
  return 0;
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
static WINAPI void GetSystemTimeAsFileTime(uint64_t* ft) {
  int d = k_date(), t = k_time();
  uint64_t secs;
  if (d <= 0) d = 20260101;
  if (t < 0) t = 0;
  secs = days_from_civil(d / 10000, (unsigned)(d / 100 % 100), (unsigned)(d % 100)) * 86400u +
         (uint64_t)(t / 10000) * 3600u + (uint64_t)(t / 100 % 100) * 60u + (uint64_t)(t % 100);
  *ft = (secs + 11644473600ull) * 10000000ull + (uint64_t)(k_ms() % 1000) * 10000u;
}
static WINAPI void GetSystemTimePreciseAsFileTime(uint64_t* ft) { GetSystemTimeAsFileTime(ft); }

/* ---------- ambiente / sistema ---------- */
static WINAPI DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD n) {
  char tmp[260];
  int len = k_getenv(name, tmp, (int)sizeof(tmp));
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
  char tmp[260];
  DWORD len = nm ? GetEnvironmentVariableA(nm, tmp, sizeof(tmp)) : 0;
  int w;
  k_free(nm);
  if (!len) return 0;
  w = wutf8_to_16(tmp, (int)len, 0, 0);
  if ((DWORD)w + 1 > n) return (DWORD)w + 1;
  wutf8_to_16(tmp, (int)len, buf, w);
  buf[w] = 0;
  return (DWORD)w;
}
static WINAPI BOOL SetEnvironmentVariableA(const char* name, const char* v) {
  return k_setenv(name, v ? v : "") == 0;
}
static WINAPI char* GetEnvironmentStringsA(void) { return (char*)wzalloc(2); }
static WINAPI wchar16* GetEnvironmentStringsW(void) { return (wchar16*)wzalloc(4); }
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
static WINAPI void* SetUnhandledExceptionFilter(void* f) {
  (void)f;
  return 0;
}
static WINAPI long UnhandledExceptionFilter(void* e) {
  (void)e;
  return 1; /* EXCEPTION_EXECUTE_HANDLER */
}
static WINAPI DWORD SetErrorMode(DWORD m) {
  (void)m;
  return 0;
}
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
static WINAPI int lstrlenA(const char* s) { return s ? (int)strlen(s) : 0; }
static WINAPI int lstrlenW(const wchar16* s) { return s ? (int)wstrlen16(s) : 0; }

/* ---------- ntdll / excecoes (W0: sem SEH de verdade) ---------- */
static WINAPI void RtlCaptureContext(void* ctx) { memset(ctx, 0, 1232); }
static WINAPI void* RtlLookupFunctionEntry(uint64_t pc, uint64_t* base, void* hist) {
  (void)pc;
  (void)hist;
  if (base) *base = 0;
  return 0;
}
static WINAPI void* RtlVirtualUnwind(DWORD t, uint64_t b, uint64_t pc, void* f, void* c, void** h,
                                     uint64_t* e, void* p) {
  (void)t;
  (void)b;
  (void)pc;
  (void)f;
  (void)c;
  (void)e;
  (void)p;
  if (h) *h = 0;
  return 0;
}
static WINAPI void RtlUnwindEx(void* a, void* b, void* c, void* d, void* e, void* f) {
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
  wsay("RtlUnwindEx: excecoes C++/SEH ainda nao (marco W1)", 0);
  wexit(0xC0000027);
}
static WINAPI void RaiseException(DWORD code, DWORD fl, DWORD n, const uintptr_t* args) {
  (void)fl;
  (void)n;
  (void)args;
  wlog("RaiseException", 0);
  k_puts("wine: excecao do Windows nao tratada\n");
  wexit((int)code);
}
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

/* ---------- user32 (W0: no console) ---------- */
static WINAPI int MessageBoxA(HANDLE w, const char* text, const char* cap, DWORD type) {
  (void)w;
  (void)type;
  k_puts("[");
  k_puts(cap ? cap : "Erro");
  k_puts("] ");
  k_puts(text ? text : "");
  k_puts("\n");
  return 1; /* IDOK */
}
static WINAPI int MessageBoxW(HANDLE w, const wchar16* text, const wchar16* cap, DWORD type) {
  char* t = wdup16to8(text);
  char* c = wdup16to8(cap);
  int r = MessageBoxA(w, t, c, type);
  k_free(t);
  k_free(c);
  return r;
}

#define S(n) {#n, (void*)n}

static const struct wine_sym k32_syms[] = {
    S(ExitProcess), S(TerminateProcess), S(GetCurrentProcess), S(GetCurrentThread),
    S(GetCurrentProcessId), S(GetCurrentThreadId), S(GetLastError), S(SetLastError),
    S(GetCommandLineA), S(GetCommandLineW), S(GetModuleHandleA), S(GetModuleHandleW),
    S(GetModuleHandleExW), S(LoadLibraryA), S(LoadLibraryW), S(LoadLibraryExA), S(LoadLibraryExW),
    S(FreeLibrary), S(GetProcAddress), S(GetModuleFileNameA), S(GetModuleFileNameW),
    S(GetStdHandle), S(WriteFile), S(WriteConsoleA), S(WriteConsoleW), S(ReadFile), S(GetFileType),
    S(GetConsoleMode), S(SetConsoleMode), S(GetConsoleCP), S(GetConsoleOutputCP),
    S(SetConsoleOutputCP), S(SetConsoleCtrlHandler), S(FlushFileBuffers), S(CloseHandle),
    S(GetProcessHeap), S(HeapCreate), S(HeapDestroy), S(HeapAlloc), S(HeapFree), S(HeapReAlloc),
    S(HeapSize), S(LocalAlloc), S(LocalFree), S(VirtualAlloc), S(VirtualFree), S(VirtualProtect),
    S(VirtualQuery), S(InitializeCriticalSection), S(InitializeCriticalSectionAndSpinCount),
    S(InitializeCriticalSectionEx), S(DeleteCriticalSection), S(TryEnterCriticalSection),
    S(EnterCriticalSection), S(LeaveCriticalSection), S(InitializeSRWLock),
    S(AcquireSRWLockExclusive), S(ReleaseSRWLockExclusive), S(TlsAlloc), S(TlsFree),
    S(TlsGetValue), S(TlsSetValue), S(Sleep), S(SleepEx), S(GetTickCount), S(GetTickCount64),
    S(QueryPerformanceFrequency), S(QueryPerformanceCounter), S(GetSystemTimeAsFileTime),
    S(GetSystemTimePreciseAsFileTime), S(GetEnvironmentVariableA), S(GetEnvironmentVariableW),
    S(SetEnvironmentVariableA), S(GetEnvironmentStringsA), S(GetEnvironmentStringsW),
    S(FreeEnvironmentStringsA), S(FreeEnvironmentStringsW), S(GetStartupInfoA),
    S(GetStartupInfoW), S(GetSystemInfo), S(GetNativeSystemInfo), S(IsProcessorFeaturePresent),
    S(IsDebuggerPresent), S(SetUnhandledExceptionFilter), S(UnhandledExceptionFilter),
    S(SetErrorMode), S(GetVersion), S(GetACP), S(GetOEMCP), S(IsValidCodePage),
    S(MultiByteToWideChar), S(WideCharToMultiByte), S(lstrlenA), S(lstrlenW),
    S(RtlCaptureContext), S(RtlLookupFunctionEntry), S(RtlVirtualUnwind), S(RtlUnwindEx),
    S(RaiseException), S(InitializeSListHead), S(FlsAlloc), S(FlsGetValue), S(FlsSetValue),
    S(FlsFree), S(EncodePointer), S(DecodePointer),
    {0, 0}};

static const struct wine_sym user32_syms[] = {S(MessageBoxA), S(MessageBoxW), {0, 0}};

const struct wine_builtin wine_kernel32 = {"kernel32", k32_syms};
const struct wine_builtin wine_ntdll = {"ntdll", k32_syms};
const struct wine_builtin wine_user32 = {"user32", user32_syms};

static int starts(const char* s, const char* p) {
  while (*p && *s == *p) {
    s++;
    p++;
  }
  return *p == 0;
}

const struct wine_builtin* builtin_for(const char* n) {
  if (wstreq(n, "kernel32") || wstreq(n, "kernelbase") || starts(n, "api-ms-win-core-"))
    return &wine_kernel32;
  if (wstreq(n, "ntdll")) return &wine_ntdll;
  if (wstreq(n, "ucrtbase") || wstreq(n, "msvcrt") || starts(n, "api-ms-win-crt-"))
    return &wine_ucrt;
  if (wstreq(n, "user32")) return &wine_user32;
  return 0;
}
