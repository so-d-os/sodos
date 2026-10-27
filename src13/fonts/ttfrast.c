/* Le um TTF e desenha uma frase em alfa de 8 bits.
 * fontrender fonte.ttf "frase" pixels saida.raw
 * Na saida padrao: NAME, FULL, VERSION, COPYRIGHT, TRADEMARK. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const uint8_t* file;
static int flen;

static uint16_t ru16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t ri16(const uint8_t* p) { return (int16_t)ru16(p); }
static uint32_t ru32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t* table(const char* tag, uint32_t* len) {
  int n, i;
  if (flen < 12) return 0;
  n = ru16(file + 4);
  for (i = 0; i < n; i++) {
    const uint8_t* r = file + 12 + i * 16;
    if (r + 16 > file + flen) return 0;
    if (memcmp(r, tag, 4) == 0) {
      uint32_t off = ru32(r + 8);
      uint32_t nlen = ru32(r + 12);
      if (off + nlen > (uint32_t)flen) return 0;
      if (len) *len = nlen;
      return file + off;
    }
  }
  return 0;
}

static void name_copy(int want, char* dst, int max) {
  uint32_t nlen = 0;
  const uint8_t* t = table("name", &nlen);
  int count, strOff, i;
  char best[512];
  int bl = 0;
  best[0] = 0;
  if (!dst || max < 2) return;
  dst[0] = 0;
  if (!t || nlen < 6) return;
  count = ru16(t + 2);
  strOff = ru16(t + 4);
  for (i = 0; i < count; i++) {
    const uint8_t* r = t + 6 + i * 12;
    int plat, id, len, o, abs, k, w;
    char tmp[512];
    if (r + 12 > t + nlen) break;
    plat = ru16(r);
    id = ru16(r + 6);
    len = ru16(r + 8);
    o = ru16(r + 10);
    if (id != want) continue;
    abs = strOff + o;
    if (abs < 0 || abs + len > (int)nlen) continue;
    w = 0;
    if (plat == 0 || plat == 3) {
      for (k = 0; k + 1 < len && w < (int)sizeof(tmp) - 1; k += 2) {
        int c = (t[abs + k] << 8) | t[abs + k + 1];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c >= 32 && c < 127) tmp[w++] = (char)c;
        else if (c > 127 && c < 256) tmp[w++] = (char)c;
      }
    } else {
      for (k = 0; k < len && w < (int)sizeof(tmp) - 1; k++) {
        unsigned char c = t[abs + k];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c >= 32) tmp[w++] = (char)c;
      }
    }
    tmp[w] = 0;
    if (w > bl) {
      bl = w;
      memcpy(best, tmp, (size_t)w + 1);
    }
  }
  if (bl >= max) bl = max - 1;
  memcpy(dst, best, (size_t)bl);
  dst[bl] = 0;
}

static const uint8_t* cmap4;
static const uint8_t* glyf;
static const uint8_t* loca;
static const uint8_t* hmtx;
static int loca_long;
static int upem;
static int ascent;
static int nhmtx;

static int cmap_init(void) {
  uint32_t nlen = 0;
  const uint8_t* t = table("cmap", &nlen);
  int n, i;
  if (!t) return -1;
  n = ru16(t + 2);
  for (i = 0; i < n; i++) {
    const uint8_t* r = t + 4 + i * 8;
    uint32_t off;
    const uint8_t* s;
    if (r + 8 > t + nlen) return -1;
    off = ru32(r + 4);
    if (off + 4 > nlen) continue;
    s = t + off;
    if (ru16(s) == 4) {
      cmap4 = s;
      return 0;
    }
  }
  return -1;
}

static int glyph_id(int c) {
  int segX2, seg, i;
  const uint8_t* endc;
  const uint8_t* startc;
  const uint8_t* delta;
  const uint8_t* roff;
  if (!cmap4) return 0;
  segX2 = ru16(cmap4 + 6);
  seg = segX2 / 2;
  endc = cmap4 + 14;
  startc = endc + segX2 + 2;
  delta = startc + segX2;
  roff = delta + segX2;
  for (i = 0; i < seg; i++) {
    int end = ru16(endc + i * 2);
    int start, d, r;
    if (c > end) continue;
    start = ru16(startc + i * 2);
    if (c < start) return 0;
    d = ri16(delta + i * 2);
    r = ru16(roff + i * 2);
    if (r == 0) return (c + d) & 0xFFFF;
    {
      const uint8_t* p = roff + i * 2 + r + (c - start) * 2;
      int g;
      if (p + 2 > file + flen) return 0;
      g = ru16(p);
      if (g == 0) return 0;
      return (g + d) & 0xFFFF;
    }
  }
  return 0;
}

static uint32_t glyph_off(int g) {
  if (loca_long) return ru32(loca + g * 4);
  return (uint32_t)ru16(loca + g * 2) * 2u;
}

#define MAXP 2048
#define MAXE 4096
static int px[MAXP], py[MAXP], pon[MAXP], np;
static int ex0[MAXE], ey0[MAXE], ex1[MAXE], ey1[MAXE], ne;
static uint8_t* img;
static int img_w, img_h;
static int pen_x;

static void add_edge(int x0, int y0, int x1, int y1) {
  if (ne >= MAXE || y0 == y1) return;
  ex0[ne] = x0;
  ey0[ne] = y0;
  ex1[ne] = x1;
  ey1[ne] = y1;
  ne++;
}

static void quad(int x0, int y0, int x1, int y1, int x2, int y2) {
  int i;
  int lx = x0, ly = y0;
  for (i = 1; i <= 8; i++) {
    int t = i;
    int u = 8 - t;
    int a = u * u;
    int b = 2 * u * t;
    int c = t * t;
    int x = (a * x0 + b * x1 + c * x2) / 64;
    int y = (a * y0 + b * y1 + c * y2) / 64;
    add_edge(lx, ly, x, y);
    lx = x;
    ly = y;
  }
}

static void flatten(int s, int e) {
  int i, n, start_on;
  int cx, cy;
  if (e < s) return;
  n = e - s + 1;
  start_on = -1;
  for (i = 0; i < n; i++)
    if (pon[s + i]) {
      start_on = i;
      break;
    }
  if (start_on < 0) return;
  cx = px[s + start_on];
  cy = py[s + start_on];
  {
    int sx = cx, sy = cy;
    int prev_off = 0, ox = 0, oy = 0;
    for (i = 1; i <= n; i++) {
      int k = s + ((start_on + i) % n);
      int x = px[k], y = py[k], on = pon[k];
      if (i == n) {
        x = sx;
        y = sy;
        on = 1;
      }
      if (on) {
        if (prev_off) quad(cx, cy, ox, oy, x, y);
        else add_edge(cx, cy, x, y);
        cx = x;
        cy = y;
        prev_off = 0;
      } else if (prev_off) {
        int mx = (ox + x) / 2;
        int my = (oy + y) / 2;
        quad(cx, cy, ox, oy, mx, my);
        cx = mx;
        cy = my;
        ox = x;
        oy = y;
      } else {
        prev_off = 1;
        ox = x;
        oy = y;
      }
    }
  }
}

static int sc(int v) {
  /* chamado com escala ja aplicada nos pontos */
  (void)v;
  return v;
}

