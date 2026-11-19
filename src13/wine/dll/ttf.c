/*
 * ttf.c - Leitura de fontes TrueType/OpenType para o DirectWrite (dwrite.c):
 * tabelas, nomes, metricas, cmap (formatos 4 e 12), contornos do glyf
 * (simples e compostos) e rasterizacao com antialiasing (4x4 amostras).
 *
 * Fontes CFF (OpenType com contornos PostScript) sao abertas para metricas e
 * cmap, mas ainda sem contorno: os glifos saem vazios.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "ttf.h"

static uint32_t u16(const uint8_t* p) { return (uint32_t)((p[0] << 8) | p[1]); }
static int s16(const uint8_t* p) { return (int)(int16_t)u16(p); }
static uint32_t u32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

const uint8_t* ttf_table(const struct ttfont* f, uint32_t tag, uint32_t* len) {
  const uint8_t* d = f->data + f->dir;
  uint32_t n, i;
  if (f->dir + 12 > f->len) return 0;
  n = u16(d + 4);
  for (i = 0; i < n && f->dir + 12 + (i + 1) * 16 <= f->len; i++) {
    const uint8_t* r = d + 12 + i * 16;
    if (u32(r) == tag) {
      uint32_t off = u32(r + 8), l = u32(r + 12);
      if (off > f->len || l > f->len - off) return 0;
      if (len) *len = l;
      return f->data + off;
    }
  }
  return 0;
}
#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* nome (Windows, Unicode; ingles de preferencia) -> wchar16 */
static int read_name(const struct ttfont* f, int id, wchar16* out, int max) {
  uint32_t len, n, i, sto;
  const uint8_t* t = ttf_table(f, TAG('n', 'a', 'm', 'e'), &len);
  int best = -1, best_score = -1;
  out[0] = 0;
  if (!t || len < 6) return 0;
  n = u16(t + 2);
  sto = u16(t + 4);
  for (i = 0; i < n && 6 + (i + 1) * 12 <= len; i++) {
    const uint8_t* r = t + 6 + i * 12;
    uint32_t plat = u16(r), enc = u16(r + 2), lang = u16(r + 4), nid = u16(r + 6);
    int score;
    if ((int)nid != id) continue;
    if (plat == 3 && (enc == 1 || enc == 10)) score = lang == 0x409 ? 3 : 2;
    else if (plat == 0) score = 1;
    else continue;
    if (score > best_score) {
      best_score = score;
      best = (int)i;
    }
  }
  if (best < 0) return 0;
  {
    const uint8_t* r = t + 6 + best * 12;
    uint32_t l = u16(r + 8), off = u16(r + 10), k, o = 0;
    if (sto + off + l > len) return 0;
    for (k = 0; k + 1 < l && (int)o < max - 1; k += 2) out[o++] = (wchar16)u16(t + sto + off + k);
    out[o] = 0;
    return (int)o;
  }
}

static void pick_cmap(struct ttfont* f) {
  uint32_t len, n, i;
  const uint8_t* t = ttf_table(f, TAG('c', 'm', 'a', 'p'), &len);
  f->cmap = 0;
  if (!t || len < 4) return;
  n = u16(t + 2);
  for (i = 0; i < n && 4 + (i + 1) * 8 <= len; i++) {
    const uint8_t* r = t + 4 + i * 8;
    uint32_t plat = u16(r), enc = u16(r + 2), off = u32(r + 4), fmt;
    if (off + 4 > len) continue;
    fmt = u16(t + off);
    if (fmt == 12 && ((plat == 3 && enc == 10) || plat == 0)) {
      f->cmap = t + off;
      f->cmap_fmt = 12;
      f->cmap_len = len - off;
      return; /* o mais completo */
    }
    if (fmt == 4 && ((plat == 3 && (enc == 1 || enc == 0)) || plat == 0) && !f->cmap) {
      f->cmap = t + off;
      f->cmap_fmt = 4;
      f->cmap_len = len - off;
    }
  }
}

