/*
 * winrt.c - COMBASE: HSTRING e ativacao de classes do Windows Runtime.
 *
 * O SODOS nao tem as classes do WinRT. RoGetActivationFactory e
 * RoActivateInstance devolvem um objeto COM generico com o nome da classe:
 * QueryInterface aceita qualquer interface e cada metodo chamado e registrado
 * na serial com o numero do slot da vtable. Os metodos conhecidos de
 * Windows.Management.Deployment.PackageManager (instalar um pacote MSIX) sao
 * atendidos por pkg.c; os demais respondem E_NOTIMPL.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "winrt.h"

/* ---------- HSTRING ----------
 * Layout do Windows (o mesmo do HSTRING_HEADER e do windows-rs, que monta os
 * seus sem chamar a combase): flags, tamanho, 8 bytes reservados, ponteiro
 * para o texto em +16 e, nas alocadas, a contagem de referencias em +24 com
 * o texto logo depois. O windows-rs libera as nossas com HeapFree e conta as
 * referencias ele mesmo; nos aceitamos as dele pelo mesmo layout. */
struct hstring {
  DWORD flags;   /* 1 = referencia (WindowsCreateStringReference): nada a liberar */
  DWORD len;
  DWORD magic;   /* reservado no Windows; aqui marca as nossas */
  DWORD pad;
  wchar16* buf;
  volatile int32_t count;
  wchar16 data[1];
};
#define HS_MAGIC 0x52545348u
#define HS_REF 1u

static int hs_ok(const struct hstring* h) {
  if (!wptr_ok(h) || (uintptr_t)h <= 0x100000 || ((uintptr_t)h & 7)) return 0;
  if (h->magic == HS_MAGIC) return 1;
  /* de fora (windows-rs): referencia, ou alocada com o texto logo depois */
  if (h->len > 0x1000000u || !wptr_ok(h->buf)) return 0;
  if (h->flags == HS_REF) return 1;
  return h->flags == 0 && h->buf == (const wchar16*)((const uint8_t*)h + 28) && h->count > 0;
}

long WINAPI wWindowsCreateString(const wchar16* src, DWORD len, void** out) {
  struct hstring* h;
  if (!out) return E_POINTER;
  *out = 0;
  if (!len) return S_OK; /* HSTRING nulo = string vazia */
  if (!src) return E_POINTER;
  h = (struct hstring*)wzalloc(28 + ((size_t)len + 1) * 2);
  if (!h) return E_OUTOFMEMORY;
  h->flags = 0;
  h->magic = HS_MAGIC;
  h->len = len;
  h->buf = (wchar16*)((uint8_t*)h + 28);
  h->count = 1;
  memcpy(h->buf, src, (size_t)len * 2);
  *out = h;
  return S_OK;
}
static WINAPI long WindowsDeleteString(struct hstring* h) {
  if (!h || !hs_ok(h) || (h->flags & HS_REF)) return S_OK;
  if (__sync_sub_and_fetch(&h->count, 1) == 0) {
    h->magic = 0;
    k_free(h);
  }
  return S_OK;
}
const wchar16* WINAPI wWindowsGetStringRawBuffer(void* hv, DWORD* len) {
  struct hstring* h = (struct hstring*)hv;
  static const wchar16 empty[1] = {0};
  if (!h || !hs_ok(h)) {
    if (len) *len = 0;
    return empty;
  }
  if (len) *len = h->len;
  return h->buf;
}
static WINAPI DWORD WindowsGetStringLen(struct hstring* h) { return h && hs_ok(h) ? h->len : 0; }
static WINAPI long WindowsDuplicateString(struct hstring* h, void** out) {
  DWORD len = 0;
  const wchar16* b;
  if (!out) return E_POINTER;
  if (h && hs_ok(h) && !(h->flags & HS_REF)) { /* alocada: so mais uma referencia */
    __sync_add_and_fetch(&h->count, 1);
    *out = h;
    return S_OK;
  }
  b = wWindowsGetStringRawBuffer(h, &len);
  return wWindowsCreateString(b, len, out);
}