static void fill_glyph(void) {
  int y, i, k;
  int ys = 0, ye = img_h;
  for (y = ys; y < ye; y++) {
    int xs[512];
    int nx = 0;
    int mid = y * 256 + 128;
    for (i = 0; i < ne; i++) {
      int y0 = ey0[i], y1 = ey1[i];
      int x0 = ex0[i], x1 = ex1[i];
      int ya, yb, xa, xb;
      if (y0 == y1) continue;
      if (y0 < y1) {
        ya = y0;
        yb = y1;
        xa = x0;
        xb = x1;
      } else {
        ya = y1;
        yb = y0;
        xa = x1;
        xb = x0;
      }
      if (mid < ya || mid >= yb) continue;
      if (nx < 512) {
        xs[nx++] = xa + (int)((long long)(xb - xa) * (mid - ya) / (yb - ya));
      }
    }
    for (i = 1; i < nx; i++) {
      int v = xs[i];
      k = i;
      while (k > 0 && xs[k - 1] > v) {
        xs[k] = xs[k - 1];
        k--;
      }
      xs[k] = v;
    }
    for (i = 0; i + 1 < nx; i += 2) {
      int a = xs[i] >> 8;
      int b = xs[i + 1] >> 8;
      if (a < 0) a = 0;
      if (b > img_w) b = img_w;
      for (k = a; k < b; k++) img[y * img_w + k] = 255;
    }
  }
}

static int draw_glyph(int g, int depth);

