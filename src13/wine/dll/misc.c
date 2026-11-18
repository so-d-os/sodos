/*
 * misc.c - Funcoes avulsas de kernel32, ntdll, winhttp, psapi, userenv...
 * Respostas fixas de um Windows 10 parado: um processador, um usuario, sem
 * proxy, sem outros processos.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

uint64_t wfiletime_now(void);
const char* wine_dir(const char* rel);

static DWORD put16(const char* s, wchar16* buf, DWORD n) {
  int len = wutf8_to_16(s, -1, 0, 0);
  if (!buf || (DWORD)len + 1 > n) return (DWORD)len + 1;
  wutf8_to_16(s, -1, buf, len);
  buf[len] = 0;
  return (DWORD)len;
}

/* ---------- tempo ---------- */
struct systemtime {
  uint16_t year, month, dow, day, hour, minute, second, ms;
};
static void ft_to_st(uint64_t ft, struct systemtime* st) {
  uint64_t secs = ft / 10000000ull, days = secs / 86400ull;
  uint32_t rem = (uint32_t)(secs % 86400ull);
  /* dias desde 1601-01-01 -> data civil */
  int64_t z = (int64_t)days - 134774 + 719468; /* 134774 = dias de 1601 a 1970 */
  int64_t era = z / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  st->year = (uint16_t)(yoe + era * 400 + (m <= 2));
  st->month = (uint16_t)m;
  st->day = (uint16_t)d;
  st->dow = (uint16_t)((days + 1) % 7); /* 1601-01-01 foi segunda */
  st->hour = (uint16_t)(rem / 3600);
  st->minute = (uint16_t)(rem / 60 % 60);
  st->second = (uint16_t)(rem % 60);
  st->ms = (uint16_t)(ft / 10000ull % 1000);
}
static WINAPI void GetSystemTime(struct systemtime* st) { ft_to_st(wfiletime_now(), st); }
static WINAPI void GetLocalTime(struct systemtime* st) { ft_to_st(wfiletime_now(), st); }
static WINAPI BOOL FileTimeToSystemTime(const uint64_t* ft, struct systemtime* st) {
  ft_to_st(*ft, st);
  return 1;
}
static WINAPI BOOL SystemTimeToFileTime(const struct systemtime* st, uint64_t* ft) {
  int y = st->year;
  unsigned m = st->month, d = st->day;
  int era;
  unsigned yoe, doy, doe;
  int64_t days;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = (int64_t)era * 146097 + (int64_t)doe - 719468 + 134774;
  *ft = ((uint64_t)days * 86400ull + st->hour * 3600u + st->minute * 60u + st->second) * 10000000ull +
        (uint64_t)st->ms * 10000ull;
  return 1;
}
static WINAPI BOOL FileTimeToLocalFileTime(const uint64_t* a, uint64_t* b) {
  *b = *a;
  return 1;
}
/* Fuso: UTC, sem horario de verao (o relogio do SODOS e UTC). */
static WINAPI DWORD GetTimeZoneInformation(uint8_t* tz) {
  static const wchar16 utc[] = {'U', 'T', 'C', 0};
  memset(tz, 0, 172);
  memcpy(tz + 4, utc, sizeof(utc));
  memcpy(tz + 4 + 64 + 16 + 4, utc, sizeof(utc));
  return 0; /* TIME_ZONE_ID_UNKNOWN */
}
static WINAPI DWORD GetDynamicTimeZoneInformation(uint8_t* tz) {
  memset(tz, 0, 432);
  return GetTimeZoneInformation(tz);
}

