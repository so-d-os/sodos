/*
 * gdi.c - GDI32: DCs, pinceis, canetas, fontes, bitmaps e desenho em
 * superficies de 32 bits (a das janelas e as dos bitmaps).
 *
 * DC de janela: origem e recorte vem da posicao da janela dentro da sua
 * janela de topo (dc_prepare). DC de memoria: desenha no bitmap selecionado.
 */
#include <so.h>
#include <string.h>
#include "gui_int.h"

/* ---------- cores do sistema (tema claro do Windows 10) ---------- */
DWORD sys_color(int i) {
  static const DWORD c[31] = {
      0xC8C8C8, /* 0 SCROLLBAR */       0x000000, /* 1 BACKGROUND */  0xD1B499, /* 2 ACTIVECAPTION */
      0xDBCDBF, /* 3 INACTIVECAPTION */ 0xF0F0F0, /* 4 MENU */        0xFFFFFF, /* 5 WINDOW */
      0x646464, /* 6 WINDOWFRAME */     0x000000, /* 7 MENUTEXT */    0x000000, /* 8 WINDOWTEXT */
      0x000000, /* 9 CAPTIONTEXT */     0xB4B4B4, /* 10 ACTIVEBORDER */ 0xFCF7F4, /* 11 INACTIVEBORDER */
      0xABABAB, /* 12 APPWORKSPACE */   0xD77800, /* 13 HIGHLIGHT */  0xFFFFFF, /* 14 HIGHLIGHTTEXT */
      0xF0F0F0, /* 15 BTNFACE */        0xA0A0A0, /* 16 BTNSHADOW */  0x6D6D6D, /* 17 GRAYTEXT */
      0x000000, /* 18 BTNTEXT */        0x000000, /* 19 INACTIVECAPTIONTEXT */ 0xFFFFFF, /* 20 BTNHIGHLIGHT */
      0x696969, /* 21 3DDKSHADOW */     0xE3E3E3, /* 22 3DLIGHT */    0x000000, /* 23 INFOTEXT */
      0xE1FFFF, /* 24 INFOBK */         0x000000, /* 25 */            0xCC6600, /* 26 HOTLIGHT */
      0xEAD1B9, /* 27 GRADIENTACTIVECAPTION */ 0xF2E4D7, /* 28 GRADIENTINACTIVECAPTION */
      0xFF9933, /* 29 MENUHILIGHT */    0xF0F0F0, /* 30 MENUBAR */
  };
  return (i >= 0 && i < 31) ? c[i] : 0;
}

struct wgdi* gdi_of(HANDLE h) {
  struct wgdi* o = (struct wgdi*)h;
  return wptr_ok(o) && o->magic == GDI_MAGIC ? o : 0;
}
struct wdc* dc_of(HANDLE h) {
  struct wdc* d = (struct wdc*)h;
  return wptr_ok(d) && d->magic == DC_MAGIC ? d : 0;
}

/* Cor de um pincel; aceita (HBRUSH)(COLOR_x + 1). */
DWORD brush_color(HANDLE brush, int* null) {
  struct wgdi* b = gdi_of(brush);
  if (null) *null = 0;
  if (b) {
    if (null) *null = b->null;
    return b->color;
  }
  if ((uintptr_t)brush > 0 && (uintptr_t)brush <= 31) return sys_color((int)(uintptr_t)brush - 1);
  if (null) *null = 1;
  return 0;
}

/* ---------- superficies ---------- */
int surf_alloc(struct surf* s, int w, int h, int rgba) {
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  s->px = (uint8_t*)k_malloc((unsigned)w * (unsigned)h * 4u);
  if (!s->px) return 0;
  s->w = w;
  s->h = h;
  s->rgba = rgba;
  s->bottom_up = 0;
  memset(s->px, rgba ? 255 : 0, (size_t)w * (size_t)h * 4u);
  return 1;
}
static inline uint8_t* surf_row(const struct surf* s, int y) {
  return s->px + (size_t)(s->bottom_up ? s->h - 1 - y : y) * (size_t)s->w * 4u;
}
static inline void put_px(const struct surf* s, uint8_t* p, DWORD c) {
  uint8_t r = (uint8_t)c, g = (uint8_t)(c >> 8), b = (uint8_t)(c >> 16);
  if (s->rgba) {
    p[0] = r;
    p[2] = b;
  } else {
    p[0] = b;
    p[2] = r;
  }
  p[1] = g;
  p[3] = 255;
}
void surf_fill(struct surf* s, int l, int t, int r, int b, DWORD c) {
  int x, y;
  if (!s || !s->px) return;
  if (l < 0) l = 0;
  if (t < 0) t = 0;
  if (r > s->w) r = s->w;
  if (b > s->h) b = s->h;
  for (y = t; y < b; y++) {
    uint8_t* row = surf_row(s, y);
    for (x = l; x < r; x++) put_px(s, row + x * 4, c);
  }
}

