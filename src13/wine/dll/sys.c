/*
 * sys.c - Pedacos de ntdll, bcryptprimitives, winmm, advapi32, version,
 * userenv, shell32, ole32... que programas pedem por GetProcAddress.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

const char* wine_dir(const char* rel); /* file.c */

/* ---------- aleatorio ---------- */
static WINAPI BOOL ProcessPrng(uint8_t* buf, size_t n) {
  while (n) {
    int k = n > 4096 ? 4096 : (int)n;
    k_random(buf, k);
    buf += k;
    n -= (size_t)k;
  }
  return 1;
}
static WINAPI BOOL SystemFunction036(uint8_t* buf, DWORD n) { return ProcessPrng(buf, n); } /* RtlGenRandom */
static WINAPI long BCryptGenRandom(void* alg, uint8_t* buf, DWORD n, DWORD fl) {
  (void)alg;
  (void)fl;
  ProcessPrng(buf, n);
  return 0;
}

/* ---------- versao ---------- */
struct osversioninfoexw {
  DWORD size, major, minor, build, platform;
  wchar16 csd[128];
  uint16_t sp_major, sp_minor, suite;
  uint8_t product, reserved;
};
static WINAPI long RtlGetVersion(struct osversioninfoexw* v) {
  DWORD size = v->size;
  v->major = 10;
  v->minor = 0;
  v->build = 19045;
  v->platform = 2;
  v->csd[0] = 0;
  if (size >= sizeof(*v)) {
    v->sp_major = v->sp_minor = 0;
    v->suite = 0x100;
    v->product = 1; /* VER_NT_WORKSTATION */
    v->reserved = 0;
  }
  return 0;
}
static WINAPI void RtlGetNtVersionNumbers(DWORD* major, DWORD* minor, DWORD* build) {
  if (major) *major = 10;
  if (minor) *minor = 0;
  if (build) *build = 19045 | 0xF0000000u;
}
static WINAPI BOOL GetVersionExW(struct osversioninfoexw* v) { return RtlGetVersion(v) == 0; }
static WINAPI BOOL GetVersionExA(DWORD* v) {
  v[1] = 10;
  v[2] = 0;
  v[3] = 19045;
  v[4] = 2;
  ((char*)&v[5])[0] = 0;
  return 1;
}
static WINAPI void* RtlGetCurrentPeb(void) { return wp()->peb; }
static WINAPI BOOL IsWow64Process(HANDLE h, BOOL* w) {
  (void)h;
  *w = 0;
  return 1;
}
static WINAPI BOOL IsWow64Process2(HANDLE h, uint16_t* proc, uint16_t* native) {
  (void)h;
  if (proc) *proc = 0;
  if (native) *native = 0x8664;
  return 1;
}
static WINAPI BOOL GetProductInfo(DWORD a, DWORD b, DWORD c, DWORD d, DWORD* type) {
  (void)a; (void)b; (void)c; (void)d;
  *type = 0x30; /* PRODUCT_PROFESSIONAL */
  return 1;
}
static WINAPI DWORD RtlNtStatusToDosError(long st) {
  if (st == 0) return 0;
  if ((DWORD)st == 0xC0000034u) return ERROR_FILE_NOT_FOUND;
  if ((DWORD)st == 0xC0000022u) return ERROR_ACCESS_DENIED;
  if ((DWORD)st == 0xC0000002u) return ERROR_NOT_SUPPORTED;
  return 317; /* ERROR_MR_MID_NOT_FOUND */
}

/* ---------- winmm ---------- */
static WINAPI DWORD timeBeginPeriod(DWORD p) {
  (void)p;
  return 0;
}
static WINAPI DWORD timeEndPeriod(DWORD p) {
  (void)p;
  return 0;
}
static WINAPI DWORD timeGetTime(void) { return k_ms(); }