/* "fast-pass string": o texto fica com o chamador, o cabecalho (24 bytes) tambem */
static WINAPI long WindowsCreateStringReference(const wchar16* src, DWORD len, void* hdr, void** out) {
  struct hstring* h = (struct hstring*)hdr;
  if (!out || !hdr) return E_POINTER;
  *out = 0;
  if (!len) return S_OK;
  if (!src) return E_POINTER;
  h->flags = HS_REF;
  h->len = len;
  h->magic = HS_MAGIC;
  h->pad = 0;
  h->buf = (wchar16*)src;
  *out = h;
  return S_OK;
}
static WINAPI BOOL WindowsIsStringEmpty(struct hstring* h) { return WindowsGetStringLen(h) == 0; }
static WINAPI long WindowsCompareStringOrdinal(struct hstring* a, struct hstring* b, int* res) {
  DWORD la = 0, lb = 0, i;
  const wchar16* x = wWindowsGetStringRawBuffer(a, &la);
  const wchar16* y = wWindowsGetStringRawBuffer(b, &lb);
  int r = 0;
  if (!res) return E_POINTER;
  for (i = 0; i < la && i < lb && !r; i++) r = x[i] < y[i] ? -1 : x[i] > y[i] ? 1 : 0;
  if (!r) r = la < lb ? -1 : la > lb ? 1 : 0;
  *res = r;
  return S_OK;
}
static WINAPI long WindowsConcatString(struct hstring* a, struct hstring* b, void** out) {
  DWORD la = 0, lb = 0;
  const wchar16* x = wWindowsGetStringRawBuffer(a, &la);
  const wchar16* y = wWindowsGetStringRawBuffer(b, &lb);
  wchar16* t;
  long r;
  if (!out) return E_POINTER;
  t = (wchar16*)wzalloc(((size_t)la + lb + 1) * 2);
  if (!t) return E_OUTOFMEMORY;
  memcpy(t, x, (size_t)la * 2);
  memcpy(t + la, y, (size_t)lb * 2);
  r = wWindowsCreateString(t, la + lb, out);
  k_free(t);
  return r;
}
static WINAPI long WindowsSubstring(struct hstring* h, DWORD start, void** out) {
  DWORD len = 0;
  const wchar16* b = wWindowsGetStringRawBuffer(h, &len);
  if (start > len) return (long)0x8000000B; /* E_BOUNDS */
  return wWindowsCreateString(b + start, len - start, out);
}
static WINAPI long WindowsSubstringWithSpecifiedLength(struct hstring* h, DWORD start, DWORD n, void** out) {
  DWORD len = 0;
  const wchar16* b = wWindowsGetStringRawBuffer(h, &len);
  if (start > len || n > len - start) return (long)0x8000000B;
  return wWindowsCreateString(b + start, n, out);
}

char* whstring8(void* h) { return wdup16to8(wWindowsGetStringRawBuffer(h, 0)); }
int wmem_readable(const void* p, size_t n); /* vm.c */
int whstring_is(void* h) {
  if (!wmem_readable(h, 32)) return 0;
  return h && hs_ok((const struct hstring*)h);
}
long whstring_from8(const char* s, void** out) {
  wchar16* w = wdup8to16(s ? s : "");
  long r;
  if (!w) return E_OUTOFMEMORY;
  r = wWindowsCreateString(w, (DWORD)wstrlen16(w), out);
  k_free(w);
  return r;
}

/* ---------- objeto COM generico ---------- */
static void log_guid(const char* pre, const char* name, const uint8_t* g) {
  char t[160];
  static const char hx[] = "0123456789abcdef";
  static const int order[] = {3, 2, 1, 0, -1, 5, 4, -1, 7, 6, -1, 8, 9, -1, 10, 11, 12, 13, 14, 15};
  int o = 0, i;
  for (i = 0; name[i] && o < 90; i++) t[o++] = name[i];
  t[o++] = ' ';
  t[o++] = '{';
  for (i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
    if (order[i] < 0) {
      t[o++] = '-';
    } else {
      t[o++] = hx[g[order[i]] >> 4];
      t[o++] = hx[g[order[i]] & 15];
    }
  }
  t[o++] = '}';
  t[o] = 0;
  wlog(pre, t);
}

static long com_dispatch(struct comobj* self, int slot, uintptr_t* a) {
  if (slot == 0) { /* QueryInterface(iid, out) */
    if (wine_debug) log_guid("COM QI ", self->name, (const uint8_t*)a[0]);
    if (self->handler) {
      long r = self->handler(self, 0, a);
      if (r != E_NOTIMPL) return r;
    }
    *(void**)a[1] = self;
    __sync_fetch_and_add(&self->refs, 1);
    return S_OK;
  }
  if (slot == 1) return __sync_add_and_fetch(&self->refs, 1);
  if (slot == 2) {
    int r = __sync_sub_and_fetch(&self->refs, 1);
    return r < 0 ? 0 : r; /* os objetos ficam ate o fim do processo */
  }
  if (slot == 3) { /* GetIids(count, iids) */
    *(DWORD*)a[0] = 0;
    *(void**)a[1] = 0;
    return S_OK;
  }
  if (slot == 4) { /* GetRuntimeClassName(hstring*) */
    wchar16* w = wdup8to16(self->name);
    long r = wWindowsCreateString(w, (DWORD)wstrlen16(w), (void**)a[0]);
    k_free(w);
    return r;
  }
  if (slot == 5) { /* GetTrustLevel */
    *(int*)a[0] = 0;
    return S_OK;
  }
  if (self->handler) {
    long r = self->handler(self, slot, a);
    if (r != E_NOTIMPL) return r;
  }
  {
    char t[120];
    int o = 0, i, n = slot;
    for (i = 0; self->name[i] && o < 90; i++) t[o++] = self->name[i];
    t[o++] = ' ';
    t[o++] = 's';
    t[o++] = 'l';
    t[o++] = 'o';
    t[o++] = 't';
    t[o++] = ' ';
    if (n >= 10) t[o++] = (char)('0' + n / 10);
    t[o++] = (char)('0' + n % 10);
    t[o] = 0;
    wlog("COM metodo nao implementado: ", t);
    for (i = 0; i < 5; i++) {
      if (whstring_is((void*)a[i])) {
        char* s8 = whstring8((void*)a[i]);
        wlog("    arg (HSTRING) ", s8 ? s8 : "");
        k_free(s8);
      } else {
        wlogx("    arg ", a[i]);
      }
    }
  }
  return E_NOTIMPL;
}

