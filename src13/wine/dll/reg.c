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
  {
    /* navegador padrao: o Electron confere HKCR\<esquema>\shell\open antes do
     * ShellExecute da URL. O comando e so para constar: quem escolhe o
     * navegador e o LINKEXT do SODOS (LINKEXT.CFG, HTTP,HTTPS=programa; proc.c) */
    static const char* const sch[] = {"http", "https", "mailto", 0};
    int i;
    char kp[80];
    for (i = 0; sch[i]; i++) {
      strcpy(kp, "hkcr\\");
      strcat(kp, sch[i]);
      k = key_make(r, kp);
      if (k) {
        set_sz(k, "", "URL:Protocol");
        set_sz(k, "URL Protocol", "");
      }
      strcat(kp, "\\shell");
      key_make(r, kp);
      strcat(kp, "\\open");
      key_make(r, kp);
      strcat(kp, "\\command");
      k = key_make(r, kp);
      if (k) set_sz(k, "", "rundll32.exe url.dll,FileProtocolHandler %1");
    }
  }
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

/* ---------- registro nativo da ntdll (Nt*Key) ----------
 * O chrome_elf.dll do Chrome le e grava o registro por aqui, com caminhos
 * "\Registry\Machine\..." e "\Registry\User\<SID>\..." (a SID vem do
 * RtlFormatCurrentUserKeyPath, misc.c). Sao as mesmas chaves do Reg*:
 * Machine -> hklm, User\<SID> -> hkcu, User\<SID>_Classes -> hkcr, o resto
 * de User -> hku. Com RootDirectory, o nome e relativo a esse HKEY. */
#define ST_OK 0L
#define ST_BUFFER_OVERFLOW ((long)0x80000005L)
#define ST_INVALID_INFO_CLASS ((long)0xC0000003L)
#define ST_INVALID_HANDLE ((long)0xC0000008L)
#define ST_INVALID_PARAMETER ((long)0xC000000DL)
#define ST_NO_MEMORY ((long)0xC0000017L)
#define ST_BUFFER_TOO_SMALL ((long)0xC0000023L)
#define ST_NAME_NOT_FOUND ((long)0xC0000034L)

/* a mesma SID do sec.c e do misc.c, em minusculas */
#define USER_SID "s-1-5-21-1329876819-1464486739-555819301-1000"

struct nt_ustr { /* UNICODE_STRING: len em bytes, sem 0 no fim */
  uint16_t len, max;
  wchar16* buf;
};
struct nt_oa { /* OBJECT_ATTRIBUTES */
  DWORD len;
  HANDLE root;
  const struct nt_ustr* name;
  DWORD attr;
  void* sd;
  void* sqos;
};

static int ustr8(const struct nt_ustr* u, char* d, int max) {
  int n = (u && u->buf) ? wutf16_to_8(u->buf, u->len / 2, d, max - 1) : 0;
  d[n] = 0;
  return n;
}

/* "\Registry\..." -> "hklm\..." (0 = fora do registro) */
static int native_path(const char* s, char* out, int max) {
  char low[400];
  const char* rest;
  const char* root;
  int n;
  wlower(low, s, (int)sizeof(low));
  if (strncmp(low, "\\registry\\machine", 17) == 0) {
    root = "hklm";
    rest = low + 17;
  } else if (strncmp(low, "\\registry\\user", 14) == 0) {
    root = "hku";
    rest = low + 14;
    if (rest[0] == '\\' && strncmp(rest + 1, USER_SID, sizeof(USER_SID) - 1) == 0) {
      const char* t = rest + sizeof(USER_SID);
      if (strncmp(t, "_classes", 8) == 0 && (t[8] == 0 || t[8] == '\\')) {
        root = "hkcr";
        rest = t + 8;
      } else if (*t == 0 || *t == '\\') {
        root = "hkcu";
        rest = t;
      }
    }
  } else {
    return 0;
  }
  if (*rest && *rest != '\\') return 0;
  if ((int)(strlen(root) + strlen(rest)) >= max) return 0;
  strcpy(out, root);
  strcat(out, rest);
  n = (int)strlen(out);
  while (n > 0 && out[n - 1] == '\\') out[--n] = 0;
  return 1;
}

