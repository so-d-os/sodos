/* mf.c - Media Foundation minimo: a webcam USB do SODOS para o Chrome.
 *
 * O Chromium 158 (media/capture/video/win) so usa a camera pelo Media
 * Foundation se mf.dll, mfplat.dll e MFCaptureEngine.dll carregam, os imports
 * atrasados de MF/MFPlat/MFReadWrite resolvem e MFStartup da S_OK. Depois:
 *   lista   MFEnumDeviceSources -> IMFActivate (nome e link simbolico);
 *   formatos MFCreateDeviceSource + MFCreateSourceReaderFromMediaSource ->
 *           IMFSourceReader::GetNativeMediaType;
 *   abrir   CoCreateInstance(CLSID_MFCaptureEngineClassFactory) -> engine:
 *           Initialize (evento INITIALIZED), GetSource -> IMFCaptureSource
 *           (tipos, SetCurrentDeviceMediaType), GetSink(PREVIEW) ->
 *           AddStream/SetSampleCallback, StartPreview (PREVIEW_STARTED) e um
 *           IMFSample por quadro no OnSample do Chrome.
 * Os tipos sao MJPEG com o subtipo GUID_ContainerFormatJpeg: o Chrome repassa
 * sem converter (passthrough) e decodifica o JPEG ele mesmo (libyuv). Os
 * quadros vem do kernel (k_cam_open/k_cam_frame, kernel/xhci.c) com as
 * tabelas de Huffman padrao inseridas quando faltam.
 *
 * Objetos: comobj de winrt.c (plain), um handler por tipo. QueryInterface
 * nega as interfaces opcionais que o Chrome testa (IAMCameraControl,
 * IMFGetService, IMF2DBuffer...) e aceita o resto. Slots das vtables pelos
 * cabecalhos do SDK (mfobjects.h, mfidl.h, mfreadwrite.h, mfcaptureengine.h).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "winrt.h"

void* builtin_lookup(const char* name);

#define MF_E_ATTRIBUTENOTFOUND ((long)0xC00D36E6)
#define MF_E_INVALIDTYPE ((long)0xC00D36B4)
#define MF_E_NO_MORE_TYPES ((long)0xC00D36B9)
#define MF_E_INVALIDREQUEST ((long)0xC00D36B2)
#define MF_E_INVALIDSTREAMNUMBER ((long)0xC00D36B3)
#define E_INVALIDARG ((long)0x80070057)

#define VT_EMPTY 0
#define VT_R8 5
#define VT_UNKNOWN 13
#define VT_UI4 19
#define VT_UI8 21
#define VT_LPWSTR 31
#define VT_CLSID 72
#define VT_BLOB 0x1011 /* VT_VECTOR | VT_UI1 */

typedef uint8_t guid_t[16];

/* GUIDs (bytes como na memoria: Data1..3 little-endian). Conferidos no
 * chrome.dll do Chrome 158. */
#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                                     \
  {(uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), (uint8_t)((a) >> 24), (uint8_t)(b),        \
   (uint8_t)((b) >> 8), (uint8_t)(c), (uint8_t)((c) >> 8), d0, d1, d2, d3, d4, d5, d6, d7}