static int draw_simple(const uint8_t* g, int glen, int dx, int dy) {
  int ncont, i, npts, inst, fi, x, y;
  const uint8_t* p;
  const uint8_t* ends;
  if (glen < 10) return 0;
  ncont = ri16(g);
  if (ncont <= 0 || ncont > 64) return 0;
  ends = g + 10;
  if (10 + ncont * 2 + 2 > glen) return 0;
  npts = ru16(ends + (ncont - 1) * 2) + 1;
  if (npts <= 0 || npts > MAXP) return 0;
  p = ends + ncont * 2;
  inst = ru16(p);
  p += 2 + inst;
  if (p > g + glen) return 0;
  np = 0;
  {
    uint8_t flags[MAXP];
    int repeat = 0, fr = 0;
    for (i = 0; i < npts; i++) {
      if (repeat) {
        flags[i] = (uint8_t)fr;
        repeat--;
      } else {
        if (p >= g + glen) return 0;
        flags[i] = *p++;
        if (flags[i] & 8) {
          if (p >= g + glen) return 0;
          repeat = *p++;
          fr = flags[i];
        }
      }
    }
    x = 0;
    for (i = 0; i < npts; i++) {
      int f = flags[i];
      if (f & 2) {
        if (p >= g + glen) return 0;
        x += (f & 16) ? *p++ : -*p++;
      } else if (!(f & 16)) {
        if (p + 2 > g + glen) return 0;
        x += ri16(p);
        p += 2;
      }
      px[i] = x;
    }
    y = 0;
    for (i = 0; i < npts; i++) {
      int f = flags[i];
      if (f & 4) {
        if (p >= g + glen) return 0;
        y += (f & 32) ? *p++ : -*p++;
      } else if (!(f & 32)) {
        if (p + 2 > g + glen) return 0;
        y += ri16(p);
        p += 2;
      }
      py[i] = y;
      pon[i] = (f & 1) ? 1 : 0;
    }
  }
  ne = 0;
  fi = 0;
  for (i = 0; i < ncont; i++) {
    int e = ru16(ends + i * 2);
    flatten(fi, e);
    fi = e + 1;
  }
  for (i = 0; i < ne; i++) {
    ex0[i] = (ex0[i] + dx) * 256;
    ey0[i] = ey0[i] + dy;
    ex1[i] = (ex1[i] + dx) * 256;
    ey1[i] = ey1[i] + dy;
  }
  fill_glyph();
  (void)sc;
  return 1;
}

static int draw_glyph(int g, int depth) {
  uint32_t off, next, glen;
  const uint8_t* gp;
  int ncont;
  if (g <= 0 || depth > 4) return 0;
  off = glyph_off(g);
  next = glyph_off(g + 1);
  if (next < off) return 0;
  glen = next - off;
  if (glen == 0) return 1;
  if (glyf + off + glen > file + flen) return 0;
  gp = glyf + off;
  if (glen < 10) return 0;
  ncont = ri16(gp);
  if (ncont < 0) {
    const uint8_t* p = gp + 10;
    int more = 1;
    while (more) {
      int flags, comp, a1, a2;
      if (p + 4 > gp + glen) break;
      flags = ru16(p);
      comp = ru16(p + 2);
      p += 4;
      if (flags & 1) {
        if (p + 4 > gp + glen) break;
        a1 = ri16(p);
        a2 = ri16(p + 2);
        p += 4;
      } else {
        if (p + 2 > gp + glen) break;
        a1 = (int8_t)p[0];
        a2 = (int8_t)p[1];
        p += 2;
      }
      if (flags & 8) p += 2;
      else if (flags & 64) p += 4;
      else if (flags & 128) p += 8;
      if (!(flags & 2)) {
        a1 = 0;
        a2 = 0;
      }
      draw_glyph(comp, depth + 1);
      (void)a1;
      (void)a2;
      more = flags & 32;
    }
    return 1;
  }
  return draw_simple(gp, (int)glen, 0, 0);
}

/* Pontos ja em pixels. Recalcula a escala dentro de draw via transform. */
static int ppem_now;
static int dy_base;
static int pen_y;

static void scale_edges(void) {
  int i;
  for (i = 0; i < ne; i++) {
    int x0 = (int)(((long long)ex0[i] * ppem_now) / upem) + pen_x;
    int y0 = dy_base - (int)(((long long)ey0[i] * ppem_now) / upem) - pen_y;
    int x1 = (int)(((long long)ex1[i] * ppem_now) / upem) + pen_x;
    int y1 = dy_base - (int)(((long long)ey1[i] * ppem_now) / upem) - pen_y;
    ex0[i] = x0 << 8;
    ey0[i] = y0 << 8;
    ex1[i] = x1 << 8;
    ey1[i] = y1 << 8;
  }
}

/* Reimplementa o desenho simples com escala correta. draw_simple acima
 * gravou fu em ex e depois *256 sem escala. Vou refazer o caminho. */

