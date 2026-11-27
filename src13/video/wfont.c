/* wfont.c - fonte TrueType do WIN (ver wfont.h).
 *
 * Caminho de cada letra: cmap (formato 4) -> indice do glifo -> contornos do
 * glyf (simples ou compostos, so deslocamento) -> pontos em pixels * 4 * 256
 * -> curvas quadraticas viram retas -> varredura por linha de amostra com
 * enrolamento nao nulo -> 4x4 amostras somadas por pixel = alfa.
 * Sem ponto flutuante e sem libc (o kernel nao tem memset/memcpy). */
#include "wfont.h"

#define SS 4            /* amostras por pixel em cada eixo */
#define FIX 256         /* fracao das coordenadas */
#define MAXP 1536       /* pontos por glifo (compostos somados) */
#define MAXC 128        /* contornos por glifo */
#define MAXE 6144       /* retas por glifo */
#define MAXX 256        /* cruzamentos por linha de amostra */
#define FIRST 32
/* O kernel desenha so ASCII; o WINE.DLL compila com WFONT_LAST=255 (Latin-1). */
#ifndef WFONT_LAST
#define LAST 126
#else
#define LAST WFONT_LAST
#endif
#define NGLYPH (LAST - FIRST + 1)

/* ---- leitura do arquivo ---- */
static const uint8_t* tf;
static int tflen;
static const uint8_t *t_cmap4, *t_glyf, *t_loca, *t_hmtx;
static uint32_t t_glyf_len, t_loca_len, t_hmtx_len, t_cmap4_len;
static int t_upem, t_loca_long, t_nhmtx, t_asc, t_desc;

static uint32_t ru16(const uint8_t* p) { return (uint32_t)((p[0] << 8) | p[1]); }
static int ri16(const uint8_t* p) { return (int)(int16_t)ru16(p); }
static uint32_t ru32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t* table(const char* tag, uint32_t* len) {
  int n, i;
  if (tflen < 12) return 0;
  n = (int)ru16(tf + 4);
  for (i = 0; i < n; i++) {
    const uint8_t* r = tf + 12 + i * 16;
    uint32_t off, l;
    if (r + 16 > tf + tflen) return 0;
    if (r[0] != (uint8_t)tag[0] || r[1] != (uint8_t)tag[1] || r[2] != (uint8_t)tag[2] ||
        r[3] != (uint8_t)tag[3])
      continue;
    off = ru32(r + 8);
    l = ru32(r + 12);
    if (off > (uint32_t)tflen || l > (uint32_t)tflen - off) return 0;
    *len = l;
    return tf + off;
  }
  return 0;
}

static int font_open(void) {
  uint32_t n = 0, cn = 0;
  const uint8_t* head = table("head", &n);
  const uint8_t* hhea;
  const uint8_t* cmap;
  int i, nt;
  if (!head || n < 54) return -1;
  t_upem = (int)ru16(head + 18);
  t_loca_long = ri16(head + 50);
  if (t_upem < 16 || t_upem > 16384) return -1;
  hhea = table("hhea", &n);
  if (!hhea || n < 36) return -1;
  t_asc = ri16(hhea + 4);
  t_desc = ri16(hhea + 6);
  t_nhmtx = (int)ru16(hhea + 34);
  if (t_nhmtx < 1 || t_asc <= 0) return -1;
  t_glyf = table("glyf", &t_glyf_len);
  t_loca = table("loca", &t_loca_len);
  t_hmtx = table("hmtx", &t_hmtx_len);
  if (!t_glyf || !t_loca || !t_hmtx || t_hmtx_len < 4) return -1;
  cmap = table("cmap", &cn);
  if (!cmap || cn < 4) return -1;
  t_cmap4 = 0;
  nt = (int)ru16(cmap + 2);
  for (i = 0; i < nt; i++) {
    const uint8_t* r = cmap + 4 + i * 8;
    uint32_t off;
    if ((uint32_t)(4 + i * 8 + 8) > cn) break;
    off = ru32(r + 4);
    if (off + 8 > cn || ru16(cmap + off) != 4) continue;
    t_cmap4 = cmap + off;
    t_cmap4_len = cn - off;
    if (ru16(r) == 3 && ru16(r + 2) == 1) break;   /* Windows Unicode: o preferido */
  }
  return t_cmap4 ? 0 : -1;
}