/* ---------- processo / usuario ---------- */
static DWORD put16(const char* s, wchar16* buf, DWORD* n) {
  int len = wutf8_to_16(s, -1, 0, 0);
  if (!buf || (DWORD)len + 1 > *n) {
    *n = (DWORD)len + 1;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  wutf8_to_16(s, -1, buf, len);
  buf[len] = 0;
  *n = (DWORD)len;
  return 1;
}
static WINAPI BOOL GetUserNameW(wchar16* buf, DWORD* n) {
  BOOL r = (BOOL)put16("sodos", buf, n);
  if (r) (*n)++;
  return r;
}
static WINAPI BOOL GetComputerNameW(wchar16* buf, DWORD* n) { return (BOOL)put16("SODOS", buf, n); }
static WINAPI BOOL GetComputerNameExW(int type, wchar16* buf, DWORD* n) {
  (void)type;
  return (BOOL)put16("SODOS", buf, n);
}
static WINAPI BOOL GetUserProfileDirectoryW(HANDLE tok, wchar16* buf, DWORD* n) {
  (void)tok;
  return (BOOL)put16(wine_dir("USERS\\SODOS"), buf, n);
}
static WINAPI BOOL OpenProcessToken(HANDLE p, DWORD acc, HANDLE* tok) {
  struct wobj* o = wobj_new(OBJ_MISC);
  (void)p;
  (void)acc;
  *tok = o ? wh_new(o) : 0;
  return *tok != 0;
}
static WINAPI BOOL OpenThreadToken(HANDLE t, DWORD acc, BOOL self, HANDLE* tok) {
  (void)t; (void)acc; (void)self; (void)tok;
  wset_lasterr(1008); /* ERROR_NO_TOKEN */
  return 0;
}
/* O SODOS nao separa privilegios: todo programa e administrador. O token
 * diz isso (TokenElevation = 1, tipo Full), senao instaladores tentam se
 * relancar elevados (ShellExecute "runas"). TokenUser(1): S-1-5-21-...-1000. */
static const uint8_t user_sid[] = {1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0x53, 0x4F, 0x44, 0x4F,
                                   0x53, 0x57, 0x49, 0x4E, 0x45, 0x21, 0x21, 0x21, 0xE8, 3, 0, 0};
static WINAPI BOOL GetTokenInformation(HANDLE tok, int cls, void* buf, DWORD len, DWORD* need) {
  (void)tok;
  if (cls == 20 || cls == 18 || cls == 25 || cls == 29) { /* Elevation, ElevationType, IntegrityLevel, IsAppContainer */
    if (need) *need = cls == 25 ? 32 : 4;
    if (len < 4) {
      wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    if (cls == 25) {
      static const uint8_t medium[] = {1, 1, 0, 0, 0, 0, 0, 16, 0, 0x30, 0, 0}; /* High */
      if (len < 32) {
        wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
        return 0;
      }
      memset(buf, 0, 32);
      *(void**)buf = (uint8_t*)buf + 16;
      memcpy((uint8_t*)buf + 16, medium, sizeof(medium));
      return 1;
    }
    /* Elevation: 1; ElevationType: 2 (Full); IsAppContainer: 0 */
    *(DWORD*)buf = cls == 20 ? 1 : cls == 18 ? 2 : 0;
    return 1;
  }
  if (cls == 1) {
    DWORD sz = 16 + sizeof(user_sid);
    if (need) *need = sz;
    if (len < sz) {
      wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    memset(buf, 0, sz);
    *(void**)buf = (uint8_t*)buf + 16;
    memcpy((uint8_t*)buf + 16, user_sid, sizeof(user_sid));
    return 1;
  }
  if (need) *need = 0;
  wset_lasterr(ERROR_INVALID_PARAMETER);
  return 0;
}
static WINAPI BOOL ConvertSidToStringSidW(const void* sid, wchar16** out) {
  (void)sid;
  *out = wdup8to16("S-1-5-21-1329876819-1464486739-555819301-1000");
  return *out != 0;
}
static WINAPI DWORD GetLengthSid(const uint8_t* sid) { return 8 + 4u * sid[1]; }
static WINAPI BOOL IsValidSid(const void* sid) { return sid != 0; }
static WINAPI BOOL CopySid(DWORD n, uint8_t* dst, const uint8_t* src) {
  DWORD len = GetLengthSid(src);
  if (n < len) return 0;
  memcpy(dst, src, len);
  return 1;
}
static WINAPI BOOL EqualSid(const uint8_t* a, const uint8_t* b) {
  return GetLengthSid(a) == GetLengthSid(b) && memcmp(a, b, GetLengthSid(a)) == 0;
}
static WINAPI BOOL AllocateAndInitializeSid(const uint8_t* auth, uint8_t n, DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                                            DWORD a4, DWORD a5, DWORD a6, DWORD a7, uint8_t** out) {
  DWORD sub[8];
  uint8_t* s = (uint8_t*)wzalloc(8 + 32);
  if (!s) return 0;
  sub[0] = a0; sub[1] = a1; sub[2] = a2; sub[3] = a3;
  sub[4] = a4; sub[5] = a5; sub[6] = a6; sub[7] = a7;
  s[0] = 1;
  s[1] = n;
  memcpy(s + 2, auth, 6);
  memcpy(s + 8, sub, 4u * n);
  *out = s;
  return 1;
}
static WINAPI void* FreeSid(void* sid) {
  if (sid) k_free(sid);
  return 0;
}
static WINAPI BOOL CheckTokenMembership(HANDLE tok, const void* sid, BOOL* member) {
  (void)tok;
  (void)sid;
  *member = 1;   /* inclusive o grupo Administradores */
  return 1;
}
static WINAPI void* LocalFree_(void* p) {
  if (p) k_free(p);
  return 0;
}
static WINAPI void* GlobalAlloc(DWORD fl, size_t n) {
  void* p = k_malloc((unsigned)(n ? n : 1));
  if (p && (fl & 0x40)) memset(p, 0, n);
  return p;
}
static WINAPI void* GlobalFree(void* p) {
  if (p) k_free(p);
  return 0;
}
static WINAPI void* GlobalLock(void* p) { return p; }
static WINAPI BOOL GlobalUnlock(void* p) {
  (void)p;
  return 1;
}

/* ---------- pastas conhecidas (shell32) ---------- */
static const char* folder_for(DWORD d1) {
  switch (d1) {
    case 0xF1B32785u: return "USERS\\SODOS\\APPDATA\\LOCAL";   /* FOLDERID_LocalAppData */
    case 0x3EB685DBu: return "USERS\\SODOS\\APPDATA\\ROAMING"; /* RoamingAppData */
    case 0x5E6C858Fu: return "USERS\\SODOS";                    /* Profile */
    case 0xB4BFCC3Au: return "USERS\\SODOS\\DESKTOP";           /* Desktop */
    case 0x374DE290u: return "USERS\\SODOS\\DOWNLOADS";         /* Downloads */
    case 0xFDD39AD0u: return "USERS\\SODOS\\DOCUMENTS";         /* Documents */
    case 0x62AB5D82u: return "PROGRAMDATA";                     /* ProgramData */
    case 0x905E63B6u: return "PROGRAM FILES";                   /* ProgramFiles */
    case 0x6D809377u: return "PROGRAM FILES";                   /* ProgramFilesX64 */
    case 0x625B53C3u: return "USERS\\SODOS\\START MENU";        /* StartMenu */
    case 0xA77F5D77u: return "USERS\\SODOS\\START MENU\\PROGRAMS"; /* Programs */
    case 0xF38BF404u: return "";                                /* Windows */
    case 0x1AC14E77u: return "SYSTEM32";                        /* System */
  }
  return 0;
}
static WINAPI long SHGetKnownFolderPath(const DWORD* id, DWORD fl, HANDLE tok, wchar16** out) {
  const char* rel = folder_for(id[0]);
  (void)fl;
  (void)tok;
  if (!rel) {
    if (wine_debug) wlogx("SHGetKnownFolderPath: pasta desconhecida ", id[0]);
    *out = 0;
    return (long)0x80070002;
  }
  *out = wdup8to16(wine_dir(rel));
  return *out ? 0 : (long)0x8007000E;
}
static WINAPI long SHGetFolderPathW(HANDLE w, int csidl, HANDLE tok, DWORD fl, wchar16* out) {
  const char* rel;
  DWORD n = 260;
  (void)w; (void)tok; (void)fl;
  switch (csidl & 0xFF) {
    case 0x1C: rel = "USERS\\SODOS\\APPDATA\\LOCAL"; break;
    case 0x1A: rel = "USERS\\SODOS\\APPDATA\\ROAMING"; break;
    case 0x28: rel = "USERS\\SODOS"; break;
    case 0x00: case 0x10: rel = "USERS\\SODOS\\DESKTOP"; break;
    case 0x05: rel = "USERS\\SODOS\\DOCUMENTS"; break;
    case 0x23: rel = "PROGRAMDATA"; break;
    case 0x26: rel = "PROGRAM FILES"; break;
    case 0x02: rel = "USERS\\SODOS\\START MENU\\PROGRAMS"; break;
    case 0x24: rel = ""; break;
    case 0x25: rel = "SYSTEM32"; break;
    default: return (long)0x80070057;
  }
  return put16(wine_dir(rel), out, &n) ? 0 : (long)0x8007007A;
}
static WINAPI void CoTaskMemFree(void* p) {
  if (p) k_free(p);
}
static WINAPI void* CoTaskMemAlloc(size_t n) { return k_malloc((unsigned)(n ? n : 1)); }

/* CommandLineToArgvW: as regras de aspas e barras do CRT da Microsoft. */
static WINAPI wchar16** CommandLineToArgvW(const wchar16* cmd, int* argc) {
  size_t len = wstrlen16(cmd);
  wchar16** argv = (wchar16**)wzalloc((len / 2 + 2) * sizeof(wchar16*) + (len + 2) * 2);
  wchar16* d;
  const wchar16* s = cmd;
  int n = 0;
  if (!argv) return 0;
  d = (wchar16*)(argv + len / 2 + 2);
  while (*s) {
    int inq = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) break;
    argv[n++] = d;
    while (*s && (inq || (*s != ' ' && *s != '\t'))) {
      int bs = 0;
      while (*s == '\\') {
        bs++;
        s++;
      }
      if (*s == '"') {
        int k;
        for (k = 0; k < bs / 2; k++) *d++ = '\\';
        if (bs & 1) *d++ = '"';
        else inq = !inq;
        s++;
      } else {
        while (bs-- > 0) *d++ = '\\';
        if (*s && (inq || (*s != ' ' && *s != '\t'))) *d++ = *s++;
      }
    }
    *d++ = 0;
  }
  argv[n] = 0;
  *argc = n;
  return argv;
}

/* ---------- COM minimo: sem objetos, tudo "classe nao registrada" ---------- */
static WINAPI long CoInitializeEx(void* r, DWORD fl) {
  (void)r;
  (void)fl;
  return 0;
}
static WINAPI long CoInitialize(void* r) {
  (void)r;
  return 0;
}
static WINAPI void CoUninitialize(void) {}
static WINAPI long CoCreateInstance(const void* clsid, void* outer, DWORD ctx, const void* iid, void** out) {
  (void)outer;
  (void)ctx;
  (void)iid;
  if (wine_debug) wlogx("CoCreateInstance: classe ", *(const DWORD*)clsid);
  *out = 0;
  return (long)0x80040154; /* REGDB_E_CLASSNOTREG */
}
static WINAPI long RoInitialize(int type) {
  (void)type;
  return 0;
}
static WINAPI void RoUninitialize(void) {}

#define S(n) {#n, (void*)n}
const struct wine_sym sys_syms[] = {
    S(ProcessPrng), S(SystemFunction036), S(BCryptGenRandom), S(RtlGetVersion),
    S(RtlGetNtVersionNumbers), S(GetVersionExW), S(GetVersionExA), S(RtlGetCurrentPeb),
    S(IsWow64Process), S(IsWow64Process2), S(GetProductInfo), S(RtlNtStatusToDosError),
    S(timeBeginPeriod), S(timeEndPeriod), S(timeGetTime), S(GetUserNameW), S(GetComputerNameW),
    S(GetComputerNameExW), S(GetUserProfileDirectoryW), S(OpenProcessToken), S(OpenThreadToken),
    S(GetTokenInformation), S(ConvertSidToStringSidW), S(GetLengthSid), S(IsValidSid), S(CopySid),
    S(EqualSid), S(AllocateAndInitializeSid), S(FreeSid), S(CheckTokenMembership),
    S(GlobalAlloc), S(GlobalFree), S(GlobalLock), S(GlobalUnlock), S(SHGetKnownFolderPath),
    S(SHGetFolderPathW), S(CoTaskMemFree), S(CoTaskMemAlloc), S(CommandLineToArgvW),
    S(CoInitializeEx), S(CoInitialize), S(CoUninitialize), S(CoCreateInstance), S(RoInitialize),
    S(RoUninitialize),
    {0, 0}};
