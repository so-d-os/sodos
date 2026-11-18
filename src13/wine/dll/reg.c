/*
 * reg.c - Registro do Windows (advapi32 Reg*), em memoria.
 *
 * Cada processo comeca com um registro quase vazio: so as chaves que
 * programas costumam consultar para saber a versao do Windows. O que o
 * programa grava vale ate ele terminar (nada vai para o disco).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define HKCR ((uintptr_t)0x80000000u)
#define HKCU ((uintptr_t)0x80000001u)
#define HKLM ((uintptr_t)0x80000002u)
#define HKU ((uintptr_t)0x80000003u)

#define REG_SZ 1
#define REG_DWORD 4

struct rval {
  struct rval* next;
  char* name;
  DWORD type;
  uint8_t* data;
  DWORD size;
};
struct rkey {
  struct rkey* next;
  char* path;       /* "HKLM\SOFTWARE\..." em minusculas */
  struct rval* vals;
};
const char* wine_dir(const char* rel); /* file.c */

struct wreg {
  struct rkey* keys;
};

static char* sdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* d = (char*)k_malloc((unsigned)n);
  if (d) memcpy(d, s, n);
  return d;
}

static struct rkey* key_find(struct wreg* r, const char* path) {
  struct rkey* k;
  for (k = r->keys; k; k = k->next)
    if (strcmp(k->path, path) == 0) return k;
  return 0;
}
static struct rkey* key_make(struct wreg* r, const char* path) {
  struct rkey* k = key_find(r, path);
  if (k) return k;
  k = (struct rkey*)wzalloc(sizeof(*k));
  if (!k) return 0;
  k->path = sdup(path);
  k->next = r->keys;
  r->keys = k;
  return k;
}
static struct rval* val_find(struct rkey* k, const char* name) {
  struct rval* v;
  for (v = k->vals; v; v = v->next)
    if (wstreq(v->name, name)) return v;
  return 0;
}
static void val_set(struct rkey* k, const char* name, DWORD type, const void* data, DWORD size) {
  struct rval* v = val_find(k, name);
  if (!v) {
    v = (struct rval*)wzalloc(sizeof(*v));
    if (!v) return;
    v->name = sdup(name);
    v->next = k->vals;
    k->vals = v;
  }
  if (v->data) k_free(v->data);
  v->data = (uint8_t*)k_malloc(size ? size : 1);
  if (v->data) memcpy(v->data, data, size);
  v->type = type;
  v->size = size;
}
static void set_sz(struct rkey* k, const char* name, const char* s) {
  wchar16* w = wdup8to16(s);
  if (!w) return;
  val_set(k, name, REG_SZ, w, (DWORD)(wstrlen16(w) + 1) * 2);
  k_free(w);
}
static void set_dw(struct rkey* k, const char* name, DWORD v) { val_set(k, name, REG_DWORD, &v, 4); }

static struct wreg* reg_get(void) {
  struct wproc* p = wp();
  struct wreg* r = (struct wreg*)p->reg;
  struct rkey* k;
  if (r) return r;
  r = (struct wreg*)wzalloc(sizeof(*r));
  if (!r) return 0;
  p->reg = r;
  k = key_make(r, "hklm\\software\\microsoft\\windows nt\\currentversion");
  if (k) {
    set_sz(k, "ProductName", "Windows 10 Pro");
    set_sz(k, "CurrentVersion", "6.3");
    set_sz(k, "CurrentBuild", "19045");
    set_sz(k, "CurrentBuildNumber", "19045");
    set_sz(k, "DisplayVersion", "22H2");
    set_sz(k, "ReleaseId", "2009");
    set_sz(k, "EditionID", "Professional");
    set_dw(k, "CurrentMajorVersionNumber", 10);
    set_dw(k, "CurrentMinorVersionNumber", 0);
    set_dw(k, "UBR", 5131);
  }
  key_make(r, "hklm\\software");
  key_make(r, "hkcu\\software");
  k = key_make(r, "hklm\\software\\microsoft\\windows\\currentversion");
  if (k) {
    /* pastas de programas: dentro de D:\WINE (file.c) */
    const char* pf = wine_dir("PROGRAM FILES");
    set_sz(k, "ProgramFilesDir", pf);
    set_sz(k, "ProgramW6432Dir", pf);
    set_sz(k, "ProgramFilesDir (x86)", pf);
    set_sz(k, "CommonFilesDir", wine_dir("PROGRAM FILES\\COMMON FILES"));
  }
  key_make(r, "hkcu\\software\\microsoft\\windows\\currentversion");
  return r;
}