/* Copia com escala (vizinho mais proximo), recortando em clip (no destino). */
void surf_blit(struct surf* d, int dx, int dy, int dw, int dh, const struct surf* s, int sx, int sy, int sw, int sh,
               const struct rect* clip) {
  int x, y, cl = 0, ct = 0, cr = d->w, cb = d->h;
  if (!d->px || !s->px || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
  if (clip) {
    if (clip->left > cl) cl = clip->left;
    if (clip->top > ct) ct = clip->top;
    if (clip->right < cr) cr = clip->right;
    if (clip->bottom < cb) cb = clip->bottom;
  }
  for (y = 0; y < dh; y++) {
    int ty = dy + y, fy = sy + (int)((int64_t)y * sh / dh);
    uint8_t* drow;
    const uint8_t* srow;
    if (ty < ct || ty >= cb || fy < 0 || fy >= s->h) continue;
    drow = surf_row(d, ty);
    srow = surf_row(s, fy);
    for (x = 0; x < dw; x++) {
      int tx = dx + x, fx = sx + (int)((int64_t)x * sw / dw);
      const uint8_t* sp;
      uint8_t* dp;
      if (tx < cl || tx >= cr || fx < 0 || fx >= s->w) continue;
      sp = srow + fx * 4;
      dp = drow + tx * 4;
      if (d->rgba == s->rgba) {
        dp[0] = sp[0];
        dp[2] = sp[2];
      } else {
        dp[0] = sp[2];
        dp[2] = sp[0];
      }
      dp[1] = sp[1];
      dp[3] = 255;
    }
  }
}

/* ---------- DCs ---------- */
struct surf* dc_surf(struct wdc* d) {
  if (d->win) {
    struct wwin* t = win_top(d->win);
    return t && t->s.px ? &t->s : 0;
  }
  return d->bitmap && d->bitmap->s.px ? &d->bitmap->s : 0;
}

void dc_prepare(struct wdc* d, struct wwin* w) {
  struct wwin* t = win_top(w);
  struct wwin* p;
  int x = 0, y = 0;
  win_abs_client(w, &x, &y);
  d->ox = x;
  d->oy = y;
  d->clip.left = x;
  d->clip.top = y;
  d->clip.right = x + w->w;
  d->clip.bottom = y + w->h;
  /* recorta pelas maes */
  for (p = w->parent; p; p = p->parent) {
    int px = 0, py = 0;
    win_abs_client(p, &px, &py);
    if (d->clip.left < px) d->clip.left = px;
    if (d->clip.top < py) d->clip.top = py;
    if (d->clip.right > px + p->w) d->clip.right = px + p->w;
    if (d->clip.bottom > py + p->h) d->clip.bottom = py + p->h;
  }
  if (t && t->s.px) {
    if (d->clip.right > t->s.w) d->clip.right = t->s.w;
    if (d->clip.bottom > t->s.h) d->clip.bottom = t->s.h;
  }
}

static void dc_clipped(struct wdc* d, int* l, int* t, int* r, int* b) {
  *l += d->ox;
  *r += d->ox;
  *t += d->oy;
  *b += d->oy;
  if (d->win) {
    if (*l < d->clip.left) *l = d->clip.left;
    if (*t < d->clip.top) *t = d->clip.top;
    if (*r > d->clip.right) *r = d->clip.right;
    if (*b > d->clip.bottom) *b = d->clip.bottom;
  }
}
void dc_fill(struct wdc* d, int l, int t, int r, int b, DWORD c) {
  struct surf* s = dc_surf(d);
  if (!s) return;
  dc_clipped(d, &l, &t, &r, &b);
  surf_fill(s, l, t, r, b, c);
}
void dc_frame(struct wdc* d, int l, int t, int r, int b, DWORD c) {
  dc_fill(d, l, t, r, t + 1, c);
  dc_fill(d, l, b - 1, r, b, c);
  dc_fill(d, l, t, l + 1, b, c);
  dc_fill(d, r - 1, t, r, b, c);
}
void dc_line(struct wdc* d, int x0, int y0, int x1, int y1, DWORD c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy, n = 0;
  while (n++ < 10000) {
    dc_fill(d, x0, y0, x0 + 1, y0 + 1, c);
    if (x0 == x1 && y0 == y1) break;
    {
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x0 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y0 += sy;
      }
    }
  }
}
static struct rect dc_clip_abs(struct wdc* d) {
  struct rect r;
  struct surf* s = dc_surf(d);
  if (d->win) return d->clip;
  r.left = r.top = 0;
  r.right = s ? s->w : 0;
  r.bottom = s ? s->h : 0;
  return r;
}

static struct wdc* dc_new(void) {
  struct wgui* g = gui_get();
  struct wdc* d = (struct wdc*)wzalloc(sizeof(*d));
  if (!d || !g) return 0;
  d->magic = DC_MAGIC;
  d->brush = &g->stock[0];
  d->pen = &g->stock[7];
  d->font = &g->stock[17];
  d->bk_color = 0xFFFFFF;
  d->bk_mode = 2;
  return d;
}

static WINAPI HANDLE GetDC(HANDLE h) {
  struct wwin* w = win_of(h);
  if (w) {
    if (!w->dc.magic) {
      w->dc.magic = DC_MAGIC;
      w->dc.win = w;
    }
    if (!w->dc.brush) w->dc.brush = &gui_get()->stock[0];
    if (!w->dc.pen) w->dc.pen = &gui_get()->stock[7];
    if (!w->dc.font) w->dc.font = &gui_get()->stock[17];
    dc_prepare(&w->dc, w);
    return &w->dc;
  }
  {
    /* janela de outro processo (processo GPU do Chromium -> janela do
     * browser): DC proprio, que desenha na superficie do dono */
    struct wgui* xg = 0;
    struct wwin* xw = h ? win_any(h, &xg) : 0;
    if (xw && xg) {
      struct wdc* d = dc_new();
      static int said;
      if (!d) return 0;
      d->win = xw;
      d->xgui = xg;
      dc_prepare(d, xw);
      if (!said && wine_debug) {
        said = 1;
        wlogx("GetDC: janela de outro processo ", (uint64_t)(uintptr_t)h);
      }
      return d;
    }
  }
  /* DC da tela: referencia para CreateCompatibleDC/GetDeviceCaps */
  return dc_new();
}
static WINAPI HANDLE GetWindowDC(HANDLE h) { return GetDC(h); }
/* CreateDC("DISPLAY"/\\.\DISPLAY1): o Chromium pede para medir a tela
 * (GetDeviceCaps); um DC da tela como o GetDC(0). Antes devolvia 0. */
static WINAPI HANDLE CreateDCW(const wchar16* drv, const wchar16* dev, const wchar16* out, const void* init) {
  (void)drv; (void)dev; (void)out; (void)init;
  return dc_new();
}
static WINAPI HANDLE CreateDCA(const char* drv, const char* dev, const char* out, const void* init) {
  (void)drv; (void)dev; (void)out; (void)init;
  return dc_new();
}
static WINAPI int ReleaseDC(HANDLE h, HANDLE dc) {
  struct wwin* w = win_of(h);
  struct wdc* d = dc_of(dc);
  if (w && d == &w->dc) {
    if (!d->painting) win_invalidate(0); /* mostra o que foi desenhado */
    return 1;
  }
  if (d && d->xgui) {
    /* desenho na janela de outro processo: o dono apresenta (o laco de
     * mensagens dele olha present_pending) */
    static volatile int nx;
    int k = __sync_add_and_fetch(&nx, 1);
    if (wine_debug >= 2 && (k <= 5 || k % 50 == 0)) wlogx("ReleaseDC de janela alheia, quadros ", (uint64_t)k);
    d->xgui->present_pending = 1;
    d->magic = 0;
    k_free(d);
    wsignal_global();
    return 1;
  }
  if (d && !d->win && !d->bitmap) {
    d->magic = 0;
    k_free(d);
  }
  return 1;
}
static WINAPI HANDLE CreateCompatibleDC(HANDLE ref) {
  (void)ref;
  return dc_new();
}
static WINAPI BOOL DeleteDC(HANDLE dc) {
  struct wdc* d = dc_of(dc);
  if (!d || d->win) return 0;
  d->magic = 0;
  k_free(d);
  return 1;
}
static WINAPI int SaveDC(HANDLE dc) {
  (void)dc;
  return 1;
}
static WINAPI BOOL RestoreDC(HANDLE dc, int n) {
  (void)dc;
  (void)n;
  return 1;
}

