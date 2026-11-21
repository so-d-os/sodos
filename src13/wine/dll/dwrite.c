/*
 * dwrite.c - DirectWrite (dwrite.dll) sobre as fontes do SODOS.
 *
 * O que o Chromium/Skia usa: a fabrica (IDWriteFactory), a colecao de fontes
 * do sistema (familias, fontes, nomes), a face (metricas, indices e metricas
 * de glifos, tabelas OpenType para o HarfBuzz, contorno dos glifos para o
 * ID2D1SimplifiedGeometrySink), o arquivo da fonte com o loader local (o Skia
 * le os bytes por stream e o caminho pelo IDWriteLocalFontFileLoader) e a
 * rasterizacao (IDWriteGlyphRunAnalysis -> textura alfa).
 *
 * As fontes vem de PATHSODOS\FONTS (ou D:/U:/Z:/C:\SODOS\FONTS). Nomes de
 * fontes do Windows (Segoe UI, Arial, Consolas, Times New Roman...) viram as
 * familias do SODOS mais parecidas. Layout de texto (TextFormat, TextLayout,
 * TextAnalyzer) ainda nao existe: o Chromium faz o layout com o HarfBuzz.
 *
 * As vtables seguem a ordem exata do dwrite.h do SDK.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "ttf.h"

const uint8_t* wfile_map(const char* path, uint64_t* size, void** tofree); /* file.c */

#define S_OK_ 0
#define E_NOTIMPL_ ((long)0x80004001)
#define E_NOINTERFACE_ ((long)0x80004002)
#define E_INVALIDARG_ ((long)0x80070057)
#define E_OUTOFMEMORY_ ((long)0x8007000E)
#define DWRITE_E_FILENOTFOUND ((long)0x88985003)

typedef long HRESULT_;
#define M(name) HRESULT_(WINAPI* name)
typedef struct {
  uint8_t b[16];
} guid_t;

static int guid_is(const void* g, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t* d4) {
  const uint8_t* p = (const uint8_t*)g;
  return *(const uint32_t*)p == d1 && *(const uint16_t*)(p + 4) == d2 && *(const uint16_t*)(p + 6) == d3 &&
         memcmp(p + 8, d4, 8) == 0;
}
static const uint8_t IUNK4[8] = {0xC0, 0, 0, 0, 0, 0, 0, 0x46};
#define IS_UNK(g) guid_is(g, 0, 0, 0, IUNK4)

/* ===================== fontes do SODOS ===================== */
struct dfile {             /* um arquivo de fonte carregado */
  wchar16 path[160];
  uint8_t* data;
  uint32_t size;
};
struct dfont {             /* uma face */
  struct dfile* file;
  struct ttfont tt;
  int family;              /* indice em fams */
};
struct dfamily {
  wchar16 name[TTF_NAME];
  int fonts[16];
  int nfonts;
};
#define MAX_FILES 160
#define MAX_FONTS 256
#define MAX_FAMS 128
struct fstate {
  struct wproc* owner; /* carregado para este processo (memoria e dele) */
  struct dfile files[MAX_FILES];
  int nfiles;
  struct dfont fonts[MAX_FONTS];
  int nfonts;
  struct dfamily fams[MAX_FAMS];
  int nfams;
};
/* um estado por processo (dois processos do Electron ao mesmo tempo) */
static struct fstate* fst(void) {
  static struct fstate empty;
  struct wproc* p = wp();
  if (!p->dwrite) p->dwrite = wzalloc(sizeof(struct fstate));
  return p->dwrite ? (struct fstate*)p->dwrite : &empty;
}
#define F (*fst())

static int wcs_ieq(const wchar16* a, const wchar16* b) {
  for (;; a++, b++) {
    wchar16 x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return 0;
    if (!x) return 1;
  }
}
static int wcs_ieq8(const wchar16* a, const char* b) {
  for (;; a++, b++) {
    wchar16 x = *a, y = (wchar16)(uint8_t)*b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return 0;
    if (!x) return 1;
  }
}
static void wcs_cpy(wchar16* d, const wchar16* s, int max) {
  int i;
  for (i = 0; s && s[i] && i < max - 1; i++) d[i] = s[i];
  d[i] = 0;
}

/* primeira pasta que tem de fato uma das fontes do SODOS */
static const char* font_dir(void) {
  static char dir[200];
  static const char* const files[] = {"INTER.TTF", "NOTOSANS.TTF", "ROBOTO.TTF", 0};
  static const char* const tries[] = {"D:\\SODOS\\FONTS", "U:\\SODOS\\FONTS", "Z:\\SODOS\\FONTS",
                                      "C:\\SODOS\\FONTS", 0};
  char base[160], probe[260];
  int i, k;
  if (k_getenv("PATHSODOS", base, (int)sizeof(base)) > 0) {
    for (k = 0; files[k]; k++) {
      strcpy(probe, base);
      strcat(probe, "\\FONTS\\");
      strcat(probe, files[k]);
      if (k_fsize(probe) > 0) {
        strcpy(dir, base);
        strcat(dir, "\\FONTS");
        return dir;
      }
    }
  }
  for (i = 0; tries[i]; i++)
    for (k = 0; files[k]; k++) {
      strcpy(probe, tries[i]);
      strcat(probe, "\\");
      strcat(probe, files[k]);
      if (k_fsize(probe) > 0) return tries[i];
    }
  return tries[0];
}

/* devolve a familia da face (-1 = nao entrou) */
static int add_face(struct dfile* fl, uint32_t idx) {
  struct dfont* d;
  int k;
  if (F.nfonts >= MAX_FONTS) return -1;
  d = &F.fonts[F.nfonts];
  if (!ttf_open(&d->tt, fl->data, fl->size, idx)) return -1;
  d->file = fl;
  for (k = 0; k < F.nfams; k++)
    if (wcs_ieq(F.fams[k].name, d->tt.family)) break;
  if (k == F.nfams) {
    if (F.nfams >= MAX_FAMS) return -1;
    wcs_cpy(F.fams[k].name, d->tt.family, TTF_NAME);
    F.fams[k].nfonts = 0;
    F.nfams++;
  }
  if (F.fams[k].nfonts >= 16) return k;
  d->family = k;
  F.fams[k].fonts[F.fams[k].nfonts++] = F.nfonts;
  F.nfonts++;
  return k;
}

static void fonts_load(void) {
  typedef HANDLE(WINAPI * ff_fn)(const wchar16*, void*);
  typedef BOOL(WINAPI * fn_fn)(HANDLE, void*);
  typedef BOOL(WINAPI * fc_fn)(HANDLE);
  ff_fn ff = (ff_fn)builtin_lookup("FindFirstFileW");
  fn_fn fnx = (fn_fn)builtin_lookup("FindNextFileW");
  fc_fn fc = (fc_fn)builtin_lookup("FindClose");
  struct {
    DWORD attr;
    uint8_t times[24];
    DWORD hi, lo, r0, r1;
    wchar16 name[260];
    wchar16 alt[14];
  } __attribute__((packed)) fd;
  const char* dir;
  char pat[220];
  wchar16* wpat;
  HANDLE h;
  if (F.owner) return;
  F.owner = wp();
  dir = font_dir();
  strcpy(pat, dir);
  strcat(pat, "\\*");
  wpat = wdup8to16(pat);
  h = (wpat && ff) ? ff(wpat, &fd) : (HANDLE)(intptr_t)-1;
  k_free(wpat);
  if (h == (HANDLE)(intptr_t)-1 || !h) {
    wlog("DirectWrite: nenhuma fonte em ", dir);
    return;
  }
  do {
    char* n = wdup16to8(fd.name);
    size_t l = n ? strlen(n) : 0;
    if (n && l > 4 && (wstreq(n + l - 4, ".ttf") || wstreq(n + l - 4, ".otf") || wstreq(n + l - 4, ".ttc")) &&
        F.nfiles < MAX_FILES) {
      char path[260];
      uint64_t size = 0;
      void* tofree = 0;
      const uint8_t* m;
      strcpy(path, dir);
      strcat(path, "\\");
      strncat(path, n, sizeof(path) - strlen(path) - 1);
      m = wfile_map(path, &size, &tofree);
      if (m && size > 12) {
        struct dfile* fl = &F.files[F.nfiles++];
        uint32_t nf = 1, i;
        fl->data = (uint8_t*)m; /* fica ate o fim do processo */
        fl->size = (uint32_t)size;
        {
          wchar16* w = wdup8to16(path);
          wcs_cpy(fl->path, w, 160);
          k_free(w);
        }
        if (size >= 12 && m[0] == 't' && m[1] == 't' && m[2] == 'c' && m[3] == 'f')
          nf = ((uint32_t)m[8] << 24) | ((uint32_t)m[9] << 16) | ((uint32_t)m[10] << 8) | m[11];
        for (i = 0; i < nf && i < 16; i++) add_face(fl, i);
      }
    }
    k_free(n);
  } while (fnx(h, &fd));
  fc(h);
  wlogx("DirectWrite: familias de fonte ", (uint64_t)F.nfams);
}