static const guid_t SOURCE_TYPE = G(0xc60ac5fe, 0x252a, 0x478f, 0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3);
static const guid_t SOURCE_TYPE_VIDCAP = G(0x8ac3587a, 0x4ae7, 0x42d8, 0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f);
static const guid_t FRIENDLY_NAME = G(0x60d0e559, 0x52f8, 0x4fa2, 0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01);
static const guid_t VIDCAP_SYMLINK = G(0x58f0aad8, 0x22bf, 0x4f8a, 0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f);
static const guid_t VIDCAP_CATEGORY = G(0x77f0ae69, 0xc3bd, 0x4509, 0x94, 0x1d, 0x46, 0x7e, 0x4d, 0x24, 0x89, 0x9e);
static const guid_t MT_MAJOR_TYPE = G(0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
static const guid_t MT_SUBTYPE = G(0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
static const guid_t MT_FRAME_SIZE = G(0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d);
static const guid_t MT_FRAME_RATE = G(0xc459a2e8, 0x3d2c, 0x4e44, 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0);
static const guid_t MT_PIXEL_ASPECT = G(0xc6376a1e, 0x8d0a, 0x4027, 0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6);
static const guid_t MT_INTERLACE = G(0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd);
static const guid_t MT_ALL_INDEP = G(0xc9173739, 0x5e56, 0x461c, 0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f);
static const guid_t MEDIATYPE_VIDEO = G(0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
static const guid_t CONTAINER_JPEG = G(0x19e4a5aa, 0x5662, 0x4fc5, 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57);
static const guid_t EV_INITIALIZED = G(0x219992bc, 0xcf92, 0x4531, 0xa1, 0xae, 0x96, 0xe1, 0xe8, 0x86, 0xc8, 0xf1);
static const guid_t EV_PREVIEW_STARTED = G(0xa416df21, 0xf9d3, 0x4a74, 0x99, 0x1b, 0xb8, 0x17, 0x29, 0x89, 0x52, 0xc4);
static const guid_t EV_PREVIEW_STOPPED = G(0x13d5143c, 0x1edd, 0x4e50, 0xa2, 0xef, 0x35, 0x0a, 0x47, 0x67, 0x80, 0x60);
static const guid_t EV_ERROR = G(0x46b89fc6, 0x33cc, 0x4399, 0x9d, 0xad, 0x78, 0x4d, 0xe7, 0x7d, 0x58, 0x7c);
static const guid_t CLSID_ENGINE_FACTORY = G(0xefce38d3, 0x8914, 0x4674, 0xa7, 0xdf, 0xae, 0x1b, 0x3d, 0x65, 0x4b, 0x8a);
static const guid_t SAMPLE_DEVREF_TIME = G(0x6523775a, 0xba2d, 0x405f, 0xb2, 0xc5, 0x01, 0xff, 0x88, 0xe2, 0xe8, 0xf6);

static const wchar16 cam_link[] = {
    '\\', '\\', '?', '\\', 'u', 's', 'b', '#', 'v', 'i', 'd', '_', '5', '3', '4', 'f', '&', 'p', 'i',
    'd', '_', '4', '4', '4', '3', '&', 'm', 'i', '_', '0', '0', '#', 's', 'o', 'd', 'o', 's', '0',
    '#', '{', 'e', '5', '3', '2', '3', '7', '7', '7', '-', 'f', '9', '7', '6', '-', '4', 'f', '5',
    'b', '-', '9', 'b', '5', '5', '-', 'b', '9', '4', '6', '9', '9', 'c', '4', '6', 'e', '4', '4',
    '}', '\\', 'g', 'l', 'o', 'b', 'a', 'l', 0};
static const wchar16 cam_name[] = {'W', 'e', 'b', 'c', 'a', 'm', ' ', 'U', 'S', 'B', ' ',
                                   '(', 'S', 'O', 'D', 'O', 'S', ')', 0};

static int geq(const void* a, const void* b) { return memcmp(a, b, 16) == 0; }

/* Metodo de um objeto COM de fora (do Chrome), pela vtable. */
typedef long (WINAPI* vfn)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static long vcall(void* obj, int slot, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
  return ((vfn)((*(void***)obj)[slot]))(obj, a, b, c, d);
}

/* ---------- kernel (k_cam_*, build 79) ---------- */

static int (*kc_modes)(unsigned int*, int);
static int (*kc_open)(int, int, int, int*);
static int (*kc_frame)(void*, int, int*, int);
static int (*kc_close)(void);

static int kc_ready(void) {
  if (!kc_modes) {
    kc_open = (int (*)(int, int, int, int*))k_dl_sym(0, "k_cam_open", 0);
    kc_frame = (int (*)(void*, int, int*, int))k_dl_sym(0, "k_cam_frame", 0);
    kc_close = (int (*)(void))k_dl_sym(0, "k_cam_close", 0);
    if (kc_open && kc_frame && kc_close)
      kc_modes = (int (*)(unsigned int*, int))k_dl_sym(0, "k_cam_modes", 0);
  }
  return kc_modes != 0;
}

/* Modos MJPEG da camera (largura, altura, intervalo em 100 ns). */
#define MODES_MAX 16
static int nmodes = -1;
static unsigned int modes[MODES_MAX][3];

static int load_modes(void) {
  unsigned int m[4 * 32];
  int n, i;
  if (nmodes >= 0) return nmodes;
  if (!kc_ready()) {
    wlog("MF: kernel sem k_cam_* (webcam precisa do build 79)", 0);
    return 0;
  }
  n = kc_modes(m, 32);
  if (n < 0) return 0;  /* sem webcam agora: tenta de novo na proxima */
  nmodes = 0;
  for (i = 0; i < n && nmodes < MODES_MAX; i++)
    if (m[i * 4] == 1) {
      modes[nmodes][0] = m[i * 4 + 1];
      modes[nmodes][1] = m[i * 4 + 2];
      modes[nmodes][2] = m[i * 4 + 3] ? m[i * 4 + 3] : 333333u;
      nmodes++;
    }
  wlogx("MF: modos MJPEG da webcam ", (uint64_t)nmodes);
  return nmodes;
}

/* ---------- atributos (IMFAttributes, slots 3-32) ---------- */

struct attr {
  guid_t key;
  int vt;
  uint64_t u;
  guid_t g;
  wchar16* s;
  uint8_t* blob;
  uint32_t blen;
  void* unk;
};

/* Dados de todo objeto deste arquivo. */
struct mfo {
  volatile int lk;
  int n, cap;
  struct attr* a;
  int kind;
  /* por tipo */
  struct comobj* src;       /* engine: fonte; reader: fonte */
  void* evcb;               /* engine: IMFCaptureEngineOnEventCallback */
  void* samplecb;           /* engine: IMFCaptureEngineOnSampleCallback */
  struct comobj* csrc;      /* engine: IMFCaptureSource */
  struct comobj* sink;      /* engine: preview sink */
  struct mfo* eng;          /* csrc/sink: o engine */
  volatile int cmd, state;  /* engine: 0 nada, 1 ligar, 2 desligar; estado 0/1 */
  int w, h, ival;
  uint8_t* data;            /* buffer */
  uint32_t len, max;
  struct comobj* buf;       /* sample: o buffer */
  int64_t time;             /* sample */
  guid_t ext;               /* event */
  long status;
};

enum { K_ATTR = 1, K_TYPE, K_ACT, K_EVENT, K_SAMPLE, K_BUF, K_SRC, K_READER, K_FACT, K_ENG, K_CSRC, K_SINK };

static struct attr* at_find(struct mfo* m, const void* key) {
  int i;
  for (i = 0; i < m->n; i++)
    if (geq(m->a[i].key, key)) return &m->a[i];
  return 0;
}

static struct attr* at_put(struct mfo* m, const void* key, int vt) {
  struct attr* t = at_find(m, key);
  if (!t) {
    if (m->n == m->cap) {
      int nc = m->cap ? m->cap * 2 : 8;
      struct attr* na = (struct attr*)wzalloc(sizeof(struct attr) * (size_t)nc);
      if (!na) return 0;
      if (m->a) memcpy(na, m->a, sizeof(struct attr) * (size_t)m->n);
      m->a = na;  /* o velho fica (pequeno; objetos vivem ate o fim) */
      m->cap = nc;
    }
    t = &m->a[m->n++];
    memset(t, 0, sizeof(*t));
    memcpy(t->key, key, 16);
  }
  t->vt = vt;
  return t;
}

static void at_u32(struct mfo* m, const void* key, uint32_t v) {
  struct attr* t = at_put(m, key, VT_UI4);
  if (t) t->u = v;
}
static void at_u64(struct mfo* m, const void* key, uint64_t v) {
  struct attr* t = at_put(m, key, VT_UI8);
  if (t) t->u = v;
}
static void at_guid(struct mfo* m, const void* key, const void* g) {
  struct attr* t = at_put(m, key, VT_CLSID);
  if (t) memcpy(t->g, g, 16);
}
static void at_str(struct mfo* m, const void* key, const wchar16* s) {
  struct attr* t = at_put(m, key, VT_LPWSTR);
  size_t n;
  if (!t || !s) return;
  n = wstrlen16(s);
  t->s = (wchar16*)wzalloc((n + 1) * 2);
  if (t->s) memcpy(t->s, s, n * 2);
}

/* PROPVARIANT: vt em +0, valor em +8 (24 bytes no x64). */
static void pv_from(uint8_t* pv, const struct attr* t) {
  memset(pv, 0, 24);
  *(uint16_t*)pv = (uint16_t)t->vt;
  switch (t->vt) {
    case VT_UI4: *(uint32_t*)(pv + 8) = (uint32_t)t->u; break;
    case VT_UI8: case VT_R8: *(uint64_t*)(pv + 8) = t->u; break;
    case VT_CLSID: {
      uint8_t* g = (uint8_t*)wheap_alloc(16);
      if (g) memcpy(g, t->g, 16);
      *(void**)(pv + 8) = g;
      break;
    }
    case VT_LPWSTR: {
      size_t n = t->s ? wstrlen16(t->s) : 0;
      wchar16* s = (wchar16*)wheap_alloc((n + 1) * 2);
      if (s) {
        if (n) memcpy(s, t->s, n * 2);
        s[n] = 0;
      }
      *(void**)(pv + 8) = s;
      break;
    }
    case VT_UNKNOWN:
      *(void**)(pv + 8) = t->unk;
      if (t->unk) vcall(t->unk, 1, 0, 0, 0, 0);
      break;
    case VT_BLOB: {
      uint8_t* b = (uint8_t*)wheap_alloc(t->blen ? t->blen : 1);
      if (b && t->blen) memcpy(b, t->blob, t->blen);
      *(uint32_t*)(pv + 8) = t->blen;
      *(void**)(pv + 16) = b;
      break;
    }
    default: break;
  }
}

static long pv_to(struct mfo* m, const void* key, const uint8_t* pv) {
  int vt = *(const uint16_t*)pv;
  struct attr* t;
  switch (vt) {
    case VT_UI4: at_u32(m, key, *(const uint32_t*)(pv + 8)); return S_OK;
    case VT_UI8: at_u64(m, key, *(const uint64_t*)(pv + 8)); return S_OK;
    case VT_R8:
      t = at_put(m, key, VT_R8);
      if (t) t->u = *(const uint64_t*)(pv + 8);
      return S_OK;
    case VT_CLSID:
      if (!*(void* const*)(pv + 8)) return E_INVALIDARG;
      at_guid(m, key, *(void* const*)(pv + 8));
      return S_OK;
    case VT_LPWSTR: at_str(m, key, *(wchar16* const*)(pv + 8)); return S_OK;
    case VT_UNKNOWN:
      t = at_put(m, key, VT_UNKNOWN);
      if (t) {
        t->unk = *(void* const*)(pv + 8);
        if (t->unk) vcall(t->unk, 1, 0, 0, 0, 0);
      }
      return S_OK;
    case VT_BLOB: {
      uint32_t n = *(const uint32_t*)(pv + 8);
      t = at_put(m, key, VT_BLOB);
      if (!t) return E_OUTOFMEMORY;
      t->blob = (uint8_t*)wzalloc(n ? n : 1);
      if (t->blob && n) memcpy(t->blob, *(void* const*)(pv + 16), n);
      t->blen = n;
      return S_OK;
    }
    default: return MF_E_INVALIDTYPE;
  }
}

static long at_typed(struct mfo* m, const void* key, int vt, struct attr** out) {
  struct attr* t = at_find(m, key);
  if (!t) return MF_E_ATTRIBUTENOTFOUND;
  if (t->vt != vt) return MF_E_INVALIDTYPE;
  *out = t;
  return S_OK;
}

/* IMFAttributes 3..32. E_NOTIMPL = slot fora desta faixa. */
static long attrs_call(struct mfo* m, int slot, uintptr_t* a) {
  struct attr* t;
  long r;
  switch (slot) {
    case 3: /* GetItem(key, pv) */
      t = at_find(m, (void*)a[0]);
      if (!t) return MF_E_ATTRIBUTENOTFOUND;
      if (a[1]) pv_from((uint8_t*)a[1], t);
      return S_OK;
    case 4: /* GetItemType */
      t = at_find(m, (void*)a[0]);
      if (!t) return MF_E_ATTRIBUTENOTFOUND;
      *(int*)a[1] = t->vt;
      return S_OK;
    case 5: /* CompareItem(key, pv, BOOL*) */
      t = at_find(m, (void*)a[0]);
      *(int*)a[2] = 0;
      if (t && t->vt == *(uint16_t*)a[1]) {
        const uint8_t* pv = (const uint8_t*)a[1];
        if (t->vt == VT_UI4) *(int*)a[2] = (uint32_t)t->u == *(const uint32_t*)(pv + 8);
        else if (t->vt == VT_UI8) *(int*)a[2] = t->u == *(const uint64_t*)(pv + 8);
        else if (t->vt == VT_CLSID) *(int*)a[2] = geq(t->g, *(void* const*)(pv + 8));
      }
      return S_OK;
    case 6: /* Compare */
      *(int*)a[2] = 0;
      return S_OK;
    case 7: /* GetUINT32 */
      r = at_typed(m, (void*)a[0], VT_UI4, &t);
      if (r == S_OK) *(uint32_t*)a[1] = (uint32_t)t->u;
      return r;
    case 8: /* GetUINT64 */
      r = at_typed(m, (void*)a[0], VT_UI8, &t);
      if (r == S_OK) *(uint64_t*)a[1] = t->u;
      return r;
    case 9: /* GetDouble */
      r = at_typed(m, (void*)a[0], VT_R8, &t);
      if (r == S_OK) *(uint64_t*)a[1] = t->u;
      return r;
    case 10: /* GetGUID */
      r = at_typed(m, (void*)a[0], VT_CLSID, &t);
      if (r == S_OK) memcpy((void*)a[1], t->g, 16);
      return r;
    case 11: /* GetStringLength */
      r = at_typed(m, (void*)a[0], VT_LPWSTR, &t);
      if (r == S_OK) *(uint32_t*)a[1] = (uint32_t)(t->s ? wstrlen16(t->s) : 0);
      return r;
    case 12: { /* GetString(key, buf, size, len*) */
      size_t n;
      r = at_typed(m, (void*)a[0], VT_LPWSTR, &t);
      if (r != S_OK) return r;
      n = t->s ? wstrlen16(t->s) : 0;
      if (a[3]) *(uint32_t*)a[3] = (uint32_t)n;
      if ((uint32_t)a[2] < n + 1) return (long)0x8007007A; /* ERROR_INSUFFICIENT_BUFFER */
      if (n) memcpy((void*)a[1], t->s, n * 2);
      ((wchar16*)a[1])[n] = 0;
      return S_OK;
    }
    case 13: { /* GetAllocatedString(key, wchar**, len*) */
      size_t n;
      wchar16* s;
      r = at_typed(m, (void*)a[0], VT_LPWSTR, &t);
      if (r != S_OK) return r;
      n = t->s ? wstrlen16(t->s) : 0;
      s = (wchar16*)wheap_alloc((n + 1) * 2);
      if (!s) return E_OUTOFMEMORY;
      if (n) memcpy(s, t->s, n * 2);
      s[n] = 0;
      *(wchar16**)a[1] = s;
      if (a[2]) *(uint32_t*)a[2] = (uint32_t)n;
      return S_OK;
    }
    case 14: /* GetBlobSize */
      r = at_typed(m, (void*)a[0], VT_BLOB, &t);
      if (r == S_OK) *(uint32_t*)a[1] = t->blen;
      return r;
    case 15: /* GetBlob(key, buf, size, len*) */
      r = at_typed(m, (void*)a[0], VT_BLOB, &t);
      if (r != S_OK) return r;
      if ((uint32_t)a[2] < t->blen) return (long)0x8007007A;
      memcpy((void*)a[1], t->blob, t->blen);
      if (a[3]) *(uint32_t*)a[3] = t->blen;
      return S_OK;
    case 16: { /* GetAllocatedBlob(key, u8**, len*) */
      uint8_t* b;
      r = at_typed(m, (void*)a[0], VT_BLOB, &t);
      if (r != S_OK) return r;
      b = (uint8_t*)wheap_alloc(t->blen ? t->blen : 1);
      if (!b) return E_OUTOFMEMORY;
      memcpy(b, t->blob, t->blen);
      *(uint8_t**)a[1] = b;
      *(uint32_t*)a[2] = t->blen;
      return S_OK;
    }
    case 17: /* GetUnknown(key, riid, ppv) */
      r = at_typed(m, (void*)a[0], VT_UNKNOWN, &t);
      if (r != S_OK) return r;
      if (!t->unk) return E_NOINTERFACE;
      return vcall(t->unk, 0, a[1], a[2], 0, 0);
    case 18: /* SetItem(key, pv) */
      return pv_to(m, (void*)a[0], (const uint8_t*)a[1]);
    case 19: { /* DeleteItem */
      int i;
      for (i = 0; i < m->n; i++)
        if (geq(m->a[i].key, (void*)a[0])) {
          m->a[i] = m->a[--m->n];
          break;
        }
      return S_OK;
    }
    case 20: m->n = 0; return S_OK; /* DeleteAllItems */
    case 21: at_u32(m, (void*)a[0], (uint32_t)a[1]); return S_OK;
    case 22: at_u64(m, (void*)a[0], (uint64_t)a[1]); return S_OK;
    case 23: /* SetDouble: o double vem no xmm1, que o slot generico nao ve */
      t = at_put(m, (void*)a[0], VT_R8);
      if (t) t->u = 0;
      return S_OK;
    case 24: at_guid(m, (void*)a[0], (void*)a[1]); return S_OK;
    case 25: at_str(m, (void*)a[0], (const wchar16*)a[1]); return S_OK;
    case 26: { /* SetBlob(key, buf, size) */
      uint32_t n = (uint32_t)a[2];
      t = at_put(m, (void*)a[0], VT_BLOB);
      if (!t) return E_OUTOFMEMORY;
      t->blob = (uint8_t*)wzalloc(n ? n : 1);
      if (t->blob && n) memcpy(t->blob, (void*)a[1], n);
      t->blen = n;
      return S_OK;
    }
    case 27: /* SetUnknown */
      t = at_put(m, (void*)a[0], VT_UNKNOWN);
      if (!t) return E_OUTOFMEMORY;
      t->unk = (void*)a[1];
      if (t->unk) vcall(t->unk, 1, 0, 0, 0, 0);
      return S_OK;
    case 28: case 29: return S_OK; /* LockStore, UnlockStore */
    case 30: *(uint32_t*)a[0] = (uint32_t)m->n; return S_OK;
    case 31: /* GetItemByIndex(i, GUID*, pv) */
      if ((int)a[0] < 0 || (int)a[0] >= m->n) return E_INVALIDARG;
      memcpy((void*)a[1], m->a[(int)a[0]].key, 16);
      if (a[2]) pv_from((uint8_t*)a[2], &m->a[(int)a[0]]);
      return S_OK;
    case 32: { /* CopyAllItems(dest) */
      int i;
      uint8_t pv[24];
      for (i = 0; i < m->n; i++) {
        pv_from(pv, &m->a[i]);
        vcall((void*)a[0], 18, (uintptr_t)m->a[i].key, (uintptr_t)pv, 0, 0);
      }
      return S_OK;
    }
  }
  return E_NOTIMPL;
}

/* Interfaces opcionais que o Chrome testa e que nao temos (Data1). */
static long qi_deny(struct comobj* self, uintptr_t* a) {
  static const uint32_t deny[] = {
      0xC6E13370u, /* IAMCameraControl */
      0xC6E13360u, /* IAMVideoProcAmp */
      0xFA993888u, /* IMFGetService */
      0x7DC9D5F9u, /* IMF2DBuffer */
      0x33AE5EA6u, /* IMF2DBuffer2 */
      0xE7174CFAu, /* IMFDXGIBuffer */
      0x28F54685u, /* IKsControl */
      0x00000003u, /* IMarshal */
      0x3C9B2EB9u, /* IMFMediaSourceEx */
      0x7B981CF0u, /* IMFSourceReaderEx */
      0x94EA2B94u, /* IAgileObject */
      0};
  uint32_t d1 = *(const uint32_t*)a[0];
  int i;
  for (i = 0; deny[i]; i++)
    if (d1 == deny[i]) {
      if (wine_debug) wlogx("MF: QI negado ", d1);
      *(void**)a[1] = 0;
      return E_NOINTERFACE;
    }
  if (wine_debug && d1) {
    char t[100];
    int k = 0;
    const char* n = self->name;
    while (*n && k < 60) t[k++] = *n++;
    t[k] = 0;
    wlogx(t, d1);
  }
  return E_NOTIMPL; /* aceita: devolve o proprio objeto */
}

static struct comobj* mf_new(const char* name, com_handler h, int kind);

/* ---------- media type ---------- */

static long type_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* m = (struct mfo*)self->data;
  long r;
  if (slot == 0) return qi_deny(self, a);
  r = attrs_call(m, slot, a);
  if (r != E_NOTIMPL || slot <= 32) return r;
  switch (slot) {
    case 33: { /* GetMajorType */
      struct attr* t;
      r = at_typed(m, MT_MAJOR_TYPE, VT_CLSID, &t);
      if (r == S_OK) memcpy((void*)a[0], t->g, 16);
      return r;
    }
    case 34: *(int*)a[0] = 1; return S_OK; /* IsCompressedFormat */
    case 35: *(uint32_t*)a[1] = 0; return 1; /* IsEqual: S_FALSE */
  }
  return E_NOTIMPL;
}

static struct comobj* type_for(int i) {
  struct comobj* o = mf_new("MF MediaType", type_h, K_TYPE);
  struct mfo* m;
  if (!o) return 0;
  m = (struct mfo*)o->data;
  at_guid(m, MT_MAJOR_TYPE, MEDIATYPE_VIDEO);
  at_guid(m, MT_SUBTYPE, CONTAINER_JPEG);
  at_u64(m, MT_FRAME_SIZE, ((uint64_t)modes[i][0] << 32) | modes[i][1]);
  at_u64(m, MT_FRAME_RATE, ((uint64_t)10000000u << 32) | modes[i][2]);
  at_u64(m, MT_PIXEL_ASPECT, ((uint64_t)1 << 32) | 1u);
  at_u32(m, MT_INTERLACE, 2); /* MFVideoInterlace_Progressive */
  at_u32(m, MT_ALL_INDEP, 1);
  return o;
}

/* ---------- atributos soltos, ativacao, evento ---------- */

static long attr_h(struct comobj* self, int slot, uintptr_t* a) {
  if (slot == 0) return qi_deny(self, a);
  return attrs_call((struct mfo*)self->data, slot, a);
}

static struct comobj* source_new(void);

static long act_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* m = (struct mfo*)self->data;
  long r;
  if (slot == 0) return qi_deny(self, a);
  r = attrs_call(m, slot, a);
  if (r != E_NOTIMPL || slot <= 32) return r;
  if (slot == 33) { /* ActivateObject(riid, ppv) */
    struct comobj* s = source_new();
    if (!s) return E_OUTOFMEMORY;
    *(void**)a[1] = s;
    return S_OK;
  }
  if (slot == 34 || slot == 35) return S_OK; /* ShutdownObject, DetachObject */
  return E_NOTIMPL;
}

static long event_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* m = (struct mfo*)self->data;
  long r;
  if (slot == 0) return qi_deny(self, a);
  r = attrs_call(m, slot, a);
  if (r != E_NOTIMPL || slot <= 32) return r;
  switch (slot) {
    case 33: *(uint32_t*)a[0] = 2; return S_OK;             /* GetType: MEExtendedType */
    case 34: memcpy((void*)a[0], m->ext, 16); return S_OK; /* GetExtendedType */
    case 35: *(long*)a[0] = m->status; return S_OK;        /* GetStatus */
    case 36: memset((void*)a[0], 0, 24); return S_OK;      /* GetValue: VT_EMPTY */
  }
  return E_NOTIMPL;
}