/* ---------- objetos ---------- */
static struct wgdi* obj_new(int kind) {
  struct wgdi* o = (struct wgdi*)wzalloc(sizeof(*o));
  if (!o) return 0;
  o->magic = GDI_MAGIC;
  o->kind = kind;
  return o;
}
static WINAPI HANDLE CreateSolidBrush(DWORD color) {
  struct wgdi* b = obj_new(G_BRUSH);
  if (b) b->color = color & 0xFFFFFF;
  return b;
}
static WINAPI HANDLE CreateBrushIndirect(const DWORD* lb) {
  struct wgdi* b = obj_new(G_BRUSH);
  if (b) {
    b->color = lb[1] & 0xFFFFFF;
    b->null = lb[0] == 1; /* BS_NULL */
  }
  return b;
}
static WINAPI HANDLE GetSysColorBrush(int i) {
  struct wgui* g = gui_get();
  if (!g || i < 0 || i >= 31) return 0;
  g->sysbrush[i].magic = GDI_MAGIC;
  g->sysbrush[i].kind = G_BRUSH;
  g->sysbrush[i].stock = 1;
  g->sysbrush[i].color = sys_color(i);
  return &g->sysbrush[i];
}
static WINAPI DWORD GetSysColor(int i) { return sys_color(i); }
static WINAPI HANDLE CreatePen(int style, int width, DWORD color) {
  struct wgdi* b = obj_new(G_PEN);
  (void)width;
  if (b) {
    b->color = color & 0xFFFFFF;
    b->null = style == 5; /* PS_NULL */
  }
  return b;
}
static void font_face_set(struct wgdi* f, const wchar16* face) {
  int k;
  for (k = 0; face && k < 31 && face[k]; k++) f->face[k] = face[k];
  f->face[k] = 0;
}
static WINAPI HANDLE CreateFontIndirectW(const int32_t* lf) {
  struct wgdi* f = obj_new(G_FONT);
  if (f) {
    f->height = lf[0] < 0 ? -lf[0] : lf[0];
    f->bold = lf[4] >= 600;
    font_face_set(f, (const wchar16*)((const uint8_t*)lf + 28)); /* lfFaceName */
  }
  return f;
}
static WINAPI HANDLE CreateFontW(int h, int w, int e, int o, int wt, DWORD i, DWORD u, DWORD so, DWORD cs, DWORD op,
                                 DWORD cp, DWORD q, DWORD pf, const wchar16* face) {
  struct wgdi* f = obj_new(G_FONT);
  (void)w; (void)e; (void)o; (void)i; (void)u; (void)so; (void)cs; (void)op; (void)cp; (void)q; (void)pf;
  if (f) {
    f->height = h < 0 ? -h : h;
    f->bold = wt >= 600;
    font_face_set(f, face);
  }
  return f;
}
HANDLE WINAPI wGetStockObject(int i) {
  struct wgui* g = gui_get();
  if (!g || i < 0 || i >= 20) return 0;
  return &g->stock[i];
}
HANDLE WINAPI wSelectObject(HANDLE dc, HANDLE obj) {
  struct wdc* d = dc_of(dc);
  struct wgdi* o = gdi_of(obj);
  struct wgdi* old = 0;
  struct wgui* g = gui_get();
  if (!d || !o) return 0;
  switch (o->kind) {
    case G_BRUSH:
      old = d->brush;
      d->brush = o;
      break;
    case G_PEN:
      old = d->pen;
      d->pen = o;
      break;
    case G_BITMAP:
      if (d->win) return 0;
      old = d->bitmap ? d->bitmap : &g->stock[19];
      d->bitmap = o == &g->stock[19] ? 0 : o;
      break;
    case G_FONT:
      old = d->font ? d->font : &g->stock[17];
      d->font = o;
      break;
  }
  return old;
}
static WINAPI BOOL DeleteObject(HANDLE obj) {
  struct wgdi* o = gdi_of(obj);
  if (!o || o->stock) return o != 0;
  if (o->s.px && !o->s.foreign) k_free(o->s.px);
  o->magic = 0;
  k_free(o);
  return 1;
}
struct bitmap_info {
  int32_t type, width, height, width_bytes;
  uint16_t planes, bpp;
  void* bits;
};
static WINAPI int GetObjectW(HANDLE h, int n, void* out) {
  struct wgdi* o = gdi_of(h);
  if (!o) return 0;
  if (o->kind == G_BITMAP) {
    struct bitmap_info* b = (struct bitmap_info*)out;
    if (!out) return (int)sizeof(*b);
    if (n < (int)sizeof(*b)) return 0;
    memset(b, 0, sizeof(*b));
    b->width = o->s.w;
    b->height = o->s.h;
    b->width_bytes = o->s.w * 4;
    b->planes = 1;
    b->bpp = 32;
    b->bits = o->s.px;
    return (int)sizeof(*b);
  }
  if (o->kind == G_FONT && out && n >= 92) {
    memset(out, 0, (size_t)n);
    ((int32_t*)out)[0] = -(o->height ? o->height : 12);
    ((int32_t*)out)[4] = o->bold ? 700 : 400;
    ((uint8_t*)out)[23] = 1; /* DEFAULT_CHARSET */
    memcpy((uint8_t*)out + 28, o->face, sizeof(o->face)); /* lfFaceName (gfx::win::GetSystemFont) */
    return 92;
  }
  if (out && n >= 12) memset(out, 0, (size_t)n);
  return 0;
}
static WINAPI DWORD GetObjectType(HANDLE h) {
  struct wgdi* o = gdi_of(h);
  if (dc_of(h)) return 3; /* OBJ_DC */
  if (!o) return 0;
  return o->kind == G_BRUSH ? 2 : o->kind == G_PEN ? 1 : o->kind == G_BITMAP ? 7 : 6;
}
static WINAPI HANDLE GetCurrentObject(HANDLE dc, DWORD type) {
  struct wdc* d = dc_of(dc);
  if (!d) return 0;
  switch (type) {
    case 1: return d->pen;
    case 2: return d->brush;
    case 6: return d->font;
    case 7: return d->bitmap;
  }
  return 0;
}