/* Um ponto de entrada por slot: a vtable diz qual metodo foi chamado. */
#define SLOT(n)                                                                                        \
  static WINAPI long com_slot##n(struct comobj* self, uintptr_t a1, uintptr_t a2, uintptr_t a3,       \
                                 uintptr_t a4, uintptr_t a5, uintptr_t a6) {                          \
    uintptr_t a[6];                                                                                    \
    a[0] = a1; a[1] = a2; a[2] = a3; a[3] = a4; a[4] = a5; a[5] = a6;                                  \
    return com_dispatch(self, n, a);                                                                   \
  }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9)
SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19)
SLOT(20) SLOT(21) SLOT(22) SLOT(23) SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29)
SLOT(30) SLOT(31) SLOT(32) SLOT(33) SLOT(34) SLOT(35) SLOT(36) SLOT(37) SLOT(38) SLOT(39)
#define V(n) (void*)com_slot##n
static void* com_vtbl[COM_SLOTS] = {V(0),  V(1),  V(2),  V(3),  V(4),  V(5),  V(6),  V(7),  V(8),  V(9),
                                    V(10), V(11), V(12), V(13), V(14), V(15), V(16), V(17), V(18), V(19),
                                    V(20), V(21), V(22), V(23), V(24), V(25), V(26), V(27), V(28), V(29),
                                    V(30), V(31), V(32), V(33), V(34), V(35), V(36), V(37), V(38), V(39)};

struct comobj* com_new(const char* name, com_handler h, void* data) {
  struct comobj* o = (struct comobj*)wzalloc(sizeof(*o));
  if (!o) return 0;
  o->vtbl = com_vtbl;
  o->refs = 1;
  strncpy(o->name, name, sizeof(o->name) - 1);
  o->handler = h;
  o->data = data;
  return o;
}

com_handler pkg_handler_for(const char* cls); /* pkg.c */

static WINAPI long RoGetActivationFactory(struct hstring* cls, const uint8_t* iid, void** out) {
  char* name = whstring8(cls);
  struct comobj* o;
  if (!out) return E_POINTER;
  if (wine_debug) log_guid("RoGetActivationFactory ", name ? name : "?", iid);
  o = com_new(name ? name : "?", pkg_handler_for(name ? name : ""), 0);
  k_free(name);
  *out = o;
  return o ? S_OK : E_OUTOFMEMORY;
}
static WINAPI long RoActivateInstance(struct hstring* cls, void** out) {
  char* name = whstring8(cls);
  struct comobj* o;
  if (!out) return E_POINTER;
  if (wine_debug) wlog("RoActivateInstance ", name ? name : "?");
  o = com_new(name ? name : "?", pkg_handler_for(name ? name : ""), 0);
  k_free(name);
  *out = o;
  return o ? S_OK : E_OUTOFMEMORY;
}
static WINAPI long RoGetErrorReportingFlags(DWORD* f) {
  if (f) *f = 0;
  return S_OK;
}
static WINAPI long RoOriginateError(long hr, void* msg) {
  (void)msg;
  return hr;
}
static WINAPI long CLSIDFromString(const wchar16* s, uint8_t* out) {
  (void)s;
  memset(out, 0, 16);
  return (long)0x800401F3; /* CO_E_CLASSSTRING */
}

#define S(n) {#n, (void*)n}
const struct wine_sym winrt_syms[] = {{"WindowsCreateString", (void*)wWindowsCreateString},
                                      S(WindowsDeleteString),
                                      {"WindowsGetStringRawBuffer", (void*)wWindowsGetStringRawBuffer},
                                      S(WindowsGetStringLen),
                                      S(WindowsDuplicateString),
                                      S(WindowsCreateStringReference),
                                      S(WindowsIsStringEmpty),
                                      S(WindowsCompareStringOrdinal),
                                      S(WindowsConcatString),
                                      S(WindowsSubstring),
                                      S(WindowsSubstringWithSpecifiedLength),
                                      S(RoGetActivationFactory),
                                      S(RoActivateInstance),
                                      S(RoGetErrorReportingFlags),
                                      S(RoOriginateError),
                                      S(CLSIDFromString),
                                      {0, 0}};