static long nt_path(const struct nt_oa* oa, char* out, int max) {
  char s8[400];
  if (!oa || !oa->name) return ST_INVALID_PARAMETER;
  ustr8(oa->name, s8, (int)sizeof(s8));
  if (oa->root) return key_path(oa->root, s8, out, max) ? ST_OK : ST_INVALID_HANDLE;
  if (!native_path(s8, out, max)) {
    if (wine_debug) wlog("NtOpenKey/NtCreateKey: caminho fora do registro ", s8);
    return ST_NAME_NOT_FOUND;
  }
  return ST_OK;
}

/* A lista guarda so as chaves criadas: a raiz (hklm, hkcu...) e as pastas
 * acima de uma chave existente tambem existem, como no Windows. */
static int key_exists(struct wreg* r, const char* path) {
  struct rkey* k;
  size_t n = strlen(path);
  if (!strchr(path, '\\')) return 1;
  for (k = r->keys; k; k = k->next)
    if (strncmp(k->path, path, n) == 0 && (k->path[n] == 0 || k->path[n] == '\\')) return 1;
  return 0;
}

static WINAPI long NtOpenKeyEx(HANDLE* out, DWORD acc, const struct nt_oa* oa, DWORD opt) {
  struct wreg* r = reg_get();
  char path[400];
  long st;
  (void)acc;
  (void)opt;
  if (!out) return ST_INVALID_PARAMETER;
  *out = 0;
  if (!r) return ST_NO_MEMORY;
  st = nt_path(oa, path, (int)sizeof(path));
  if (st != ST_OK) return st;
  if (!key_exists(r, path)) {
    if (wine_debug) wlog("NtOpenKey (nao existe) ", path);
    return ST_NAME_NOT_FOUND;
  }
  *out = key_handle(path);
  return *out ? ST_OK : ST_NO_MEMORY;
}
static WINAPI long NtOpenKey(HANDLE* out, DWORD acc, const struct nt_oa* oa) { return NtOpenKeyEx(out, acc, oa, 0); }

static WINAPI long NtCreateKey(HANDLE* out, DWORD acc, const struct nt_oa* oa, DWORD title, const struct nt_ustr* cls,
                               DWORD opt, DWORD* disp) {
  struct wreg* r = reg_get();
  char path[400];
  int existed;
  long st;
  (void)acc; (void)title; (void)cls; (void)opt;
  if (!out) return ST_INVALID_PARAMETER;
  *out = 0;
  if (!r) return ST_NO_MEMORY;
  st = nt_path(oa, path, (int)sizeof(path));
  if (st != ST_OK) return st;
  existed = key_exists(r, path);
  if (!key_make(r, path)) return ST_NO_MEMORY;
  if (disp) *disp = existed ? 2 : 1; /* REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY */
  if (wine_debug) wlog("NtCreateKey ", path);
  *out = key_handle(path);
  return *out ? ST_OK : ST_NO_MEMORY;
}

static void put32(uint8_t* p, DWORD v) { memcpy(p, &v, 4); }

/* KEY_VALUE_{BASIC,FULL,PARTIAL}_INFORMATION e a PARTIAL_ALIGN64. O Chrome
 * pede FULL com buffer 0 para saber o tamanho: espera BUFFER_TOO_SMALL. */
