/*
 * compat.c - O que programas de console classicos (MSVC + msvcrt.dll, como
 * o 7-Zip) pedem alem do basico: BSTR/VARIANT do OLEAUT32 (importados por
 * ordinal), datas do DOS, _beginthreadex, e respostas fixas para recursos
 * que o SODOS nao tem (mapeamento de arquivos, streams NTFS, hard links).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

uint64_t wfiletime_now(void);

/* ---------- OLEAUT32: BSTR = wchar16* com o tamanho em bytes 4 antes ---------- */
static wchar16* bstr_alloc(const void* src, DWORD bytes) {
  DWORD* p = (DWORD*)k_malloc(bytes + 4 + 2 + 8);
  if (!p) return 0;
  p[0] = 0;       /* alinhamento: o tamanho fica colado no texto */
  p[1] = bytes;
  if (src) memcpy(p + 2, src, bytes);
  else memset(p + 2, 0, bytes);
  ((uint8_t*)(p + 2))[bytes] = 0;
  ((uint8_t*)(p + 2))[bytes + 1] = 0;
  return (wchar16*)(p + 2);
}
static WINAPI wchar16* SysAllocStringLen(const wchar16* s, DWORD len) { return bstr_alloc(s, len * 2); }
static WINAPI wchar16* SysAllocString(const wchar16* s) { return s ? bstr_alloc(s, (DWORD)wstrlen16(s) * 2) : 0; }
static WINAPI wchar16* SysAllocStringByteLen(const char* s, DWORD len) { return bstr_alloc(s, len); }
static WINAPI void SysFreeString(wchar16* b) {
  if (b) k_free((DWORD*)b - 2);
}
static WINAPI DWORD SysStringLen(const wchar16* b) { return b ? ((const DWORD*)b)[-1] / 2 : 0; }
static WINAPI DWORD SysStringByteLen(const wchar16* b) { return b ? ((const DWORD*)b)[-1] : 0; }
static WINAPI int SysReAllocStringLen(wchar16** pb, const wchar16* s, DWORD len) {
  wchar16* n = bstr_alloc(s, len * 2);
  if (!n) return 0;
  SysFreeString(*pb);
  *pb = n;
  return 1;
}
struct variant {
  uint16_t vt, r1, r2, r3;
  uint64_t val, val2;
};
static WINAPI void VariantInit(struct variant* v) { memset(v, 0, sizeof(*v)); }
static WINAPI long VariantClear(struct variant* v) {
  if (v->vt == 8) SysFreeString((wchar16*)(uintptr_t)v->val); /* VT_BSTR */
  memset(v, 0, sizeof(*v));
  return 0;
}
static WINAPI long VariantCopy(struct variant* d, const struct variant* s) {
  if (d == s) return 0;
  VariantClear(d);
  *d = *s;
  if (s->vt == 8 && s->val) {
    const wchar16* b = (const wchar16*)(uintptr_t)s->val;
    d->val = (uint64_t)(uintptr_t)bstr_alloc(b, SysStringByteLen(b));
    if (!d->val) return (long)0x8007000E;
  }
  return 0;
}

static const struct {
  uint16_t ord;
  void* fn;
} oleaut_ord[] = {{2, SysAllocString},   {4, SysAllocStringLen},      {5, SysReAllocStringLen}, {6, SysFreeString},
                  {7, SysStringLen},     {8, VariantInit},            {9, VariantClear},        {10, VariantCopy},
                  {149, SysStringByteLen}, {150, SysAllocStringByteLen}};
static WINAPI long IUnknown_QueryService(void* unk, const void* sid, const void* iid, void** out);
static const struct {
  uint16_t ord;
  void* fn;
} shlwapi_ord[] = {{176, IUnknown_QueryService}};

/* Import por ordinal de uma DLL embutida (pe.c). */
void* builtin_ordinal(const char* dll, unsigned ord) {
  size_t i;
  char n[16];
  wlower(n, dll, (int)sizeof(n));
  {
    void* f = uimisc_ordinal(n, ord);
    if (f) return f;
  }
  if (strncmp(n, "shlwapi", 7) == 0) {
    for (i = 0; i < sizeof(shlwapi_ord) / sizeof(shlwapi_ord[0]); i++)
      if (shlwapi_ord[i].ord == ord) return shlwapi_ord[i].fn;
    return 0;
  }
  if (strncmp(n, "oleaut32", 8) != 0) return 0;
  for (i = 0; i < sizeof(oleaut_ord) / sizeof(oleaut_ord[0]); i++)
    if (oleaut_ord[i].ord == ord) return oleaut_ord[i].fn;
  return 0;
}

