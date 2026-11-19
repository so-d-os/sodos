/*
 * safearray.c - SAFEARRAY do oleaut32 (o Chromium usa para acessibilidade e
 * WMI). Layout x64: {USHORT cDims; USHORT fFeatures; ULONG cbElements;
 * ULONG cLocks; PVOID pvData; SAFEARRAYBOUND rgsabound[cDims]}, com os
 * limites guardados da ultima dimensao para a primeira, como no Windows.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

struct sabound {
  DWORD count;
  int32_t lbound;
};
struct safearray {
  uint16_t dims, features;
  DWORD elem_size, locks;
  uint8_t* data;
  struct sabound b[1];
};
#define FADF_HAVEVARTYPE 0x80
#define FADF_BSTR 0x100
#define FADF_UNKNOWN 0x200
#define FADF_DISPATCH 0x400
#define FADF_VARIANT 0x800
#define E_INVALIDARG_ ((long)0x80070057)
#define E_OUTOFMEMORY_ ((long)0x8007000E)
#define DISP_E_BADINDEX_ ((long)0x8002000B)
#define DISP_E_ARRAYISLOCKED_ ((long)0x8002000D)

static DWORD vt_size(uint16_t vt) {
  switch (vt) {
    case 16: case 17: return 1;                       /* I1 UI1 */
    case 2: case 18: case 11: return 2;               /* I2 UI2 BOOL */
    case 3: case 19: case 4: case 22: case 23: case 10: return 4; /* I4 UI4 R4 INT UINT ERROR */
    case 20: case 21: case 5: case 6: case 7: return 8; /* I8 UI8 R8 CY DATE */
    case 8: case 9: case 13: return 8;                /* BSTR DISPATCH UNKNOWN */
    case 12: return 24;                               /* VARIANT */
  }
  return 0;
}
static uint64_t total(const struct safearray* a) {
  uint64_t n = 1;
  int i;
  for (i = 0; i < a->dims; i++) n *= a->b[i].count;
  return n;
}

static struct safearray* sa_alloc(uint16_t vt, DWORD dims) {
  /* vartype guardado 4 bytes antes do descritor (FADF_HAVEVARTYPE) */
  uint8_t* raw = (uint8_t*)wzalloc(16 + sizeof(struct safearray) + sizeof(struct sabound) * (dims ? dims - 1 : 0));
  struct safearray* a;
  if (!raw) return 0;
  a = (struct safearray*)(raw + 16);
  *(DWORD*)(raw + 12) = vt;
  a->dims = (uint16_t)dims;
  a->elem_size = vt_size(vt);
  a->features = FADF_HAVEVARTYPE | (vt == 8 ? FADF_BSTR : vt == 13 ? FADF_UNKNOWN : vt == 9 ? FADF_DISPATCH
                                                         : vt == 12 ? FADF_VARIANT : 0);
  return a;
}
static void sa_free(struct safearray* a) {
  if (a) k_free((uint8_t*)a - 16);
}