static WINAPI int SetBkMode(HANDLE dc, int mode) {
  struct wdc* d = dc_of(dc);
  int old;
  if (!d) return 0;
  old = d->bk_mode;
  d->bk_mode = mode;
  return old;
}
static WINAPI DWORD SetBkColor(HANDLE dc, DWORD c) {
  struct wdc* d = dc_of(dc);
  DWORD old;
  if (!d) return 0xFFFFFFFFu;
  old = d->bk_color;
  d->bk_color = c & 0xFFFFFF;
  return old;
}
static WINAPI DWORD SetTextColor(HANDLE dc, DWORD c) {
  struct wdc* d = dc_of(dc);
  DWORD old;
  if (!d) return 0xFFFFFFFFu;
  old = d->text_color;
  d->text_color = c & 0xFFFFFF;
  return old;
}
static WINAPI DWORD GetTextColor(HANDLE dc) {
  struct wdc* d = dc_of(dc);
  return d ? d->text_color : 0;
}
static WINAPI DWORD SetTextAlign(HANDLE dc, DWORD a) {
  struct wdc* d = dc_of(dc);
  DWORD old;
  if (!d) return 0;
  old = d->align;
  d->align = a;
  return old;
}
static WINAPI int SetStretchBltMode(HANDLE dc, int mode) {
  (void)dc;
  (void)mode;
  return 1;
}
static WINAPI int SetMapMode(HANDLE dc, int mode) {
  (void)dc;
  (void)mode;
  return 1;
}
static WINAPI BOOL SetBrushOrgEx(HANDLE dc, int x, int y, struct point* old) {
  (void)dc; (void)x; (void)y;
  if (old) old->x = old->y = 0;
  return 1;
}
static WINAPI int GetDeviceCaps(HANDLE dc, int i) {
  int sw = 0, sh = 0;
  (void)dc;
  { int k_win_primary(int* w, int* h); if (k_win_primary(&sw, &sh) != 0) k_win_size(&sw, &sh); }
  switch (i) {
    case 8: return sw > 0 ? sw : 1024;  /* HORZRES */
    case 10: return sh > 0 ? sh : 768;  /* VERTRES */
    case 12: return 32;                 /* BITSPIXEL */
    case 14: return 1;                  /* PLANES */
    case 24: return -1;                 /* NUMCOLORS */
    case 88: case 90: return 96;        /* LOGPIXELSX/Y */
  }
  return 0;
}
static WINAPI int IntersectClipRect(HANDLE dc, int l, int t, int r, int b) {
  (void)dc; (void)l; (void)t; (void)r; (void)b;
  return 2; /* SIMPLEREGION */
}
static WINAPI int SelectClipRgn(HANDLE dc, HANDLE rgn) {
  (void)dc;
  (void)rgn;
  return 2;
}


/* ---------- regioes (retangulares) e transformacao do mundo ----------
 * O Skia (InitializeDC) liga GM_ADVANCED e poe a identidade; o Chromium
 * recorta com regioes retangulares. Regiao aqui e so o retangulo. */