/* ---------- msvcrt ---------- */
static WINAPI int ms_wcscmp(const wchar16* a, const wchar16* b) {
  for (;; a++, b++) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (!*a) return 0;
  }
}
static WINAPI wchar16* ms_wcsstr(const wchar16* h, const wchar16* n) {
  size_t nl = wstrlen16(n);
  if (!nl) return (wchar16*)h;
  for (; *h; h++)
    if (*h == *n && memcmp(h, n, nl * 2) == 0) return (wchar16*)h;
  return 0;
}
static WINAPI wchar16* ms_wcschr(const wchar16* s, wchar16 c) {
  for (;; s++) {
    if (*s == c) return (wchar16*)s;
    if (!*s) return 0;
  }
}
static WINAPI wchar16* ms_wcscpy(wchar16* d, const wchar16* s) {
  wchar16* r = d;
  while ((*d++ = *s++)) {
  }
  return r;
}
static WINAPI wchar16* ms_wcscat(wchar16* d, const wchar16* s) {
  ms_wcscpy(d + wstrlen16(d), s);
  return d;
}
static WINAPI int ms_wcsncmp(const wchar16* a, const wchar16* b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (!*a) return 0;
  }
  return 0;
}
static WINAPI void* __dllonexit(void* fn, void** begin, void** end) {
  (void)begin;
  (void)end;
  return fn; /* sem lista: o processo some inteiro no fim */
}
static WINAPI uintptr_t _beginthreadex(void* sec, unsigned stack, void* start, void* arg, unsigned flags,
                                       unsigned* tid) {
  typedef HANDLE (WINAPI * ct_fn)(void*, size_t, void*, void*, DWORD, DWORD*);
  ct_fn ct = (ct_fn)builtin_lookup("CreateThread");
  DWORD id = 0;
  HANDLE h = ct ? ct(sec, stack, start, arg, flags, &id) : 0;
  if (tid) *tid = id;
  return (uintptr_t)h;
}
static WINAPI void _endthreadex(unsigned code) {
  typedef void (WINAPI * et_fn)(DWORD);
  et_fn et = (et_fn)builtin_lookup("ExitThread");
  if (et) et(code);
}
static WINAPI intptr_t _get_osfhandle(int fd) {
  if (fd == 0) return 0x10;
  if (fd == 1) return 0x14;
  if (fd == 2) return 0x18;
  return -1;
}

/* Excecoes C++ da MSVC: sem o desempilhamento por tabela (marco W1c), um
 * throw nao tem como chegar ao catch. Diz o que houve e encerra. */
static WINAPI void _CxxThrowException(void* obj, const uint32_t* info) {
  (void)info;
  wlogx("throw do C++ (sem suporte a excecoes C++ ainda), objeto ", (uint64_t)(uintptr_t)obj);
  if (obj) wlogx("  primeiro campo do objeto ", *(const uint32_t*)obj);
  k_puts("wine: o programa lancou uma excecao C++ (ainda nao suportado)\n");
  wexit(0xE06D7363);
}
static WINAPI int __CxxFrameHandler(void* a, void* b, void* c, void* d) {
  (void)a; (void)b; (void)c; (void)d;
  return 1; /* ExceptionContinueSearch */
}