static int glyph_id(int c) {
  int segx2, seg, i;
  const uint8_t *endc, *startc, *delta, *roff;
  if (!t_cmap4 || t_cmap4_len < 16) return 0;
  segx2 = (int)ru16(t_cmap4 + 6);
  seg = segx2 / 2;
  if ((uint32_t)(16 + segx2 * 4) > t_cmap4_len) return 0;
  endc = t_cmap4 + 14;
  startc = endc + segx2 + 2;
  delta = startc + segx2;
  roff = delta + segx2;
  for (i = 0; i < seg; i++) {
    int end = (int)ru16(endc + i * 2), start, d, r;
    if (c > end) continue;
    start = (int)ru16(startc + i * 2);
    if (c < start) return 0;
    d = ri16(delta + i * 2);
    r = (int)ru16(roff + i * 2);
    if (r == 0) return (c + d) & 0xFFFF;
    {
      const uint8_t* p = roff + i * 2 + r + (c - start) * 2;
      int g;
      if (p < t_cmap4 || p + 2 > t_cmap4 + t_cmap4_len) return 0;
      g = (int)ru16(p);
      return g ? ((g + d) & 0xFFFF) : 0;
    }
  }
  return 0;
}

/* [off, end) do glifo g no glyf; 0 = vazio ou invalido. */
static int glyph_span(int g, uint32_t* off, uint32_t* end) {
  uint32_t a, b;
  if (g < 0) return 0;
  if (t_loca_long) {
    if ((uint32_t)(g + 2) * 4u > t_loca_len) return 0;
    a = ru32(t_loca + g * 4);
    b = ru32(t_loca + g * 4 + 4);
  } else {
    if ((uint32_t)(g + 2) * 2u > t_loca_len) return 0;
    a = ru16(t_loca + g * 2) * 2u;
    b = ru16(t_loca + g * 2 + 2) * 2u;
  }
  if (b <= a || b > t_glyf_len) return 0;
  *off = a;
  *end = b;
  return 1;
}

static int advance_fu(int g) {
  int k = g < t_nhmtx ? g : t_nhmtx - 1;
  if ((uint32_t)(k * 4 + 2) > t_hmtx_len) return 0;
  return (int)ru16(t_hmtx + k * 4);
}

/* ---- contornos do glifo (unidades da fonte) ---- */
struct work {
  int px[MAXP], py[MAXP];
  uint8_t on[MAXP];
  int np;
  int cend[MAXC];   /* ultimo ponto de cada contorno */
  int nc;
  int ex0[MAXE], ey0[MAXE], ex1[MAXE], ey1[MAXE];
  int ne;
  int xs[MAXX], xd[MAXX];
};
static struct work* W;

static int load_simple(const uint8_t* g, int glen, int dx, int dy) {
  int ncont = ri16(g), i, npts, inst, base = W->np, x, y, rep = 0, fr = 0;
  const uint8_t* ends = g + 10;
  const uint8_t* p;
  const uint8_t* lim = g + glen;
  if (ncont <= 0) return 1;
  if (W->nc + ncont > MAXC || 10 + ncont * 2 + 2 > glen) return 0;
  npts = (int)ru16(ends + (ncont - 1) * 2) + 1;
  if (npts <= 0 || base + npts > MAXP) return 0;
  for (i = 0; i < ncont; i++) {
    int e = (int)ru16(ends + i * 2);
    if (e >= npts) return 0;
    W->cend[W->nc + i] = base + e;
  }
  p = ends + ncont * 2;
  inst = (int)ru16(p);
  p += 2 + inst;
  for (i = 0; i < npts; i++) {   /* bandeiras, guardadas em on[] por enquanto */
    if (rep) {
      W->on[base + i] = (uint8_t)fr;
      rep--;
      continue;
    }
    if (p >= lim) return 0;
    W->on[base + i] = *p++;
    if (W->on[base + i] & 8) {
      if (p >= lim) return 0;
      rep = *p++;
      fr = W->on[base + i];
    }
  }
  x = 0;
  for (i = 0; i < npts; i++) {
    int f = W->on[base + i];
    if (f & 2) {
      if (p >= lim) return 0;
      x += (f & 16) ? *p : -*p;
      p++;
    } else if (!(f & 16)) {
      if (p + 2 > lim) return 0;
      x += ri16(p);
      p += 2;
    }
    W->px[base + i] = x + dx;
  }
  y = 0;
  for (i = 0; i < npts; i++) {
    int f = W->on[base + i];
    if (f & 4) {
      if (p >= lim) return 0;
      y += (f & 32) ? *p : -*p;
      p++;
    } else if (!(f & 32)) {
      if (p + 2 > lim) return 0;
      y += ri16(p);
      p += 2;
    }
    W->py[base + i] = y + dy;
    W->on[base + i] = (uint8_t)(f & 1);
  }
  W->np = base + npts;
  W->nc += ncont;
  return 1;
}