int ttf_open(struct ttfont* f, const uint8_t* data, uint32_t len, uint32_t index) {
  const uint8_t* t;
  uint32_t l;
  memset(f, 0, sizeof(*f));
  f->data = data;
  f->len = len;
  if (len < 12) return 0;
  if (u32(data) == TAG('t', 't', 'c', 'f')) {
    uint32_t n = u32(data + 8);
    if (index >= n || 12 + (index + 1) * 4 > len) return 0;
    f->dir = u32(data + 12 + index * 4);
    f->nfaces = n;
  } else {
    f->nfaces = 1;
    if (index) return 0;
  }
  f->index = index;
  if (f->dir + 12 > len) return 0;
  f->cff = u32(data + f->dir) == TAG('O', 'T', 'T', 'O');
  t = ttf_table(f, TAG('h', 'e', 'a', 'd'), &l);
  if (!t || l < 54) return 0;
  f->upem = u16(t + 18);
  f->xmin = s16(t + 36);
  f->ymin = s16(t + 38);
  f->xmax = s16(t + 40);
  f->ymax = s16(t + 42);
  f->mac_style = u16(t + 44);
  f->loca_long = s16(t + 50) != 0;
  if (!f->upem) f->upem = 1000;
  t = ttf_table(f, TAG('m', 'a', 'x', 'p'), &l);
  f->nglyphs = t && l >= 6 ? u16(t + 4) : 0;
  t = ttf_table(f, TAG('h', 'h', 'e', 'a'), &l);
  if (t && l >= 36) {
    f->asc = s16(t + 4);
    f->desc = -s16(t + 6);
    f->gap = s16(t + 8);
    f->nhmtx = u16(t + 34);
  }
  f->hmtx = ttf_table(f, TAG('h', 'm', 't', 'x'), &f->hmtx_len);
  f->glyf = ttf_table(f, TAG('g', 'l', 'y', 'f'), &f->glyf_len);
  f->loca = ttf_table(f, TAG('l', 'o', 'c', 'a'), &f->loca_len);
  f->weight = 400;
  f->width = 5;
  t = ttf_table(f, TAG('O', 'S', '/', '2'), &l);
  if (t && l >= 78) {
    uint32_t sel = u16(t + 62);
    f->weight = (int)u16(t + 4);
    f->width = (int)u16(t + 6);
    f->strike_size = s16(t + 26);
    f->strike_pos = s16(t + 28);
    f->italic = (sel & 1) || (sel & 0x200);
    if (sel & 0x80) { /* USE_TYPO_METRICS */
      f->asc = s16(t + 68);
      f->desc = -s16(t + 70);
      f->gap = s16(t + 72);
    } else if (u16(t + 74) || u16(t + 76)) {
      f->asc = (int)u16(t + 74);  /* usWinAscent */
      f->desc = (int)u16(t + 76); /* usWinDescent */
      f->gap = 0;
    }
    if (l >= 90 && u16(t) >= 2) {
      f->x_height = s16(t + 86);
      f->cap_height = s16(t + 88);
    }
  } else {
    f->italic = (f->mac_style & 2) != 0;
    if (f->mac_style & 1) f->weight = 700;
  }
  if (f->weight < 1 || f->weight > 1000) f->weight = 400;
  if (f->width < 1 || f->width > 9) f->width = 5;
  if (!f->cap_height) f->cap_height = f->asc * 7 / 10;
  if (!f->x_height) f->x_height = f->asc / 2;
  if (!f->strike_size) f->strike_size = f->upem / 20;
  if (!f->strike_pos) f->strike_pos = f->x_height / 2;
  t = ttf_table(f, TAG('p', 'o', 's', 't'), &l);
  if (t && l >= 12) {
    f->ul_pos = s16(t + 8);
    f->ul_size = s16(t + 10);
  }
  if (!f->ul_size) f->ul_size = f->upem / 20;
  if (!f->ul_pos) f->ul_pos = -(f->upem / 10);
  pick_cmap(f);
  if (!read_name(f, 16, f->family, TTF_NAME)) read_name(f, 1, f->family, TTF_NAME);
  if (!read_name(f, 17, f->style, TTF_NAME)) read_name(f, 2, f->style, TTF_NAME);
  read_name(f, 1, f->win_family, TTF_NAME);
  read_name(f, 2, f->win_style, TTF_NAME);
  read_name(f, 4, f->full, TTF_NAME);
  read_name(f, 6, f->postscript, TTF_NAME);
  return f->family[0] != 0;
}