/* ---------- kernel32 ---------- */
static WINAPI size_t GetLargePageMinimum(void) { return 0; }
static WINAPI void SetFileApisToOEM(void) {}
static WINAPI void SetFileApisToANSI(void) {}
static WINAPI BOOL AreFileApisANSI(void) { return 1; }
static WINAPI BOOL SetProcessAffinityMask(HANDLE h, uintptr_t m) {
  (void)h;
  (void)m;
  return 1;
}
static WINAPI HANDLE OpenEventW(DWORD a, BOOL i, const wchar16* n) {
  (void)a; (void)i; (void)n;
  wset_lasterr(ERROR_FILE_NOT_FOUND);
  return 0;
}
static WINAPI HANDLE OpenFileMappingW(DWORD a, BOOL i, const wchar16* n) {
  (void)a; (void)i; (void)n;
  wset_lasterr(ERROR_FILE_NOT_FOUND);
  return 0;
}
static WINAPI HANDLE CreateFileMappingW(HANDLE f, void* sa, DWORD prot, DWORD hi, DWORD lo, const wchar16* n) {
  (void)f; (void)sa; (void)prot; (void)hi; (void)lo; (void)n;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI void* MapViewOfFile(HANDLE m, DWORD acc, DWORD hi, DWORD lo, size_t n) {
  (void)m; (void)acc; (void)hi; (void)lo; (void)n;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI BOOL UnmapViewOfFile(const void* p) {
  (void)p;
  return 0;
}
static WINAPI BOOL DeviceIoControl(HANDLE h, DWORD code, void* in, DWORD nin, void* out, DWORD nout, DWORD* ret,
                                   void* ov) {
  (void)h; (void)code; (void)in; (void)nin; (void)out; (void)nout; (void)ov;
  if (ret) *ret = 0;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI BOOL CreateHardLinkW(const wchar16* a, const wchar16* b, void* sa) {
  (void)a; (void)b; (void)sa;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI HANDLE FindFirstStreamW(const wchar16* f, int lvl, void* data, DWORD fl) {
  (void)f; (void)lvl; (void)data; (void)fl;
  wset_lasterr(ERROR_HANDLE_EOF); /* sem streams alternativos */
  return INVALID_HANDLE_VALUE;
}
static WINAPI BOOL FindNextStreamW(HANDLE h, void* data) {
  (void)h;
  (void)data;
  wset_lasterr(ERROR_HANDLE_EOF);
  return 0;
}
static WINAPI BOOL MoveFileWithProgressW(const wchar16* a, const wchar16* b, void* cb, void* data, DWORD fl) {
  typedef BOOL (WINAPI * mv_fn)(const wchar16*, const wchar16*, DWORD);
  mv_fn mv = (mv_fn)builtin_lookup("MoveFileExW");
  (void)cb;
  (void)data;
  return mv ? mv(a, b, fl) : 0;
}
static WINAPI BOOL GetDiskFreeSpaceW(const wchar16* root, DWORD* spc, DWORD* bps, DWORD* freec, DWORD* totalc) {
  unsigned t = 0, f = 0;
  char r = wp()->cwd[0];
  if (root && root[0] && root[1] == ':') r = (char)root[0];
  if (k_dfree(r, &t, &f) < 0) {
    t = 64u << 20;
    f = 32u << 20;
  }
  if (spc) *spc = 8;
  if (bps) *bps = 512;
  if (freec) *freec = f / 4; /* clusters de 4 KB */
  if (totalc) *totalc = t / 4;
  return 1;
}
static WINAPI DWORD GetLogicalDriveStringsW(DWORD n, wchar16* buf) {
  unsigned mask = k_drive_mask();
  DWORD need = 1, o = 0;
  int i;
  for (i = 0; i < 26; i++)
    if (mask & (1u << i)) need += 4;
  if (need > n) return need;
  for (i = 0; i < 26; i++)
    if (mask & (1u << i)) {
      buf[o++] = (wchar16)('A' + i);
      buf[o++] = ':';
      buf[o++] = '\\';
      buf[o++] = 0;
    }
  buf[o] = 0;
  return o;
}
static WINAPI long CompareFileTime(const uint64_t* a, const uint64_t* b) { return *a < *b ? -1 : *a > *b ? 1 : 0; }
static WINAPI BOOL LocalFileTimeToFileTime(const uint64_t* a, uint64_t* b) {
  *b = *a;
  return 1;
}
/* FILETIME <-> data/hora do DOS (FAT): por SYSTEMTIME, do misc.c */
struct systemtime {
  uint16_t year, month, dow, day, hour, minute, second, ms;
};
static WINAPI BOOL FileTimeToDosDateTime(const uint64_t* ft, uint16_t* date, uint16_t* time) {
  typedef BOOL (WINAPI * f2s)(const uint64_t*, struct systemtime*);
  f2s conv = (f2s)builtin_lookup("FileTimeToSystemTime");
  struct systemtime st;
  if (!conv || !conv(ft, &st) || st.year < 1980 || st.year > 2107) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  *date = (uint16_t)(((st.year - 1980) << 9) | (st.month << 5) | st.day);
  *time = (uint16_t)((st.hour << 11) | (st.minute << 5) | (st.second / 2));
  return 1;
}
static WINAPI BOOL DosDateTimeToFileTime(uint16_t date, uint16_t time, uint64_t* ft) {
  typedef BOOL (WINAPI * s2f)(const struct systemtime*, uint64_t*);
  s2f conv = (s2f)builtin_lookup("SystemTimeToFileTime");
  struct systemtime st;
  memset(&st, 0, sizeof(st));
  st.year = (uint16_t)(1980 + (date >> 9));
  st.month = (uint16_t)((date >> 5) & 15);
  st.day = (uint16_t)(date & 31);
  st.hour = (uint16_t)(time >> 11);
  st.minute = (uint16_t)((time >> 5) & 63);
  st.second = (uint16_t)((time & 31) * 2);
  return conv ? conv(&st, ft) : 0;
}

/* ---------- user32 / advapi32 ---------- */
static WINAPI wchar16* CharUpperW(wchar16* s) {
  /* ponteiro ou um caractere so (palavra alta zero) */
  if ((uintptr_t)s < 0x10000) {
    wchar16 c = (wchar16)(uintptr_t)s;
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c = (wchar16)(c - 32);
    return (wchar16*)(uintptr_t)c;
  }
  {
    wchar16* p;
    for (p = s; *p; p++)
      if ((*p >= 'a' && *p <= 'z') || (*p >= 0xE0 && *p <= 0xFE && *p != 0xF7)) *p = (wchar16)(*p - 32);
  }
  return s;
}
static WINAPI wchar16* CharLowerW(wchar16* s) {
  if ((uintptr_t)s < 0x10000) {
    wchar16 c = (wchar16)(uintptr_t)s;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c = (wchar16)(c + 32);
    return (wchar16*)(uintptr_t)c;
  }
  {
    wchar16* p;
    for (p = s; *p; p++)
      if ((*p >= 'A' && *p <= 'Z') || (*p >= 0xC0 && *p <= 0xDE && *p != 0xD7)) *p = (wchar16)(*p + 32);
  }
  return s;
}
static WINAPI char* CharPrevExA(uint16_t cp, const char* start, const char* cur, DWORD fl) {
  (void)cp;
  (void)fl;
  return (char*)(cur > start ? cur - 1 : start);
}
static WINAPI char* CharNextA(const char* s) { return (char*)(*s ? s + 1 : s); }
static WINAPI BOOL GetFileSecurityW(const wchar16* f, DWORD info, void* sd, DWORD n, DWORD* need) {
  (void)f; (void)info; (void)sd; (void)n;
  if (need) *need = 0;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI BOOL SetFileSecurityW(const wchar16* f, DWORD info, void* sd) {
  (void)f; (void)info; (void)sd;
  return 1;
}

/* ---------- miudezas de console, OEM, seguranca ---------- */
static WINAPI void OutputDebugStringW(const wchar16* s) {
  char* t = wdup16to8(s);
  wlog("OutputDebugString: ", t ? t : "");
  k_free(t);
}
static WINAPI void OutputDebugStringA(const char* s) { wlog("OutputDebugString: ", s ? s : ""); }
static WINAPI BOOL ReadConsoleW(HANDLE h, wchar16* buf, DWORD n, DWORD* got, void* ctl) {
  char line[256];
  int len = wcon_readline(line, (int)sizeof(line) - 2), w;
  (void)h;
  (void)ctl;
  if (len < 0) len = 0;
  line[len++] = 13;
  line[len++] = 10;
  w = wutf8_to_16(line, len, buf, (int)n);
  if (got) *got = (DWORD)w;
  return 1;
}
/* OEM <-> ANSI: o SODOS e UTF-8/Latin-1 nas duas pontas: so copia */
static WINAPI BOOL OemToCharA(const char* s, char* d) {
  if (d != s) strcpy(d, s);
  return 1;
}
static WINAPI BOOL OemToCharBuffA(const char* s, char* d, DWORD n) {
  if (d != s) memcpy(d, s, n);
  return 1;
}
static WINAPI BOOL OemToCharBuffW(const char* s, wchar16* d, DWORD n) {
  DWORD i;
  for (i = 0; i < n; i++) d[i] = (uint8_t)s[i];
  return 1;
}
static WINAPI BOOL CharToOemA(const char* s, char* d) { return OemToCharA(s, d); }
static WINAPI BOOL CharToOemBuffA(const char* s, char* d, DWORD n) { return OemToCharBuffA(s, d, n); }
static WINAPI BOOL CharToOemBuffW(const wchar16* s, char* d, DWORD n) {
  DWORD i;
  for (i = 0; i < n; i++) d[i] = s[i] < 256 ? (char)s[i] : '?';
  return 1;
}
static WINAPI int CompareStringA(DWORD lcid, DWORD fl, const char* a, int na, const char* b, int nb) {
  int i;
  (void)lcid;
  if (na < 0) na = (int)strlen(a);
  if (nb < 0) nb = (int)strlen(b);
  for (i = 0; i < na && i < nb; i++) {
    char x = a[i], y = b[i];
    if (fl & 1) {
      if (x >= 'a' && x <= 'z') x = (char)(x - 32);
      if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    }
    if (x != y) return (uint8_t)x < (uint8_t)y ? 1 : 3;
  }
  return na == nb ? 2 : na < nb ? 1 : 3;
}
static WINAPI BOOL BackupRead(HANDLE h, void* b, DWORD n, DWORD* got, BOOL abort, BOOL sec, void** ctx) {
  (void)h; (void)b; (void)n; (void)abort; (void)sec; (void)ctx;
  if (got) *got = 0;
  return 1; /* fim: sem streams de backup */
}
static WINAPI BOOL BackupSeek(HANDLE h, DWORD lo, DWORD hi, DWORD* slo, DWORD* shi, void** ctx) {
  (void)h; (void)lo; (void)hi; (void)ctx;
  if (slo) *slo = 0;
  if (shi) *shi = 0;
  return 0;
}
static WINAPI BOOL ExitWindowsEx(DWORD fl, DWORD r) {
  (void)fl;
  (void)r;
  wlog("ExitWindowsEx ignorado (o programa pediu para reiniciar o Windows)", 0);
  return 0;
}
static WINAPI BOOL SetSuspendState(BOOL h, BOOL f, BOOL w) {
  (void)h; (void)f; (void)w;
  return 0;
}
static WINAPI long LsaOpenPolicy(void* sys, void* attr, DWORD acc, HANDLE* h) {
  (void)sys; (void)attr; (void)acc;
  if (h) *h = 0;
  return (long)0xC0000022; /* STATUS_ACCESS_DENIED */
}
static WINAPI long LsaClose(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI long LsaAddAccountRights(HANDLE h, void* sid, void* r, DWORD n) {
  (void)h; (void)sid; (void)r; (void)n;
  return (long)0xC0000022;
}
static WINAPI BOOL ConvertStringSidToSidW(const wchar16* s, void** sid) {
  uint8_t* p = (uint8_t*)wzalloc(16);
  (void)s;
  if (!p) return 0;
  p[0] = 1;
  p[1] = 1;
  p[7] = 5;
  *sid = p;
  return 1;
}
static WINAPI DWORD GetSecurityDescriptorLength(void* sd) {
  (void)sd;
  return 20;
}
/* SHLWAPI #176: IUnknown_QueryService */
static WINAPI long IUnknown_QueryService(void* unk, const void* sid, const void* iid, void** out) {
  (void)unk; (void)sid; (void)iid;
  if (out) *out = 0;
  return (long)0x80004002; /* E_NOINTERFACE */
}

#define S(n) {#n, (void*)n}
const struct wine_sym compat_syms[] = {
    S(OutputDebugStringW), S(OutputDebugStringA), S(ReadConsoleW), S(OemToCharA), S(OemToCharBuffA),
    S(OemToCharBuffW), S(CharToOemA), S(CharToOemBuffA), S(CharToOemBuffW), S(CompareStringA), S(BackupRead),
    S(BackupSeek), S(ExitWindowsEx), S(SetSuspendState), S(LsaOpenPolicy), S(LsaClose), S(LsaAddAccountRights),
    S(ConvertStringSidToSidW), S(GetSecurityDescriptorLength), S(IUnknown_QueryService),
    S(SysAllocStringLen), S(SysAllocString), S(SysAllocStringByteLen), S(SysFreeString), S(SysStringLen),
    S(SysStringByteLen), S(SysReAllocStringLen), S(VariantInit), S(VariantClear), S(VariantCopy),
    {"wcscmp", (void*)ms_wcscmp}, {"wcsstr", (void*)ms_wcsstr}, {"wcschr", (void*)ms_wcschr},
    {"wcscpy", (void*)ms_wcscpy}, {"wcscat", (void*)ms_wcscat}, {"wcsncmp", (void*)ms_wcsncmp},
    S(__dllonexit), S(_beginthreadex), S(_endthreadex), S(_get_osfhandle), S(_CxxThrowException),
    S(__CxxFrameHandler), {"__CxxFrameHandler3", (void*)__CxxFrameHandler}, S(GetLargePageMinimum),
    S(SetFileApisToOEM), S(SetFileApisToANSI), S(AreFileApisANSI), S(SetProcessAffinityMask), S(OpenEventW),
    S(OpenFileMappingW), S(CreateFileMappingW), S(MapViewOfFile), S(UnmapViewOfFile), S(DeviceIoControl),
    S(CreateHardLinkW), S(FindFirstStreamW), S(FindNextStreamW), S(MoveFileWithProgressW),
    S(GetDiskFreeSpaceW), S(GetLogicalDriveStringsW), S(CompareFileTime), S(LocalFileTimeToFileTime),
    S(FileTimeToDosDateTime), S(DosDateTimeToFileTime), S(CharUpperW), S(CharLowerW), S(CharPrevExA),
    S(CharNextA), S(GetFileSecurityW), S(SetFileSecurityW),
    {0, 0}};