static int load_glyph(int g, int dx, int dy, int depth) {
  uint32_t off, end;
  const uint8_t* gp;
  int glen;
  if (depth > 6) return 0;
  if (!glyph_span(g, &off, &end)) return 1;   /* espaco: sem contorno */
  gp = t_glyf + off;
  glen = (int)(end - off);
  if (glen < 10) return 1;
  if (ri16(gp) >= 0) return load_simple(gp, glen, dx, dy);
  {
    const uint8_t* p = gp + 10;
    const uint8_t* lim = gp + glen;
    int more = 1;
    while (more) {
      int flags, comp, a1, a2;
      if (p + 4 > lim) return 0;
      flags = (int)ru16(p);
      comp = (int)ru16(p + 2);
      p += 4;
      if (flags & 1) {
        if (p + 4 > lim) return 0;
        a1 = ri16(p);
        a2 = ri16(p + 2);
        p += 4;
      } else {
        if (p + 2 > lim) return 0;
        a1 = (int8_t)p[0];
        a2 = (int8_t)p[1];
        p += 2;
      }
      {
        /* escala ou matriz 2x2 (F2Dot14): x' = a*x + c*y, y' = b*x + d*y.
         * Antes era ignorada: a aspa ” do Noto Mono (a “ girada) sumia. */
        int sa = 16384, sb = 0, sc = 0, sd = 16384, base = W->np, k;
        if (flags & 8) {
          if (p + 2 > lim) return 0;
          sa = sd = ri16(p);
          p += 2;
        } else if (flags & 64) {
          if (p + 4 > lim) return 0;
          sa = ri16(p);
          sd = ri16(p + 2);
          p += 4;
        } else if (flags & 128) {
          if (p + 8 > lim) return 0;
          sa = ri16(p);
          sb = ri16(p + 2);
          sc = ri16(p + 4);
          sd = ri16(p + 6);
          p += 8;
        }
        if (!(flags & 2)) a1 = a2 = 0;    /* pontos casados: nao usado */
        if (sa == 16384 && sb == 0 && sc == 0 && sd == 16384) {
          if (!load_glyph(comp, dx + a1, dy + a2, depth + 1)) return 0;
        } else {
          if (!load_glyph(comp, 0, 0, depth + 1)) return 0;
          for (k = base; k < W->np; k++) {
            int x = W->px[k], y = W->py[k];
            W->px[k] = (int)(((long long)x * sa + (long long)y * sc) / 16384) + dx + a1;
            W->py[k] = (int)(((long long)x * sb + (long long)y * sd) / 16384) + dy + a2;
          }
        }
      }
      more = flags & 32;
    }
  }
  return 1;
}

/* ---- retas em coordenadas de amostra (x direita, y baixo, * FIX) ---- */
static void add_edge(int x0, int y0, int x1, int y1) {
  if (y0 == y1 || W->ne >= MAXE) return;
  W->ex0[W->ne] = x0;
  W->ey0[W->ne] = y0;
  W->ex1[W->ne] = x1;
  W->ey1[W->ne] = y1;
  W->ne++;
}

static void add_quad(int x0, int y0, int cx, int cy, int x1, int y1) {
  /* passos conforme o tamanho da curva em amostras */
  int dx = (x0 - 2 * cx + x1), dy = (y0 - 2 * cy + y1);
  int dev, n, i, lx = x0, ly = y0;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  dev = (dx > dy ? dx : dy) / FIX;   /* desvio ~ |p0 - 2c + p1| / 4 */
  n = 2;
  while (n < 16 && n * n < dev * 2) n++;
  for (i = 1; i <= n; i++) {
    long long t = i, u = n - i, nn = (long long)n * n;
    int x = (int)((u * u * x0 + 2 * u * t * cx + t * t * x1) / nn);
    int y = (int)((u * u * y0 + 2 * u * t * cy + t * t * y1) / nn);
    add_edge(lx, ly, x, y);
    lx = x;
    ly = y;
  }
}