uint32_t ttf_glyph(const struct ttfont* f, uint32_t c) {
  const uint8_t* t = f->cmap;
  if (!t) return 0;
  if (f->cmap_fmt == 12) {
    uint32_t n = u32(t + 12), lo = 0, hi = n;
    if (16 + n * 12 > f->cmap_len) return 0;
    while (lo < hi) { /* grupos em ordem */
      uint32_t mid = (lo + hi) / 2;
      const uint8_t* g = t + 16 + mid * 12;
      uint32_t s = u32(g), e = u32(g + 4);
      if (c < s) hi = mid;
      else if (c > e) lo = mid + 1;
      else return u32(g + 8) + (c - s);
    }
    return 0;
  }
  {
    uint32_t segx2 = u16(t + 6), seg = segx2 / 2, i;
    const uint8_t *endc, *startc, *delta, *roff;
    if (c > 0xFFFF || 16 + segx2 * 4 > f->cmap_len) return 0;
    endc = t + 14;
    startc = endc + segx2 + 2;
    delta = startc + segx2;
    roff = delta + segx2;
    for (i = 0; i < seg; i++) {
      uint32_t end = u16(endc + i * 2), start, r;
      int d;
      if (c > end) continue;
      start = u16(startc + i * 2);
      if (c < start) return 0;
      d = s16(delta + i * 2);
      r = u16(roff + i * 2);
      if (!r) return (c + (uint32_t)d) & 0xFFFF;
      {
        const uint8_t* p = roff + i * 2 + r + (c - start) * 2;
        uint32_t g;
        if (p + 2 > t + f->cmap_len) return 0;
        g = u16(p);
        return g ? ((g + (uint32_t)d) & 0xFFFF) : 0;
      }
    }
  }
  return 0;
}

int ttf_advance(const struct ttfont* f, uint32_t g) {
  uint32_t k;
  if (!f->hmtx || !f->nhmtx) return (int)f->upem / 2;
  k = g < f->nhmtx ? g : f->nhmtx - 1;
  if (k * 4 + 4 > f->hmtx_len) return 0;
  return (int)u16(f->hmtx + k * 4);
}
int ttf_lsb(const struct ttfont* f, uint32_t g) {
  if (!f->hmtx || !f->nhmtx) return 0;
  if (g < f->nhmtx) return g * 4 + 4 <= f->hmtx_len ? s16(f->hmtx + g * 4 + 2) : 0;
  {
    uint32_t o = f->nhmtx * 4 + (g - f->nhmtx) * 2;
    return o + 2 <= f->hmtx_len ? s16(f->hmtx + o) : 0;
  }
}

static int glyph_span(const struct ttfont* f, uint32_t g, uint32_t* off, uint32_t* end) {
  uint32_t a, b;
  if (!f->loca || !f->glyf) return 0;
  if (f->loca_long) {
    if ((g + 2) * 4 > f->loca_len) return 0;
    a = u32(f->loca + g * 4);
    b = u32(f->loca + g * 4 + 4);
  } else {
    if ((g + 2) * 2 > f->loca_len) return 0;
    a = u16(f->loca + g * 2) * 2;
    b = u16(f->loca + g * 2 + 2) * 2;
  }
  if (b <= a || b > f->glyf_len) return 0;
  *off = a;
  *end = b;
  return 1;
}

int ttf_bbox(const struct ttfont* f, uint32_t g, int* x0, int* y0, int* x1, int* y1) {
  uint32_t off, end;
  *x0 = *y0 = *x1 = *y1 = 0;
  if (!glyph_span(f, g, &off, &end) || end - off < 10) return 0;
  *x0 = s16(f->glyf + off + 2);
  *y0 = s16(f->glyf + off + 4);
  *x1 = s16(f->glyf + off + 6);
  *y1 = s16(f->glyf + off + 8);
  return 1;
}

/* ---------- contornos ---------- */
struct outline_work {
  float* x;
  float* y;
  uint8_t* on;
  int* cend;
  int np, nc, maxp, maxc;
};