/* HKEY + subchave -> caminho em minusculas. HKEY aberto = handle de OBJ_KEY. */
static int key_path(HANDLE h, const char* sub, char* out, int max) {
  uintptr_t v = (uintptr_t)h & 0xFFFFFFFFu;
  const char* root = 0;
  int n;
  if (v == HKLM) root = "hklm";
  else if (v == HKCU) root = "hkcu";
  else if (v == HKCR) root = "hkcr";
  else if (v == HKU) root = "hku";
  if (root) {
    strcpy(out, root);
  } else {
    struct wobj* o = wh_get(h, OBJ_KEY);
    if (!o || !o->path) return 0;
    strncpy(out, o->path, (size_t)max - 1);
    out[max - 1] = 0;
  }
  if (sub && *sub) {
    while (*sub == '\\') sub++;
    n = (int)strlen(out);
    if (n + 2 >= max) return 0;
    out[n++] = '\\';
    wlower(out + n, sub, max - n);
    n = (int)strlen(out);
    while (n > 0 && out[n - 1] == '\\') out[--n] = 0;
  }
  return 1;
}

static HANDLE key_handle(const char* path) {
  struct wobj* o = wobj_new(OBJ_KEY);
  if (!o) return 0;
  o->path = sdup(path);
  return wh_new(o);
}