/* nome pedido -> familia (com os nomes do Windows trocados pelos do SODOS) */
static int fam_find(const wchar16* name) {
  static const char* const sans[] = {"segoe ui", "segoe ui variable", "segoe ui variable display",
                                     "segoe ui variable text", "segoe ui semibold", "segoe ui light",
                                     "tahoma", "microsoft sans serif", "ms shell dlg", "ms shell dlg 2",
                                     "arial", "helvetica", "verdana", "calibri", "system-ui", "sans-serif",
                                     "segoe ui emoji", "segoe ui symbol", "segoe ui historic",
                                     "segoe mdl2 assets", "segoe fluent icons", "trebuchet ms",
                                     "malgun gothic", "microsoft yahei", "yu gothic", "meiryo", "nirmala ui",
                                     "ebrima", "gadugi", "leelawadee ui", "microsoft jhenghei", 0};
  static const char* const serif[] = {"times new roman", "georgia", "cambria", "serif", "times",
                                      "book antiqua", "palatino linotype", 0};
  static const char* const mono[] = {"consolas", "courier new", "lucida console", "monospace", "courier",
                                     "cascadia code", "cascadia mono", 0};
  static const char* const want_sans[] = {"inter", "noto sans", "roboto", 0};
  static const char* const want_serif[] = {"noto serif", "inter", 0};
  static const char* const want_mono[] = {"jetbrains mono", "noto sans mono", "fira mono", 0};
  const char* const* want = 0;
  int i, k;
  for (i = 0; i < F.nfams; i++)
    if (wcs_ieq(F.fams[i].name, name)) return i;
  for (i = 0; sans[i]; i++)
    if (wcs_ieq8(name, sans[i])) want = want_sans;
  for (i = 0; !want && serif[i]; i++)
    if (wcs_ieq8(name, serif[i])) want = want_serif;
  for (i = 0; !want && mono[i]; i++)
    if (wcs_ieq8(name, mono[i])) want = want_mono;
  if (!want) return -1;
  for (k = 0; want[k]; k++)
    for (i = 0; i < F.nfams; i++)
      if (wcs_ieq8(F.fams[i].name, want[k])) return i;
  return F.nfams ? 0 : -1;
}
static int dw_style(const struct dfont* d) { return d->tt.italic ? 2 : 0; } /* DWRITE_FONT_STYLE_ITALIC */
static int match_score(const struct dfont* d, int weight, int stretch, int style) {
  int s = d->tt.weight - weight;
  if (s < 0) s = -s;
  s += (d->tt.width - stretch < 0 ? stretch - d->tt.width : d->tt.width - stretch) * 100;
  if ((dw_style(d) != 0) != (style != 0)) s += 1000;
  return s;
}