static int load_simple(struct outline_work* w, const uint8_t* g, int glen, const float* m) {
  int ncont = s16(g), i, npts, inst, base = w->np, rep = 0, fr = 0;
  int xx = 0, yy = 0;
  const uint8_t* ends = g + 10;
  const uint8_t* p;
  const uint8_t* lim = g + glen;
  int* xs;
  if (ncont <= 0) return 1;
  if (w->nc + ncont > w->maxc || 10 + ncont * 2 + 2 > glen) return 0;
  npts = (int)u16(ends + (ncont - 1) * 2) + 1;
  if (npts <= 0 || base + npts > w->maxp) return 0;
  for (i = 0; i < ncont; i++) {
    int e = (int)u16(ends + i * 2);
    if (e >= npts) return 0;
    w->cend[w->nc + i] = base + e;
  }
  p = ends + ncont * 2;
  inst = (int)u16(p);
  p += 2 + inst;
  for (i = 0; i < npts; i++) { /* bandeiras */
    if (rep) {
      w->on[base + i] = (uint8_t)fr;
      rep--;
      continue;
    }
    if (p >= lim) return 0;
    w->on[base + i] = *p++;
    if (w->on[base + i] & 8) {
      if (p >= lim) return 0;
      rep = *p++;
      fr = w->on[base + i];
    }
  }
  xs = (int*)w->cend + w->maxc; /* rascunho: logo depois de cend */
  for (i = 0; i < npts; i++) {
    int fl = w->on[base + i];
    if (fl & 2) {
      if (p >= lim) return 0;
      xx += (fl & 16) ? *p : -*p;
      p++;
    } else if (!(fl & 16)) {
      if (p + 2 > lim) return 0;
      xx += s16(p);
      p += 2;
    }
    xs[i] = xx;
  }
  for (i = 0; i < npts; i++) {
    int fl = w->on[base + i];
    if (fl & 4) {
      if (p >= lim) return 0;
      yy += (fl & 32) ? *p : -*p;
      p++;
    } else if (!(fl & 32)) {
      if (p + 2 > lim) return 0;
      yy += s16(p);
      p += 2;
    }
    /* m = [a b c d dx dy]: x' = a*x + c*y + dx; y' = b*x + d*y + dy */
    w->x[base + i] = m[0] * (float)xs[i] + m[2] * (float)yy + m[4];
    w->y[base + i] = m[1] * (float)xs[i] + m[3] * (float)yy + m[5];
    w->on[base + i] = (uint8_t)(fl & 1);
  }
  w->np = base + npts;
  w->nc += ncont;
  return 1;
}

static float f2dot14(const uint8_t* p) { return (float)s16(p) / 16384.0f; }

static int load_glyph(const struct ttfont* f, struct outline_work* w, uint32_t g, const float* m, int depth) {
  uint32_t off, end;
  const uint8_t* gp;
  int glen;
  if (depth > 8) return 0;
  if (!glyph_span(f, g, &off, &end)) return 1; /* vazio */
  gp = f->glyf + off;
  glen = (int)(end - off);
  if (glen < 10) return 1;
  if (s16(gp) >= 0) return load_simple(w, gp, glen, m);
  {
    const uint8_t* p = gp + 10;
    const uint8_t* lim = gp + glen;
    int more = 1;
    while (more) {
      int flags;
      uint32_t comp;
      float a1, a2, sa = 1, sb = 0, sc = 0, sd = 1, cm[6];
      if (p + 4 > lim) return 0;
      flags = (int)u16(p);
      comp = u16(p + 2);
      p += 4;
      if (flags & 1) {
        if (p + 4 > lim) return 0;
        a1 = (float)s16(p);
        a2 = (float)s16(p + 2);
        p += 4;
      } else {
        if (p + 2 > lim) return 0;
        a1 = (float)(int8_t)p[0];
        a2 = (float)(int8_t)p[1];
        p += 2;
      }
      if (flags & 8) {
        if (p + 2 > lim) return 0;
        sa = sd = f2dot14(p);
        p += 2;
      } else if (flags & 64) {
        if (p + 4 > lim) return 0;
        sa = f2dot14(p);
        sd = f2dot14(p + 2);
        p += 4;
      } else if (flags & 128) {
        if (p + 8 > lim) return 0;
        sa = f2dot14(p);
        sb = f2dot14(p + 2);
        sc = f2dot14(p + 4);
        sd = f2dot14(p + 6);
        p += 8;
      }
      if (!(flags & 2)) a1 = a2 = 0; /* pontos casados: raro, ignorado */
      /* composicao: primeiro o do componente, depois o do pai */
      cm[0] = m[0] * sa + m[2] * sb;
      cm[1] = m[1] * sa + m[3] * sb;
      cm[2] = m[0] * sc + m[2] * sd;
      cm[3] = m[1] * sc + m[3] * sd;
      cm[4] = m[0] * a1 + m[2] * a2 + m[4];
      cm[5] = m[1] * a1 + m[3] * a2 + m[5];
      if (!load_glyph(f, w, comp, cm, depth + 1)) return 0;
      more = flags & 32;
    }
  }
  return 1;
}