/* ---------- processo ---------- */
static WINAPI DWORD GetProcessId(HANDLE h) {
  (void)h;
  return 0x100;
}
static WINAPI HANDLE OpenProcess(DWORD acc, BOOL inh, DWORD pid) {
  (void)acc;
  (void)inh;
  if (pid == 0x100) return (HANDLE)(intptr_t)-1;
  wset_lasterr(ERROR_INVALID_PARAMETER);
  return 0;
}
static WINAPI BOOL ProcessIdToSessionId(DWORD pid, DWORD* sid) {
  (void)pid;
  *sid = 1;
  return 1;
}
static WINAPI DWORD WTSGetActiveConsoleSessionId(void) { return 1; }
static WINAPI BOOL QueryFullProcessImageNameW(HANDLE h, DWORD fl, wchar16* buf, DWORD* n) {
  struct wproc* p = wp();
  DWORD len;
  (void)h;
  (void)fl;
  len = put16(p->exe ? p->exe->path : "", buf, *n);
  if (len + 1 > *n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  *n = len;
  return 1;
}
static WINAPI DWORD GetActiveProcessorCount(uint16_t g) {
  (void)g;
  return 1;
}
static WINAPI DWORD GetMaximumProcessorCount(uint16_t g) {
  (void)g;
  return 1;
}
static WINAPI DWORD GetPriorityClass(HANDLE h) {
  (void)h;
  return 0x20;
}
static WINAPI BOOL GetProcessTimes(HANDLE h, uint64_t* c, uint64_t* e, uint64_t* k, uint64_t* u) {
  (void)h;
  if (c) *c = wfiletime_now();
  if (e) *e = 0;
  if (k) *k = 0;
  if (u) *u = (uint64_t)k_ms() * 10000ull;
  return 1;
}
static WINAPI BOOL SetDefaultDllDirectories(DWORD f) {
  (void)f;
  return 1;
}
static WINAPI void* AddDllDirectory(const wchar16* d) {
  (void)d;
  return (void*)1;
}
static WINAPI BOOL SetDllDirectoryW(const wchar16* d) {
  (void)d;
  return 1;
}
static WINAPI BOOL RtlAddFunctionTable(void* t, DWORD n, uint64_t base) {
  (void)t; (void)n; (void)base;
  return 1;
}
/* Idiomas: "en-US" (lista MULTI_SZ). */
static WINAPI BOOL GetUserPreferredUILanguages(DWORD fl, DWORD* num, wchar16* buf, DWORD* n) {
  static const wchar16 en[] = {'e', 'n', '-', 'U', 'S', 0, 0};
  (void)fl;
  if (num) *num = 1;
  if (!buf || *n < 7) {
    *n = 7;
    if (buf) wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return buf == 0;
  }
  memcpy(buf, en, sizeof(en));
  *n = 7;
  return 1;
}
static WINAPI uint16_t GetUserDefaultUILanguage(void) { return 0x0409; }
static WINAPI DWORD GetUserDefaultLCID(void) { return 0x0409; }
static WINAPI int GetUserDefaultLocaleName(wchar16* buf, int n) { return (int)put16("en-US", buf, (DWORD)n) + 1; }
static WINAPI DWORD GetTempPath2W(DWORD n, wchar16* buf) {
  char p[200];
  strcpy(p, wine_dir("TEMP"));
  strcat(p, "\\");
  return put16(p, buf, n);
}
static WINAPI DWORD GetShortPathNameW(const wchar16* s, wchar16* buf, DWORD n) {
  DWORD len = (DWORD)wstrlen16(s);
  if (len + 1 > n) return len + 1;
  memcpy(buf, s, (len + 1) * 2);
  return len;
}
static WINAPI BOOL GetConsoleScreenBufferInfo(HANDLE h, void* info) {
  (void)h;
  (void)info;
  wset_lasterr(ERROR_INVALID_HANDLE);
  return 0;
}
static WINAPI BOOL SetConsoleCP(DWORD cp) {
  (void)cp;
  return 1;
}
static WINAPI HANDLE GetConsoleWindow(void) { return 0; }
static WINAPI BOOL AttachConsole(DWORD pid) {
  (void)pid;
  return 1;
}
static WINAPI BOOL AllocConsole(void) { return 1; }
static WINAPI BOOL FreeConsole(void) { return 1; }
static WINAPI BOOL SetStdHandle(DWORD which, HANDLE h) {
  (void)which;
  (void)h;
  return 1;
}

/* ---------- recursos (.rsrc) ---------- */
static WINAPI HANDLE FindResourceW(HANDLE mod, const wchar16* name, const wchar16* type) {
  struct wmod* m = pe_from_handle(mod);
  uint32_t size = 0;
  const uint8_t* e = m ? pe_find_resource(m, (uintptr_t)type, (uintptr_t)name, &size) : 0;
  if (!e) wset_lasterr(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
  return (HANDLE)e;
}
static WINAPI HANDLE LoadResource(HANDLE mod, HANDLE res) {
  struct wmod* m = pe_from_handle(mod);
  const uint32_t* e = (const uint32_t*)res; /* IMAGE_RESOURCE_DATA_ENTRY */
  if (!m || !e) return 0;
  return m->base + e[0];
}
static WINAPI void* LockResource(HANDLE h) { return h; }
static WINAPI DWORD SizeofResource(HANDLE mod, HANDLE res) {
  (void)mod;
  return res ? ((const uint32_t*)res)[1] : 0;
}

/* ---------- ntdll ---------- */
struct ustr {
  uint16_t len, max;
  wchar16* buf;
};
static WINAPI void RtlInitUnicodeString(struct ustr* u, const wchar16* s) {
  size_t n = s ? wstrlen16(s) : 0;
  u->len = (uint16_t)(n * 2);
  u->max = (uint16_t)(s ? n * 2 + 2 : 0);
  u->buf = (wchar16*)s;
}
static WINAPI long NtQueryInformationProcess(HANDLE h, int cls, void* buf, DWORD len, DWORD* ret) {
  (void)h;
  memset(buf, 0, len);
  if (cls == 0 && len >= 48) { /* ProcessBasicInformation */
    ((uint64_t*)buf)[1] = (uint64_t)(uintptr_t)wp()->peb;
    ((uint64_t*)buf)[4] = 0x100;
    if (ret) *ret = 48;
    return 0;
  }
  if (ret) *ret = len;
  return (long)0xC0000003; /* STATUS_INVALID_INFO_CLASS */
}
static WINAPI long NtQuerySystemInformation(int cls, void* buf, DWORD len, DWORD* ret) {
  (void)cls;
  if (buf) memset(buf, 0, len);
  if (ret) *ret = 0;
  return (long)0xC0000003;
}
static WINAPI DWORD RtlNtStatusToDosErrorNoTeb(long st) { return st ? 317 : 0; }

/* ---------- winhttp: sem proxy ---------- */
static WINAPI HANDLE WinHttpOpen(const wchar16* agent, DWORD type, const wchar16* proxy, const wchar16* bypass,
                                 DWORD fl) {
  struct wobj* o = wobj_new(OBJ_MISC);
  (void)agent; (void)type; (void)proxy; (void)bypass; (void)fl;
  return o ? wh_new(o) : 0;
}
static WINAPI BOOL WinHttpCloseHandle(HANDLE h) {
  wh_close(h);
  return 1;
}
static WINAPI BOOL WinHttpGetIEProxyConfigForCurrentUser(void* cfg) {
  memset(cfg, 0, 32);
  return 1;
}
static WINAPI BOOL WinHttpGetProxyForUrl(HANDLE h, const wchar16* url, void* opt, void* info) {
  (void)h; (void)url; (void)opt;
  if (info) memset(info, 0, 24);
  wset_lasterr(12180); /* ERROR_WINHTTP_AUTODETECTION_FAILED */
  return 0;
}
static WINAPI BOOL WinHttpDetectAutoProxyConfigUrl(DWORD fl, wchar16** url) {
  (void)fl;
  if (url) *url = 0;
  wset_lasterr(12180);
  return 0;
}

/* ---------- userenv / psapi / secur32 ---------- */
static WINAPI BOOL GetProfilesDirectoryW(wchar16* buf, DWORD* n) {
  DWORD len = put16(wine_dir("USERS"), buf, *n);
  if (len + 1 > *n) {
    *n = len + 1;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  *n = len + 1;
  return 1;
}
static WINAPI BOOL GetUserNameExW(int fmt, wchar16* buf, DWORD* n) {
  DWORD len = put16(fmt == 2 ? "SODOS\\sodos" : "sodos", buf, *n);
  if (len + 1 > *n) {
    *n = len + 1;
    wset_lasterr(ERROR_MORE_DATA);
    return 0;
  }
  *n = len;
  return 1;
}
static WINAPI DWORD GetModuleFileNameExW(HANDLE proc, HANDLE mod, wchar16* buf, DWORD n) {
  struct wmod* m = pe_from_handle(mod);
  (void)proc;
  return m ? put16(m->path[0] ? m->path : m->name, buf, n) : 0;
}
static WINAPI BOOL GetProcessMemoryInfo(HANDLE h, void* c, DWORD n) {
  (void)h;
  memset(c, 0, n);
  return 1;
}
/* version.dll: os EXEs de teste nao tem bloco de versao. */
static WINAPI DWORD GetFileVersionInfoSizeW(const wchar16* f, DWORD* h) {
  (void)f;
  if (h) *h = 0;
  wset_lasterr(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
  return 0;
}
static WINAPI BOOL GetFileVersionInfoW(const wchar16* f, DWORD h, DWORD n, void* d) {
  (void)f; (void)h; (void)n; (void)d;
  return 0;
}
static WINAPI BOOL VerQueryValueW(const void* b, const wchar16* sub, void** out, DWORD* n) {
  (void)b; (void)sub;
  if (out) *out = 0;
  if (n) *n = 0;
  return 0;
}
static WINAPI long DwmSetWindowAttribute(HANDLE w, DWORD a, const void* v, DWORD n) {
  (void)w; (void)a; (void)v; (void)n;
  return 0;
}
static WINAPI long DwmGetWindowAttribute(HANDLE w, DWORD a, void* v, DWORD n) {
  (void)w; (void)a;
  memset(v, 0, n);
  return (long)0x80004001;
}
static WINAPI long CoCreateGuid(uint8_t* g) {
  k_random(g, 16);
  g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);
  g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
  return 0;
}
static WINAPI int StringFromGUID2(const uint8_t* g, wchar16* out, int max) {
  static const char hx[] = "0123456789ABCDEF";
  static const int order[] = {3, 2, 1, 0, -1, 5, 4, -1, 7, 6, -1, 8, 9, -1, 10, 11, 12, 13, 14, 15};
  int o = 0, i;
  if (max < 39) return 0;
  out[o++] = '{';
  for (i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
    if (order[i] < 0) {
      out[o++] = '-';
    } else {
      out[o++] = (wchar16)hx[g[order[i]] >> 4];
      out[o++] = (wchar16)hx[g[order[i]] & 15];
    }
  }
  out[o++] = '}';
  out[o++] = 0;
  return o;
}

/* ---------- servicos: nenhum instalado ---------- */
static WINAPI HANDLE OpenSCManagerW(const wchar16* m, const wchar16* db, DWORD acc) {
  struct wobj* o = wobj_new(OBJ_MISC);
  (void)m; (void)db; (void)acc;
  return o ? wh_new(o) : 0;
}
static WINAPI HANDLE OpenServiceW(HANDLE scm, const wchar16* name, DWORD acc) {
  (void)scm; (void)name; (void)acc;
  wset_lasterr(1060); /* ERROR_SERVICE_DOES_NOT_EXIST */
  return 0;
}
static WINAPI BOOL CloseServiceHandle(HANDLE h) {
  wh_close(h);
  return 1;
}
static WINAPI BOOL QueryServiceStatus(HANDLE h, void* st) {
  (void)h;
  memset(st, 0, 28);
  wset_lasterr(ERROR_INVALID_HANDLE);
  return 0;
}
static WINAPI BOOL QueryServiceStatusEx(HANDLE h, int lvl, void* buf, DWORD n, DWORD* need) {
  (void)h; (void)lvl;
  memset(buf, 0, n);
  if (need) *need = 0;
  wset_lasterr(ERROR_INVALID_HANDLE);
  return 0;
}
static WINAPI BOOL EnumServicesStatusExW(HANDLE h, int lvl, DWORD type, DWORD state, void* buf, DWORD n,
                                         DWORD* need, DWORD* count, DWORD* resume, const wchar16* grp) {
  (void)h; (void)lvl; (void)type; (void)state; (void)buf; (void)n; (void)grp;
  if (need) *need = 0;
  if (count) *count = 0;
  if (resume) *resume = 0;
  return 1;
}
/* ---------- toolhelp: so este processo ---------- */
static WINAPI HANDLE CreateToolhelp32Snapshot(DWORD fl, DWORD pid) {
  struct wobj* o = wobj_new(OBJ_MISC);
  (void)fl; (void)pid;
  return o ? wh_new(o) : INVALID_HANDLE_VALUE;
}
struct procentryw {
  DWORD size, usage, pid;
  uintptr_t heap;
  DWORD module, threads, parent;
  long pri;
  DWORD flags;
  wchar16 exe[260];
};
static WINAPI BOOL Process32FirstW(HANDLE snap, struct procentryw* e) {
  struct wobj* o = wh_get(snap, OBJ_MISC);
  struct wproc* p = wp();
  if (!o) return 0;
  o->u2 = 1;
  e->usage = 0;
  e->pid = 0x100;
  e->heap = 0;
  e->module = 0;
  e->threads = 1;
  e->parent = 4;
  e->pri = 8;
  e->flags = 0;
  put16(p->exe ? p->exe->name : "app.exe", e->exe, 260);
  return 1;
}
static WINAPI BOOL Process32NextW(HANDLE snap, struct procentryw* e) {
  (void)snap; (void)e;
  wset_lasterr(ERROR_NO_MORE_FILES);
  return 0;
}
static WINAPI BOOL EnumProcesses(DWORD* ids, DWORD n, DWORD* need) {
  if (n >= 4) ids[0] = 0x100;
  if (need) *need = 4;
  return 1;
}
/* ---------- privilegios / seguranca: tudo permitido ---------- */
static WINAPI BOOL LookupPrivilegeValueW(const wchar16* sys, const wchar16* name, uint64_t* luid) {
  (void)sys; (void)name;
  *luid = 20;
  return 1;
}
static WINAPI BOOL AdjustTokenPrivileges(HANDLE t, BOOL dis, void* n, DWORD len, void* prev, DWORD* ret) {
  (void)t; (void)dis; (void)n; (void)len; (void)prev;
  if (ret) *ret = 0;
  wset_lasterr(0);
  return 1;
}
static WINAPI BOOL IsUserAnAdmin(void) { return 1; }
static WINAPI BOOL GetSystemPowerStatus(uint8_t* s) {
  memset(s, 0, 12);
  s[0] = 1;   /* na tomada */
  s[1] = 128; /* sem bateria */
  s[2] = 255;
  return 1;
}
static WINAPI DWORD PowerRegisterSuspendResumeNotification(DWORD fl, HANDLE rcp, HANDLE* reg) {
  (void)fl; (void)rcp;
  if (reg) *reg = (HANDLE)(uintptr_t)1;
  return 0;
}
static WINAPI DWORD PowerUnregisterSuspendResumeNotification(HANDLE reg) {
  (void)reg;
  return 0;
}
static WINAPI BOOL GetNamedPipeInfo(HANDLE h, DWORD* fl, DWORD* o, DWORD* i, DWORD* m) {
  (void)h; (void)fl; (void)o; (void)i; (void)m;
  return 0;
}
static WINAPI BOOL GetSystemTimes(uint64_t* idle, uint64_t* kern, uint64_t* user) {
  if (idle) *idle = 0;
  if (kern) *kern = 0;
  if (user) *user = (uint64_t)k_ms() * 10000ull;
  return 1;
}
static WINAPI BOOL IsWindows10OrGreater_(void) { return 1; }

/* ---------- SHA-256 (nomes de familia de pacotes) ---------- */
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void wsha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t blk[64];
  size_t done = 0;
  int finished = 0, padded = 0;
  while (!finished) {
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    size_t n = len - done > 64 ? 64 : len - done;
    int i;
    memcpy(blk, data + done, n);
    done += n;
    if (n < 64) {
      memset(blk + n, 0, 64 - n);
      if (!padded) {
        blk[n] = 0x80;
        padded = 1;
      }
      if (n < 56) {
        uint64_t bits = (uint64_t)len * 8;
        for (i = 0; i < 8; i++) blk[63 - i] = (uint8_t)(bits >> (8 * i));
        finished = 1;
      }
    } else if (done == len && !padded) {
      /* bloco cheio no fim: o 0x80 vai no proximo */
    }
    for (i = 0; i < 16; i++)
      w[i] = ((uint32_t)blk[4 * i] << 24) | ((uint32_t)blk[4 * i + 1] << 16) | ((uint32_t)blk[4 * i + 2] << 8) |
             blk[4 * i + 3];
    for (; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  {
    int i;
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
  }
}

/* PublisherId: 8 primeiros bytes do SHA-256 do publicador (UTF-16LE) em 13
 * letras de base32 (alfabeto do Windows, sem i, l, o, u). */
void wpublisher_id(const wchar16* publisher, char out[14]);
static void publisher_id(const wchar16* publisher, char out[14]) { wpublisher_id(publisher, out); }
void wpublisher_id(const wchar16* publisher, char out[14]) {
  static const char al[] = "0123456789abcdefghjkmnpqrstvwxyz";
  uint8_t hash[32];
  uint64_t v = 0;
  int i;
  wsha256((const uint8_t*)publisher, wstrlen16(publisher) * 2, hash);
  for (i = 0; i < 8; i++) v = (v << 8) | hash[i];
  for (i = 0; i < 12; i++) out[i] = al[(v >> (59 - 5 * i)) & 31];
  out[12] = al[(v << 1) & 31]; /* os 4 bits que sobram + 1 zero */
  out[13] = 0;
}
struct package_id {
  DWORD reserved, arch;
  uint64_t version;
  const wchar16* name;
  const wchar16* publisher;
  const wchar16* resource;
  const wchar16* publisher_id;
};
static WINAPI long PackageFamilyNameFromId(const struct package_id* id, DWORD* len, wchar16* buf) {
  char name[200], pid[32], out[260];
  int n;
  if (!id || !id->name || !len) return ERROR_INVALID_PARAMETER;
  n = wutf16_to_8(id->name, -1, name, (int)sizeof(name) - 1);
  name[n] = 0;
  if (id->publisher_id) {
    n = wutf16_to_8(id->publisher_id, -1, pid, (int)sizeof(pid) - 1);
    pid[n] = 0;
  } else if (id->publisher) {
    publisher_id(id->publisher, pid);
  } else {
    return ERROR_INVALID_PARAMETER;
  }
  strcpy(out, name);
  strcat(out, "_");
  strcat(out, pid);
  n = wutf8_to_16(out, -1, 0, 0);
  if (!buf || *len < (DWORD)n + 1) {
    *len = (DWORD)n + 1;
    return ERROR_INSUFFICIENT_BUFFER;
  }
  wutf8_to_16(out, -1, buf, n);
  buf[n] = 0;
  *len = (DWORD)n + 1;
  return 0;
}
/* "Nome_1.2.3.4_x64__publisherid" */
static WINAPI long PackageFullNameFromId(const struct package_id* id, DWORD* len, wchar16* buf) {
  char name[200], pid[32], out[300], ver[64];
  static const char* const archs[] = {"x86", "", "", "", "", "arm", "", "", "", "x64", "", "neutral", "arm64"};
  int n, i, o = 0;
  if (!id || !id->name || !len) return ERROR_INVALID_PARAMETER;
  n = wutf16_to_8(id->name, -1, name, (int)sizeof(name) - 1);
  name[n] = 0;
  if (id->publisher_id) {
    n = wutf16_to_8(id->publisher_id, -1, pid, (int)sizeof(pid) - 1);
    pid[n] = 0;
  } else if (id->publisher) {
    publisher_id(id->publisher, pid);
  } else {
    return ERROR_INVALID_PARAMETER;
  }
  /* PACKAGE_VERSION: revision, build, minor, major (16 bits cada) */
  for (i = 3; i >= 0; i--) {
    unsigned part = (unsigned)((id->version >> (16 * i)) & 0xFFFF);
    char d[8];
    int k = 0;
    do {
      d[k++] = (char)('0' + part % 10);
      part /= 10;
    } while (part);
    while (k) ver[o++] = d[--k];
    if (i) ver[o++] = '.';
  }
  ver[o] = 0;
  strcpy(out, name);
  strcat(out, "_");
  strcat(out, ver);
  strcat(out, "_");
  strcat(out, id->arch < 13 ? archs[id->arch] : "neutral");
  strcat(out, "__");
  strcat(out, pid);
  n = wutf8_to_16(out, -1, 0, 0);
  if (!buf || *len < (DWORD)n + 1) {
    *len = (DWORD)n + 1;
    return ERROR_INSUFFICIENT_BUFFER;
  }
  wutf8_to_16(out, -1, buf, n);
  buf[n] = 0;
  *len = (DWORD)n + 1;
  return 0;
}

#define S(n) {#n, (void*)n}
const struct wine_sym misc_syms[] = {
    S(GetSystemTime), S(GetLocalTime), S(FileTimeToSystemTime), S(SystemTimeToFileTime),
    S(FileTimeToLocalFileTime), S(GetTimeZoneInformation), S(GetDynamicTimeZoneInformation),
    S(GetProcessId), S(OpenProcess), S(ProcessIdToSessionId), S(WTSGetActiveConsoleSessionId),
    S(QueryFullProcessImageNameW), S(GetActiveProcessorCount), S(GetMaximumProcessorCount),
    S(GetPriorityClass), S(GetProcessTimes), S(SetDefaultDllDirectories), S(AddDllDirectory),
    S(SetDllDirectoryW), S(RtlAddFunctionTable), S(GetUserPreferredUILanguages),
    S(GetUserDefaultUILanguage), S(GetUserDefaultLCID), S(GetUserDefaultLocaleName),
    S(GetTempPath2W), S(GetShortPathNameW), S(GetConsoleScreenBufferInfo), S(SetConsoleCP),
    S(GetConsoleWindow), S(AttachConsole), S(AllocConsole), S(FreeConsole), S(SetStdHandle),
    S(FindResourceW), S(LoadResource),
    S(LockResource), S(SizeofResource), S(RtlInitUnicodeString), S(NtQueryInformationProcess),
    S(NtQuerySystemInformation), S(RtlNtStatusToDosErrorNoTeb), S(WinHttpOpen),
    S(WinHttpCloseHandle), S(WinHttpGetIEProxyConfigForCurrentUser), S(WinHttpGetProxyForUrl),
    S(WinHttpDetectAutoProxyConfigUrl), S(GetProfilesDirectoryW), S(GetUserNameExW),
    S(GetModuleFileNameExW), S(GetProcessMemoryInfo), S(GetFileVersionInfoSizeW),
    S(GetFileVersionInfoW), S(VerQueryValueW), 
    S(OpenSCManagerW), S(OpenServiceW), S(CloseServiceHandle), S(QueryServiceStatus),
    S(QueryServiceStatusEx), S(EnumServicesStatusExW), S(CreateToolhelp32Snapshot),
    S(Process32FirstW), S(Process32NextW), S(EnumProcesses), S(LookupPrivilegeValueW),
    S(AdjustTokenPrivileges), S(IsUserAnAdmin), S(GetSystemPowerStatus),
    S(PowerRegisterSuspendResumeNotification), S(PowerUnregisterSuspendResumeNotification),
    S(GetNamedPipeInfo), S(GetSystemTimes), S(PackageFamilyNameFromId), S(PackageFullNameFromId), S(DwmSetWindowAttribute), S(DwmGetWindowAttribute), S(CoCreateGuid), S(StringFromGUID2),
    {0, 0}};