/* Contorno [s, e] ja convertido para amostras em px[]/py[]. */
static void flatten(int s, int e) {
  int n = e - s + 1, i, k0 = -1;
  int cx, cy, sx, sy, have_off = 0, ox = 0, oy = 0;
  if (n < 2) return;
  for (i = 0; i < n; i++)
    if (W->on[s + i]) {
      k0 = i;
      break;
    }
  if (k0 < 0) {
    /* so pontos fora da curva: comeca no meio dos dois primeiros */
    sx = (W->px[s] + W->px[s + 1]) / 2;
    sy = (W->py[s] + W->py[s + 1]) / 2;
    k0 = 0;
    have_off = 0;
    cx = sx;
    cy = sy;
    for (i = 1; i <= n; i++) {
      int k = s + (i % n);
      int x = W->px[k], y = W->py[k];
      if (have_off) {
        int mx = (ox + x) / 2, my = (oy + y) / 2;
        add_quad(cx, cy, ox, oy, mx, my);
        cx = mx;
        cy = my;
      }
      ox = x;
      oy = y;
      have_off = 1;
    }
    add_quad(cx, cy, ox, oy, sx, sy);
    return;
  }
  sx = cx = W->px[s + k0];
  sy = cy = W->py[s + k0];
  for (i = 1; i <= n; i++) {
    int k = s + ((k0 + i) % n);
    int x = W->px[k], y = W->py[k], on = W->on[k];
    if (i == n) {
      x = sx;
      y = sy;
      on = 1;
    }
    if (on) {
      if (have_off) add_quad(cx, cy, ox, oy, x, y);
      else add_edge(cx, cy, x, y);
      cx = x;
      cy = y;
      have_off = 0;
    } else if (have_off) {
      int mx = (ox + x) / 2, my = (oy + y) / 2;
      add_quad(cx, cy, ox, oy, mx, my);
      cx = mx;
      cy = my;
      ox = x;
      oy = y;
    } else {
      have_off = 1;
      ox = x;
      oy = y;
    }
  }
}

static int floor_div(long long a, int b) {
  long long q = a / b;
  if ((a % b) && ((a < 0) != (b < 0))) q--;
  return (int)q;
}

/* ---- tabela pronta ---- */
static struct wfont_glyph G[NGLYPH];
static uint8_t* pool;
static wfont_free_fn pool_free;
static int f_px, f_asc, f_desc, f_ready;

#ifdef WFONT_CP1252
/* NOTEPAD.EXE: 0x80..0x9F sao as letras extras do Windows-1252 (aspas
 * curvas, travessao, reticencias, euro...), nao os controles C1 do Unicode. */
static const uint16_t cp1252_hi[32] = {
  0x20AC, 0x003F, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x003F, 0x017D, 0x003F,
  0x003F, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x003F, 0x017E, 0x0178,
};
#endif