/* Contorno do glifo em unidades da fonte (y para cima), em segmentos. */
int ttf_outline(const struct ttfont* f, uint32_t g, const struct ttf_sink* s) {
  struct outline_work w;
  static const float ident[6] = {1, 0, 0, 1, 0, 0};
  int c, start = 0;
  if (f->cff) return 1; /* ainda sem CFF */
  memset(&w, 0, sizeof(w));
  w.maxp = 4096;
  w.maxc = 512;
  w.x = (float*)wzalloc(sizeof(float) * (size_t)w.maxp);
  w.y = (float*)wzalloc(sizeof(float) * (size_t)w.maxp);
  w.on = (uint8_t*)wzalloc((size_t)w.maxp);
  w.cend = (int*)wzalloc(sizeof(int) * (size_t)(w.maxc + w.maxp));
  if (!w.x || !w.y || !w.on || !w.cend || !load_glyph(f, &w, g, ident, 0)) {
    k_free(w.x);
    k_free(w.y);
    k_free(w.on);
    k_free(w.cend);
    return 0;
  }
  for (c = 0; c < w.nc; c++) {
    int e = w.cend[c], n = e - start + 1, i, k0 = -1;
    float sx, sy, ox = 0, oy = 0;
    int have_off = 0;
    if (n < 2) {
      start = e + 1;
      continue;
    }
    for (i = 0; i < n; i++)
      if (w.on[start + i]) {
        k0 = i;
        break;
      }
    if (k0 < 0) { /* so pontos fora: comeca no meio dos dois primeiros */
      sx = (w.x[start] + w.x[start + 1]) / 2;
      sy = (w.y[start] + w.y[start + 1]) / 2;
      s->move(s->ctx, sx, sy);
      for (i = 1; i <= n + 1; i++) {
        int k = start + (i % n);
        if (have_off) {
          float mx = (ox + w.x[k]) / 2, my = (oy + w.y[k]) / 2;
          if (i == n + 1) {
            mx = sx;
            my = sy;
          }
          s->quad(s->ctx, ox, oy, mx, my);
        }
        ox = w.x[k];
        oy = w.y[k];
        have_off = 1;
      }
      s->close(s->ctx);
      start = e + 1;
      continue;
    }
    sx = w.x[start + k0];
    sy = w.y[start + k0];
    s->move(s->ctx, sx, sy);
    for (i = 1; i <= n; i++) {
      int k = start + ((k0 + i) % n);
      float x = w.x[k], y = w.y[k];
      int on = w.on[k];
      if (i == n) {
        x = sx;
        y = sy;
        on = 1;
      }
      if (on) {
        if (have_off) s->quad(s->ctx, ox, oy, x, y);
        else s->line(s->ctx, x, y);
        have_off = 0;
      } else if (have_off) {
        float mx = (ox + x) / 2, my = (oy + y) / 2;
        s->quad(s->ctx, ox, oy, mx, my);
        ox = x;
        oy = y;
      } else {
        have_off = 1;
        ox = x;
        oy = y;
      }
    }
    s->close(s->ctx);
    start = e + 1;
  }
  k_free(w.x);
  k_free(w.y);
  k_free(w.on);
  k_free(w.cend);
  return 1;
}