/* ---------- buffer e sample ---------- */

static long buf_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* m = (struct mfo*)self->data;
  switch (slot) {
    case 0: return qi_deny(self, a);
    case 3: /* Lock(BYTE**, max*, cur*) */
      *(uint8_t**)a[0] = m->data;
      if (a[1]) *(uint32_t*)a[1] = m->max;
      if (a[2]) *(uint32_t*)a[2] = m->len;
      return S_OK;
    case 4: return S_OK; /* Unlock */
    case 5: *(uint32_t*)a[0] = m->len; return S_OK;
    case 6: if ((uint32_t)a[0] > m->max) return E_INVALIDARG; m->len = (uint32_t)a[0]; return S_OK;
    case 7: *(uint32_t*)a[0] = m->max; return S_OK;
  }
  return E_NOTIMPL;
}

static long sample_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* m = (struct mfo*)self->data;
  struct mfo* b = (struct mfo*)m->buf->data;
  long r;
  if (slot == 0) return qi_deny(self, a);
  r = attrs_call(m, slot, a);
  if (r != E_NOTIMPL || slot <= 32) return r;
  switch (slot) {
    case 33: *(uint32_t*)a[0] = 0; return S_OK;           /* GetSampleFlags */
    case 34: return S_OK;                                  /* SetSampleFlags */
    case 35: *(int64_t*)a[0] = m->time; return S_OK;      /* GetSampleTime */
    case 36: m->time = (int64_t)a[0]; return S_OK;         /* SetSampleTime */
    case 37: *(int64_t*)a[0] = 333333; return S_OK;       /* GetSampleDuration */
    case 38: return S_OK;                                  /* SetSampleDuration */
    case 39: *(uint32_t*)a[0] = 1; return S_OK;           /* GetBufferCount */
    case 40: /* GetBufferByIndex(i, IMFMediaBuffer**) */
      if (a[0] != 0) return E_INVALIDARG;
      __sync_fetch_and_add(&m->buf->refs, 1);
      *(void**)a[1] = m->buf;
      return S_OK;
    case 41: /* ConvertToContiguousBuffer */
      __sync_fetch_and_add(&m->buf->refs, 1);
      *(void**)a[0] = m->buf;
      return S_OK;
    case 42: case 43: case 44: return S_OK;               /* Add/Remove buffers */
    case 45: *(uint32_t*)a[0] = b->len; return S_OK;      /* GetTotalLength */
  }
  return E_NOTIMPL;
}