static WINAPI struct safearray* SafeArrayCreate(uint16_t vt, DWORD dims, const struct sabound* bounds) {
  struct safearray* a;
  DWORD i;
  uint64_t n;
  if (!dims || !bounds || !vt_size(vt)) return 0;
  a = sa_alloc(vt, dims);
  if (!a) return 0;
  for (i = 0; i < dims; i++) a->b[dims - 1 - i] = bounds[i];
  n = total(a) * a->elem_size;
  a->data = (uint8_t*)wzalloc(n ? (size_t)n : 1);
  if (!a->data) {
    sa_free(a);
    return 0;
  }
  return a;
}
static WINAPI struct safearray* SafeArrayCreateVector(uint16_t vt, int32_t lb, DWORD n) {
  struct sabound b;
  b.count = n;
  b.lbound = lb;
  return SafeArrayCreate(vt, 1, &b);
}
static void release_elems(struct safearray* a) {
  typedef void(WINAPI * sfs)(void*);
  typedef long(WINAPI * vc)(void*);
  uint64_t i, n = total(a);
  if (a->features & FADF_BSTR) {
    sfs f = (sfs)builtin_lookup("SysFreeString");
    for (i = 0; f && i < n; i++) f(((void**)a->data)[i]);
  } else if (a->features & (FADF_UNKNOWN | FADF_DISPATCH)) {
    for (i = 0; i < n; i++) {
      void** o = ((void***)a->data)[i];
      if (o) ((DWORD(WINAPI*)(void*))((void**)*o)[2])(o); /* Release */
    }
  } else if (a->features & FADF_VARIANT) {
    vc f = (vc)builtin_lookup("VariantClear");
    for (i = 0; f && i < n; i++) f(a->data + i * 24);
  }
}
static WINAPI long SafeArrayDestroy(struct safearray* a) {
  if (!a) return 0;
  if (a->locks) return DISP_E_ARRAYISLOCKED_;
  release_elems(a);
  k_free(a->data);
  sa_free(a);
  return 0;
}
static WINAPI DWORD SafeArrayGetDim(struct safearray* a) { return a ? a->dims : 0; }
static WINAPI DWORD SafeArrayGetElemsize(struct safearray* a) { return a ? a->elem_size : 0; }
static WINAPI long SafeArrayGetUBound(struct safearray* a, DWORD dim, int32_t* ub) {
  if (!a || !ub || dim < 1 || dim > a->dims) return DISP_E_BADINDEX_;
  *ub = a->b[a->dims - dim].lbound + (int32_t)a->b[a->dims - dim].count - 1;
  return 0;
}
static WINAPI long SafeArrayGetLBound(struct safearray* a, DWORD dim, int32_t* lb) {
  if (!a || !lb || dim < 1 || dim > a->dims) return DISP_E_BADINDEX_;
  *lb = a->b[a->dims - dim].lbound;
  return 0;
}
static WINAPI long SafeArrayLock(struct safearray* a) {
  if (!a) return E_INVALIDARG_;
  a->locks++;
  return 0;
}
static WINAPI long SafeArrayUnlock(struct safearray* a) {
  if (!a || !a->locks) return E_INVALIDARG_;
  a->locks--;
  return 0;
}
static WINAPI long SafeArrayAccessData(struct safearray* a, void** out) {
  if (!a || !out) return E_INVALIDARG_;
  a->locks++;
  *out = a->data;
  return 0;
}
static WINAPI long SafeArrayUnaccessData(struct safearray* a) { return SafeArrayUnlock(a); }
static long elem_at(struct safearray* a, const int32_t* idx, uint8_t** at) {
  uint64_t off = 0, mul = 1;
  int i;
  if (!a || !idx) return E_INVALIDARG_;
  /* idx[0] e a dimensao mais a esquerda; b[] esta ao contrario */
  for (i = a->dims - 1; i >= 0; i--) {
    const struct sabound* b = &a->b[i];
    int32_t k = idx[a->dims - 1 - i] - b->lbound;
    if (k < 0 || (DWORD)k >= b->count) return DISP_E_BADINDEX_;
    off += (uint64_t)k * mul;
    mul *= b->count;
  }
  *at = a->data + off * a->elem_size;
  return 0;
}
static WINAPI long SafeArrayGetElement(struct safearray* a, const int32_t* idx, void* out) {
  uint8_t* at;
  long r = elem_at(a, idx, &at);
  if (r) return r;
  if (a->features & FADF_BSTR) {
    typedef void*(WINAPI * dup)(const wchar16*, DWORD);
    typedef DWORD(WINAPI * len)(const wchar16*);
    dup d = (dup)builtin_lookup("SysAllocStringLen");
    len l = (len)builtin_lookup("SysStringLen");
    const wchar16* s = *(const wchar16**)at;
    *(void**)out = s && d && l ? d(s, l(s)) : 0;
  } else if (a->features & FADF_VARIANT) {
    typedef long(WINAPI * vcp)(void*, const void*);
    vcp f = (vcp)builtin_lookup("VariantCopy");
    memset(out, 0, 24);
    if (f) f(out, at);
  } else {
    memcpy(out, at, a->elem_size);
    if (a->features & (FADF_UNKNOWN | FADF_DISPATCH)) {
      void** o = *(void***)at;
      if (o) ((DWORD(WINAPI*)(void*))((void**)*o)[1])(o); /* AddRef */
    }
  }
  return 0;
}
static WINAPI long SafeArrayPutElement(struct safearray* a, const int32_t* idx, const void* v) {
  uint8_t* at;
  long r = elem_at(a, idx, &at);
  if (r) return r;
  if (a->features & FADF_BSTR) {
    typedef void*(WINAPI * dup)(const wchar16*, DWORD);
    typedef DWORD(WINAPI * len)(const wchar16*);
    typedef void(WINAPI * sfs)(void*);
    dup d = (dup)builtin_lookup("SysAllocStringLen");
    len l = (len)builtin_lookup("SysStringLen");
    sfs f = (sfs)builtin_lookup("SysFreeString");
    if (f) f(*(void**)at);
    *(void**)at = v && d && l ? d((const wchar16*)v, l((const wchar16*)v)) : 0;
  } else if (a->features & FADF_VARIANT) {
    typedef long(WINAPI * vcp)(void*, const void*);
    vcp f = (vcp)builtin_lookup("VariantCopy");
    if (f) f(at, v);
  } else if (a->features & (FADF_UNKNOWN | FADF_DISPATCH)) {
    void** o = (void**)v;
    void** old = *(void***)at;
    if (o) ((DWORD(WINAPI*)(void*))((void**)*o)[1])(o);
    if (old) ((DWORD(WINAPI*)(void*))((void**)*old)[2])(old);
    *(void***)at = o;
  } else {
    memcpy(at, v, a->elem_size);
  }
  return 0;
}
static WINAPI long SafeArrayGetVartype(struct safearray* a, uint16_t* vt) {
  if (!a || !vt) return E_INVALIDARG_;
  *vt = (uint16_t)*(DWORD*)((uint8_t*)a - 4);
  return 0;
}
static WINAPI long SafeArrayCopy(struct safearray* a, struct safearray** out) {
  uint16_t vt = 0;
  struct safearray* c;
  uint64_t i, n;
  if (!a || !out) return E_INVALIDARG_;
  SafeArrayGetVartype(a, &vt);
  c = sa_alloc(vt, a->dims);
  if (!c) return E_OUTOFMEMORY_;
  memcpy(c->b, a->b, sizeof(struct sabound) * a->dims);
  n = total(a);
  c->data = (uint8_t*)wzalloc((n * a->elem_size) != 0 ? (size_t)(n * a->elem_size) : 1);
  if (!c->data) {
    sa_free(c);
    return E_OUTOFMEMORY_;
  }
  if (a->features & (FADF_BSTR | FADF_VARIANT | FADF_UNKNOWN | FADF_DISPATCH)) {
    /* copia elemento a elemento (BSTR, VARIANT e interfaces tem dono) */
    int32_t idx[1];
    if (a->dims == 1)
      for (i = 0; i < n; i++) {
        uint8_t tmp[24];
        idx[0] = a->b[0].lbound + (int32_t)i;
        SafeArrayGetElement(a, idx, tmp);
        memcpy(c->data + i * a->elem_size, tmp, a->elem_size);
      }
  } else {
    memcpy(c->data, a->data, (size_t)(n * a->elem_size));
  }
  *out = c;
  return 0;
}