/* Desenha o glifo do codigo c em G[c - FIRST]; mascara em *dst (avanca). */
static int rasterize(int c, int px, uint8_t** dst, uint8_t* dst_end) {
  struct wfont_glyph* gl = &G[c - FIRST];
#ifdef WFONT_CP1252
  int g = glyph_id(c >= 0x80 && c < 0xA0 ? cp1252_hi[c - 0x80] : c);
#else
  int g = glyph_id(c);
#endif
  long long k = (long long)px * FIX;   /* pixels*FIX por unidade = k / upem */
  int i, minx, maxx, miny, maxy, left, right, top, bot, w, h, r;
  uint8_t* a;
  gl->adv = (int32_t)(((long long)advance_fu(g) * k + t_upem / 2) / t_upem);
  gl->left = gl->top = gl->w = gl->h = 0;
  gl->a = 0;
  W->np = W->nc = W->ne = 0;
  /* sem contorno (espaco, letra que a fonte nao tem, glifo estragado): so avanca */
  if (!load_glyph(g, 0, 0, 0) || W->np == 0) return 1;
  /* unidades -> pixels*FIX (y para cima) */
  minx = maxx = miny = maxy = 0;
  for (i = 0; i < W->np; i++) {
    int x = (int)(W->px[i] * k / t_upem), y = (int)(W->py[i] * k / t_upem);
    W->px[i] = x;
    W->py[i] = y;
    if (i == 0 || x < minx) minx = x;
    if (i == 0 || x > maxx) maxx = x;
    if (i == 0 || y < miny) miny = y;
    if (i == 0 || y > maxy) maxy = y;
  }
  left = floor_div(minx, FIX);
  right = floor_div(maxx + FIX - 1, FIX);
  bot = floor_div(miny, FIX);
  top = floor_div(maxy + FIX - 1, FIX);
  w = right - left;
  h = top - bot;
  if (w <= 0 || h <= 0 || w > 4 * px + 8 || h > 4 * px + 8) return 1;
  if (*dst + w * h > dst_end) return 0;
  /* pixels*FIX -> amostras*FIX, origem no canto de cima da mascara */
  for (i = 0; i < W->np; i++) {
    W->px[i] = (W->px[i] - left * FIX) * SS;
    W->py[i] = (top * FIX - W->py[i]) * SS;
  }
  {
    int s = 0;
    for (i = 0; i < W->nc; i++) {
      flatten(s, W->cend[i]);
      s = W->cend[i] + 1;
    }
  }
  a = *dst;
  for (i = 0; i < w * h; i++) a[i] = 0;
  for (r = 0; r < h * SS; r++) {
    int yc = r * FIX + FIX / 2, nx = 0, j, wind;
    uint8_t* row = a + (r / SS) * w;
    for (i = 0; i < W->ne; i++) {
      int y0 = W->ey0[i], y1 = W->ey1[i], lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
      if (yc < lo || yc >= hi || nx >= MAXX) continue;
      W->xs[nx] = W->ex0[i] +
                  (int)((long long)(W->ex1[i] - W->ex0[i]) * (yc - y0) / (y1 - y0));
      W->xd[nx] = y1 > y0 ? 1 : -1;
      nx++;
    }
    for (i = 1; i < nx; i++) {   /* poucos cruzamentos: insercao */
      int v = W->xs[i], d = W->xd[i];
      j = i;
      while (j > 0 && W->xs[j - 1] > v) {
        W->xs[j] = W->xs[j - 1];
        W->xd[j] = W->xd[j - 1];
        j--;
      }
      W->xs[j] = v;
      W->xd[j] = d;
    }
    wind = 0;
    for (i = 0; i + 1 < nx; i++) {
      wind += W->xd[i];
      if (!wind) continue;
      {
        /* amostras com centro em [xs[i], xs[i+1]) */
        int c0 = floor_div(W->xs[i] - FIX / 2 + FIX - 1, FIX);
        int c1 = floor_div(W->xs[i + 1] - FIX / 2 + FIX - 1, FIX);
        if (c0 < 0) c0 = 0;
        if (c1 > w * SS) c1 = w * SS;
        for (j = c0; j < c1; j++) row[j / SS]++;
      }
    }
  }
  /* 16 amostras -> 0..255, com um leve reforco para o traco fino nao sumir */
  for (i = 0; i < w * h; i++) {
    int v = a[i] * 255 / (SS * SS);
    v = v + ((255 - v) * v) / 512;
    a[i] = (uint8_t)(v > 255 ? 255 : v);
  }
  gl->left = (int16_t)left;
  gl->top = (int16_t)top;
  gl->w = (int16_t)w;
  gl->h = (int16_t)h;
  gl->a = a;
  *dst += w * h;
  return 1;
}

void wfont_drop(void) {
  if (pool && pool_free) pool_free(pool);
  pool = 0;
  f_ready = 0;
}

int wfont_build(const uint8_t* ttf, int len, int px, wfont_alloc_fn alloc, wfont_free_fn release) {
  uint32_t cap;
  uint8_t *p, *end;
  int c, ok = 1;
  wfont_drop();
  if (!ttf || len < 12 || !alloc || !release || px < 6 || px > 64) return -1;
  tf = ttf;
  tflen = len;
  if (font_open() != 0) return -2;
  W = (struct work*)alloc((uint32_t)sizeof(struct work));
  if (!W) return -3;
  cap = (uint32_t)NGLYPH * (uint32_t)(px + 6) * (uint32_t)(px * 2 + 6);
  pool = (uint8_t*)alloc(cap);
  if (!pool) {
    release(W);
    W = 0;
    return -3;
  }
  pool_free = release;
  p = pool;
  end = pool + cap;
  for (c = FIRST; c <= LAST && ok; c++) ok = rasterize(c, px, &p, end);
  release(W);
  W = 0;
  tf = 0;
  if (!ok || !G['?' - FIRST].a) {
    wfont_drop();
    return -4;
  }
  f_px = px;
  f_asc = (int)(((long long)t_asc * px + t_upem - 1) / t_upem);
  f_desc = (int)(((long long)(t_desc < 0 ? -t_desc : t_desc) * px + t_upem - 1) / t_upem);
  f_ready = 1;
  return 0;
}

int wfont_ready(void) { return f_ready; }
int wfont_px(void) { return f_ready ? f_px : 0; }
int wfont_ascent(void) { return f_ready ? f_asc : 0; }
int wfont_height(void) { return f_ready ? f_asc + f_desc : 0; }

const struct wfont_glyph* wfont_glyph(unsigned char c) {
  if (c < FIRST || c > LAST) c = '?';
  return &G[c - FIRST];
}

int wfont_width(const char* s) {
  int pen = 0;
  if (!f_ready || !s) return 0;
  while (*s) pen += wfont_glyph((unsigned char)*s++)->adv;
  return (pen + FIX / 2) / FIX;
}