/* ---------- fonte e source reader ---------- */

static long source_h(struct comobj* self, int slot, uintptr_t* a) {
  switch (slot) {
    case 0: return qi_deny(self, a);
    case 7: *(uint32_t*)a[0] = 1; return S_OK; /* GetCharacteristics: MFMEDIASOURCE_IS_LIVE */
    case 10: case 11: case 12: return S_OK;    /* Stop, Pause, Shutdown */
  }
  return E_NOTIMPL;
}

static struct comobj* source_new(void) {
  (void)load_modes();
  return mf_new("MF MediaSource (webcam)", source_h, K_SRC);
}

static long reader_h(struct comobj* self, int slot, uintptr_t* a) {
  uint32_t stream = (uint32_t)a[0];
  int first = stream == 0xFFFFFFFCu || stream == 0;
  switch (slot) {
    case 0: return qi_deny(self, a);
    case 3: /* GetStreamSelection */
      if (!first) return MF_E_INVALIDSTREAMNUMBER;
      *(int*)a[1] = 1;
      return S_OK;
    case 4: return first ? S_OK : MF_E_INVALIDSTREAMNUMBER; /* SetStreamSelection */
    case 5: /* GetNativeMediaType(stream, i, IMFMediaType**) */
    case 6: { /* GetCurrentMediaType(stream, IMFMediaType**) */
      int i = slot == 5 ? (int)a[1] : 0;
      struct comobj* t;
      if (!first) return MF_E_INVALIDSTREAMNUMBER;
      if (i < 0 || i >= load_modes()) return MF_E_NO_MORE_TYPES;
      t = type_for(i);
      if (!t) return E_OUTOFMEMORY;
      *(void**)(slot == 5 ? a[2] : a[1]) = t;
      return S_OK;
    }
    case 7: return S_OK; /* SetCurrentMediaType */
  }
  return E_NOTIMPL;
}