/* VARCMP_LT 0, EQ 1, GT 2; NULL vale como "" */
static WINAPI long VarBstrCmp(const wchar16* a, const wchar16* b, DWORD lcid, DWORD fl) {
  static const wchar16 empty[1] = {0};
  (void)lcid;
  if (!a) a = empty;
  if (!b) b = empty;
  for (;; a++, b++) {
    wchar16 x = *a, y = *b;
    if (fl & 1) { /* NORM_IGNORECASE */
      if (x >= 'a' && x <= 'z') x -= 32;
      if (y >= 'a' && y <= 'z') y -= 32;
    }
    if (x != y) return x < y ? 0 : 2;
    if (!x) return 1;
  }
}

#define S(n) {#n, (void*)n}
const struct wine_sym safearray_syms[] = {
    S(VarBstrCmp),
    S(SafeArrayCreate), S(SafeArrayCreateVector), S(SafeArrayDestroy), S(SafeArrayGetDim), S(SafeArrayGetElemsize),
    S(SafeArrayGetUBound), S(SafeArrayGetLBound), S(SafeArrayLock), S(SafeArrayUnlock), S(SafeArrayAccessData),
    S(SafeArrayUnaccessData), S(SafeArrayGetElement), S(SafeArrayPutElement), S(SafeArrayGetVartype),
    S(SafeArrayCopy), {0, 0}};

/* ordinais do oleaut32 */
void* safearray_ordinal(unsigned ord) {
  switch (ord) {
    case 15: return (void*)SafeArrayCreate;
    case 16: return (void*)SafeArrayDestroy;
    case 17: return (void*)SafeArrayGetDim;
    case 18: return (void*)SafeArrayGetElemsize;
    case 19: return (void*)SafeArrayGetUBound;
    case 20: return (void*)SafeArrayGetLBound;
    case 21: return (void*)SafeArrayLock;
    case 22: return (void*)SafeArrayUnlock;
    case 23: return (void*)SafeArrayAccessData;
    case 24: return (void*)SafeArrayUnaccessData;
    case 25: return (void*)SafeArrayGetElement;
    case 26: return (void*)SafeArrayPutElement;
    case 27: return (void*)SafeArrayCopy;
    case 77: return (void*)SafeArrayGetVartype;
    case 411: return (void*)SafeArrayCreateVector;
  }
  return 0;
}
