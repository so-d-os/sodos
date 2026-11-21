/*
 * shell.c - Localidade (en-US fixo), variaveis de condicao, SHELL32, OLE32,
 * COMCTL32 e seguranca do ADVAPI32: o que instaladores classicos pedem.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

static DWORD put16(const char* s, wchar16* buf, int n) {
  int len = wutf8_to_16(s, -1, 0, 0);
  if (!buf || n <= 0) return (DWORD)len + 1;
  if (len + 1 > n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  wutf8_to_16(s, -1, buf, len);
  buf[len] = 0;
  return (DWORD)len + 1;
}
static wchar16 up16(wchar16 c) {
  if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (wchar16)(c - 32);
  return c;
}
static wchar16 lo16(wchar16 c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return (wchar16)(c + 32);
  return c;
}

/* ---------- localidade ---------- */
static WINAPI int CompareStringW(DWORD lcid, DWORD fl, const wchar16* a, int na, const wchar16* b, int nb) {
  int i;
  (void)lcid;
  if (na < 0) na = (int)wstrlen16(a);
  if (nb < 0) nb = (int)wstrlen16(b);
  for (i = 0; i < na && i < nb; i++) {
    wchar16 x = a[i], y = b[i];
    if (fl & 1) { /* NORM_IGNORECASE */
      x = up16(x);
      y = up16(y);
    }
    if (x != y) return x < y ? 1 : 3;
  }
  return na == nb ? 2 : na < nb ? 1 : 3;
}
static WINAPI int CompareStringEx(const wchar16* loc, DWORD fl, const wchar16* a, int na, const wchar16* b, int nb,
                                  void* v, void* r, intptr_t h) {
  (void)loc; (void)v; (void)r; (void)h;
  return CompareStringW(0, fl, a, na, b, nb);
}
static WINAPI int CompareStringOrdinal(const wchar16* a, int na, const wchar16* b, int nb, BOOL ic) {
  return CompareStringW(0, ic ? 1 : 0, a, na, b, nb);
}
static WINAPI int LCMapStringW(DWORD lcid, DWORD fl, const wchar16* s, int n, wchar16* d, int dn) {
  int i;
  (void)lcid;
  if (n < 0) n = (int)wstrlen16(s) + 1;
  if (!dn) return n;
  if (dn < n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  for (i = 0; i < n; i++) d[i] = (fl & 0x100) ? lo16(s[i]) : (fl & 0x200) ? up16(s[i]) : s[i];
  return n;
}
static WINAPI int LCMapStringEx(const wchar16* loc, DWORD fl, const wchar16* s, int n, wchar16* d, int dn, void* v,
                                void* r, intptr_t h) {
  (void)loc; (void)v; (void)r; (void)h;
  return LCMapStringW(0, fl, s, n, d, dn);
}
static WINAPI int FoldStringW(DWORD fl, const wchar16* s, int n, wchar16* d, int dn) {
  (void)fl;
  if (n < 0) n = (int)wstrlen16(s) + 1;
  if (!dn) return n;
  if (dn < n) n = dn;
  memcpy(d, s, (size_t)n * 2);
  return n;
}
static WINAPI BOOL GetStringTypeW(DWORD type, const wchar16* s, int n, uint16_t* out) {
  int i;
  if (n < 0) n = (int)wstrlen16(s) + 1;
  for (i = 0; i < n; i++) {
    wchar16 c = s[i];
    uint16_t t = 0;
    if (type != 1) {
      out[i] = 0;
      continue;
    }
    if (c >= 'A' && c <= 'Z') t |= 1 | 0x100 | (c <= 'F' ? 0x80 : 0);
    else if (c >= 'a' && c <= 'z') t |= 2 | 0x100 | (c <= 'f' ? 0x80 : 0);
    else if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) t |= 0x100 | (c < 0xE0 ? 1 : 2);
    else if (c >= '0' && c <= '9') t |= 4 | 0x80;
    else if (c == ' ' || c == '\t') t |= 8 | 0x40;
    else if (c == '\r' || c == '\n' || c == 11 || c == 12) t |= 8 | 0x20;
    else if (c < 32 || c == 127) t |= 0x20;
    else if (c < 127 || (c >= 0xA0 && c < 0xC0)) t |= 0x10;
    else t |= 0x100;
    out[i] = t;
  }
  return 1;
}
static WINAPI BOOL GetStringTypeExW(DWORD lcid, DWORD type, const wchar16* s, int n, uint16_t* out) {
  (void)lcid;
  return GetStringTypeW(type, s, n, out);
}
struct cpinfo {
  uint32_t max;
  uint8_t def[2];
  uint8_t lead[12];
};
static WINAPI BOOL GetCPInfo(DWORD cp, struct cpinfo* ci) {
  memset(ci, 0, sizeof(*ci));
  ci->max = (cp == 65001 || cp == 0 || cp == 3) ? 4 : 1;
  ci->def[0] = '?';
  return 1;
}
static WINAPI BOOL IsDBCSLeadByte(uint8_t b) {
  (void)b;
  return 0;
}
static WINAPI BOOL IsDBCSLeadByteEx(DWORD cp, uint8_t b) {
  (void)cp;
  (void)b;
  return 0;
}
static WINAPI int GetLocaleInfoW(DWORD lcid, DWORD type, wchar16* buf, int n) {
  const char* v = "";
  DWORD t = type & 0xFFFF;
  int num = -1;
  (void)lcid;
  switch (t) {
    case 0x01: v = "0409"; break;                 /* LOCALE_ILANGUAGE */
    case 0x02: v = "English (United States)"; break;
    case 0x03: v = "ENU"; break;                  /* LOCALE_SABBREVLANGNAME */
    case 0x0B: v = "1252"; num = 1252; break;     /* LOCALE_IDEFAULTCODEPAGE (OEM 437) */
    case 0x0E: v = "."; break;                    /* LOCALE_SDECIMAL */
    case 0x0F: v = ","; break;                    /* LOCALE_STHOUSAND */
    case 0x10: v = "3;0"; break;                  /* LOCALE_SGROUPING */
    case 0x11: v = "2"; num = 2; break;           /* LOCALE_IDIGITS */
    case 0x12: v = "1"; num = 1; break;           /* LOCALE_ILZERO */
    case 0x14: v = "$"; break;                    /* LOCALE_SCURRENCY */
    case 0x1D: v = "/"; break;                    /* LOCALE_SDATE */
    case 0x1E: v = ":"; break;                    /* LOCALE_STIME */
    case 0x1F: v = "M/d/yyyy"; break;             /* LOCALE_SSHORTDATE */
    case 0x20: v = "dddd, MMMM d, yyyy"; break;   /* LOCALE_SLONGDATE */
    case 0x21: v = "0"; num = 0; break;           /* LOCALE_IDATE */
    case 0x23: v = "0"; num = 0; break;           /* LOCALE_ITIME */
    case 0x28: v = "AM"; break;                   /* LOCALE_S1159 */
    case 0x29: v = "PM"; break;                   /* LOCALE_S2359 */
    case 0x59: v = "en"; break;                   /* LOCALE_SISO639LANGNAME */
    case 0x5A: v = "US"; break;                   /* LOCALE_SISO3166CTRYNAME */
    case 0x5C: v = "en-US"; break;                /* LOCALE_SNAME */
    case 0x1001: v = "English"; break;            /* LOCALE_SENGLANGUAGE */
    case 0x1002: v = "United States"; break;
    case 0x1003: v = "h:mm:ss tt"; break;         /* LOCALE_STIMEFORMAT */
    case 0x1004: v = "1252"; num = 1252; break;   /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x1009: v = "0"; num = 0; break;         /* LOCALE_IFIRSTDAYOFWEEK... */
    case 0x100C: v = "0"; num = 0; break;
  }
  if (type & 0x20000000u) { /* LOCALE_RETURN_NUMBER */
    if (num < 0) num = 0;
    if (buf && n >= 2) *(DWORD*)buf = (DWORD)num;
    return 2;
  }
  return (int)put16(v, buf, n);
}
static WINAPI int GetLocaleInfoEx(const wchar16* loc, DWORD type, wchar16* buf, int n) {
  (void)loc;
  return GetLocaleInfoW(0x409, type, buf, n);
}
static WINAPI DWORD GetThreadLocale(void) { return 0x409; }
static WINAPI DWORD GetSystemDefaultLCID(void) { return 0x409; }
static WINAPI uint16_t GetSystemDefaultLangID(void) { return 0x409; }
static WINAPI uint16_t GetUserDefaultLangID(void) { return 0x409; }
static WINAPI uint16_t GetSystemDefaultUILanguage(void) { return 0x409; }
static WINAPI BOOL IsValidLocale(DWORD l, DWORD f) {
  (void)l;
  (void)f;
  return 1;
}
struct systemtime {
  uint16_t year, month, dow, day, hour, minute, second, ms;
};
static void num2(char* o, int v, int width) {
  int i;
  for (i = width - 1; i >= 0; i--) {
    o[i] = (char)('0' + v % 10);
    v /= 10;
  }
  o[width] = 0;
}
static WINAPI int GetDateFormatW(DWORD lcid, DWORD fl, const struct systemtime* st, const wchar16* fmt, wchar16* buf,
                                 int n) {
  struct systemtime now;
  char out[32], t[8];
  (void)lcid;
  (void)fl;
  (void)fmt;
  if (!st) {
    typedef void (WINAPI * gst)(struct systemtime*);
    gst f = (gst)builtin_lookup("GetLocalTime");
    if (f) f(&now);
    st = &now;
  }
  num2(t, st->month, 2);
  strcpy(out, t);
  strcat(out, "/");
  num2(t, st->day, 2);
  strcat(out, t);
  strcat(out, "/");
  num2(t, st->year, 4);
  strcat(out, t);
  return (int)put16(out, buf, n);
}
static WINAPI int GetTimeFormatW(DWORD lcid, DWORD fl, const struct systemtime* st, const wchar16* fmt, wchar16* buf,
                                 int n) {
  struct systemtime now;
  char out[32], t[8];
  (void)lcid;
  (void)fmt;
  if (!st) {
    typedef void (WINAPI * gst)(struct systemtime*);
    gst f = (gst)builtin_lookup("GetLocalTime");
    if (f) f(&now);
    st = &now;
  }
  num2(t, st->hour, 2);
  strcpy(out, t);
  strcat(out, ":");
  num2(t, st->minute, 2);
  strcat(out, t);
  if (!(fl & 2)) { /* TIME_NOSECONDS */
    strcat(out, ":");
    num2(t, st->second, 2);
    strcat(out, t);
  }
  return (int)put16(out, buf, n);
}
static WINAPI int GetDateFormatEx(const wchar16* loc, DWORD fl, const struct systemtime* st, const wchar16* fmt,
                                  wchar16* buf, int n, const wchar16* cal) {
  (void)loc;
  (void)cal;
  return GetDateFormatW(0, fl, st, fmt, buf, n);
}
static WINAPI int GetTimeFormatEx(const wchar16* loc, DWORD fl, const struct systemtime* st, const wchar16* fmt,
                                  wchar16* buf, int n) {
  (void)loc;
  return GetTimeFormatW(0, fl, st, fmt, buf, n);
}
static WINAPI int GetNumberFormatW(DWORD lcid, DWORD fl, const wchar16* v, const void* fmt, wchar16* buf, int n) {
  int len = (int)wstrlen16(v);
  (void)lcid;
  (void)fl;
  (void)fmt;
  if (!n) return len + 1;
  if (len + 1 > n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(buf, v, (size_t)(len + 1) * 2);
  return len + 1;
}
static WINAPI BOOL SystemTimeToTzSpecificLocalTime(const void* tz, const struct systemtime* a, struct systemtime* b) {
  (void)tz;
  *b = *a;
  return 1;
}
static WINAPI BOOL TzSpecificLocalTimeToSystemTime(const void* tz, const struct systemtime* a, struct systemtime* b) {
  (void)tz;
  *b = *a;
  return 1;
}

/* ---------- variaveis de condicao e SRW compartilhado ---------- */
static WINAPI void InitializeConditionVariable(volatile int* cv) { *cv = 0; }
static WINAPI void WakeConditionVariable(volatile int* cv) {
  __sync_fetch_and_add(cv, 1);
  k_futex_wake(cv, 1);
}
static WINAPI void WakeAllConditionVariable(volatile int* cv) {
  __sync_fetch_and_add(cv, 1);
  k_futex_wake(cv, -1);
}
typedef void (WINAPI * lock_fn)(void*);
static BOOL cv_wait(volatile int* cv, void* lock, const char* rel, const char* acq, DWORD ms, void* caller) {
  lock_fn r = (lock_fn)builtin_lookup(rel), a = (lock_fn)builtin_lookup(acq);
  int seq = *cv, rc;
  if (r) r(lock);
  if (wine_debug >= 2 && ms == INFINITE) {
    rc = k_futex_wait(cv, seq, 5000);
    if (rc == -110 && *cv == seq) {
      wwait_note("variavel de condicao", caller, 5000);
      rc = k_futex_wait(cv, seq, -1);
    }
  } else {
    rc = k_futex_wait(cv, seq, ms == INFINITE ? -1 : (int)ms);
  }
  if (a) a(lock);
  /* INFINITE nunca devolve FALSE (o Chromium chama abort() se devolver);
   * acordar sem sinal e permitido no Windows tambem */
  if (rc == -110 && ms != INFINITE) {
    wset_lasterr(1460); /* ERROR_TIMEOUT */
    return 0;
  }
  return 1;
}
static WINAPI BOOL SleepConditionVariableSRW(volatile int* cv, void* srw, DWORD ms, DWORD fl) {
  if (fl & 1)
    return cv_wait(cv, srw, "ReleaseSRWLockShared", "AcquireSRWLockShared", ms, __builtin_return_address(0));
  return cv_wait(cv, srw, "ReleaseSRWLockExclusive", "AcquireSRWLockExclusive", ms, __builtin_return_address(0));
}
static WINAPI BOOL SleepConditionVariableCS(volatile int* cv, void* cs, DWORD ms) {
  return cv_wait(cv, cs, "LeaveCriticalSection", "EnterCriticalSection", ms, __builtin_return_address(0));
}
/* leitores de verdade: varios ao mesmo tempo (ver k32.c: -1 = escritor) */
void wsrw_wait(volatile intptr_t* v, intptr_t seen); /* k32.c */
void wsrw_wake(volatile intptr_t* v);
static WINAPI void AcquireSRWLockShared(void** l) {
  volatile intptr_t* v = (volatile intptr_t*)l;
  int spins = 0;
  for (;;) {
    intptr_t cur = *v;
    if (cur >= 0 && __sync_val_compare_and_swap(v, cur, cur + 1) == cur) return;
    if (cur >= 0) continue; /* outro leitor mexeu: tenta de novo */
    if (++spins < 64) {
      __builtin_ia32_pause();
      continue;
    }
    wsrw_wait(v, cur);
  }
}
static WINAPI void ReleaseSRWLockShared(void** l) {
  if (__sync_sub_and_fetch((volatile intptr_t*)l, 1) == 0) wsrw_wake((volatile intptr_t*)l);
}
void wsrw_note(void* l, void* caller); /* k32.c */
static WINAPI BOOL TryAcquireSRWLockExclusive(void** l) {
  if (__sync_val_compare_and_swap((volatile intptr_t*)l, 0, -1) != 0) return 0;
  wsrw_note(l, __builtin_return_address(0));
  return 1;
}
static WINAPI BOOL InitOnceExecuteOnce(volatile intptr_t* once, BOOL (WINAPI* fn)(void*, void*, void**), void* p,
                                       void** ctx) {
  /* 0 = nunca, 1 = rodando, 2 = feito */
  if (*once == 2) return 1;
  if (__sync_val_compare_and_swap(once, 0, 1) == 0) {
    BOOL r = fn((void*)once, p, ctx);
    *once = r ? 2 : 0;
    return r;
  }
  while (*once == 1) k_thread_yield();
  return *once == 2;
}

static WINAPI void* RtlPcToFileHeader(void* pc, void** base) {
  struct wmod* m = pe_from_addr((uint64_t)(uintptr_t)pc);
  *base = m ? m->base : 0;
  return *base;
}
static WINAPI DWORD SetThreadExecutionState(DWORD f) {
  (void)f;
  return 0x80000000u; /* ES_CONTINUOUS */
}

/* ---------- seguranca (advapi32): tudo permitido ---------- */
static WINAPI BOOL InitializeSecurityDescriptor(void* sd, DWORD rev) {
  (void)rev;
  if (sd) memset(sd, 0, 40);
  return 1;
}
static WINAPI BOOL SetSecurityDescriptorDacl(void* sd, BOOL present, void* acl, BOOL def) {
  (void)sd; (void)present; (void)acl; (void)def;
  return 1;
}
static WINAPI DWORD SetEntriesInAclW(DWORD n, void* e, void* old, void** out) {
  (void)n; (void)e; (void)old;
  if (out) *out = 0;
  return 0;
}
static WINAPI DWORD SetNamedSecurityInfoW(wchar16* name, int type, DWORD info, void* o, void* g, void* d, void* s) {
  (void)name; (void)type; (void)info; (void)o; (void)g; (void)d; (void)s;
  return 0;
}

/* ---------- shell32 ---------- */
static WINAPI uintptr_t SHGetFileInfoW(const wchar16* path, DWORD attr, void* info, DWORD n, DWORD fl) {
  (void)path; (void)attr; (void)fl;
  if (info) memset(info, 0, n);
  return 0;
}
static WINAPI void* SHBrowseForFolderW(void* bi) {
  (void)bi;
  wlog("SHBrowseForFolder: sem dialogo de pastas (cancelado)", 0);
  return 0;
}
/* PIDL de pasta especial: um bloco nosso com o caminho (o programa so passa
 * adiante para SHGetPathFromIDList e libera com CoTaskMemFree/ILFree). */
#define PIDL_MAGIC 0x4C444950u /* "PIDL" */
static WINAPI BOOL SHGetPathFromIDListW(const void* pidl, wchar16* path) {
  const uint32_t* p = (const uint32_t*)pidl;
  if (path) path[0] = 0;
  if (!p || p[0] != PIDL_MAGIC || !path) return 0;
  memcpy(path, p + 1, (wstrlen16((const wchar16*)(p + 1)) + 1) * 2);
  return 1;
}
static WINAPI long SHGetSpecialFolderLocation(HANDLE w, int csidl, void** out) {
  typedef long (WINAPI * gfp)(HANDLE, int, HANDLE, DWORD, wchar16*);
  gfp f = (gfp)builtin_lookup("SHGetFolderPathW");
  wchar16 path[260];
  uint32_t* p;
  size_t n;
  *out = 0;
  if (!f || f(w, csidl, 0, 0, path) != 0) return (long)0x80070057;
  n = wstrlen16(path);
  p = (uint32_t*)k_malloc((unsigned)(4 + (n + 1) * 2));
  if (!p) return (long)0x8007000E;
  p[0] = PIDL_MAGIC;
  memcpy(p + 1, path, (n + 1) * 2);
  *out = p;
  return 0;
}
static WINAPI void ILFree(void* p) {
  if (p) k_free(p);
}
static void delete_tree(const char* path) {
  struct k_dirent ents[128];
  int n = k_dir(path, ents, 128), i;
  if (n < 0) {
    k_fdelete(path);
    return;
  }
  for (i = 0; i < n; i++) {
    char sub[300];
    strcpy(sub, path);
    strcat(sub, "\\");
    strncat(sub, ents[i].name, sizeof(sub) - strlen(sub) - 1);
    if (ents[i].dir) delete_tree(sub);
    else k_fdelete(sub);
  }
  k_rmdir(path);
}
struct shfileop {
  HANDLE hwnd;
  uint32_t func;
  const wchar16* from;
  const wchar16* to;
  uint16_t flags;
  BOOL aborted;
  void* mappings;
  const wchar16* title;
};
static WINAPI int SHFileOperationW(struct shfileop* op) {
  const wchar16* p;
  if (!op || !op->from) return 2;
  for (p = op->from; *p; p += wstrlen16(p) + 1) {
    char path[300];
    wpath16(p, path, (int)sizeof(path));
    if (op->func == 3) { /* FO_DELETE */
      delete_tree(path);
    } else {
      typedef BOOL (WINAPI * mv_fn)(const wchar16*, const wchar16*, DWORD);
      typedef BOOL (WINAPI * cp_fn)(const wchar16*, const wchar16*, BOOL);
      if (op->func == 1) { /* FO_MOVE */
        mv_fn f = (mv_fn)builtin_lookup("MoveFileExW");
        if (f) f(p, op->to, 1);
      } else if (op->func == 2) { /* FO_COPY */
        cp_fn f = (cp_fn)builtin_lookup("CopyFileW");
        if (f) f(p, op->to, 0);
      }
    }
  }
  op->aborted = 0;
  return 0;
}
static WINAPI void SHChangeNotify(long ev, DWORD fl, const void* a, const void* b) {
  (void)ev; (void)fl; (void)a; (void)b;
}
static WINAPI long SHAutoComplete(HANDLE h, DWORD fl) {
  (void)h;
  (void)fl;
  return 0;
}
static WINAPI BOOL ShellExecuteExA_stub(void* p) {
  (void)p;
  return 0;
}
static WINAPI void* SHCreateItemFromParsingName_stub(void) { return 0; }

/* IMalloc: o "alocador da shell" */
static WINAPI long im_qi(void* self, const void* iid, void** out) {
  (void)iid;
  *out = self;
  return 0;
}
static WINAPI DWORD im_ref(void* self) {
  (void)self;
  return 1;
}
static WINAPI void* im_alloc(void* self, size_t n) {
  (void)self;
  return wheap_alloc(n);
}
static WINAPI void* im_realloc(void* self, void* p, size_t n) {
  (void)self;
  return wheap_realloc(p, n);
}
static WINAPI void im_free(void* self, void* p) {
  (void)self;
  wheap_free(p);
}
static WINAPI size_t im_size(void* self, void* p) {
  (void)self;
  return wheap_size(p);
}
static WINAPI int im_did(void* self, void* p) {
  (void)self;
  (void)p;
  return -1;
}
static WINAPI void im_min(void* self) { (void)self; }
static void* const imalloc_vtbl[] = {(void*)im_qi,   (void*)im_ref,  (void*)im_ref, (void*)im_alloc, (void*)im_realloc,
                                     (void*)im_free, (void*)im_size, (void*)im_did, (void*)im_min};
static const void* const imalloc_obj = imalloc_vtbl;
static WINAPI long SHGetMalloc(void** out) {
  *out = (void*)&imalloc_obj;
  return 0;
}
static WINAPI long CoGetMalloc(DWORD ctx, void** out) {
  (void)ctx;
  return SHGetMalloc(out);
}

/* ---------- ole32 / comctl32 ---------- */
static WINAPI long OleInitialize(void* r) {
  (void)r;
  return 0;
}
static WINAPI void OleUninitialize(void) {}
static WINAPI long CreateStreamOnHGlobal(void* h, BOOL del, void** out) {
  (void)h;
  (void)del;
  if (out) *out = 0;
  return (long)0x80004001; /* E_NOTIMPL: sem GDI+, nao ha quem leia */
}
static WINAPI BOOL InitCommonControlsEx(const void* p) {
  (void)p;
  return 1;
}
static WINAPI void InitCommonControls(void) {}

#define S(n) {#n, (void*)n}
const struct wine_sym shell_syms[] = {
    S(CompareStringW), S(CompareStringEx), S(CompareStringOrdinal), S(LCMapStringW), S(LCMapStringEx), S(FoldStringW),
    S(GetStringTypeW), S(GetStringTypeExW), S(GetCPInfo), S(IsDBCSLeadByte), S(IsDBCSLeadByteEx), S(GetLocaleInfoW),
    S(GetLocaleInfoEx), S(GetThreadLocale), S(GetSystemDefaultLCID), S(GetSystemDefaultLangID),
    S(GetUserDefaultLangID), S(GetSystemDefaultUILanguage), S(IsValidLocale), S(GetDateFormatW), S(GetTimeFormatW),
    S(GetDateFormatEx), S(GetTimeFormatEx), S(GetNumberFormatW), S(SystemTimeToTzSpecificLocalTime),
    S(TzSpecificLocalTimeToSystemTime), S(InitializeConditionVariable), S(WakeConditionVariable),
    S(WakeAllConditionVariable), S(SleepConditionVariableSRW), S(SleepConditionVariableCS), S(AcquireSRWLockShared),
    S(ReleaseSRWLockShared), S(TryAcquireSRWLockExclusive), S(InitOnceExecuteOnce), S(RtlPcToFileHeader),
    S(SetThreadExecutionState), S(InitializeSecurityDescriptor), S(SetSecurityDescriptorDacl), S(SetEntriesInAclW),
    S(SetNamedSecurityInfoW), S(SHGetSpecialFolderLocation), S(ILFree), S(SHGetFileInfoW), S(SHBrowseForFolderW), S(SHGetPathFromIDListW), S(SHFileOperationW),
    S(SHChangeNotify), S(SHAutoComplete), S(SHGetMalloc), S(CoGetMalloc), S(OleInitialize), S(OleUninitialize),
    S(CreateStreamOnHGlobal), S(InitCommonControlsEx), S(InitCommonControls),
    {"ShellExecuteExA", (void*)ShellExecuteExA_stub},
    {"SHCreateItemFromParsingName", (void*)SHCreateItemFromParsingName_stub},
    {0, 0}};