/* ---------- capture engine ---------- */

static long csrc_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* e = ((struct mfo*)self->data)->eng;
  switch (slot) {
    case 0: return qi_deny(self, a);
    case 3: /* GetCaptureDeviceSource(type, IMFMediaSource**) */
      if (!e->src) return MF_E_INVALIDREQUEST;
      __sync_fetch_and_add(&e->src->refs, 1);
      *(void**)a[1] = e->src;
      return S_OK;
    case 5: /* GetService */
      *(void**)a[2] = 0;
      return E_NOINTERFACE;
    case 9: { /* GetAvailableDeviceMediaType(stream, i, IMFMediaType**) */
      int i = (int)a[1];
      struct comobj* t;
      if (a[0] != 0) return MF_E_INVALIDSTREAMNUMBER;
      if (i < 0 || i >= load_modes()) return MF_E_NO_MORE_TYPES;
      t = type_for(i);
      if (!t) return E_OUTOFMEMORY;
      *(void**)a[2] = t;
      return S_OK;
    }
    case 10: { /* SetCurrentDeviceMediaType(stream, IMFMediaType*) */
      uint64_t sz = 0, fr = 0;
      if (a[0] != 0) return MF_E_INVALIDSTREAMNUMBER;
      if (vcall((void*)a[1], 8, (uintptr_t)MT_FRAME_SIZE, (uintptr_t)&sz, 0, 0) != S_OK) return E_INVALIDARG;
      (void)vcall((void*)a[1], 8, (uintptr_t)MT_FRAME_RATE, (uintptr_t)&fr, 0, 0);
      e->w = (int)(sz >> 32);
      e->h = (int)(uint32_t)sz;
      wlogx("MF: tamanho escolhido (largura<<16|altura) ", ((uint64_t)e->w << 16) | (uint64_t)e->h);
      return S_OK;
    }
    case 11: { /* GetCurrentDeviceMediaType(stream, IMFMediaType**) */
      int i;
      for (i = 0; i < load_modes(); i++)
        if ((int)modes[i][0] == e->w && (int)modes[i][1] == e->h) break;
      if (i >= load_modes()) i = 0;
      if (!load_modes()) return MF_E_INVALIDREQUEST;
      *(void**)a[1] = type_for(i);
      return S_OK;
    }
    case 12: *(uint32_t*)a[0] = 1; return S_OK;      /* GetDeviceStreamCount */
    case 13: /* GetDeviceStreamCategory: VIDEO_PREVIEW */
      if (a[0] != 0) return MF_E_INVALIDSTREAMNUMBER;
      *(int*)a[1] = 0;
      return S_OK;
    case 14: *(int*)a[1] = 0; return S_OK;            /* GetMirrorState */
    case 15: return S_OK;                             /* SetMirrorState */
  }
  return E_NOTIMPL;
}