/* ---------- rasterizacao (arestas em pixels, y para baixo) ---------- */
#define SS 4
struct edge {
  float x0, y0, x1, y1;
};
struct raster {
  struct edge* e;
  int n, cap;
  float cx, cy, sx, sy;
  const float* m; /* unidades -> pixels: x' = m0*x + m2*y + m4 ... */
};
static void r_add(struct raster* r, float x0, float y0, float x1, float y1) {
  if (y0 == y1) return;
  if (r->n >= r->cap) {
    int nc = r->cap ? r->cap * 2 : 256;
    struct edge* ne = (struct edge*)wzalloc(sizeof(struct edge) * (size_t)nc);
    if (!ne) return;
    if (r->e) {
      memcpy(ne, r->e, sizeof(struct edge) * (size_t)r->n);
      k_free(r->e);
    }
    r->e = ne;
    r->cap = nc;
  }
  r->e[r->n].x0 = x0;
  r->e[r->n].y0 = y0;
  r->e[r->n].x1 = x1;
  r->e[r->n].y1 = y1;
  r->n++;
}
static void r_xf(const struct raster* r, float x, float y, float* ox, float* oy) {
  *ox = r->m[0] * x + r->m[2] * y + r->m[4];
  *oy = r->m[1] * x + r->m[3] * y + r->m[5];
}
static void r_move(void* c, float x, float y) {
  struct raster* r = (struct raster*)c;
  r_xf(r, x, y, &r->cx, &r->cy);
  r->sx = r->cx;
  r->sy = r->cy;
}
static void r_line(void* c, float x, float y) {
  struct raster* r = (struct raster*)c;
  float nx, ny;
  r_xf(r, x, y, &nx, &ny);
  r_add(r, r->cx, r->cy, nx, ny);
  r->cx = nx;
  r->cy = ny;
}
static void r_quad(void* c, float qx, float qy, float x, float y) {
  struct raster* r = (struct raster*)c;
  float cx, cy, ex, ey, lx = r->cx, ly = r->cy, dx, dy, dev;
  int n, i;
  r_xf(r, qx, qy, &cx, &cy);
  r_xf(r, x, y, &ex, &ey);
  dx = r->cx - 2 * cx + ex;
  dy = r->cy - 2 * cy + ey;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  dev = dx > dy ? dx : dy;
  n = 2;
  while (n < 24 && (float)(n * n) < dev * 4) n++;
  for (i = 1; i <= n; i++) {
    float t = (float)i / (float)n, u = 1 - t;
    float px = u * u * r->cx + 2 * u * t * cx + t * t * ex;
    float py = u * u * r->cy + 2 * u * t * cy + t * t * ey;
    r_add(r, lx, ly, px, py);
    lx = px;
    ly = py;
  }
  r->cx = ex;
  r->cy = ey;
}
static void r_close(void* c) {
  struct raster* r = (struct raster*)c;
  r_add(r, r->cx, r->cy, r->sx, r->sy);
  r->cx = r->sx;
  r->cy = r->sy;
}

void ttf_raster_begin(struct ttf_raster* rs) { memset(rs, 0, sizeof(*rs)); }

/* acrescenta o glifo g com a matriz m (unidades da fonte -> pixels, y para baixo) */
void ttf_raster_add(struct ttf_raster* rs, const struct ttfont* f, uint32_t g, const float* m) {
  struct raster r;
  struct ttf_sink s;
  memset(&r, 0, sizeof(r));
  r.e = (struct edge*)rs->edges;
  r.n = rs->n;
  r.cap = rs->cap;
  r.m = m;
  s.ctx = &r;
  s.move = r_move;
  s.line = r_line;
  s.quad = r_quad;
  s.close = r_close;
  ttf_outline(f, g, &s);
  rs->edges = r.e;
  rs->n = r.n;
  rs->cap = r.cap;
}