/* ===================== objetos COM ===================== */
struct dw_obj {
  const void* vtbl;
  volatile int refs;
};
static DWORD WINAPI o_addref(struct dw_obj* o) { return (DWORD)__sync_add_and_fetch(&o->refs, 1); }
/* release generico: objetos fixos nunca saem; os outros sao soltos aqui */
static DWORD WINAPI o_release(struct dw_obj* o) {
  int r = __sync_sub_and_fetch(&o->refs, 1);
  if (r == 0) k_free(o);
  return (DWORD)(r < 0 ? 0 : r);
}
static DWORD WINAPI o_release_static(struct dw_obj* o) {
  (void)o;
  return 1;
}
static void* o_new(const void* vtbl, size_t size) {
  struct dw_obj* o = (struct dw_obj*)wzalloc(size);
  if (o) {
    o->vtbl = vtbl;
    o->refs = 1;
  }
  return o;
}
/* WINEDEBUG=2: interface pedida que nao temos (os 80 primeiros) */
static void dw_qi_miss(const char* who, const void* iid) {
  /* uma linha por (processo, IID) */
  static struct {
    void* proc;
    uint32_t d1;
  } seen[96];
  static volatile int nseen;
  uint32_t d1 = *(const uint32_t*)iid;
  void* me = wp();
  int i, n;
  if (wine_debug < 2) return;
  n = nseen;
  for (i = 0; i < n && i < 96; i++)
    if (seen[i].proc == me && seen[i].d1 == d1) return;
  i = __sync_fetch_and_add(&nseen, 1);
  if (i >= 96) return;
  seen[i].proc = me;
  seen[i].d1 = d1;
  wlogx(who, d1);
}
#define QI_SIMPLE(fn, d1, d2, d3, ...)                                                          \
  static HRESULT_ WINAPI fn(struct dw_obj* o, const void* iid, void** out) {                  \
    static const uint8_t d4[8] = {__VA_ARGS__};                                                \
    if (!out) return E_INVALIDARG_;                                                            \
    if (IS_UNK(iid) || guid_is(iid, d1, d2, d3, d4)) {                                         \
      o_addref(o);                                                                             \
      *out = o;                                                                                \
      return S_OK_;                                                                            \
    }                                                                                          \
    dw_qi_miss("DirectWrite QI recusado (" #fn ") iid ", iid);                                 \
    *out = 0;                                                                                  \
    return E_NOINTERFACE_;                                                                     \
  }

/* ---------- IDWriteLocalizedStrings (uma entrada "en-us") ---------- */
struct dw_strings {
  struct dw_obj o;
  wchar16 s[TTF_NAME];
};
QI_SIMPLE(ls_qi, 0x08256209, 0x099a, 0x4b34, 0xb8, 0x6d, 0xc2, 0x2b, 0x11, 0x0e, 0x77, 0x71)
static DWORD WINAPI ls_count(struct dw_strings* s) {
  (void)s;
  return 1;
}
static HRESULT_ WINAPI ls_find(struct dw_strings* s, const wchar16* loc, DWORD* idx, BOOL* exists) {
  (void)s;
  (void)loc;
  if (idx) *idx = 0;
  if (exists) *exists = 1; /* qualquer idioma cai no unico nome */
  return S_OK_;
}
static const wchar16 en_us[] = {'e', 'n', '-', 'u', 's', 0};
static HRESULT_ WINAPI ls_loclen(struct dw_strings* s, DWORD i, DWORD* len) {
  (void)s;
  if (i) return E_INVALIDARG_;
  *len = 5;
  return S_OK_;
}
static HRESULT_ WINAPI ls_loc(struct dw_strings* s, DWORD i, wchar16* buf, DWORD size) {
  (void)s;
  if (i || size < 6) return E_INVALIDARG_;
  memcpy(buf, en_us, sizeof(en_us));
  return S_OK_;
}
static HRESULT_ WINAPI ls_strlen(struct dw_strings* s, DWORD i, DWORD* len) {
  if (i) return E_INVALIDARG_;
  *len = (DWORD)wstrlen16(s->s);
  return S_OK_;
}
static HRESULT_ WINAPI ls_str(struct dw_strings* s, DWORD i, wchar16* buf, DWORD size) {
  DWORD n = (DWORD)wstrlen16(s->s);
  if (i || size < n + 1) return E_INVALIDARG_;
  memcpy(buf, s->s, (n + 1) * 2);
  return S_OK_;
}
static const void* const ls_vtbl[] = {ls_qi, o_addref, o_release, ls_count, ls_find,
                                      ls_loclen, ls_loc, ls_strlen, ls_str};
static struct dw_strings* strings_new(const wchar16* s) {
  struct dw_strings* o = (struct dw_strings*)o_new(ls_vtbl, sizeof(struct dw_strings));
  if (o) wcs_cpy(o->s, s, TTF_NAME);
  return o;
}

/* ---------- arquivo, loader local e stream ---------- */
struct dw_stream {
  struct dw_obj o;
  struct dfile* f;
};
QI_SIMPLE(st_qi, 0x6d4865fe, 0x0ab8, 0x4d91, 0x8f, 0x62, 0x5d, 0xd6, 0xbe, 0x34, 0xa3, 0xe0)
static HRESULT_ WINAPI st_read(struct dw_stream* s, const void** start, uint64_t off, uint64_t size, void** ctx) {
  if (ctx) *ctx = 0;
  if (off > s->f->size || size > s->f->size - off) {
    *start = 0;
    return E_INVALIDARG_;
  }
  *start = s->f->data + off;
  return S_OK_;
}
static void WINAPI st_release_frag(struct dw_stream* s, void* ctx) {
  (void)s;
  (void)ctx;
}
static HRESULT_ WINAPI st_size(struct dw_stream* s, uint64_t* size) {
  *size = s->f->size;
  return S_OK_;
}
static HRESULT_ WINAPI st_time(struct dw_stream* s, uint64_t* t) {
  (void)s;
  *t = 0x01D5000000000000ull;
  return S_OK_;
}
static const void* const st_vtbl[] = {st_qi, o_addref, o_release, st_read, st_release_frag, st_size, st_time};

/* loader local (fixo): chave = indice do arquivo (4 bytes) */
static HRESULT_ WINAPI ld_qi(struct dw_obj* o, const void* iid, void** out) {
  static const uint8_t ld4[8] = {0x8a, 0x08, 0xd6, 0x95, 0xb1, 0x1c, 0xaa, 0x49};    /* IDWriteFontFileLoader */
  static const uint8_t lld4[8] = {0xa2, 0xec, 0xd8, 0x62, 0x08, 0xf7, 0xc0, 0xa2};   /* IDWriteLocalFontFileLoader */
  if (IS_UNK(iid) || guid_is(iid, 0x727cad4e, 0xd6af, 0x4c9e, ld4) ||
      guid_is(iid, 0xb2d9f3ec, 0xc9fe, 0x4a11, lld4)) {
    *out = o;
    return S_OK_;
  }
  *out = 0;
  return E_NOINTERFACE_;
}
static struct dfile* key_file(const void* key, DWORD size) {
  uint32_t i;
  if (size != 4) return 0;
  memcpy(&i, key, 4);
  return i < (uint32_t)F.nfiles ? &F.files[i] : 0;
}
static HRESULT_ WINAPI ld_stream(struct dw_obj* o, const void* key, DWORD size, struct dw_stream** out) {
  struct dfile* f = key_file(key, size);
  (void)o;
  if (!f) return E_INVALIDARG_;
  *out = (struct dw_stream*)o_new(st_vtbl, sizeof(struct dw_stream));
  if (!*out) return E_OUTOFMEMORY_;
  (*out)->f = f;
  return S_OK_;
}
static HRESULT_ WINAPI ld_pathlen(struct dw_obj* o, const void* key, DWORD size, DWORD* len) {
  struct dfile* f = key_file(key, size);
  (void)o;
  if (!f) return E_INVALIDARG_;
  *len = (DWORD)wstrlen16(f->path);
  return S_OK_;
}
static HRESULT_ WINAPI ld_path(struct dw_obj* o, const void* key, DWORD size, wchar16* buf, DWORD n) {
  struct dfile* f = key_file(key, size);
  DWORD l;
  (void)o;
  if (!f) return E_INVALIDARG_;
  l = (DWORD)wstrlen16(f->path);
  if (n < l + 1) return E_INVALIDARG_;
  memcpy(buf, f->path, (l + 1) * 2);
  return S_OK_;
}
static HRESULT_ WINAPI ld_time(struct dw_obj* o, const void* key, DWORD size, uint64_t* t) {
  (void)o; (void)key; (void)size;
  *t = 0x01D5000000000000ull;
  return S_OK_;
}
static const void* const ld_vtbl[] = {ld_qi, o_addref, o_release_static, ld_stream, ld_pathlen, ld_path, ld_time};
static struct dw_obj loader = {ld_vtbl, 1};

struct dw_file {
  struct dw_obj o;
  uint32_t idx;
};
QI_SIMPLE(fi_qi, 0x739d886a, 0xcef5, 0x47dc, 0x87, 0x69, 0x1a, 0x8b, 0x41, 0xbe, 0xbb, 0xb0)
static HRESULT_ WINAPI fi_key(struct dw_file* f, const void** key, DWORD* size) {
  *key = &f->idx;
  *size = 4;
  return S_OK_;
}
static HRESULT_ WINAPI fi_loader(struct dw_file* f, struct dw_obj** out) {
  (void)f;
  *out = &loader;
  return S_OK_;
}
static HRESULT_ WINAPI fi_analyze(struct dw_file* f, BOOL* ok, int* ftype, int* facetype, DWORD* nfaces) {
  struct dfile* d = &F.files[f->idx];
  int ttc = d->size >= 4 && d->data[0] == 't' && d->data[1] == 't' && d->data[2] == 'c' && d->data[3] == 'f';
  *ok = 1;
  if (ftype) *ftype = ttc ? 4 : 2;     /* DWRITE_FONT_FILE_TYPE_TRUETYPE(_COLLECTION) */
  if (facetype) *facetype = ttc ? 2 : 1; /* DWRITE_FONT_FACE_TYPE_TRUETYPE(_COLLECTION) */
  if (nfaces) *nfaces = 1;
  return S_OK_;
}
static const void* const fi_vtbl[] = {fi_qi, o_addref, o_release, fi_key, fi_loader, fi_analyze};
static struct dw_file* file_new(struct dfile* f) {
  struct dw_file* o = (struct dw_file*)o_new(fi_vtbl, sizeof(struct dw_file));
  if (o) o->idx = (uint32_t)(f - F.files);
  return o;
}

/* ---------- IDWriteFontFace ---------- */
struct dw_face {
  struct dw_obj o;
  int font; /* indice em F.fonts */
};
struct font_metrics {
  uint16_t upem, asc, desc;
  int16_t gap;
  uint16_t cap, xh;
  int16_t ul_pos;
  uint16_t ul_size;
  int16_t st_pos;
  uint16_t st_size;
};
static void fill_metrics(const struct ttfont* t, struct font_metrics* m) {
  m->upem = (uint16_t)t->upem;
  m->asc = (uint16_t)t->asc;
  m->desc = (uint16_t)t->desc;
  m->gap = (int16_t)t->gap;
  m->cap = (uint16_t)t->cap_height;
  m->xh = (uint16_t)t->x_height;
  m->ul_pos = (int16_t)t->ul_pos;
  m->ul_size = (uint16_t)t->ul_size;
  m->st_pos = (int16_t)t->strike_pos;
  m->st_size = (uint16_t)t->strike_size;
}
QI_SIMPLE(fc_qi, 0x5f49804d, 0x7024, 0x4d43, 0xbf, 0xa9, 0xd2, 0x59, 0x84, 0xf5, 0x38, 0x49)
static int WINAPI fc_type(struct dw_face* f) { return F.fonts[f->font].tt.nfaces > 1 ? 2 : F.fonts[f->font].tt.cff ? 0 : 1; }
static HRESULT_ WINAPI fc_files(struct dw_face* f, DWORD* n, struct dw_file** files) {
  if (!files) {
    *n = 1;
    return S_OK_;
  }
  if (*n < 1) return E_INVALIDARG_;
  files[0] = file_new(F.fonts[f->font].file);
  *n = 1;
  return files[0] ? S_OK_ : E_OUTOFMEMORY_;
}
static DWORD WINAPI fc_index(struct dw_face* f) { return F.fonts[f->font].tt.index; }
static int WINAPI fc_sims(struct dw_face* f) {
  (void)f;
  return 0;
}
static BOOL WINAPI fc_symbol(struct dw_face* f) {
  (void)f;
  return 0;
}
static void WINAPI fc_metrics(struct dw_face* f, struct font_metrics* m) { fill_metrics(&F.fonts[f->font].tt, m); }
static uint16_t WINAPI fc_nglyphs(struct dw_face* f) { return (uint16_t)F.fonts[f->font].tt.nglyphs; }
struct glyph_metrics {
  int32_t lsb;
  uint32_t adv;
  int32_t rsb, tsb;
  uint32_t adv_h;
  int32_t bsb, vorigin;
};
static HRESULT_ WINAPI fc_gmetrics(struct dw_face* f, const uint16_t* idx, DWORD n, struct glyph_metrics* out,
                                   BOOL sideways) {
  const struct ttfont* t = &F.fonts[f->font].tt;
  DWORD i;
  (void)sideways;
  for (i = 0; i < n; i++) {
    int x0, y0, x1, y1, adv = ttf_advance(t, idx[i]);
    ttf_bbox(t, idx[i], &x0, &y0, &x1, &y1);
    out[i].lsb = ttf_lsb(t, idx[i]);
    out[i].adv = (uint32_t)adv;
    out[i].rsb = adv - (out[i].lsb + (x1 - x0));
    out[i].adv_h = (uint32_t)(t->asc + t->desc);
    out[i].tsb = t->asc - y1;
    out[i].bsb = (int32_t)out[i].adv_h - out[i].tsb - (y1 - y0);
    out[i].vorigin = t->asc;
  }
  return S_OK_;
}
static HRESULT_ WINAPI fc_indices(struct dw_face* f, const uint32_t* cps, DWORD n, uint16_t* out) {
  DWORD i;
  for (i = 0; i < n; i++) out[i] = (uint16_t)ttf_glyph(&F.fonts[f->font].tt, cps[i]);
  return S_OK_;
}
static HRESULT_ WINAPI fc_table(struct dw_face* f, uint32_t tag, const void** data, DWORD* size, void** ctx,
                                BOOL* exists) {
  /* DWRITE_MAKE_OPENTYPE_TAG e little-endian ('c'|'m'<<8...); as tabelas sao big-endian */
  uint32_t be = ((tag & 0xFF) << 24) | ((tag & 0xFF00) << 8) | ((tag >> 8) & 0xFF00) | (tag >> 24);
  uint32_t len = 0;
  const uint8_t* t = ttf_table(&F.fonts[f->font].tt, be, &len);
  *data = t;
  *size = t ? len : 0;
  if (ctx) *ctx = 0;
  *exists = t != 0;
  return S_OK_;
}
static void WINAPI fc_reltable(struct dw_face* f, void* ctx) {
  (void)f;
  (void)ctx;
}

/* contorno -> ID2D1SimplifiedGeometrySink (vtbl: 3 SetFillMode, 4 SetSegmentFlags,
 * 5 BeginFigure, 6 AddLines, 7 AddBeziers, 8 EndFigure, 9 Close) */
struct sink_ctx {
  void** sink;
  float scale, ox, oy;
  float cx, cy;
  int open;
};
typedef void(WINAPI* sink_begin)(void*, uint64_t, int);
typedef void(WINAPI* sink_lines)(void*, const float*, DWORD);
typedef void(WINAPI* sink_bez)(void*, const float*, DWORD);
typedef void(WINAPI* sink_end)(void*, int);
static void sk_pt(struct sink_ctx* c, float x, float y, float* ox, float* oy) {
  *ox = c->ox + x * c->scale;
  *oy = c->oy - y * c->scale;
}
static void sk_move(void* ctx, float x, float y) {
  struct sink_ctx* c = (struct sink_ctx*)ctx;
  float p[2];
  uint64_t packed;
  sk_pt(c, x, y, &p[0], &p[1]);
  memcpy(&packed, p, 8); /* D2D1_POINT_2F por valor: 8 bytes num registrador */
  ((sink_begin)((void**)*c->sink)[5])(c->sink, packed, 0 /* FILLED */);
  c->cx = p[0];
  c->cy = p[1];
  c->open = 1;
}
static void sk_line(void* ctx, float x, float y) {
  struct sink_ctx* c = (struct sink_ctx*)ctx;
  float p[2];
  sk_pt(c, x, y, &p[0], &p[1]);
  ((sink_lines)((void**)*c->sink)[6])(c->sink, p, 1);
  c->cx = p[0];
  c->cy = p[1];
}
static void sk_quad(void* ctx, float qx, float qy, float x, float y) {
  struct sink_ctx* c = (struct sink_ctx*)ctx;
  float q[2], e[2], b[6];
  sk_pt(c, qx, qy, &q[0], &q[1]);
  sk_pt(c, x, y, &e[0], &e[1]);
  /* quadratica -> cubica */
  b[0] = c->cx + 2.0f / 3.0f * (q[0] - c->cx);
  b[1] = c->cy + 2.0f / 3.0f * (q[1] - c->cy);
  b[2] = e[0] + 2.0f / 3.0f * (q[0] - e[0]);
  b[3] = e[1] + 2.0f / 3.0f * (q[1] - e[1]);
  b[4] = e[0];
  b[5] = e[1];
  ((sink_bez)((void**)*c->sink)[7])(c->sink, b, 1);
  c->cx = e[0];
  c->cy = e[1];
}
static void sk_close(void* ctx) {
  struct sink_ctx* c = (struct sink_ctx*)ctx;
  if (c->open) ((sink_end)((void**)*c->sink)[8])(c->sink, 1 /* CLOSED */);
  c->open = 0;
}
struct glyph_offset {
  float adv, asc;
};
/* WINEDEBUG=2: o que o Skia pede para desenhar (familia, tamanho, glifos) e
 * se saiu algo. Os 60 primeiros e depois 1 a cada 500. */
static int dw_put(char* b, int j, const char* s) {
  while (*s && j < 150) b[j++] = *s++;
  return j;
}
static int dw_num(char* b, int j, int v) {
  char t[12];
  int k = 0;
  if (v < 0) {
    if (j < 150) b[j++] = '-';
    v = -v;
  }
  do t[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 11);
  while (k && j < 150) b[j++] = t[--k];
  return j;
}
static void dw_note(const char* what, int font, float em, const uint16_t* idx, DWORD n, int empty) {
  static volatile int cnt;
  char b[160];
  int k, j = 0, i;
  const wchar16* fam;
  if (wine_debug < 2) return;
  k = __sync_add_and_fetch(&cnt, 1);
  if (k > 60 && k % 500) return;
  fam = F.fams[F.fonts[font].family].name;
  for (i = 0; fam[i] && j < 40; i++) b[j++] = (char)(fam[i] < 128 ? fam[i] : '?');
  j = dw_put(b, j, " em10=");
  j = dw_num(b, j, (int)(em * 10));
  j = dw_put(b, j, " n=");
  j = dw_num(b, j, (int)n);
  j = dw_put(b, j, " g=");
  for (i = 0; i < (int)n && i < 6; i++) {
    j = dw_num(b, j, idx ? idx[i] : -1);
    j = dw_put(b, j, ",");
  }
  j = dw_put(b, j, empty ? " VAZIO" : " ok");
  b[j] = 0;
  wlog(what, b);
}
static HRESULT_ WINAPI fc_outline(struct dw_face* f, float em, const uint16_t* idx, const float* advs,
                                  const struct glyph_offset* offs, DWORD n, BOOL sideways, BOOL rtl, void** sink) {
  const struct ttfont* t = &F.fonts[f->font].tt;
  struct sink_ctx c;
  struct ttf_sink s;
  float pen = 0;
  DWORD i;
  (void)sideways;
  if (!sink) return E_INVALIDARG_;
  ((void(WINAPI*)(void*, int))((void**)*sink)[3])(sink, 1); /* D2D1_FILL_MODE_WINDING */
  dw_note("DirectWrite contorno: ", f->font, em, idx, n, 0);
  memset(&c, 0, sizeof(c));
  c.sink = sink;
  c.scale = em / (float)t->upem;
  s.ctx = &c;
  s.move = sk_move;
  s.line = sk_line;
  s.quad = sk_quad;
  s.close = sk_close;
  for (i = 0; i < n; i++) {
    float adv = advs ? advs[i] : (float)ttf_advance(t, idx[i]) * c.scale;
    if (rtl) pen -= adv;
    c.ox = pen + (offs ? (rtl ? -offs[i].adv : offs[i].adv) : 0);
    c.oy = offs ? -offs[i].asc : 0;
    ttf_outline(t, idx[i], &s);
    if (!rtl) pen += adv;
  }
  return S_OK_;
}
static HRESULT_ WINAPI fc_rendermode(struct dw_face* f, float em, float ppd, int measuring, void* params, int* mode) {
  (void)f; (void)em; (void)ppd; (void)measuring; (void)params;
  *mode = 5; /* DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC */
  return S_OK_;
}
static HRESULT_ WINAPI fc_gdimetrics(struct dw_face* f, float em, float ppd, const void* xf, struct font_metrics* m) {
  (void)em; (void)ppd; (void)xf;
  fill_metrics(&F.fonts[f->font].tt, m);
  return S_OK_;
}
static HRESULT_ WINAPI fc_gdigmetrics(struct dw_face* f, float em, float ppd, const void* xf, BOOL natural,
                                      const uint16_t* idx, DWORD n, struct glyph_metrics* out, BOOL sideways) {
  (void)em; (void)ppd; (void)xf; (void)natural;
  return fc_gmetrics(f, idx, n, out, sideways);
}
static const void* const fc_vtbl[] = {fc_qi,      o_addref,  o_release,   fc_type,       fc_files,     fc_index,
                                      fc_sims,    fc_symbol, fc_metrics,  fc_nglyphs,    fc_gmetrics,  fc_indices,
                                      fc_table,   fc_reltable, fc_outline, fc_rendermode, fc_gdimetrics,
                                      fc_gdigmetrics};
static struct dw_face* face_new(int font) {
  struct dw_face* o = (struct dw_face*)o_new(fc_vtbl, sizeof(struct dw_face));
  if (o) o->font = font;
  return o;
}

/* ---------- IDWriteFont ---------- */
struct dw_font {
  struct dw_obj o;
  int font;
};
static void* family_get(int fam);
QI_SIMPLE(ft_qi, 0xacd16696, 0x8c14, 0x4f5d, 0x87, 0x7e, 0xfe, 0x3f, 0xc1, 0xd3, 0x27, 0x37)
static HRESULT_ WINAPI ft_family(struct dw_font* f, void** out) {
  *out = family_get(F.fonts[f->font].family);
  return S_OK_;
}
static int WINAPI ft_weight(struct dw_font* f) { return F.fonts[f->font].tt.weight; }
static int WINAPI ft_stretch(struct dw_font* f) { return F.fonts[f->font].tt.width; }
static int WINAPI ft_style(struct dw_font* f) { return dw_style(&F.fonts[f->font]); }
static BOOL WINAPI ft_symbol(struct dw_font* f) {
  (void)f;
  return 0;
}
static HRESULT_ WINAPI ft_facenames(struct dw_font* f, struct dw_strings** out) {
  const wchar16* st = F.fonts[f->font].tt.style;
  static const wchar16 reg[] = {'R', 'e', 'g', 'u', 'l', 'a', 'r', 0};
  *out = strings_new(st[0] ? st : reg);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT_ WINAPI ft_info(struct dw_font* f, int id, struct dw_strings** out, BOOL* exists) {
  const struct ttfont* t = &F.fonts[f->font].tt;
  const wchar16* s = id == 11 ? t->win_family : id == 12 ? t->win_style : id == 13 ? t->family
                   : id == 14 ? t->style : id == 16 ? t->full : id == 17 ? t->postscript : 0;
  *out = 0;
  *exists = 0;
  if (!s || !s[0]) return S_OK_;
  *out = strings_new(s);
  *exists = *out != 0;
  return S_OK_;
}
static int WINAPI ft_sims(struct dw_font* f) {
  (void)f;
  return 0;
}
static void WINAPI ft_metrics(struct dw_font* f, struct font_metrics* m) { fill_metrics(&F.fonts[f->font].tt, m); }
static HRESULT_ WINAPI ft_has(struct dw_font* f, uint32_t cp, BOOL* exists) {
  *exists = ttf_glyph(&F.fonts[f->font].tt, cp) != 0;
  return S_OK_;
}
static HRESULT_ WINAPI ft_face(struct dw_font* f, struct dw_face** out) {
  *out = face_new(f->font);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static const void* const font_vtbl[] = {ft_qi,     o_addref,   o_release,  ft_family, ft_weight,
                                        ft_stretch, ft_style,  ft_symbol,  ft_facenames, ft_info,
                                        ft_sims,   ft_metrics, ft_has,     ft_face};
static struct dw_font* font_new(int font) {
  struct dw_font* o = (struct dw_font*)o_new(font_vtbl, sizeof(struct dw_font));
  if (o) o->font = font;
  return o;
}

/* ---------- IDWriteFontList e IDWriteFontFamily ---------- */
struct dw_list {
  struct dw_obj o;
  int fam;
  int order[16];
  int n;
};
static void* collection_get(void);
static const void* const fam_vtbl[];
static HRESULT_ WINAPI fl_qi(struct dw_obj* o, const void* iid, void** out) {
  static const uint8_t fl4[8] = {0xae, 0xf9, 0xa2, 0xfb, 0x86, 0xed, 0x6a, 0xcb};  /* IDWriteFontList */
  static const uint8_t fam4[8] = {0x98, 0x02, 0x62, 0xec, 0x4a, 0xbd, 0x7a, 0xdd}; /* IDWriteFontFamily */
  if (IS_UNK(iid) || guid_is(iid, 0x1a0d8438, 0x1d97, 0x4ec1, fl4) ||
      (o->vtbl == (const void*)fam_vtbl && guid_is(iid, 0xda20d8ef, 0x812a, 0x4c43, fam4))) {
    o_addref(o);
    *out = o;
    return S_OK_;
  }
  *out = 0;
  return E_NOINTERFACE_;
}
static HRESULT_ WINAPI fl_collection(struct dw_list* l, void** out) {
  (void)l;
  *out = collection_get();
  return S_OK_;
}
static DWORD WINAPI fl_count(struct dw_list* l) { return (DWORD)l->n; }
static HRESULT_ WINAPI fl_font(struct dw_list* l, DWORD i, struct dw_font** out) {
  if (i >= (DWORD)l->n) return E_INVALIDARG_;
  *out = font_new(l->order[i]);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT_ WINAPI fm_names(struct dw_list* l, struct dw_strings** out) {
  *out = strings_new(F.fams[l->fam].name);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static void sort_by(struct dw_list* l, int weight, int stretch, int style) {
  int i, j;
  for (i = 1; i < l->n; i++) {
    int v = l->order[i];
    j = i;
    while (j > 0 && match_score(&F.fonts[l->order[j - 1]], weight, stretch, style) >
                        match_score(&F.fonts[v], weight, stretch, style)) {
      l->order[j] = l->order[j - 1];
      j--;
    }
    l->order[j] = v;
  }
}
static HRESULT_ WINAPI fm_first(struct dw_list* l, int weight, int stretch, int style, struct dw_font** out) {
  struct dw_list tmp = *l;
  if (!l->n) return E_INVALIDARG_;
  sort_by(&tmp, weight, stretch, style);
  *out = font_new(tmp.order[0]);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static const void* const list_vtbl[] = {fl_qi, o_addref, o_release, fl_collection, fl_count, fl_font};
static HRESULT_ WINAPI fm_matching(struct dw_list* l, int weight, int stretch, int style, struct dw_list** out) {
  struct dw_list* o = (struct dw_list*)o_new(list_vtbl, sizeof(struct dw_list));
  if (!o) return E_OUTOFMEMORY_;
  o->fam = l->fam;
  o->n = l->n;
  memcpy(o->order, l->order, sizeof(o->order));
  sort_by(o, weight, stretch, style);
  *out = o;
  return S_OK_;
}
static const void* const fam_vtbl[] = {fl_qi,    o_addref,  o_release,   fl_collection, fl_count,
                                       fl_font,  fm_names,  fm_first,    fm_matching};
static void* family_get(int fam) {
  struct dw_list* o = (struct dw_list*)o_new(fam_vtbl, sizeof(struct dw_list));
  int i;
  if (!o) return 0;
  o->fam = fam;
  o->n = F.fams[fam].nfonts;
  for (i = 0; i < o->n; i++) o->order[i] = F.fams[fam].fonts[i];
  return o;
}

/* ---------- IDWriteFontCollection (a do sistema, fixa) ---------- */
QI_SIMPLE(co_qi, 0xa84cee02, 0x3eea, 0x4eee, 0xa8, 0x27, 0x87, 0xc1, 0xa0, 0x2a, 0x0f, 0xcc)
static DWORD WINAPI co_count(struct dw_obj* o) {
  (void)o;
  return (DWORD)F.nfams;
}
static HRESULT_ WINAPI co_family(struct dw_obj* o, DWORD i, void** out) {
  (void)o;
  if (i >= (DWORD)F.nfams) return E_INVALIDARG_;
  *out = family_get((int)i);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT_ WINAPI co_find(struct dw_obj* o, const wchar16* name, DWORD* idx, BOOL* exists) {
  int k = name ? fam_find(name) : -1;
  (void)o;
  *exists = k >= 0;
  *idx = k >= 0 ? (DWORD)k : 0xFFFFFFFFu;
  return S_OK_;
}
static HRESULT_ WINAPI co_fromface(struct dw_obj* o, struct dw_face* face, struct dw_font** out) {
  (void)o;
  if (!face) return E_INVALIDARG_;
  *out = font_new(face->font);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static const void* const co_vtbl[] = {co_qi, o_addref, o_release_static, co_count, co_family, co_find, co_fromface};
static struct dw_obj collection = {co_vtbl, 1};
static void* collection_get(void) { return &collection; }

/* ---------- colecao personalizada (CreateCustomFontCollection) ----------
 * No renderer do Chromium as fontes do sistema vem por aqui: o
 * DWriteFontCollectionProxy e o loader, e a cada familia pedida o browser
 * manda os arquivos (handles) e ele chama CreateCustomFontCollection com a
 * chave da familia. Lemos cada arquivo pelo stream do loader do app (ou
 * reconhecemos um arquivo nosso), acrescentamos as faces e devolvemos uma
 * colecao so com as familias delas. Antes devolvia E_NOTIMPL e o renderer
 * ficava sem fontes do sistema: todo texto que nao era fonte web sumia. */
struct dw_coll {
  struct dw_obj o;
  int n;
  int fams[32];
};
static HRESULT_ WINAPI cc_qi(struct dw_obj* o, const void* iid, void** out) {
  static const uint8_t c4[8] = {0xa8, 0x27, 0x87, 0xc1, 0xa0, 0x2a, 0x0f, 0xcc}; /* IDWriteFontCollection */
  if (IS_UNK(iid) || guid_is(iid, 0xa84cee02, 0x3eea, 0x4eee, c4)) {
    o_addref(o);
    *out = o;
    return S_OK_;
  }
  dw_qi_miss("DirectWrite QI recusado (colecao propria) iid ", iid);
  *out = 0;
  return E_NOINTERFACE_;
}
static DWORD WINAPI cc_count(struct dw_coll* c) { return (DWORD)c->n; }
static HRESULT_ WINAPI cc_family(struct dw_coll* c, DWORD i, void** out) {
  if (i >= (DWORD)c->n) return E_INVALIDARG_;
  *out = family_get(c->fams[i]);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT_ WINAPI cc_find(struct dw_coll* c, const wchar16* name, DWORD* idx, BOOL* exists) {
  int i;
  *exists = 0;
  *idx = 0xFFFFFFFFu;
  for (i = 0; name && i < c->n; i++)
    if (wcs_ieq(F.fams[c->fams[i]].name, name)) {
      *exists = 1;
      *idx = (DWORD)i;
      break;
    }
  return S_OK_;
}
static const void* const cc_vtbl[] = {cc_qi, o_addref, o_release, cc_count, cc_family, cc_find, co_fromface};

/* chamadas COM nos objetos do app (vtable[i]) */
#define VT(obj, i) (((void* const*)*(void* const*)(obj))[i])
typedef HRESULT_(WINAPI* cenum_fn)(void* self, void* factory, const void* key, DWORD size, void** out);
typedef HRESULT_(WINAPI* movenext_fn)(void* self, BOOL* has);
typedef HRESULT_(WINAPI* getfile_fn)(void* self, void** out);
typedef HRESULT_(WINAPI* getkey_fn)(void* self, const void** key, DWORD* size);
typedef HRESULT_(WINAPI* getloader_fn)(void* self, void** out);
typedef HRESULT_(WINAPI* cstream_fn)(void* self, const void* key, DWORD size, void** out);
typedef HRESULT_(WINAPI* read_fn)(void* self, const void** start, uint64_t off, uint64_t size, void** ctx);
typedef void(WINAPI* relfrag_fn)(void* self, void* ctx);
typedef HRESULT_(WINAPI* fsize_fn)(void* self, uint64_t* size);
typedef DWORD(WINAPI* rel_fn)(void* self);
#define REL(obj) ((rel_fn)VT(obj, 2))(obj)

/* arquivo pelo loader do app (key, ksz) -> indice em F.files; le os bytes uma
 * vez (o mesmo arquivo de novo e reconhecido pelo conteudo) */
static int foreign_load(void* ld, const void* key, DWORD ksz) {
  void* st = 0;
  uint64_t size = 0;
  const void* frag = 0;
  void* ctx = 0;
  uint8_t* data;
  int i, r = -1;
  if (ld == &loader) {
    struct dfile* f = key_file(key, ksz);
    return f ? (int)(f - F.files) : -1;
  }
  if (((cstream_fn)VT(ld, 3))(ld, key, ksz, &st) < 0 || !st) return -1;
  if (((fsize_fn)VT(st, 5))(st, &size) < 0 || size < 12 || size > 64u << 20) goto out;
  if (((read_fn)VT(st, 3))(st, &frag, 0, size, &ctx) < 0 || !frag) goto out;
  for (i = 0; i < F.nfiles; i++)
    if (F.files[i].size == (uint32_t)size && !memcmp(F.files[i].data, frag, size < 4096 ? (size_t)size : 4096) &&
        !memcmp(F.files[i].data + size - 256, (const uint8_t*)frag + size - 256, 256)) {
      r = i;
      break;
    }
  if (r < 0 && F.nfiles < MAX_FILES && (data = (uint8_t*)wzalloc((size_t)size)) != 0) {
    struct dfile* fl = &F.files[F.nfiles];
    memcpy(data, frag, (size_t)size);
    fl->data = data; /* fica ate o fim do processo */
    fl->size = (uint32_t)size;
    fl->path[0] = 0;
    r = F.nfiles++;
  }
  ((relfrag_fn)VT(st, 4))(st, ctx);
out:
  REL(st);
  return r;
}
/* IDWriteFontFile qualquer -> indice em F.files */
static int custom_file(void* file) {
  void* ld = 0;
  const void* key = 0;
  DWORD ksz = 0;
  int r;
  if (*(void* const*)file == (const void*)fi_vtbl) return (int)((struct dw_file*)file)->idx; /* nosso */
  if (((getkey_fn)VT(file, 3))(file, &key, &ksz) < 0) return -1;
  if (((getloader_fn)VT(file, 4))(file, &ld) < 0 || !ld) return -1;
  r = foreign_load(ld, key, ksz);
  if (ld != &loader) REL(ld);
  return r;
}

static HRESULT_ WINAPI fa_customcoll(struct dw_obj* fac, void* cl, const void* key, DWORD ksz, void** out) {
  void* en = 0;
  struct dw_coll* c;
  BOOL has = 0;
  int guard = 0;
  if (!out) return E_INVALIDARG_;
  *out = 0;
  if (!cl) return E_INVALIDARG_;
  fonts_load();
  c = (struct dw_coll*)o_new(cc_vtbl, sizeof(struct dw_coll));
  if (!c) return E_OUTOFMEMORY_;
  if (((cenum_fn)VT(cl, 3))(cl, fac, key, ksz, &en) >= 0 && en) {
    while (guard++ < 256 && ((movenext_fn)VT(en, 3))(en, &has) >= 0 && has) {
      void* file = 0;
      int fi, nf = 1, j;
      if (((getfile_fn)VT(en, 4))(en, &file) < 0 || !file) continue;
      fi = custom_file(file);
      REL(file);
      if (fi < 0) continue;
      {
        const uint8_t* m = F.files[fi].data;
        if (m[0] == 't' && m[1] == 't' && m[2] == 'c' && m[3] == 'f')
          nf = (int)(((uint32_t)m[8] << 24) | ((uint32_t)m[9] << 16) | ((uint32_t)m[10] << 8) | m[11]);
      }
      for (j = 0; j < nf && j < 16; j++) {
        int k, fam = -1, x;
        /* face ja carregada deste arquivo? */
        for (k = 0; k < F.nfonts; k++)
          if (F.fonts[k].file == &F.files[fi] && F.fonts[k].tt.index == (uint32_t)j) {
            fam = F.fonts[k].family;
            break;
          }
        if (k == F.nfonts) fam = add_face(&F.files[fi], (uint32_t)j);
        if (fam < 0) continue;
        for (x = 0; x < c->n; x++)
          if (c->fams[x] == fam) break;
        if (x == c->n && c->n < 32) c->fams[c->n++] = fam;
      }
    }
    REL(en);
  }
  if (wine_debug >= 2) wlogx("DirectWrite: colecao propria, familias ", (uint64_t)c->n);
  *out = c;
  return S_OK_;
}

/* ---------- IDWriteRenderingParams ---------- */
QI_SIMPLE(rp_qi, 0x2f0da53a, 0x2add, 0x47cd, 0x82, 0xee, 0xd9, 0xec, 0x34, 0x68, 0x8e, 0x75)
static float WINAPI rp_gamma(struct dw_obj* o) {
  (void)o;
  return 1.8f;
}
static float WINAPI rp_contrast(struct dw_obj* o) {
  (void)o;
  return 0.5f;
}
static float WINAPI rp_cleartype(struct dw_obj* o) {
  (void)o;
  return 0.0f; /* tons de cinza */
}
static int WINAPI rp_geometry(struct dw_obj* o) {
  (void)o;
  return 0; /* DWRITE_PIXEL_GEOMETRY_FLAT */
}
static int WINAPI rp_mode(struct dw_obj* o) {
  (void)o;
  return 5;
}
static const void* const rp_vtbl[] = {rp_qi, o_addref, o_release, rp_gamma, rp_contrast, rp_cleartype,
                                      rp_geometry, rp_mode};

/* ---------- IDWriteGlyphRunAnalysis ---------- */
struct dw_analysis {
  struct dw_obj o;
  int aliased;          /* DWRITE_RENDERING_MODE_ALIASED: textura 1x1 */
  int gray;             /* IDWriteFactory2, antialias GRAYSCALE: textura 1x1 com tons */
  int l, t, r, b;
  uint8_t* alpha;       /* cobertura w*h */
};
QI_SIMPLE(ga_qi, 0x7d97dbf7, 0xe085, 0x42d4, 0x81, 0xe3, 0x6a, 0x88, 0x3b, 0xde, 0xd1, 0x18)
static DWORD WINAPI ga_release(struct dw_analysis* a) {
  int r = __sync_sub_and_fetch(&a->o.refs, 1);
  if (r == 0) {
    k_free(a->alpha);
    k_free(a);
  }
  return (DWORD)(r < 0 ? 0 : r);
}
struct rect_ {
  int32_t l, t, r, b;
};
static HRESULT_ WINAPI ga_bounds(struct dw_analysis* a, int type, struct rect_* rc) {
  /* tipo 0 = ALIASED_1x1, 1 = CLEARTYPE_3x1: so o do modo pedido tem pixels */
  if ((type == 0) != (a->aliased || a->gray) || a->r <= a->l) {
    rc->l = rc->t = rc->r = rc->b = 0;
    return S_OK_;
  }
  rc->l = a->l;
  rc->t = a->t;
  rc->r = a->r;
  rc->b = a->b;
  return S_OK_;
}
static HRESULT_ WINAPI ga_texture(struct dw_analysis* a, int type, const struct rect_* rc, uint8_t* buf, DWORD size) {
  int bpp = type == 1 ? 3 : 1, x, y, w = rc->r - rc->l, h = rc->b - rc->t;
  if (w <= 0 || h <= 0) return S_OK_;
  if (size < (DWORD)(w * h * bpp)) return E_INVALIDARG_;
  memset(buf, 0, (size_t)(w * h * bpp));
  if ((type == 0) != (a->aliased || a->gray) || !a->alpha) return S_OK_;
  for (y = 0; y < h; y++) {
    int sy = rc->t + y;
    if (sy < a->t || sy >= a->b) continue;
    for (x = 0; x < w; x++) {
      int sx = rc->l + x, k;
      uint8_t v;
      if (sx < a->l || sx >= a->r) continue;
      v = a->alpha[(sy - a->t) * (a->r - a->l) + (sx - a->l)];
      if (a->aliased) v = v >= 128 ? 255 : 0;
      for (k = 0; k < bpp; k++) buf[(y * w + x) * bpp + k] = v;
    }
  }
  return S_OK_;
}
static HRESULT_ WINAPI ga_blend(struct dw_analysis* a, void* params, float* gamma, float* contrast, float* ct) {
  (void)a;
  (void)params;
  *gamma = 1.8f;
  *contrast = 0.5f;
  *ct = 0.0f;
  return S_OK_;
}
static const void* const ga_vtbl[] = {ga_qi, o_addref, ga_release, ga_bounds, ga_texture, ga_blend};

struct glyph_run {
  struct dw_face* face;
  float em;
  DWORD n;
  const uint16_t* idx;
  const float* advs;
  const struct glyph_offset* offs;
  BOOL sideways;
  DWORD bidi;
};
struct dw_matrix {
  float m11, m12, m21, m22, dx, dy;
};
static HRESULT_ glyph_analysis(const struct glyph_run* run, float ppd, const struct dw_matrix* xf, int mode,
                               float bx, float by, struct dw_analysis** out) {
  struct dw_analysis* a;
  struct ttf_raster rs;
  const struct ttfont* t;
  float pen = 0, sc;
  DWORD i;
  if (!run || !run->face || !out) return E_INVALIDARG_;
  t = &F.fonts[run->face->font].tt;
  a = (struct dw_analysis*)o_new(ga_vtbl, sizeof(struct dw_analysis));
  if (!a) return E_OUTOFMEMORY_;
  a->aliased = mode == 1;
  sc = run->em / (float)t->upem;
  ttf_raster_begin(&rs);
  for (i = 0; i < run->n; i++) {
    float gx = pen + (run->offs ? run->offs[i].adv : 0), gy = run->offs ? -run->offs[i].asc : 0;
    float m11 = xf ? xf->m11 : 1, m12 = xf ? xf->m12 : 0, m21 = xf ? xf->m21 : 0, m22 = xf ? xf->m22 : 1;
    float tx = xf ? xf->dx : 0, ty = xf ? xf->dy : 0;
    float m[6];
    /* glifo (unidades, y para cima) -> run (DIPs, y para baixo) -> transformacao -> pixels */
    m[0] = sc * m11 * ppd;
    m[1] = sc * m12 * ppd;
    m[2] = -sc * m21 * ppd;
    m[3] = -sc * m22 * ppd;
    m[4] = ((gx * m11 + gy * m21) + tx + bx) * ppd;
    m[5] = ((gx * m12 + gy * m22) + ty + by) * ppd;
    ttf_raster_add(&rs, t, run->idx[i], m);
    pen += run->advs ? run->advs[i] : (float)ttf_advance(t, run->idx[i]) * sc;
  }
  ttf_raster_bounds(&rs, &a->l, &a->t, &a->r, &a->b);
  if (a->r > a->l && a->b > a->t && (a->r - a->l) * (a->b - a->t) < 4096 * 4096) {
    a->alpha = (uint8_t*)wzalloc((size_t)((a->r - a->l) * (a->b - a->t)));
    if (a->alpha) ttf_raster_fill(&rs, a->l, a->t, a->r, a->b, a->alpha, a->r - a->l, 1);
  } else {
    a->l = a->t = a->r = a->b = 0;
  }
  ttf_raster_end(&rs);
  dw_note("DirectWrite textura: ", run->face->font, run->em, run->idx, run->n, !(a->r > a->l));
  *out = a;
  return S_OK_;
}

/* ---------- IDWriteFactory ---------- */
static HRESULT_ WINAPI fa_qi(struct dw_obj* o, const void* iid, void** out) {
  static const uint8_t f4[8] = {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}; /* IDWriteFactory */
  static const uint8_t f1[8] = {0xa1, 0x6e, 0x04, 0x86, 0x30, 0x7e, 0x60, 0x6a}; /* IDWriteFactory1 */
  static const uint8_t f2[8] = {0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec}; /* IDWriteFactory2 */
  static const uint8_t f3[8] = {0x87, 0xfc, 0xfe, 0x67, 0x55, 0x6a, 0x3b, 0x65}; /* IDWriteFactory3 */
  if (IS_UNK(iid) || guid_is(iid, 0xb859ee5a, 0xd838, 0x4b5b, f4) || guid_is(iid, 0x30572f99, 0xdac6, 0x41db, f1) ||
      guid_is(iid, 0x0439fc60, 0xca44, 0x4994, f2) || guid_is(iid, 0x9a1b41c3, 0xd3bb, 0x466a, f3)) {
    *out = o;
    return S_OK_;
  }
  dw_qi_miss("DirectWrite QI recusado (fabrica) iid ", iid);
  *out = 0;
  return E_NOINTERFACE_; /* IDWriteFactory1..7 ainda nao */
}
static HRESULT_ WINAPI fa_syscoll(struct dw_obj* o, void** out, BOOL check) {
  (void)o;
  (void)check;
  fonts_load();
  *out = collection_get();
  return S_OK_;
}
static HRESULT_ WINAPI fa_notimpl(void) {
  if (wine_debug >= 2) wwait_note("DirectWrite: metodo da fabrica sem implementacao, chamado de", __builtin_return_address(0), 0);
  return E_NOTIMPL_;
}
static HRESULT_ WINAPI fa_ok(void) { return S_OK_; }
static HRESULT_ WINAPI fa_fileref(struct dw_obj* o, const wchar16* path, const void* t, struct dw_file** out) {
  int i;
  (void)o;
  (void)t;
  fonts_load();
  for (i = 0; i < F.nfiles; i++)
    if (wcs_ieq(F.files[i].path, path)) {
      *out = file_new(&F.files[i]);
      return *out ? S_OK_ : E_OUTOFMEMORY_;
    }
  *out = 0;
  return DWRITE_E_FILENOTFOUND; /* so as fontes do sistema */
}
static int foreign_load(void* ld, const void* key, DWORD ksz);
/* Loader do programa (o proxy de fontes do Chromium com os handles que o
 * browser mandou, ou o stream de uma fonte da web do Skia): le o arquivo
 * agora pelo stream dele e devolve um arquivo nosso. */
static HRESULT_ WINAPI fa_customref(struct dw_obj* o, const void* key, DWORD size, void* ld, struct dw_file** out) {
  int fi;
  (void)o;
  if (!out) return E_INVALIDARG_;
  *out = 0;
  if (!ld) return E_INVALIDARG_;
  fonts_load();
  fi = foreign_load(ld, key, size);
  if (fi < 0) {
    if (wine_debug >= 2) wlog("DirectWrite: CreateCustomFontFileReference sem dados do loader", 0);
    return DWRITE_E_FILENOTFOUND;
  }
  *out = file_new(&F.files[fi]);
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT_ WINAPI fa_face(struct dw_obj* o, int type, DWORD nfiles, struct dw_file* const* files, DWORD idx,
                               int sims, struct dw_face** out) {
  int i;
  (void)o;
  (void)type;
  (void)sims;
  *out = 0;
  if (!nfiles || !files || !files[0]) return E_INVALIDARG_;
  for (i = 0; i < F.nfonts; i++)
    if (F.fonts[i].file == &F.files[files[0]->idx] && F.fonts[i].tt.index == idx) {
      *out = face_new(i);
      return *out ? S_OK_ : E_OUTOFMEMORY_;
    }
  return E_INVALIDARG_;
}
static HRESULT_ WINAPI fa_params(struct dw_obj* o, void** out) {
  (void)o;
  *out = o_new(rp_vtbl, sizeof(struct dw_obj));
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT_ WINAPI fa_monparams(struct dw_obj* o, HANDLE mon, void** out) {
  (void)mon;
  return fa_params(o, out);
}
static HRESULT_ WINAPI fa_customparams(struct dw_obj* o, float g, float c, float ct, int geo, int mode, void** out) {
  (void)g; (void)c; (void)ct; (void)geo; (void)mode;
  return fa_params(o, out);
}
static HRESULT_ WINAPI fa_analysis(struct dw_obj* o, const struct glyph_run* run, float ppd,
                                   const struct dw_matrix* xf, int mode, int measuring, float bx, float by,
                                   struct dw_analysis** out) {
  (void)o;
  (void)measuring;
  return glyph_analysis(run, ppd, xf, mode, bx, by, out);
}
/* ---------- IDWriteFactory1/2 ----------
 * O browser do Chromium (dwrite_font_proxy_impl_win.cc) da CHECK(factory2_)
 * quando um renderer pede fonte de reserva para um caractere: sem a v2 o
 * browser abortava (int3) e o Claude fechava. O renderer usa a v2 tambem para
 * rasterizar (CreateGlyphRunAnalysis com antialias em tons de cinza). */
static const void* const fb_vtbl[];
static struct dw_obj fallback = {fb_vtbl, 1};
static HRESULT_ WINAPI fb_qi(struct dw_obj* o, const void* iid, void** out) {
  static const uint8_t d4[8] = {0xb0, 0x5c, 0xf2, 0x24, 0x71, 0x3c, 0xc0, 0xff}; /* IDWriteFontFallback */
  if (IS_UNK(iid) || guid_is(iid, 0xefa008f9, 0xf7a1, 0x48bf, d4)) {
    *out = o;
    return S_OK_;
  }
  dw_qi_miss("DirectWrite QI recusado (fallback) iid ", iid);
  *out = 0;
  return E_NOINTERFACE_;
}
static int font_has(int font, uint32_t cp) { return ttf_glyph(&F.fonts[font].tt, cp) != 0; }
/* MapCharacters: a primeira fonte (a da familia pedida antes) que tem o
 * caractere em pos; mapped_len = quantas unidades UTF-16 seguidas ela cobre. */
static HRESULT_ WINAPI fb_map(struct dw_obj* o, void* src, DWORD pos, DWORD len, void* basecoll,
                              const wchar16* basefam, int weight, int style, int stretch, DWORD* mapped_len,
                              struct dw_font** font, float* scale) {
  typedef HRESULT_(WINAPI * gtp_fn)(void* self, DWORD pos, const wchar16** text, DWORD* n);
  const wchar16* t = 0;
  DWORD n = 0, i = 0;
  int best = -1, k, fam;
  uint32_t cp;
  (void)o; (void)basecoll; (void)style; (void)stretch;
  if (!mapped_len || !font || !scale) return E_INVALIDARG_;
  *font = 0;
  *scale = 1.0f;
  *mapped_len = len;
  if (!src || !len) return S_OK_;
  fonts_load();
  if (((gtp_fn)VT(src, 3))(src, pos, &t, &n) < 0 || !t || !n) return S_OK_;
  if (n > len) n = len;
  cp = t[0];
  if (cp >= 0xD800 && cp < 0xDC00 && n > 1 && t[1] >= 0xDC00 && t[1] < 0xE000)
    cp = 0x10000u + ((cp - 0xD800u) << 10) + (t[1] - 0xDC00u);
  fam = basefam ? fam_find(basefam) : -1;
  if (fam >= 0)
    for (k = 0; k < F.fams[fam].nfonts; k++)
      if (font_has(F.fams[fam].fonts[k], cp)) {
        best = F.fams[fam].fonts[k];
        break;
      }
  for (k = 0; best < 0 && k < F.nfonts; k++)
    if (font_has(k, cp)) best = k;
  /* do mesmo peso, se a familia achada tem varios */
  if (best >= 0) {
    struct dfamily* df = &F.fams[F.fonts[best].family];
    int bs = 1 << 30;
    for (k = 0; k < df->nfonts; k++) {
      int f = df->fonts[k], d = F.fonts[f].tt.weight - weight;
      if (d < 0) d = -d;
      if (font_has(f, cp) && d < bs) {
        bs = d;
        best = f;
      }
    }
  }
  /* quantas unidades seguidas a fonte (ou a falta dela) cobre */
  while (i < n) {
    uint32_t c = t[i];
    DWORD w = 1;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000) {
      c = 0x10000u + ((c - 0xD800u) << 10) + (t[i + 1] - 0xDC00u);
      w = 2;
    }
    if (i > 0 && (best >= 0 ? !font_has(best, c) : 0)) break;
    if (best < 0 && i > 0) break; /* sem fonte: um caractere por vez */
    i += w;
  }
  *mapped_len = i ? i : 1;
  if (best >= 0) *font = font_new(best);
  return S_OK_;
}
static const void* const fb_vtbl[] = {fb_qi, o_addref, o_release_static, fb_map};

/* IDWriteFontFallbackBuilder: as regras do programa sao aceitas e o fallback
 * e o do sistema (o browser do Chromium monta um assim) */
static HRESULT_ WINAPI bb_qi(struct dw_obj* o, const void* iid, void** out) {
  static const uint8_t d4[8] = {0xb8, 0x49, 0x8b, 0xe8, 0xb7, 0x3e, 0x14, 0xde};
  if (IS_UNK(iid) || guid_is(iid, 0xfd882d06, 0x8aba, 0x4fb8, d4)) {
    o_addref(o);
    *out = o;
    return S_OK_;
  }
  *out = 0;
  return E_NOINTERFACE_;
}
static HRESULT_ WINAPI bb_create(struct dw_obj* o, void** out) {
  (void)o;
  *out = &fallback;
  return S_OK_;
}
static const void* const bb_vtbl[] = {bb_qi, o_addref, o_release, fa_ok /* AddMapping */, fa_ok /* AddMappings */,
                                      bb_create};

/* IDWriteNumberSubstitution (so IUnknown). O Skia cria um antes de pedir
 * fonte de reserva (a fonte de texto dele devolve o objeto); com E_NOTIMPL a
 * busca falhava e o browser tentava de novo recursivamente ate estourar a
 * pilha (tudo em ring 0: reinicio do PC). */
QI_SIMPLE(ns_qi, 0x14885cc9, 0xbab0, 0x4f90, 0xb6, 0xed, 0x5c, 0x36, 0x6a, 0x2c, 0xd0, 0x3d)
static const void* const ns_vtbl[] = {ns_qi, o_addref, o_release};
static HRESULT_ WINAPI fa_numsubst(struct dw_obj* o, int method, const wchar16* locale, BOOL ignore, void** out) {
  (void)o; (void)method; (void)locale; (void)ignore;
  if (!out) return E_INVALIDARG_;
  *out = o_new(ns_vtbl, sizeof(struct dw_obj));
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}

static HRESULT_ WINAPI fa_eudc(struct dw_obj* o, void** out, BOOL check) { return fa_syscoll(o, out, check); }
static HRESULT_ WINAPI fa_customparams1(struct dw_obj* o, float g, float c, float ge, float ct, int geo, int mode,
                                        void** out) {
  (void)g; (void)c; (void)ge; (void)ct; (void)geo; (void)mode;
  return fa_params(o, out);
}
static HRESULT_ WINAPI fa_sysfallback(struct dw_obj* o, void** out) {
  (void)o;
  if (!out) return E_INVALIDARG_;
  fonts_load();
  *out = &fallback;
  return S_OK_;
}
static HRESULT_ WINAPI fa_fbbuilder(struct dw_obj* o, void** out) {
  (void)o;
  if (!out) return E_INVALIDARG_;
  *out = o_new(bb_vtbl, sizeof(struct dw_obj));
  return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT_ WINAPI fa_colorrun(void) { return (long)0x8898500C; } /* DWRITE_E_NOCOLOR: sem fontes coloridas */
static HRESULT_ WINAPI fa_customparams2(struct dw_obj* o, float g, float c, float ge, float ct, int geo, int mode,
                                        int grid, void** out) {
  (void)grid;
  return fa_customparams1(o, g, c, ge, ct, geo, mode, out);
}
static HRESULT_ WINAPI fa_analysis2(struct dw_obj* o, const struct glyph_run* run, const struct dw_matrix* xf,
                                    int mode, int measuring, int grid, int aa, float bx, float by,
                                    struct dw_analysis** out) {
  HRESULT_ r;
  (void)o; (void)measuring; (void)grid;
  r = glyph_analysis(run, 1.0f, xf, mode, bx, by, out);
  if (r == S_OK_ && *out && aa == 1 /* DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE */ && mode != 1) (*out)->gray = 1;
  return r;
}

static const void* const fa_vtbl[] = {
    fa_qi,       o_addref,        o_release_static, fa_syscoll,   fa_customcoll,
    fa_ok /* RegisterFontCollectionLoader */, fa_ok /* UnregisterFontCollectionLoader */, fa_fileref,
    fa_customref, fa_face,        fa_params,        fa_monparams, fa_customparams,
    fa_ok /* RegisterFontFileLoader */, fa_ok /* UnregisterFontFileLoader */,
    fa_notimpl /* CreateTextFormat */, fa_notimpl /* CreateTypography */, fa_notimpl /* GetGdiInterop */,
    fa_notimpl /* CreateTextLayout */, fa_notimpl /* CreateGdiCompatibleTextLayout */,
    fa_notimpl /* CreateEllipsisTrimmingSign */, fa_notimpl /* CreateTextAnalyzer */,
    fa_numsubst, fa_analysis,
    /* IDWriteFactory1 */ fa_eudc, fa_customparams1,
    /* IDWriteFactory2 */ fa_sysfallback, fa_fbbuilder, fa_colorrun, fa_customparams2, fa_analysis2,
    /* IDWriteFactory3: o browser do Chromium da CHECK(factory3_) ao iniciar o
     * proxy de fontes; o que ele usa no caminho normal e o GetSystemFontCollection
     * da v1. Conjuntos de fontes (MatchUniqueFont, so para local()) aceitam
     * falhar. Sem IDWriteFontCollection1: a nossa colecao so tem a vtable v1. */
    fa_analysis2 /* CreateGlyphRunAnalysis (RENDERING_MODE1) */, fa_customparams2 /* CreateCustomRenderingParams */,
    fa_notimpl /* CreateFontFaceReference (arquivo) */, fa_notimpl /* CreateFontFaceReference (caminho) */,
    fa_notimpl /* GetSystemFontSet */, fa_notimpl /* CreateFontSetBuilder */,
    fa_notimpl /* CreateFontCollectionFromFontSet */, fa_notimpl /* GetSystemFontCollection (v3) */,
    fa_notimpl /* GetFontDownloadQueue */};
static struct dw_obj factory = {fa_vtbl, 1};

static WINAPI long DWriteCreateFactory(int type, const void* iid, void** out) {
  (void)type;
  if (!out) return E_INVALIDARG_;
  fonts_load();
  if (!F.nfams) {
    *out = 0;
    wlog("DWriteCreateFactory: sem fontes", 0);
    return E_NOTIMPL_;
  }
  return fa_qi(&factory, iid, out);
}

#define S(n) {#n, (void*)n}
const struct wine_sym dwrite_syms[] = {S(DWriteCreateFactory), {0, 0}};