#define NULLREGION 1
#define SIMPLEREGION 2
static int rgn_kind(const struct wgdi* r) { return r->rr > r->rl && r->rb > r->rt ? SIMPLEREGION : NULLREGION; }
static WINAPI HANDLE CreateRectRgn(int l, int t, int r, int b) {
  struct wgdi* o = obj_new(G_RGN);
  if (!o) return 0;
  o->rl = l < r ? l : r;
  o->rr = l < r ? r : l;
  o->rt = t < b ? t : b;
  o->rb = t < b ? b : t;
  return o;
}
static WINAPI HANDLE CreateRectRgnIndirect(const int32_t* r) { return r ? CreateRectRgn(r[0], r[1], r[2], r[3]) : 0; }
static WINAPI BOOL SetRectRgn(HANDLE h, int l, int t, int r, int b) {
  struct wgdi* o = gdi_of(h);
  if (!o || o->kind != G_RGN) return 0;
  o->rl = l; o->rt = t; o->rr = r; o->rb = b;
  return 1;
}
static WINAPI int GetRgnBox(HANDLE h, int32_t* r) {
  struct wgdi* o = gdi_of(h);
  if (!o || o->kind != G_RGN || !r) return 0;
  r[0] = o->rl; r[1] = o->rt; r[2] = o->rr; r[3] = o->rb;
  return rgn_kind(o);
}
static WINAPI int CombineRgn(HANDLE dst, HANDLE a, HANDLE b, int mode) {
  struct wgdi* d = gdi_of(dst);
  struct wgdi* x = gdi_of(a);
  struct wgdi* y = gdi_of(b);
  int l, t, r, bt;
  if (!d || !x || d->kind != G_RGN || x->kind != G_RGN) return 0;
  l = x->rl; t = x->rt; r = x->rr; bt = x->rb;
  if (mode != 5 && y && y->kind == G_RGN) { /* 5 = RGN_COPY */
    if (mode == 1) { /* RGN_AND */
      if (y->rl > l) l = y->rl;
      if (y->rt > t) t = y->rt;
      if (y->rr < r) r = y->rr;
      if (y->rb < bt) bt = y->rb;
    } else if (mode == 2 || mode == 3) { /* RGN_OR / RGN_XOR: caixa que cobre os dois */
      if (rgn_kind(y) == SIMPLEREGION) {
        if (rgn_kind(x) == NULLREGION) {
          l = y->rl; t = y->rt; r = y->rr; bt = y->rb;
        } else {
          if (y->rl < l) l = y->rl;
          if (y->rt < t) t = y->rt;
          if (y->rr > r) r = y->rr;
          if (y->rb > bt) bt = y->rb;
        }
      }
    } /* 4 = RGN_DIFF: fica a (aproximacao) */
  }
  d->rl = l; d->rt = t; d->rr = r > l ? r : l; d->rb = bt > t ? bt : t;
  return rgn_kind(d);
}
static WINAPI BOOL EqualRgn(HANDLE a, HANDLE b) {
  struct wgdi* x = gdi_of(a);
  struct wgdi* y = gdi_of(b);
  return x && y && x->rl == y->rl && x->rt == y->rt && x->rr == y->rr && x->rb == y->rb;
}
static WINAPI int ExtSelectClipRgn(HANDLE dc, HANDLE rgn, int mode) {
  (void)mode;
  return SelectClipRgn(dc, rgn);
}
static WINAPI int GetClipBox(HANDLE dc, int32_t* r) {
  struct wdc* d = dc_of(dc);
  if (!d || !r) return 0;
  if (d->bitmap) {
    r[0] = 0; r[1] = 0; r[2] = d->bitmap->s.w; r[3] = d->bitmap->s.h;
  } else if (d->win) {
    r[0] = 0; r[1] = 0; r[2] = d->clip.right - d->clip.left; r[3] = d->clip.bottom - d->clip.top;
  } else {
    r[0] = r[1] = 0; r[2] = r[3] = 1;
  }
  return SIMPLEREGION;
}
static WINAPI int GetClipRgn(HANDLE dc, HANDLE rgn) {
  (void)dc;
  (void)rgn;
  return 0; /* sem regiao de recorte propria */
}
static WINAPI int SetGraphicsMode(HANDLE dc, int mode) {
  (void)dc;
  (void)mode;
  return 1; /* GM_COMPATIBLE era o anterior */
}
static WINAPI BOOL SetWorldTransform(HANDLE dc, const float* x) {
  (void)dc;
  (void)x;
  return 1; /* so a identidade e usada (Skia) */
}
static WINAPI BOOL ModifyWorldTransform(HANDLE dc, const float* x, DWORD mode) {
  (void)dc; (void)x; (void)mode;
  return 1;
}
static WINAPI BOOL GetWorldTransform(HANDLE dc, float* x) {
  (void)dc;
  if (!x) return 0;
  x[0] = 1; x[1] = 0; x[2] = 0; x[3] = 1; x[4] = 0; x[5] = 0;
  return 1;
}
/* GetDIBits/SetDIBits: so 32 bpp, de cima para baixo conforme biHeight */
static WINAPI int GetDIBits(HANDLE dc, HANDLE bmp, DWORD start, DWORD lines, void* bits, uint8_t* bmi, DWORD use) {
  struct wgdi* b = gdi_of(bmp);
  DWORD y;
  int h;
  (void)dc; (void)use;
  if (!b || b->kind != G_BITMAP || !bmi) return 0;
  if (!bits) { /* so o cabecalho */
    *(int32_t*)(bmi + 4) = b->s.w;
    *(int32_t*)(bmi + 8) = b->s.bottom_up ? b->s.h : -b->s.h;
    *(uint16_t*)(bmi + 12) = 1;
    *(uint16_t*)(bmi + 14) = 32;
    *(DWORD*)(bmi + 16) = 0;
    *(DWORD*)(bmi + 20) = (DWORD)(b->s.w * b->s.h * 4);
    return b->s.h;
  }
  h = *(const int32_t*)(bmi + 8);
  for (y = 0; y < lines && (int)(start + y) < b->s.h; y++) {
    int sy = h < 0 ? (int)(start + y) : b->s.h - 1 - (int)(start + y); /* linha pedida, de cima */
    memcpy((uint8_t*)bits + (size_t)y * (size_t)b->s.w * 4u, surf_row(&b->s, sy), (size_t)b->s.w * 4u);
  }
  return (int)y;
}
static WINAPI int SetDIBits(HANDLE dc, HANDLE bmp, DWORD start, DWORD lines, const void* bits, const uint8_t* bmi,
                            DWORD use) {
  struct wgdi* b = gdi_of(bmp);
  DWORD y;
  int h, w;
  (void)dc; (void)use;
  if (!b || b->kind != G_BITMAP || !bmi || !bits || *(const uint16_t*)(bmi + 14) != 32) return 0;
  h = *(const int32_t*)(bmi + 8);
  w = *(const int32_t*)(bmi + 4) < b->s.w ? *(const int32_t*)(bmi + 4) : b->s.w;
  for (y = 0; y < lines && (int)(start + y) < b->s.h; y++) {
    int sy = h < 0 ? (int)(start + y) : b->s.h - 1 - (int)(start + y);
    memcpy(surf_row(&b->s, sy), (const uint8_t*)bits + (size_t)y * (size_t)(*(const int32_t*)(bmi + 4)) * 4u,
           (size_t)w * 4u);
  }
  return (int)y;
}

/* ---------- desenho ---------- */
static WINAPI BOOL Rectangle(HANDLE dc, int l, int t, int r, int b) {
  struct wdc* d = dc_of(dc);
  if (!d) return 0;
  if (d->brush && !d->brush->null) dc_fill(d, l, t, r, b, d->brush->color);
  if (d->pen && !d->pen->null) dc_frame(d, l, t, r, b, d->pen->color);
  return 1;
}
static WINAPI int FillRect(HANDLE dc, const struct rect* r, HANDLE brush) {
  struct wdc* d = dc_of(dc);
  int null;
  DWORD c = brush_color(brush, &null);
  if (!d || null) return 0;
  dc_fill(d, r->left, r->top, r->right, r->bottom, c);
  return 1;
}
static WINAPI int FrameRect(HANDLE dc, const struct rect* r, HANDLE brush) {
  struct wdc* d = dc_of(dc);
  int null;
  DWORD c = brush_color(brush, &null);
  if (!d || null) return 0;
  dc_frame(d, r->left, r->top, r->right, r->bottom, c);
  return 1;
}
static WINAPI BOOL DrawFocusRect(HANDLE dc, const struct rect* r) {
  struct wdc* d = dc_of(dc);
  if (d) dc_frame(d, r->left, r->top, r->right, r->bottom, 0x808080);
  return 1;
}
static WINAPI BOOL DrawEdge(HANDLE dc, struct rect* r, DWORD edge, DWORD flags) {
  struct wdc* d = dc_of(dc);
  (void)edge;
  (void)flags;
  if (d) dc_frame(d, r->left, r->top, r->right, r->bottom, 0xA0A0A0);
  return 1;
}
static WINAPI BOOL PatBlt(HANDLE dc, int x, int y, int w, int h, DWORD rop) {
  struct wdc* d = dc_of(dc);
  if (!d) return 0;
  dc_fill(d, x, y, x + w, y + h,
          rop == 0x42 ? 0 : rop == 0xFF0062 ? 0xFFFFFF : d->brush ? d->brush->color : 0);
  return 1;
}
static WINAPI BOOL MoveToEx(HANDLE dc, int x, int y, struct point* old) {
  struct wdc* d = dc_of(dc);
  if (!d) return 0;
  if (old) {
    old->x = d->cur_x;
    old->y = d->cur_y;
  }
  d->cur_x = x;
  d->cur_y = y;
  return 1;
}
static WINAPI BOOL LineTo(HANDLE dc, int x, int y) {
  struct wdc* d = dc_of(dc);
  if (!d) return 0;
  if (d->pen && !d->pen->null) dc_line(d, d->cur_x, d->cur_y, x, y, d->pen->color);
  d->cur_x = x;
  d->cur_y = y;
  return 1;
}
static WINAPI DWORD SetPixel(HANDLE dc, int x, int y, DWORD c) {
  struct wdc* d = dc_of(dc);
  if (d) dc_fill(d, x, y, x + 1, y + 1, c);
  return c;
}
static WINAPI BOOL Ellipse(HANDLE dc, int l, int t, int r, int b) { return Rectangle(dc, l, t, r, b); }
static WINAPI BOOL RoundRect(HANDLE dc, int l, int t, int r, int b, int w, int h) {
  (void)w;
  (void)h;
  return Rectangle(dc, l, t, r, b);
}