static long sink_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* e = ((struct mfo*)self->data)->eng;
  switch (slot) {
    case 0: return qi_deny(self, a);
    case 5: if (a[3]) *(uint32_t*)a[3] = 0; return S_OK; /* AddStream(src, type, attrs, idx*) */
    case 6: case 7: case 8: case 9: case 10: return S_OK; /* Prepare, RemoveAllStreams, render */
    case 11: /* SetSampleCallback(idx, cb) */
      e->samplecb = (void*)a[1];
      if (e->samplecb) vcall(e->samplecb, 1, 0, 0, 0, 0);
      return S_OK;
    case 12: *(int*)a[1] = 0; return S_OK;  /* GetMirrorState */
    case 13: case 15: return S_OK;          /* SetMirrorState, SetRotation */
    case 14: *(uint32_t*)a[1] = 0; return S_OK; /* GetRotation */
  }
  return E_NOTIMPL;
}

/* Evento ao Chrome (IMFCaptureEngineOnEventCallback::OnEvent, slot 3). */
static void send_event(struct mfo* e, const void* ext, long status) {
  struct comobj* ev = mf_new("MF MediaEvent", event_h, K_EVENT);
  struct mfo* m;
  if (!ev || !e->evcb) return;
  m = (struct mfo*)ev->data;
  memcpy(m->ext, ext, 16);
  m->status = status;
  (void)vcall(e->evcb, 3, (uintptr_t)ev, 0, 0, 0);
}