static int emit_simple(const uint8_t* g, int glen) {
  int ncont, i, npts, inst;
  const uint8_t* p;
  const uint8_t* ends;
  uint8_t flags[MAXP];
  int x, y, repeat, fr;
  if (glen < 10) return 0;
  ncont = ri16(g);
  if (ncont <= 0 || ncont > 64) return 0;
  ends = g + 10;
  npts = ru16(ends + (ncont - 1) * 2) + 1;
  if (npts <= 0 || npts > MAXP) return 0;
  p = ends + ncont * 2;
  if (p + 2 > g + glen) return 0;
  inst = ru16(p);
  p += 2 + inst;
  repeat = 0;
  fr = 0;
  for (i = 0; i < npts; i++) {
    if (repeat) {
      flags[i] = (uint8_t)fr;
      repeat--;
    } else {
      if (p >= g + glen) return 0;
      flags[i] = *p++;
      if (flags[i] & 8) {
        if (p >= g + glen) return 0;
        repeat = *p++;
        fr = flags[i];
      }
    }
  }
  x = 0;
  for (i = 0; i < npts; i++) {
    int f = flags[i];
    if (f & 2) {
      if (p >= g + glen) return 0;
      x += (f & 16) ? *p++ : -*p++;
    } else if (!(f & 16)) {
      if (p + 2 > g + glen) return 0;
      x += ri16(p);
      p += 2;
    }
    px[i] = x;
    pon[i] = (f & 1) ? 1 : 0;
  }
  y = 0;
  for (i = 0; i < npts; i++) {
    int f = flags[i];
    if (f & 4) {
      if (p >= g + glen) return 0;
      y += (f & 32) ? *p++ : -*p++;
    } else if (!(f & 32)) {
      if (p + 2 > g + glen) return 0;
      y += ri16(p);
      p += 2;
    }
    py[i] = y;
  }
  ne = 0;
  {
    int fi = 0;
    for (i = 0; i < ncont; i++) {
      int e = ru16(ends + i * 2);
      flatten(fi, e);
      fi = e + 1;
    }
  }
  scale_edges();
  fill_glyph();
  return 1;
}

static int emit_glyph(int g, int depth, int ox, int oy) {
  uint32_t off, next;
  int glen, ncont;
  const uint8_t* gp;
  if (depth > 4) return 0;
  off = glyph_off(g);
  next = glyph_off(g + 1);
  if (next <= off) return 1;
  glen = (int)(next - off);
  if (glyf + off + glen > file + flen || glen < 10) return 0;
  gp = glyf + off;
  ncont = ri16(gp);
  if (ncont < 0) {
    const uint8_t* p = gp + 10;
    int more = 1;
    while (more) {
      int flags, comp, a1 = 0, a2 = 0;
      if (p + 4 > gp + glen) break;
      flags = ru16(p);
      comp = ru16(p + 2);
      p += 4;
      if (flags & 1) {
        if (p + 4 > gp + glen) break;
        a1 = ri16(p);
        a2 = ri16(p + 2);
        p += 4;
      } else {
        if (p + 2 > gp + glen) break;
        a1 = (int8_t)p[0];
        a2 = (int8_t)p[1];
        p += 2;
      }
      if (flags & 8) p += 2;
      else if (flags & 64) p += 4;
      else if (flags & 128) p += 8;
      if (flags & 2) {
        int sx = (int)(((long long)a1 * ppem_now) / upem);
        int sy = (int)(((long long)a2 * ppem_now) / upem);
        pen_x += sx;
        pen_y += sy;
        emit_glyph(comp, depth + 1, ox, oy);
        pen_x -= sx;
        pen_y -= sy;
      } else {
        emit_glyph(comp, depth + 1, ox, oy);
      }
      more = flags & 32;
    }
    return 1;
  }
  (void)ox;
  (void)oy;
  return emit_simple(gp, glen);
}

static int advance_of(int g) {
  int aw;
  if (g < nhmtx) aw = ru16(hmtx + g * 4);
  else aw = ru16(hmtx + (nhmtx - 1) * 4);
  return (int)(((long long)aw * ppem_now) / upem);
}

static int load_font(void) {
  uint32_t n = 0;
  const uint8_t* head = table("head", &n);
  const uint8_t* hhea;
  if (!head || n < 54) return -1;
  upem = ru16(head + 18);
  loca_long = ri16(head + 50);
  if (upem < 16) return -1;
  hhea = table("hhea", &n);
  if (!hhea || n < 36) return -1;
  ascent = ri16(hhea + 4);
  nhmtx = ru16(hhea + 34);
  if (nhmtx < 1) return -1;
  glyf = table("glyf", &n);
  loca = table("loca", &n);
  hmtx = table("hmtx", &n);
  if (!glyf || !loca || !hmtx) return -1;
  return cmap_init();
}