static WINAPI long NtQueryValueKey(HANDLE h, const struct nt_ustr* name, DWORD cls, void* info, DWORD len,
                                   DWORD* ret) {
  struct rkey* k = key_of(h, 0);
  struct rval* v;
  uint8_t* o = (uint8_t*)info;
  char n8[200];
  DWORD nl = name ? name->len : 0, fixed, total;
  ustr8(name, n8, (int)sizeof(n8));
  v = k ? val_find(k, n8) : 0;
  if (!v) {
    if (wine_debug) wlog("NtQueryValueKey (nao existe) ", n8);
    return ST_NAME_NOT_FOUND;
  }
  if (cls == 0) { /* Basic: indice, tipo, NameLength, Name */
    fixed = 12;
    total = fixed + nl;
  } else if (cls == 1) { /* Full: indice, tipo, DataOffset, DataLength, NameLength, Name, dados */
    fixed = 20;
    total = fixed + nl + v->size;
  } else if (cls == 2) { /* Partial: indice, tipo, DataLength, Data */
    fixed = 12;
    total = fixed + v->size;
  } else if (cls == 3) { /* PartialAlign64: tipo, DataLength, Data */
    fixed = 8;
    total = fixed + v->size;
  } else {
    return ST_INVALID_INFO_CLASS;
  }
  if (ret) *ret = total;
  if (!o || len < fixed) return ST_BUFFER_TOO_SMALL;
  if (cls == 3) {
    put32(o, v->type);
    put32(o + 4, v->size);
  } else {
    put32(o, 0);
    put32(o + 4, v->type);
    if (cls == 0) put32(o + 8, nl);
    else if (cls == 2) put32(o + 8, v->size);
    else {
      put32(o + 8, fixed + nl);
      put32(o + 12, v->size);
      put32(o + 16, nl);
    }
  }
  if (len < total) return ST_BUFFER_OVERFLOW;
  if (cls == 0 || cls == 1) {
    if (nl) memcpy(o + fixed, name->buf, nl);
    if (cls == 1 && v->size) memcpy(o + fixed + nl, v->data, v->size);
  } else if (v->size) {
    memcpy(o + fixed, v->data, v->size);
  }
  return ST_OK;
}

static WINAPI long NtSetValueKey(HANDLE h, const struct nt_ustr* name, DWORD title, DWORD type, const void* data,
                                 DWORD size) {
  struct wreg* r = reg_get();
  struct rkey* k;
  char path[400], n8[200];
  (void)title;
  if (!r || !key_path(h, 0, path, (int)sizeof(path))) return ST_INVALID_HANDLE;
  k = key_make(r, path); /* chave so implicita (pasta acima de outra) passa a existir */
  if (!k) return ST_NO_MEMORY;
  ustr8(name, n8, (int)sizeof(n8));
  val_set(k, n8, type, data, size);
  return ST_OK;
}

static WINAPI long NtDeleteKey(HANDLE h) {
  struct wreg* r = reg_get();
  struct rkey** pp;
  char path[400];
  if (!r || !key_path(h, 0, path, (int)sizeof(path))) return ST_INVALID_HANDLE;
  for (pp = &r->keys; *pp; pp = &(*pp)->next) {
    struct rkey* k = *pp;
    if (strcmp(k->path, path) == 0) {
      *pp = k->next;
      while (k->vals) {
        struct rval* v = k->vals;
        k->vals = v->next;
        k_free(v->name);
        k_free(v->data);
        k_free(v);
      }
      k_free(k->path);
      k_free(k);
      break;
    }
  }
  return ST_OK;
}

#define S(n) {#n, (void*)n}
const struct wine_sym reg_syms[] = {
    S(RegOpenKeyExW), S(RegOpenKeyExA), S(RegCreateKeyExW), S(RegCloseKey), S(RegQueryValueExW),
    S(RegGetValueW), S(RegSetValueExW), S(RegDeleteValueW), S(RegDeleteKeyW), S(RegDeleteKeyExW),
    S(RegDeleteTreeW), S(RegEnumKeyExW), S(RegEnumValueW), S(RegQueryInfoKeyW), S(RegFlushKey),
    S(RegNotifyChangeKeyValue),
    S(NtOpenKeyEx), S(NtOpenKey), S(NtCreateKey), S(NtQueryValueKey), S(NtSetValueKey), S(NtDeleteKey),
    {0, 0}};