/* Tabelas de Huffman padrao (JPEG, anexo K): o MJPEG da webcam vem sem DHT. */
static const uint8_t dc_l[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_c[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t ac_l[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_c[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_lv[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};
static const uint8_t ac_cv[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};

static uint32_t dht_tab(uint8_t* o, uint32_t n, uint8_t tc, const uint8_t* bits, const uint8_t* val) {
  uint32_t i, cnt = 0;
  o[n++] = tc;
  for (i = 0; i < 16; i++) {
    o[n++] = bits[i];
    cnt += bits[i];
  }
  for (i = 0; i < cnt; i++) o[n++] = val ? val[i] : (uint8_t)i;
  return n;
}

static int has_dht(const uint8_t* p, uint32_t n) {
  uint32_t i = 2;
  while (i + 4 <= n) {
    uint8_t mk;
    if (p[i] != 0xFF) return 0;
    mk = p[i + 1];
    if (mk == 0xC4) return 1;
    if (mk == 0xDA || mk == 0xD9) return 0;
    if (mk == 0xFF || (mk >= 0xD0 && mk <= 0xD7) || mk == 0x01) {
      i += mk == 0xFF ? 1 : 2;
      continue;
    }
    i += 2 + ((uint32_t)p[i + 2] << 8 | p[i + 3]);
  }
  return 0;
}

/* JPEG de src (n bytes) em dst, com o DHT padrao depois do SOI se faltar. */
static uint32_t jpeg_fix(uint8_t* dst, const uint8_t* src, uint32_t n) {
  uint32_t o = 0, at;
  if (n < 4 || has_dht(src, n)) {
    memcpy(dst, src, n);
    return n;
  }
  dst[o++] = 0xFF;
  dst[o++] = 0xD8;
  dst[o++] = 0xFF;
  dst[o++] = 0xC4;
  at = o;
  o += 2;
  o = dht_tab(dst, o, 0x00, dc_l, 0);
  o = dht_tab(dst, o, 0x10, ac_l, ac_lv);
  o = dht_tab(dst, o, 0x01, dc_c, 0);
  o = dht_tab(dst, o, 0x11, ac_c, ac_cv);
  dst[at] = (uint8_t)((o - at) >> 8);
  dst[at + 1] = (uint8_t)(o - at);
  memcpy(dst + o, src + 2, n - 2);
  return o + n - 2;
}

static WINAPI int64_t MFGetSystemTime(void) { return (int64_t)k_ms() * 10000; }

/* Quadros: POOL samples, cada um com seu buffer. Um par esta livre quando so
 * nos o seguramos (o Chrome solta o buffer depois de copiar, na thread dele). */
#define POOL 6
static void stream_frames(struct mfo* e) {
  struct comobj* smp[POOL];
  uint8_t* raw;
  int maxlen = 0, seq = 0, i, r, sent = 0;
  uint32_t t0 = k_ms();
  r = kc_open(1, e->w, e->h, &maxlen);
  if (r != 0) {
    wlogx("MF: k_cam_open recusou ", (uint64_t)(uint32_t)r);
    send_event(e, EV_ERROR, E_FAIL);
    e->state = 0;
    e->cmd = 0;
    return;
  }
  raw = (uint8_t*)k_malloc((unsigned)maxlen);
  for (i = 0; i < POOL; i++) {
    struct comobj* b = mf_new("MF MediaBuffer", buf_h, K_BUF);
    smp[i] = mf_new("MF Sample", sample_h, K_SAMPLE);
    if (!b || !smp[i] || !raw) {
      wlog("MF: sem memoria para os quadros", 0);
      kc_close();
      send_event(e, EV_ERROR, E_OUTOFMEMORY);
      e->cmd = 0;
      return;
    }
    ((struct mfo*)b->data)->max = (uint32_t)maxlen + 1024u;
    ((struct mfo*)b->data)->data = (uint8_t*)k_malloc((unsigned)maxlen + 1024u);
    ((struct mfo*)smp[i]->data)->buf = b;
    if (!((struct mfo*)b->data)->data) smp[i] = 0;
  }
  e->state = 1;
  send_event(e, EV_PREVIEW_STARTED, S_OK);
  wlogx("MF: preview ligado, quadro maximo ", (uint64_t)maxlen);
  while (e->cmd == 1) {
    struct comobj* s = 0;
    struct mfo* sm;
    struct mfo* bm;
    int64_t now;
    r = kc_frame(raw, maxlen, &seq, 500);
    if (r < 0) {
      wlog("MF: a webcam parou no meio", 0);
      send_event(e, EV_ERROR, E_FAIL);
      break;
    }
    if (r == 0 || !e->samplecb) continue;
    for (i = 0; i < POOL; i++)
      if (smp[i] && smp[i]->refs <= 1 && smp[i] && ((struct mfo*)smp[i]->data)->buf->refs <= 1) {
        s = smp[i];
        break;
      }
    if (!s) continue; /* o Chrome ainda segura todos: pula o quadro */
    sm = (struct mfo*)s->data;
    bm = (struct mfo*)sm->buf->data;
    bm->len = jpeg_fix(bm->data, raw, (uint32_t)r);
    now = MFGetSystemTime();
    sm->time = now;
    at_u64(sm, SAMPLE_DEVREF_TIME, (uint64_t)now);
    __sync_fetch_and_add(&s->refs, 1);
    (void)vcall(e->samplecb, 3, (uintptr_t)s, 0, 0, 0); /* OnSample */
    __sync_fetch_and_sub(&s->refs, 1);
    if (++sent == 1) wlogx("MF: primeiro quadro entregue, bytes ", (uint64_t)bm->len);
  }
  kc_close();
  e->state = 0;
  wlogx("MF: preview desligado; quadros entregues ", (uint64_t)sent);
  wlogx("MF: segundos ", (uint64_t)((k_ms() - t0) / 1000u));
  if (e->cmd == 2) send_event(e, EV_PREVIEW_STOPPED, S_OK);
  e->cmd = 0;
  k_free(raw);
}

/* Thread do engine (thread do Windows, com TEB: chama o codigo do Chrome). */
static WINAPI DWORD engine_main(void* p) {
  struct comobj* self = (struct comobj*)p;
  struct mfo* e = (struct mfo*)self->data;
  send_event(e, EV_INITIALIZED, S_OK);
  for (;;) {
    int c = e->cmd;
    if (c == 1) {
      stream_frames(e);
      continue;
    }
    if (c == 2) { /* parar sem estar ligado */
      e->cmd = 0;
      send_event(e, EV_PREVIEW_STOPPED, S_OK);
      continue;
    }
    if (self->refs <= 0) break; /* o Chrome soltou o engine */
    k_futex_wait((int*)&e->cmd, c, 500);
  }
  return 0;
}

static long engine_h(struct comobj* self, int slot, uintptr_t* a) {
  struct mfo* e = (struct mfo*)self->data;
  switch (slot) {
    case 0: return qi_deny(self, a);
    case 3: { /* Initialize(evcb, attrs, audio, video) */
      HANDLE(WINAPI * ct)(void*, size_t, void*, void*, DWORD, DWORD*) =
          (HANDLE(WINAPI*)(void*, size_t, void*, void*, DWORD, DWORD*))builtin_lookup("CreateThread");
      DWORD tid = 0;
      if (!a[0] || !a[3] || !ct) return E_INVALIDARG;
      e->evcb = (void*)a[0];
      vcall(e->evcb, 1, 0, 0, 0, 0);
      e->src = (struct comobj*)a[3];
      __sync_fetch_and_add(&e->src->refs, 1);
      if (!ct(0, 0, (void*)engine_main, self, 0, &tid)) return E_FAIL;
      wlog("MF: capture engine iniciado", 0);
      return S_OK;
    }
    case 4: /* StartPreview */
      if (!e->samplecb) return MF_E_INVALIDREQUEST;
      if (!e->w) {
        if (!load_modes()) return MF_E_INVALIDREQUEST;
        e->w = (int)modes[0][0];
        e->h = (int)modes[0][1];
      }
      e->cmd = 1;
      k_futex_wake((int*)&e->cmd, 1);
      return S_OK;
    case 5: /* StopPreview */
      e->cmd = 2;
      k_futex_wake((int*)&e->cmd, 1);
      return S_OK;
    case 9: /* GetSink(type, IMFCaptureSink**) */
      if (a[0] != 1) return MF_E_INVALIDREQUEST; /* so PREVIEW */
      if (!e->sink) {
        e->sink = mf_new("MF CapturePreviewSink", sink_h, K_SINK);
        if (!e->sink) return E_OUTOFMEMORY;
        ((struct mfo*)e->sink->data)->eng = e;
      }
      __sync_fetch_and_add(&e->sink->refs, 1);
      *(void**)a[1] = e->sink;
      return S_OK;
    case 10: /* GetSource(IMFCaptureSource**) */
      if (!e->csrc) {
        e->csrc = mf_new("MF CaptureSource", csrc_h, K_CSRC);
        if (!e->csrc) return E_OUTOFMEMORY;
        ((struct mfo*)e->csrc->data)->eng = e;
      }
      __sync_fetch_and_add(&e->csrc->refs, 1);
      *(void**)a[0] = e->csrc;
      return S_OK;
  }
  return E_NOTIMPL; /* StartRecord, StopRecord, TakePhoto */
}

static long factory_h(struct comobj* self, int slot, uintptr_t* a) {
  if (slot == 0) return qi_deny(self, a);
  if (slot == 3) { /* CreateInstance(clsid, riid, ppv) */
    struct comobj* e = mf_new("MF CaptureEngine", engine_h, K_ENG);
    if (!e) return E_OUTOFMEMORY;
    *(void**)a[2] = e;
    return S_OK;
  }
  return E_NOTIMPL;
}

static struct comobj* mf_new(const char* name, com_handler h, int kind) {
  struct mfo* m = (struct mfo*)wzalloc(sizeof(*m));
  struct comobj* o;
  if (!m) return 0;
  m->kind = kind;
  o = com_new(name, h, m);
  if (o) o->plain = 1;
  return o;
}

/* sys.c (CoCreateInstance): 1 = criado. */
int wmf_cocreate(const void* clsid, void** out) {
  if (!geq(clsid, CLSID_ENGINE_FACTORY)) return 0;
  *out = mf_new("MF CaptureEngineClassFactory", factory_h, K_FACT);
  wlog("MF: CoCreateInstance(CLSID_MFCaptureEngineClassFactory)", 0);
  return *out != 0;
}

/* ---------- funcoes ---------- */

static WINAPI long MFStartup(uint32_t version, DWORD flags) {
  (void)version;
  (void)flags;
  if (wine_debug) wlog("MF: MFStartup", 0);
  return S_OK;
}
static WINAPI long MFShutdown(void) { return S_OK; }

static WINAPI long MFCreateAttributes(void** out, uint32_t n) {
  (void)n;
  if (!out) return E_POINTER;
  *out = mf_new("MF Attributes", attr_h, K_ATTR);
  return *out ? S_OK : E_OUTOFMEMORY;
}

static WINAPI long MFCreateMediaType(void** out) {
  if (!out) return E_POINTER;
  *out = mf_new("MF MediaType", type_h, K_TYPE);
  return *out ? S_OK : E_OUTOFMEMORY;
}

/* Webcam pedida com SOURCE_TYPE = VIDCAP e sem categoria de sensor (o Chrome
 * faz uma segunda passada so para cameras IR, com KSCATEGORY_SENSOR_CAMERA). */
static int wants_webcam(void* attrs) {
  guid_t g;
  if (!attrs) return 0;
  if (vcall(attrs, 10, (uintptr_t)SOURCE_TYPE, (uintptr_t)g, 0, 0) != S_OK || !geq(g, SOURCE_TYPE_VIDCAP))
    return 0;
  if (vcall(attrs, 10, (uintptr_t)VIDCAP_CATEGORY, (uintptr_t)g, 0, 0) == S_OK) return 0;
  return 1;
}

static WINAPI long MFEnumDeviceSources(void* attrs, void*** list, uint32_t* count) {
  struct comobj* act;
  struct mfo* m;
  if (!list || !count) return E_POINTER;
  *list = 0;
  *count = 0;
  if (!wants_webcam(attrs) || load_modes() <= 0) {
    if (wine_debug) wlog("MF: MFEnumDeviceSources: nenhuma camera", 0);
    return S_OK;
  }
  act = mf_new("MF Activate (webcam)", act_h, K_ACT);
  *list = (void**)wheap_alloc(sizeof(void*));
  if (!act || !*list) return E_OUTOFMEMORY;
  m = (struct mfo*)act->data;
  at_guid(m, SOURCE_TYPE, SOURCE_TYPE_VIDCAP);
  at_str(m, FRIENDLY_NAME, cam_name);
  at_str(m, VIDCAP_SYMLINK, cam_link);
  (*list)[0] = act;
  *count = 1;
  wlog("MF: MFEnumDeviceSources: Webcam USB (SODOS)", 0);
  return S_OK;
}

static WINAPI long MFCreateDeviceSource(void* attrs, void** out) {
  if (!out) return E_POINTER;
  *out = 0;
  if (!wants_webcam(attrs) || load_modes() <= 0) return (long)0xC00D36D5; /* MF_E_NOT_FOUND */
  *out = source_new();
  if (wine_debug) wlog("MF: MFCreateDeviceSource", 0);
  return *out ? S_OK : E_OUTOFMEMORY;
}

static WINAPI long MFCreateSourceReaderFromMediaSource(void* src, void* attrs, void** out) {
  (void)attrs;
  if (!out || !src) return E_POINTER;
  *out = mf_new("MF SourceReader", reader_h, K_READER);
  return *out ? S_OK : E_OUTOFMEMORY;
}

#define S(n) {#n, (void*)n}
const struct wine_sym mf_syms[] = {S(MFStartup), S(MFShutdown), S(MFCreateAttributes), S(MFCreateMediaType),
                                   S(MFGetSystemTime), S(MFEnumDeviceSources), S(MFCreateDeviceSource),
                                   S(MFCreateSourceReaderFromMediaSource), {0, 0}};