static int utf8_next(const char* s, int* i) {
  unsigned char c = (unsigned char)s[*i];
  if (c == 0) return -1;
  if (c < 0x80) {
    (*i)++;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && s[*i + 1]) {
    int cp = ((c & 0x1F) << 6) | ((unsigned char)s[*i + 1] & 0x3F);
    *i += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && s[*i + 1] && s[*i + 2]) {
    int cp = ((c & 0x0F) << 12) | (((unsigned char)s[*i + 1] & 0x3F) << 6) |
             ((unsigned char)s[*i + 2] & 0x3F);
    *i += 3;
    return cp;
  }
  (*i)++;
  return c;
}

int ttf_phrase(const uint8_t* data, int data_len, const char* phrase, int pxsize,
               uint8_t** rgba, int* out_w, int* out_h,
               char* name, int name_n, char* full, int full_n,
               char* ver, int ver_n, char* copyr, int copy_n,
               char* tm, int tm_n) {
  int i;
  const char* text;
  int ow, oh, y, x;
  uint8_t* outb;
  uint8_t* pix;
  if (rgba) *rgba = 0;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (name && name_n > 0) name[0] = 0;
  if (full && full_n > 0) full[0] = 0;
  if (ver && ver_n > 0) ver[0] = 0;
  if (copyr && copy_n > 0) copyr[0] = 0;
  if (tm && tm_n > 0) tm[0] = 0;
  if (!data || data_len < 12 || !phrase || !phrase[0]) return 1;
  file = data;
  flen = data_len;
  name_copy(1, name, name_n);
  name_copy(4, full, full_n);
  name_copy(5, ver, ver_n);
  name_copy(0, copyr, copy_n);
  name_copy(7, tm, tm_n);
  if (load_font() != 0) return 2;
  if (pxsize < 8) pxsize = 8;
  if (pxsize > 64) pxsize = 64;
  text = phrase;
  /* Desenha em dobro e reduz, para o contorno nao ficar serrilhado. */
  ppem_now = pxsize * 2;
  img_h = ppem_now + ppem_now / 2 + 16;
  img_w = 16;
  for (i = 0; text[i]; i++) img_w += ppem_now;
  if (img_w < 64) img_w = 64;
  if (img_w > 4096) img_w = 4096;
  if (img_h < 2) img_h = 2;
  img_w &= ~1;
  img_h &= ~1;
  img = (uint8_t*)calloc((size_t)img_w * (size_t)img_h, 1);
  if (!img) return 1;
  dy_base = (int)(((long long)ascent * ppem_now) / upem) + 4;
  pen_x = 4;
  i = 0;
  while (text[i] && pen_x < img_w - 4) {
    int c = utf8_next(text, &i);
    int g, adv;
    if (c < 0 || c == '\n') continue;
    g = glyph_id(c);
    if (g) emit_glyph(g, 0, 0, 0);
    adv = advance_of(g);
    if (adv < ppem_now / 5) adv = c == ' ' ? ppem_now / 3 : ppem_now / 2;
    pen_x += adv;
  }
  ow = img_w / 2;
  oh = img_h / 2;
  outb = (uint8_t*)malloc((size_t)ow * (size_t)oh);
  if (!outb) {
    free(img);
    img = 0;
    return 1;
  }
  for (y = 0; y < oh; y++) {
    for (x = 0; x < ow; x++) {
      int s = img[(y * 2) * img_w + x * 2] + img[(y * 2) * img_w + x * 2 + 1] +
              img[(y * 2 + 1) * img_w + x * 2] + img[(y * 2 + 1) * img_w + x * 2 + 1];
      outb[y * ow + x] = (uint8_t)(s / 4);
    }
  }
  free(img);
  img = 0;
  pix = (uint8_t*)malloc((size_t)ow * (size_t)oh * 4);
  if (!pix) {
    free(outb);
    return 1;
  }
  for (y = 0; y < oh; y++) {
    for (x = 0; x < ow; x++) {
      int a = outb[y * ow + x];
      uint8_t* p = pix + ((size_t)y * (size_t)ow + (size_t)x) * 4;
      p[0] = (uint8_t)(0x26 + (255 - 0x26) * a / 255);
      p[1] = (uint8_t)(0x1C + (255 - 0x1C) * a / 255);
      p[2] = (uint8_t)(0x23 + (255 - 0x23) * a / 255);
      p[3] = 255;
    }
  }
  free(outb);
  if (rgba) *rgba = pix;
  else free(pix);
  if (out_w) *out_w = ow;
  if (out_h) *out_h = oh;
  return 0;
}