static WINAPI BOOL StretchBlt(HANDLE ddc, int dx, int dy, int dw, int dh, HANDLE sdc, int sx, int sy, int sw, int sh,
                              DWORD rop) {
  struct wdc* dd = dc_of(ddc);
  struct wdc* sd = dc_of(sdc);
  struct surf* d = dd ? dc_surf(dd) : 0;
  struct surf* s = sd ? dc_surf(sd) : 0;
  struct rect clip;
  (void)rop;
  if (!d || !s) return 0;
  clip = dc_clip_abs(dd);
  surf_blit(d, dx + dd->ox, dy + dd->oy, dw, dh, s, sx + sd->ox, sy + sd->oy, sw, sh, &clip);
  return 1;
}
/* WINEDEBUG=2: desenho que vai para uma janela (1o e depois 1 a cada 200) */
static void blit_note(const char* what, HANDLE dc, int w, int h) {
  static volatile int n;
  struct wdc* d = dc_of(dc);
  int k;
  if (wine_debug < 2 || !d || !d->win) return;
  k = __sync_fetch_and_add(&n, 1);
  if (k % 200) return;
  wlogx(what, (uint64_t)(uint32_t)w << 32 | (uint32_t)h);
}
static WINAPI BOOL BitBlt(HANDLE ddc, int dx, int dy, int w, int h, HANDLE sdc, int sx, int sy, DWORD rop) {
  struct wdc* sd = dc_of(sdc);
  blit_note("BitBlt na janela (larg<<32|alt) ", ddc, w, h);
  if (!sd) return PatBlt(ddc, dx, dy, w, h, rop);
  return StretchBlt(ddc, dx, dy, w, h, sdc, sx, sy, w, h, rop);
}
static WINAPI BOOL AlphaBlend(HANDLE ddc, int dx, int dy, int dw, int dh, HANDLE sdc, int sx, int sy, int sw, int sh,
                              DWORD fn) {
  (void)fn;
  return StretchBlt(ddc, dx, dy, dw, dh, sdc, sx, sy, sw, sh, 0xCC0020);
}
static WINAPI BOOL TransparentBlt(HANDLE ddc, int dx, int dy, int dw, int dh, HANDLE sdc, int sx, int sy, int sw, int sh,
                                  DWORD key) {
  (void)key;
  return StretchBlt(ddc, dx, dy, dw, dh, sdc, sx, sy, sw, sh, 0xCC0020);
}