static WINAPI long RegOpenKeyExW(HANDLE h, const wchar16* sub, DWORD opt, DWORD acc, HANDLE* out) {
  struct wreg* r = reg_get();
  char s8[300], path[400];
  int n = sub ? wutf16_to_8(sub, -1, s8, (int)sizeof(s8) - 1) : 0;
  (void)opt;
  (void)acc;
  s8[n] = 0;
  if (!r || !key_path(h, s8, path, (int)sizeof(path))) return ERROR_INVALID_HANDLE;
  if (!key_find(r, path)) {
    if (wine_debug) wlog("RegOpenKey (nao existe) ", path);
    return ERROR_FILE_NOT_FOUND;
  }
  *out = key_handle(path);
  return 0;
}
static WINAPI long RegOpenKeyExA(HANDLE h, const char* sub, DWORD opt, DWORD acc, HANDLE* out) {
  wchar16* w = sub ? wdup8to16(sub) : 0;
  long r = RegOpenKeyExW(h, w, opt, acc, out);
  k_free(w);
  return r;
}
static WINAPI long RegCreateKeyExW(HANDLE h, const wchar16* sub, DWORD res, wchar16* cls, DWORD opt, DWORD acc,
                                   void* sa, HANDLE* out, DWORD* disp) {
  struct wreg* r = reg_get();
  char s8[300], path[400];
  int n = sub ? wutf16_to_8(sub, -1, s8, (int)sizeof(s8) - 1) : 0;
  (void)res; (void)cls; (void)opt; (void)acc; (void)sa;
  s8[n] = 0;
  if (!r || !key_path(h, s8, path, (int)sizeof(path))) return ERROR_INVALID_HANDLE;
  if (disp) *disp = key_find(r, path) ? 2 : 1;
  if (!key_make(r, path)) return ERROR_NOT_ENOUGH_MEMORY;
  if (wine_debug) wlog("RegCreateKey ", path);
  *out = key_handle(path);
  return 0;
}
static WINAPI long RegCloseKey(HANDLE h) {
  if (wh_get(h, OBJ_KEY)) wh_close(h);
  return 0;
}
static struct rkey* key_of(HANDLE h, const char* sub) {
  struct wreg* r = reg_get();
  char path[400];
  if (!r || !key_path(h, sub, path, (int)sizeof(path))) return 0;
  return key_find(r, path);
}
static long query(struct rkey* k, const char* name, DWORD* type, uint8_t* data, DWORD* size) {
  struct rval* v = k ? val_find(k, name) : 0;
  if (!v) {
    if (wine_debug) wlog("RegQueryValue (nao existe) ", name);
    return ERROR_FILE_NOT_FOUND;
  }
  if (type) *type = v->type;
  if (size) {
    DWORD have = *size;
    *size = v->size;
    if (data) {
      if (have < v->size) return ERROR_MORE_DATA;
      memcpy(data, v->data, v->size);
    }
  }
  return 0;
}
static WINAPI long RegQueryValueExW(HANDLE h, const wchar16* name, DWORD* res, DWORD* type, uint8_t* data,
                                    DWORD* size) {
  char n8[200];
  int n = name ? wutf16_to_8(name, -1, n8, (int)sizeof(n8) - 1) : 0;
  (void)res;
  n8[n] = 0;
  return query(key_of(h, 0), n8, type, data, size);
}
static WINAPI long RegGetValueW(HANDLE h, const wchar16* sub, const wchar16* name, DWORD fl, DWORD* type,
                                void* data, DWORD* size) {
  char s8[300], n8[200];
  int n = sub ? wutf16_to_8(sub, -1, s8, (int)sizeof(s8) - 1) : 0;
  (void)fl;
  s8[n] = 0;
  n = name ? wutf16_to_8(name, -1, n8, (int)sizeof(n8) - 1) : 0;
  n8[n] = 0;
  return query(key_of(h, s8), n8, type, (uint8_t*)data, size);
}
static WINAPI long RegSetValueExW(HANDLE h, const wchar16* name, DWORD res, DWORD type, const uint8_t* data,
                                  DWORD size) {
  struct rkey* k = key_of(h, 0);
  char n8[200];
  int n = name ? wutf16_to_8(name, -1, n8, (int)sizeof(n8) - 1) : 0;
  (void)res;
  n8[n] = 0;
  if (!k) return ERROR_INVALID_HANDLE;
  val_set(k, n8, type, data, size);
  return 0;
}
static WINAPI long RegDeleteValueW(HANDLE h, const wchar16* name) {
  (void)h;
  (void)name;
  return 0;
}
static WINAPI long RegDeleteKeyW(HANDLE h, const wchar16* sub) {
  (void)h;
  (void)sub;
  return 0;
}
static WINAPI long RegDeleteKeyExW(HANDLE h, const wchar16* sub, DWORD sam, DWORD res) {
  (void)h; (void)sub; (void)sam; (void)res;
  return 0;
}
static WINAPI long RegDeleteTreeW(HANDLE h, const wchar16* sub) {
  (void)h;
  (void)sub;
  return 0;
}
static WINAPI long RegEnumKeyExW(HANDLE h, DWORD idx, wchar16* name, DWORD* nlen, DWORD* res, wchar16* cls,
                                 DWORD* clen, void* time) {
  (void)h; (void)idx; (void)name; (void)nlen; (void)res; (void)cls; (void)clen; (void)time;
  return ERROR_NO_MORE_ITEMS;
}
static WINAPI long RegEnumValueW(HANDLE h, DWORD idx, wchar16* name, DWORD* nlen, DWORD* res, DWORD* type,
                                 uint8_t* data, DWORD* size) {
  struct rkey* k = key_of(h, 0);
  struct rval* v;
  DWORD i = 0;
  int w;
  (void)res;
  if (!k) return ERROR_INVALID_HANDLE;
  for (v = k->vals; v && i < idx; v = v->next) i++;
  if (!v) return ERROR_NO_MORE_ITEMS;
  w = wutf8_to_16(v->name, -1, 0, 0);
  if (!nlen || (DWORD)w + 1 > *nlen) return ERROR_MORE_DATA;
  wutf8_to_16(v->name, -1, name, w);
  name[w] = 0;
  *nlen = (DWORD)w;
  if (type) *type = v->type;
  if (size) {
    DWORD have = *size;
    *size = v->size;
    if (data) {
      if (have < v->size) return ERROR_MORE_DATA;
      memcpy(data, v->data, v->size);
    }
  }
  return 0;
}
static WINAPI long RegQueryInfoKeyW(HANDLE h, wchar16* cls, DWORD* clen, DWORD* res, DWORD* nsub, DWORD* maxsub,
                                    DWORD* maxcls, DWORD* nvals, DWORD* maxname, DWORD* maxval, DWORD* sd,
                                    void* time) {
  struct rkey* k = key_of(h, 0);
  struct rval* v;
  DWORD n = 0, mn = 0, mv = 0;
  (void)cls; (void)res; (void)sd; (void)time;
  if (!k) return ERROR_INVALID_HANDLE;
  for (v = k->vals; v; v = v->next) {
    n++;
    if (strlen(v->name) > mn) mn = (DWORD)strlen(v->name);
    if (v->size > mv) mv = v->size;
  }
  if (clen) *clen = 0;
  if (nsub) *nsub = 0;
  if (maxsub) *maxsub = 0;
  if (maxcls) *maxcls = 0;
  if (nvals) *nvals = n;
  if (maxname) *maxname = mn;
  if (maxval) *maxval = mv;
  return 0;
}
static WINAPI long RegFlushKey(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI long RegNotifyChangeKeyValue(HANDLE h, BOOL sub, DWORD flt, HANDLE ev, BOOL async) {
  (void)h; (void)sub; (void)flt; (void)ev; (void)async;
  return 0;
}

#define S(n) {#n, (void*)n}
const struct wine_sym reg_syms[] = {
    S(RegOpenKeyExW), S(RegOpenKeyExA), S(RegCreateKeyExW), S(RegCloseKey), S(RegQueryValueExW),
    S(RegGetValueW), S(RegSetValueExW), S(RegDeleteValueW), S(RegDeleteKeyW), S(RegDeleteKeyExW),
    S(RegDeleteTreeW), S(RegEnumKeyExW), S(RegEnumValueW), S(RegQueryInfoKeyW), S(RegFlushKey),
    S(RegNotifyChangeKeyValue),
    {0, 0}};