static int ifloor(float v) {
  int i = (int)v;
  return (float)i > v ? i - 1 : i;
}
void ttf_raster_bounds(const struct ttf_raster* rs, int* l, int* t, int* r, int* b) {
  const struct edge* e = (const struct edge*)rs->edges;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int i;
  for (i = 0; i < rs->n; i++) {
    float ax = e[i].x0 < e[i].x1 ? e[i].x0 : e[i].x1, bx = e[i].x0 < e[i].x1 ? e[i].x1 : e[i].x0;
    float ay = e[i].y0 < e[i].y1 ? e[i].y0 : e[i].y1, by = e[i].y0 < e[i].y1 ? e[i].y1 : e[i].y0;
    if (!i || ax < x0) x0 = ax;
    if (!i || bx > x1) x1 = bx;
    if (!i || ay < y0) y0 = ay;
    if (!i || by > y1) y1 = by;
  }
  if (!rs->n) {
    *l = *t = *r = *b = 0;
    return;
  }
  *l = ifloor(x0);
  *t = ifloor(y0);
  *r = ifloor(x1) + 1;
  *b = ifloor(y1) + 1;
}

/* cobertura 0..255 no retangulo [l,t)-(r,b), uma linha de w bytes por pixel */
void ttf_raster_fill(const struct ttf_raster* rs, int l, int t, int r, int b, uint8_t* out, int stride, int bpp) {
  const struct edge* e = (const struct edge*)rs->edges;
  int w = r - l, h = b - t, row, i;
  float* xs;
  int* dir;
  uint16_t* acc;
  if (w <= 0 || h <= 0 || !rs->n) return;
  xs = (float*)wzalloc(sizeof(float) * (size_t)rs->n);
  dir = (int*)wzalloc(sizeof(int) * (size_t)rs->n);
  acc = (uint16_t*)wzalloc(sizeof(uint16_t) * (size_t)w);
  if (!xs || !dir || !acc) {
    k_free(xs);
    k_free(dir);
    k_free(acc);
    return;
  }
  for (row = 0; row < h; row++) {
    int sub;
    memset(acc, 0, sizeof(uint16_t) * (size_t)w);
    for (sub = 0; sub < SS; sub++) {
      float yc = (float)(t + row) + ((float)sub + 0.5f) / SS;
      int nx = 0, wind = 0, j;
      for (i = 0; i < rs->n; i++) {
        float y0 = e[i].y0, y1 = e[i].y1, lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
        if (yc < lo || yc >= hi) continue;
        xs[nx] = e[i].x0 + (e[i].x1 - e[i].x0) * (yc - y0) / (y1 - y0);
        dir[nx] = y1 > y0 ? 1 : -1;
        nx++;
      }
      for (i = 1; i < nx; i++) { /* insercao: poucos cruzamentos */
        float v = xs[i];
        int d = dir[i];
        j = i;
        while (j > 0 && xs[j - 1] > v) {
          xs[j] = xs[j - 1];
          dir[j] = dir[j - 1];
          j--;
        }
        xs[j] = v;
        dir[j] = d;
      }
      for (i = 0; i + 1 < nx; i++) {
        wind += dir[i];
        if (!wind) continue;
        { /* amostras horizontais com centro dentro de [xs[i], xs[i+1]) */
          float a = (xs[i] - (float)l) * SS - 0.5f, z = (xs[i + 1] - (float)l) * SS - 0.5f;
          int c0 = ifloor(a) + 1, c1 = ifloor(z) + 1, k;
          if (c0 < 0) c0 = 0;
          if (c1 > w * SS) c1 = w * SS;
          for (k = c0; k < c1; k++) acc[k / SS]++;
        }
      }
    }
    for (i = 0; i < w; i++) {
      int v = acc[i] * 255 / (SS * SS), k;
      uint8_t* px = out + (size_t)row * (size_t)stride + (size_t)i * (size_t)bpp;
      if (v > 255) v = 255;
      for (k = 0; k < bpp; k++) px[k] = (uint8_t)v;
    }
  }
  k_free(xs);
  k_free(dir);
  k_free(acc);
}
void ttf_raster_end(struct ttf_raster* rs) {
  k_free(rs->edges);
  rs->edges = 0;
  rs->n = rs->cap = 0;
}