/* ---------- bitmaps ---------- */
struct wgdi* bitmap_from_dib(const uint8_t* bih, const uint8_t* bits) {
  int32_t w = *(const int32_t*)(bih + 4), h = *(const int32_t*)(bih + 8);
  int bpp = *(const uint16_t*)(bih + 14);
  uint32_t hsz = *(const uint32_t*)bih, ncol = *(const uint32_t*)(bih + 32);
  const uint8_t* pal = bih + hsz;
  struct wgdi* b;
  int x, y, ah = h < 0 ? -h : h, stride;
  if (w <= 0 || !h || w > 8192 || ah > 8192) return 0;
  if (bpp <= 8 && !ncol) ncol = 1u << bpp;
  if (!bits) bits = pal + (bpp <= 8 ? ncol * 4 : 0) + (*(const uint32_t*)(bih + 16) == 3 ? 12 : 0);
  b = obj_new(G_BITMAP);
  if (!b || !surf_alloc(&b->s, w, ah, 0)) return 0;
  stride = ((w * bpp + 31) / 32) * 4;
  for (y = 0; y < ah; y++) {
    const uint8_t* src = bits + (size_t)stride * (size_t)(h > 0 ? ah - 1 - y : y);
    uint8_t* dst = b->s.px + (size_t)y * (size_t)w * 4u;
    for (x = 0; x < w; x++) {
      const uint8_t* c;
      uint8_t idx;
      switch (bpp) {
        case 32: c = src + x * 4; break;
        case 24: c = src + x * 3; break;
        case 8: idx = src[x]; c = pal + 4 * idx; break;
        case 4: idx = (uint8_t)((src[x / 2] >> ((x & 1) ? 0 : 4)) & 15); c = pal + 4 * idx; break;
        case 1: idx = (uint8_t)((src[x / 8] >> (7 - (x & 7))) & 1); c = pal + 4 * idx; break;
        default: c = 0;
      }
      if (!c) continue;
      dst[x * 4 + 0] = c[0];
      dst[x * 4 + 1] = c[1];
      dst[x * 4 + 2] = c[2];
      dst[x * 4 + 3] = 255;
    }
  }
  return b;
}
static WINAPI HANDLE CreateDIBSection(HANDLE dc, const uint8_t* bmi, DWORD usage, void** bits, HANDLE sec, DWORD off) {
  struct wgdi* b = obj_new(G_BITMAP);
  int w = *(const int32_t*)(bmi + 4), h = *(const int32_t*)(bmi + 8);
  int bpp = *(const uint16_t*)(bmi + 14);
  (void)dc; (void)usage;
  if (!b || bpp != 32 || w <= 0 || !h) {
    if (wine_debug) wlogx("CreateDIBSection: formato nao suportado, bpp ", (uint64_t)bpp);
    if (bits) *bits = 0;
    return 0;
  }
  if (sec) {
    /* pixels dentro da secao (memoria compartilhada): o compositor do
     * Chromium desenha la e o BitBlt daqui le o mesmo lugar */
    uint8_t* wsection_ptr(HANDLE sec, uint64_t off, uint64_t need); /* map.c */
    int ah = h < 0 ? -h : h;
    uint8_t* px = wsection_ptr(sec, off, (uint64_t)w * (uint64_t)ah * 4u);
    if (!px) {
      if (wine_debug) wlogx("CreateDIBSection: secao invalida ou pequena ", (uint64_t)(uintptr_t)sec);
      if (bits) *bits = 0;
      return 0;
    }
    b->s.px = px;
    b->s.w = w;
    b->s.h = ah;
    b->s.rgba = 0;
    b->s.foreign = 1;
    b->s.bottom_up = h > 0;
    if (bits) *bits = px;
    return b;
  }
  if (!surf_alloc(&b->s, w, h < 0 ? -h : h, 0)) return 0;
  b->s.bottom_up = h > 0;
  if (bits) *bits = b->s.px;
  return b;
}
static WINAPI HANDLE CreateCompatibleBitmap(HANDLE dc, int w, int h) {
  struct wgdi* b = obj_new(G_BITMAP);
  (void)dc;
  if (!b || !surf_alloc(&b->s, w, h, 0)) return 0;
  return b;
}
static WINAPI HANDLE CreateBitmap(int w, int h, DWORD planes, DWORD bpp, const void* bits) {
  struct wgdi* b = obj_new(G_BITMAP);
  (void)planes;
  if (!b || !surf_alloc(&b->s, w, h, 0)) return 0;
  if (bits && bpp == 32) memcpy(b->s.px, bits, (size_t)w * (size_t)h * 4u);
  return b;
}
static WINAPI int SetDIBitsToDevice(HANDLE dc, int dx, int dy, DWORD w, DWORD h, int sx, int sy, DWORD start,
                                    DWORD lines, const void* bits, const uint8_t* bmi, DWORD use) {
  struct wdc* d = dc_of(dc);
  struct wgdi* b;
  struct surf* ds;
  struct rect clip;
  (void)start;
  blit_note("SetDIBitsToDevice na janela ", dc, (int)w, (int)h);
  (void)use;
  if (!d || !(ds = dc_surf(d))) return 0;
  b = bitmap_from_dib(bmi, (const uint8_t*)bits);
  if (!b) return 0;
  clip = dc_clip_abs(d);
  surf_blit(ds, dx + d->ox, dy + d->oy, (int)w, (int)h, &b->s, sx, sy, (int)w, (int)h, &clip);
  DeleteObject(b);
  return (int)lines;
}
static WINAPI int StretchDIBits(HANDLE dc, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh,
                                const void* bits, const uint8_t* bmi, DWORD use, DWORD rop) {
  struct wdc* d = dc_of(dc);
  struct wgdi* b;
  struct surf* ds;
  struct rect clip;
  (void)use;
  blit_note("StretchDIBits na janela ", dc, dw, dh);
  (void)rop;
  if (!d || !(ds = dc_surf(d))) return 0;
  b = bitmap_from_dib(bmi, (const uint8_t*)bits);
  if (!b) return 0;
  clip = dc_clip_abs(d);
  surf_blit(ds, dx + d->ox, dy + d->oy, dw, dh, &b->s, sx, sy, sw, sh, &clip);
  DeleteObject(b);
  return dh;
}
static WINAPI HANDLE LoadBitmapW(HANDLE inst, const wchar16* name) {
  struct wmod* m = pe_from_handle(inst);
  uint32_t size = 0;
  const uint8_t* e = m ? pe_find_resource(m, 2, (uintptr_t)name, &size) : 0; /* RT_BITMAP */
  if (!e) return 0;
  return bitmap_from_dib(m->base + *(const uint32_t*)e, 0);
}

/* ---------- texto ---------- */
static int font_bold(struct wdc* d) { return d->font && d->font->bold; }

static WINAPI BOOL ExtTextOutW(HANDLE dc, int x, int y, DWORD opt, const struct rect* r, const wchar16* s, DWORD n,
                               const int* dx) {
  struct wdc* d = dc_of(dc);
  struct surf* su;
  struct rect clip;
  int w;
  (void)dx;
  if (!d || !(su = dc_surf(d))) return 0;
  if (r && (opt & 2)) dc_fill(d, r->left, r->top, r->right, r->bottom, d->bk_color); /* ETO_OPAQUE */
  if (!s || !n) return 1;
  w = text_width(s, (int)n);
  if (d->align & 6) x -= (d->align & 2) ? w : w / 2; /* TA_RIGHT / TA_CENTER */
  if (d->bk_mode == 2 && !(opt & 2)) dc_fill(d, x, y, x + w, y + text_height(), d->bk_color);
  clip = dc_clip_abs(d);
  if (r && (opt & 4)) { /* ETO_CLIPPED */
    struct rect c2 = {r->left + d->ox, r->top + d->oy, r->right + d->ox, r->bottom + d->oy};
    if (c2.left > clip.left) clip.left = c2.left;
    if (c2.top > clip.top) clip.top = c2.top;
    if (c2.right < clip.right) clip.right = c2.right;
    if (c2.bottom < clip.bottom) clip.bottom = c2.bottom;
  }
  text_draw(su, &clip, x + d->ox, y + d->oy, s, (int)n, d->text_color, font_bold(d));
  return 1;
}
static WINAPI BOOL TextOutW(HANDLE dc, int x, int y, const wchar16* s, int n) {
  return ExtTextOutW(dc, x, y, 0, 0, s, (DWORD)n, 0);
}
static WINAPI BOOL TextOutA(HANDLE dc, int x, int y, const char* s, int n) {
  wchar16 buf[512];
  int k = wutf8_to_16(s, n, buf, 511);
  return ExtTextOutW(dc, x, y, 0, 0, buf, (DWORD)k, 0);
}
static WINAPI int DrawTextW(HANDLE dc, const wchar16* s, int n, struct rect* r, DWORD fmt) {
  struct wdc* d = dc_of(dc);
  struct surf* su;
  struct rect rr, clip;
  int h;
  if (!d || !s) return 0;
  su = dc_surf(d);
  rr.left = r->left + d->ox;
  rr.right = r->right + d->ox;
  rr.top = r->top + d->oy;
  rr.bottom = r->bottom + d->oy;
  if (fmt & DT_CALCRECT) {
    int len = n < 0 ? (int)wstrlen16(s) : n;
    h = text_box(0, 0, &rr, s, n, 0, 0, fmt);
    if (fmt & DT_SINGLELINE || !(fmt & DT_WORDBREAK)) r->right = r->left + text_width(s, len);
    r->bottom = r->top + h;
    return h;
  }
  if (!su) return 0;
  clip = dc_clip_abs(d);
  if (d->bk_mode == 2) dc_fill(d, r->left, r->top, r->right, r->bottom, d->bk_color);
  return text_box(su, &clip, &rr, s, n, d->text_color, font_bold(d), fmt);
}
static WINAPI int DrawTextExW(HANDLE dc, wchar16* s, int n, struct rect* r, DWORD fmt, void* p) {
  (void)p;
  return DrawTextW(dc, s, n, r, fmt);
}
struct size {
  int32_t cx, cy;
};
static WINAPI BOOL GetTextExtentPoint32W(HANDLE dc, const wchar16* s, int n, struct size* sz) {
  (void)dc;
  sz->cx = text_width(s, n);
  sz->cy = text_height();
  return 1;
}
static WINAPI BOOL GetTextExtentExPointW(HANDLE dc, const wchar16* s, int n, int max, int* fit, int* dx,
                                         struct size* sz) {
  int i, w = 0;
  (void)dc;
  for (i = 0; i < n; i++) {
    int cw = text_width(s + i, 1);
    if (fit && w + cw > max && *fit == 0) *fit = i;
    w += cw;
    if (dx) dx[i] = w;
  }
  if (fit && (*fit == 0 || w <= max)) *fit = w <= max ? n : *fit;
  sz->cx = w;
  sz->cy = text_height();
  return 1;
}
static WINAPI BOOL GetTextMetricsW(HANDLE dc, int32_t* tm) {
  (void)dc;
  memset(tm, 0, 60);
  tm[0] = text_height();          /* tmHeight */
  tm[1] = text_ascent();          /* tmAscent */
  tm[2] = text_height() - text_ascent(); /* tmDescent */
  tm[5] = 6;                      /* tmAveCharWidth */
  tm[6] = 12;                     /* tmMaxCharWidth */
  tm[7] = 400;                    /* tmWeight */
  return 1;
}
static WINAPI int GetTextFaceW(HANDLE dc, int n, wchar16* buf) {
  static const wchar16 face[] = {'S', 'e', 'g', 'o', 'e', ' ', 'U', 'I', 0};
  (void)dc;
  if (buf && n >= 9) memcpy(buf, face, sizeof(face));
  return 8;
}

/* ---------- GDI+ : nao ha (quem chama segue sem as imagens PNG) ---------- */
static WINAPI int GdiplusStartup(uintptr_t* token, const void* in, void* out) {
  (void)in;
  (void)out;
  if (token) *token = 0;
  return 1; /* GenericError */
}
static WINAPI void GdiplusShutdown(uintptr_t token) { (void)token; }
static WINAPI int GdipNotImplemented(void) { return 6; } /* NotImplemented */
static WINAPI void* GdipAlloc(size_t n) { return k_malloc((unsigned)(n ? n : 1)); }
static WINAPI void GdipFree(void* p) {
  if (p) k_free(p);
}

#define S(n) {#n, (void*)n}
const struct wine_sym gdi_syms[] = {
    S(CreateRectRgn), S(CreateRectRgnIndirect), S(SetRectRgn), S(GetRgnBox), S(CombineRgn), S(EqualRgn),
    S(ExtSelectClipRgn), S(GetClipBox), S(GetClipRgn), S(SetGraphicsMode), S(SetWorldTransform),
    S(ModifyWorldTransform), S(GetWorldTransform), S(GetDIBits), S(SetDIBits), {"GdiAlphaBlend", (void*)AlphaBlend},
    S(GetDC), S(GetWindowDC), S(CreateDCW), S(CreateDCA), S(ReleaseDC), S(CreateCompatibleDC), S(DeleteDC), S(SaveDC), S(RestoreDC),
    S(CreateSolidBrush), S(CreateBrushIndirect), S(GetSysColorBrush), S(GetSysColor), S(CreatePen),
    S(CreateFontIndirectW), S(CreateFontW), {"GetStockObject", (void*)wGetStockObject},
    {"SelectObject", (void*)wSelectObject}, S(DeleteObject), S(GetObjectW), S(GetObjectType), S(GetCurrentObject),
    S(SetBkMode), S(SetBkColor), S(SetTextColor), S(GetTextColor), S(SetTextAlign), S(SetStretchBltMode),
    S(SetMapMode), S(SetBrushOrgEx), S(GetDeviceCaps), S(IntersectClipRect), S(SelectClipRgn), S(Rectangle),
    S(FillRect), S(FrameRect), S(DrawFocusRect), S(DrawEdge), S(PatBlt), S(MoveToEx), S(LineTo), S(SetPixel),
    S(Ellipse), S(RoundRect), S(StretchBlt), S(BitBlt), S(AlphaBlend), S(TransparentBlt), S(CreateDIBSection),
    S(CreateCompatibleBitmap), S(CreateBitmap), S(SetDIBitsToDevice), S(StretchDIBits), S(LoadBitmapW),
    S(ExtTextOutW), S(TextOutW), S(TextOutA), S(DrawTextW), S(DrawTextExW), S(GetTextExtentPoint32W),
    S(GetTextExtentExPointW), S(GetTextMetricsW), S(GetTextFaceW), S(GdiplusStartup), S(GdiplusShutdown),
    {"GdipCreateBitmapFromStream", (void*)GdipNotImplemented}, {"GdipCreateHBITMAPFromBitmap", (void*)GdipNotImplemented},
    {"GdipDisposeImage", (void*)GdipNotImplemented}, {"GdipCloneImage", (void*)GdipNotImplemented}, S(GdipAlloc),
    S(GdipFree),
    {0, 0}};
