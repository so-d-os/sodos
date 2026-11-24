/* img.c - ICO, GIF, PNG e JPEG (baseline e progressivo).
 * A saida cabe em IMG_MAXS pixels de lado (800 na IMG.DLL); a origem pode
 * ter ate IMG_MAX_IN. */
#include <img.h>
#include <stdlib.h>
#include <string.h>

/* Quem liga o img.c direto (o NetSurf) pode mudar os limites na compilacao. */
#ifndef IMG_MAXS
#define IMG_MAXS 800
#endif
#ifndef IMG_MAX_IN
#define IMG_MAX_IN 3840
#endif
#define MAXS IMG_MAXS
#define MAX_IN IMG_MAX_IN

/* tamanho da origem da ultima imagem (antes da reducao); fora da API da DLL */
int img_src_w, img_src_h;

static int sample_step(int w, int h) {
  int s = 1;
  img_src_w = w;
  img_src_h = h;
  if (w < 1 || h < 1 || w > MAX_IN || h > MAX_IN) return -1;
  while ((w + s - 1) / s > MAXS || (h + s - 1) / s > MAXS) s++;
  return s;
}

typedef unsigned char u8;

static int be32(const u8* p) {
  return ((int)p[0] << 24) | ((int)p[1] << 16) | ((int)p[2] << 8) | p[3];
}
static int le16(const u8* p) { return p[0] | (p[1] << 8); }
static int le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

static int clip8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return v;
}

__export void img_free(img_t* im) {
  if (!im) return;
  free(im->rgba);
  im->rgba = 0;
  im->w = im->h = 0;
}

static int alloc_img(img_t* o, int w, int h) {
  if (w < 1 || h < 1 || w > MAXS || h > MAXS) return -1;
  o->w = w;
  o->h = h;
  o->rgba = (u8*)malloc((size_t)w * (size_t)h * 4);
  if (!o->rgba) return -1;
  memset(o->rgba, 255, (size_t)w * (size_t)h * 4);
  return 0;
}

/* ---------- inflate (RFC 1951), bits a partir do menos significativo ---------- */
#define HMAX 15
typedef struct {
  int count[HMAX + 1];
  int symbol[320];
} huff_t;

typedef struct {
  const u8* p;
  int n, i;
  unsigned buf;
  int nbit;
} bits_t;

static int bit1(bits_t* b) {
  if (!b->nbit) {
    if (b->i >= b->n) return -1;
    b->buf = b->p[b->i++];
    b->nbit = 8;
  }
  {
    int v = (int)(b->buf & 1u);
    b->buf >>= 1;
    b->nbit--;
    return v;
  }
}

static int bits_n(bits_t* b, int n) {
  int v = 0, i;
  for (i = 0; i < n; i++) {
    int t = bit1(b);
    if (t < 0) return -1;
    v |= t << i;
  }
  return v;
}

static void bits_align(bits_t* b) { b->nbit = 0; }

static int huff_build(huff_t* h, const int* lens, int nsym) {
  int len, sym, left, offs[HMAX + 1];
  memset(h, 0, sizeof(*h));
  for (sym = 0; sym < nsym; sym++) {
    if (lens[sym] < 0 || lens[sym] > HMAX) return -1;
    h->count[lens[sym]]++;
  }
  if (h->count[0] == nsym) return 0;
  left = 1;
  for (len = 1; len <= HMAX; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return -1;
  }
  offs[1] = 0;
  for (len = 1; len < HMAX; len++) offs[len + 1] = offs[len] + h->count[len];
  for (sym = 0; sym < nsym; sym++)
    if (lens[sym]) h->symbol[offs[lens[sym]]++] = sym;
  return 0;
}

static int huff_dec(bits_t* b, const huff_t* h) {
  int len, code = 0, first = 0, index = 0;
  for (len = 1; len <= HMAX; len++) {
    int bit = bit1(b);
    int count;
    if (bit < 0) return -1;
    code |= bit;
    count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

static const int len_base[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int len_extra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const int dist_base[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const int dist_extra[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* dst e o buffer inteiro. base e o que os blocos anteriores ja gravaram,
 * para uma distancia poder apontar para fora do bloco atual. */
static int inflate_codes(bits_t* b, u8* dst, int base, int dlen, const huff_t* lit, const huff_t* dist) {
  int n = base;
  for (;;) {
    int sym = huff_dec(b, lit);
    if (sym < 0) return -1;
    if (sym < 256) {
      if (n >= dlen) return -1;
      dst[n++] = (u8)sym;
    } else if (sym == 256) {
      return n - base;
    } else if (sym <= 285) {
      int len, dsym, back, i, extra;
      extra = bits_n(b, len_extra[sym - 257]);
      if (extra < 0) return -1;
      len = len_base[sym - 257] + extra;
      dsym = huff_dec(b, dist);
      if (dsym < 0 || dsym > 29) return -1;
      extra = bits_n(b, dist_extra[dsym]);
      if (extra < 0) return -1;
      back = dist_base[dsym] + extra;
      if (back < 1 || back > n || n + len > dlen) return -1;
      for (i = 0; i < len; i++) dst[n + i] = dst[n - back + i];
      n += len;
    } else return -1;
  }
}

static int inflate(const u8* src, int slen, u8* dst, int dlen) {
  bits_t b;
  int nout = 0;
  memset(&b, 0, sizeof b);
  b.p = src;
  b.n = slen;
  for (;;) {
    int bfinal = bit1(&b);
    int btype = bits_n(&b, 2);
    if (bfinal < 0 || btype < 0) return -1;
    if (btype == 0) {
      int len, nlen, i;
      bits_align(&b);
      if (b.i + 4 > b.n) return -1;
      len = b.p[b.i] | (b.p[b.i + 1] << 8);
      nlen = b.p[b.i + 2] | (b.p[b.i + 3] << 8);
      b.i += 4;
      if ((len ^ 0xFFFF) != nlen || b.i + len > b.n || nout + len > dlen) return -1;
      for (i = 0; i < len; i++) dst[nout++] = b.p[b.i++];
    } else if (btype == 1 || btype == 2) {
      huff_t lit, dist;
      int lens[320];
      int i, got;
      if (btype == 1) {
        for (i = 0; i < 288; i++) {
          if (i <= 143 || i >= 280) lens[i] = 8;
          else if (i <= 255) lens[i] = 9;
          else lens[i] = 7;
        }
        if (huff_build(&lit, lens, 288) < 0) return -1;
        for (i = 0; i < 32; i++) lens[i] = 5;
        if (huff_build(&dist, lens, 32) < 0) return -1;
      } else {
        static const int order[19] = {
          16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
        };
        int nlit, ndist, nlen, clen[19];
        huff_t cl;
        nlit = bits_n(&b, 5);
        ndist = bits_n(&b, 5);
        nlen = bits_n(&b, 4);
        if (nlit < 0 || ndist < 0 || nlen < 0) return -1;
        nlit += 257;
        ndist += 1;
        nlen += 4;
        if (nlit > 286 || ndist > 32) return -1;
        memset(clen, 0, sizeof clen);
        for (i = 0; i < nlen; i++) {
          int L = bits_n(&b, 3);
          if (L < 0) return -1;
          clen[order[i]] = L;
        }
        if (huff_build(&cl, clen, 19) < 0) return -1;
        i = 0;
        while (i < nlit + ndist) {
          int s = huff_dec(&b, &cl);
          int rep = 0, v = 0, k;
          if (s < 0) return -1;
          if (s < 16) {
            lens[i++] = s;
            continue;
          }
          if (s == 16) {
            if (i == 0) return -1;
            rep = bits_n(&b, 2);
            if (rep < 0) return -1;
            rep += 3;
            v = lens[i - 1];
          } else if (s == 17) {
            rep = bits_n(&b, 3);
            if (rep < 0) return -1;
            rep += 3;
          } else {
            rep = bits_n(&b, 7);
            if (rep < 0) return -1;
            rep += 11;
          }
          if (i + rep > nlit + ndist) return -1;
          for (k = 0; k < rep; k++) lens[i++] = v;
        }
        if (huff_build(&lit, lens, nlit) < 0) return -1;
        if (huff_build(&dist, lens + nlit, ndist) < 0) return -1;
      }
      got = inflate_codes(&b, dst, nout, dlen, &lit, &dist);
      if (got < 0) return -1;
      nout += got;
    } else return -1;
    if (bfinal) return nout;
  }
}

/* ---------- PNG ---------- */
static int paeth(int a, int c, int b) {
  int p = a + b - c, pa, pb, pc;
  pa = p - a; if (pa < 0) pa = -pa;
  pb = p - b; if (pb < 0) pb = -pb;
  pc = p - c; if (pc < 0) pc = -pc;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

static int png_decode(const u8* data, int len, img_t* o) {
  static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  int i, w = 0, h = 0, depth = 0, ctype = 0, inter = 0;
  int have = 0, rowb = 0, bpp = 0, raw_len, got;
  u8* idat = 0;
  int idat_n = 0, idat_cap = 0;
  u8* raw = 0;
  u8 pal[256 * 3];
  u8 trns[256];
  int ntrns = 0, npal = 0;
  int trns_g = -1, trns_r = -1, trns_gg = -1, trns_b = -1;
  if (len < 8 || memcmp(data, sig, 8) != 0) return -1;
  memset(trns, 255, sizeof trns);
  i = 8;
  while (i + 12 <= len) {
    int cl = be32(data + i);
    const u8* type = data + i + 4;
    const u8* cd = data + i + 8;
    if (cl < 0 || i + 12 + cl > len) { free(idat); return -1; }
    if (memcmp(type, "IHDR", 4) == 0) {
      if (cl < 13) { free(idat); return -1; }
      w = be32(cd);
      h = be32(cd + 4);
      depth = cd[8];
      ctype = cd[9];
      inter = cd[12];
      have = 1;
    } else if (memcmp(type, "PLTE", 4) == 0 && cl <= 256 * 3 && (cl % 3) == 0) {
      memcpy(pal, cd, (size_t)cl);
      npal = cl / 3;
    } else if (memcmp(type, "tRNS", 4) == 0 && cl <= 256) {
      memcpy(trns, cd, (size_t)cl);
      ntrns = cl;
      if (ctype == 0 && cl >= 2) trns_g = (cd[0] << 8) | cd[1];
      if (ctype == 2 && cl >= 6) {
        trns_r = (cd[0] << 8) | cd[1];
        trns_gg = (cd[2] << 8) | cd[3];
        trns_b = (cd[4] << 8) | cd[5];
      }
    } else if (memcmp(type, "IDAT", 4) == 0) {
      if (idat_n + cl > idat_cap) {
        int nc = idat_cap ? idat_cap * 2 : 4096;
        u8* nb;
        while (nc < idat_n + cl) nc *= 2;
        nb = (u8*)realloc(idat, (size_t)nc);
        if (!nb) { free(idat); return -1; }
        idat = nb;
        idat_cap = nc;
      }
      memcpy(idat + idat_n, cd, (size_t)cl);
      idat_n += cl;
    } else if (memcmp(type, "IEND", 4) == 0) {
      break;
    }
    i += 12 + cl;
  }
  if (!have || inter || sample_step(w, h) < 0) { free(idat); return -1; }
  if (depth != 1 && depth != 2 && depth != 4 && depth != 8) { free(idat); return -1; }
  if (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 4 && ctype != 6) { free(idat); return -1; }
  if ((ctype == 2 || ctype == 4 || ctype == 6) && depth != 8) { free(idat); return -1; }
  if (ctype == 3 && npal < 1) { free(idat); return -1; }
  {
    int ch = (ctype == 2) ? 3 : (ctype == 4) ? 2 : (ctype == 6) ? 4 : 1;
    int bits = ch * depth;
    bpp = (bits + 7) / 8;
    if (bits >= 8) bpp = bits / 8;
    rowb = (w * bits + 7) / 8;
  }
  raw_len = (rowb + 1) * h;
  raw = (u8*)malloc((size_t)raw_len);
  if (!raw || idat_n < 2 || (idat[0] & 0x0F) != 8) {
    free(idat);
    free(raw);
    return -1;
  }
  {
    int hdr = 2;
    if (idat_n >= 2 && (idat[1] & 0x20)) hdr = 6;
    got = inflate(idat + hdr, idat_n - hdr - 4, raw, raw_len);
  }
  free(idat);
  if (got < raw_len) { free(raw); return -1; }
  {
    int step = sample_step(w, h);
    int ow = (w + step - 1) / step;
    int oh = (h + step - 1) / step;
    if (alloc_img(o, ow, oh) < 0) { free(raw); return -1; }
  }
  {
    int y;
    int step = sample_step(w, h);
    u8* prev = (u8*)calloc((size_t)rowb, 1);
    u8* cur = (u8*)malloc((size_t)rowb);
    if (!prev || !cur) {
      free(prev);
      free(cur);
      free(raw);
      img_free(o);
      return -1;
    }
    for (y = 0; y < h; y++) {
      u8* src = raw + y * (rowb + 1);
      int ft = src[0];
      int x, pix;
      int oy = y / step;
      src++;
      if (ft > 4) {
        free(prev);
        free(cur);
        free(raw);
        img_free(o);
        return -1;
      }
      for (x = 0; x < rowb; x++) {
        int a = x >= bpp ? cur[x - bpp] : 0;
        int b = prev[x];
        int c = x >= bpp ? prev[x - bpp] : 0;
        int v = src[x];
        if (ft == 1) v += a;
        else if (ft == 2) v += b;
        else if (ft == 3) v += (a + b) / 2;
        else if (ft == 4) v += paeth(a, c, b);
        cur[x] = (u8)v;
      }
      for (pix = 0; pix < w; pix++) {
        int r = 0, g = 0, bl = 0, a = 255, s, m, sh;
        int ox = pix / step;
        u8* d;
        if (y % step || pix % step) continue;
        if (ox >= o->w || oy >= o->h) continue;
        d = o->rgba + ((size_t)oy * (size_t)o->w + (size_t)ox) * 4;
        if (depth == 8) {
          u8* p = cur + pix * bpp;
          if (ctype == 0) { r = g = bl = p[0]; if (p[0] == trns_g) a = 0; }
          else if (ctype == 2) {
            r = p[0]; g = p[1]; bl = p[2];
            if (r == trns_r && g == trns_gg && bl == trns_b) a = 0;
          } else if (ctype == 3) {
            int ix = p[0];
            if (ix >= npal) ix = 0;
            r = pal[ix * 3]; g = pal[ix * 3 + 1]; bl = pal[ix * 3 + 2];
            if (ix < ntrns) a = trns[ix];
          } else if (ctype == 4) { r = g = bl = p[0]; a = p[1]; }
          else { r = p[0]; g = p[1]; bl = p[2]; a = p[3]; }
        } else {
          int bppix = depth;
          int bit = pix * bppix;
          int byte = bit >> 3;
          int shift = 8 - bppix - (bit & 7);
          int v = (cur[byte] >> shift) & ((1 << bppix) - 1);
          if (ctype == 3) {
            if (v >= npal) v = 0;
            r = pal[v * 3]; g = pal[v * 3 + 1]; bl = pal[v * 3 + 2];
            if (v < ntrns) a = trns[v];
          } else {
            r = g = bl = v * (255 / ((1 << bppix) - 1));
          }
        }
        d[0] = (u8)r; d[1] = (u8)g; d[2] = (u8)bl; d[3] = (u8)a;
        (void)s; (void)m; (void)sh;
      }
      memcpy(prev, cur, (size_t)rowb);
    }
    free(prev);
    free(cur);
  }
  free(raw);
  return 0;
}

/* ---------- JPEG baseline ---------- */
static const int ZIG[64] = {
  0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
typedef struct {
  const u8* p;
  int n, i;
  unsigned acc;
  int nbit;
  int restart;
} jbit_t;

static int jbyte(jbit_t* b) {
  int c;
  if (b->i >= b->n) return -1;
  c = b->p[b->i++];
  if (c == 0xFF) {
    int n;
    if (b->i >= b->n) return -1;
    n = b->p[b->i++];
    while (n == 0xFF) {
      if (b->i >= b->n) return -1;
      n = b->p[b->i++];
    }
    if (n == 0) return 0xFF;
    if (n >= 0xD0 && n <= 0xD7) { b->restart = 1; return -2; }
    return -1;
  }
  return c;
}

/* Enche o acumulador (ate 32 bits) sem passar de um marcador: o marcador fica
 * para o jbyte, que trata RST e fim de dados como sempre (caminho lento). */
static void jfill(jbit_t* b) {
  const u8* p = b->p;
  int i = b->i, n = b->n, nbit = b->nbit;
  unsigned acc = b->acc;
  while (nbit <= 24 && i < n) {
    int c = p[i];
    if (c == 0xFF) {
      if (i + 1 >= n || p[i + 1] != 0) break;
      i += 2;
    } else {
      i++;
    }
    acc = (acc << 8) | (unsigned)c;
    nbit += 8;
  }
  b->i = i;
  b->nbit = nbit;
  b->acc = acc;
}

static int jbits(jbit_t* b, int n) {
  int v = 0;
  if (b->nbit < n) jfill(b);
  if (b->nbit >= n) {
    b->nbit -= n;
    return (int)((b->acc >> b->nbit) & ((1u << n) - 1u));
  }
  while (n > 0) {
    if (b->nbit == 0) {
      int c = jbyte(b);
      if (c < 0) return c;
      b->acc = (b->acc << 8) | (unsigned)c;
      b->nbit = 8;
    }
    b->nbit--;
    v = (v << 1) | (int)((b->acc >> b->nbit) & 1u);
    n--;
  }
  return v;
}

/* fast[9 bits seguintes] = (tamanho << 8) | simbolo, 0 = codigo mais longo */
#define JFAST 9
typedef struct {
  u8 bits[16]; u8 val[256]; int nval; int max; int mincode[16]; int off[16];
  unsigned short fast[1 << JFAST];
} jhuff_t;

static void jhuff_build(jhuff_t* h) {
  int i, code = 0, k = 0;
  h->nval = 0;
  for (i = 0; i < 16; i++) h->nval += h->bits[i];
  for (i = 0; i < 16; i++) {
    if (!h->bits[i]) {
      h->mincode[i] = -1;
      code <<= 1;
      continue;
    }
    h->mincode[i] = code;
    h->off[i] = k - code;
    k += h->bits[i];
    code = (code + h->bits[i]) << 1;
  }
  memset(h->fast, 0, sizeof h->fast);
  code = 0;
  k = 0;
  for (i = 0; i < JFAST; i++) {
    int j;
    for (j = 0; j < h->bits[i]; j++, code++, k++) {
      int sh = JFAST - 1 - i, f, base = code << sh;
      if (k >= h->nval) break;
      for (f = 0; f < (1 << sh); f++)
        if (base + f < (1 << JFAST) && !h->fast[base + f])
          h->fast[base + f] = (unsigned short)(((i + 1) << 8) | h->val[k]);
    }
    code <<= 1;
  }
}

static int jhuff_dec(jbit_t* b, const jhuff_t* h) {
  int len, code = 0;
  if (b->nbit < 16) jfill(b);
  if (b->nbit >= JFAST) {
    int e = h->fast[(b->acc >> (b->nbit - JFAST)) & ((1 << JFAST) - 1)];
    if (e) {
      b->nbit -= e >> 8;
      return e & 255;
    }
  }
  if (b->nbit >= 16) {   /* codigo longo: os 16 bits ja estao no acumulador */
    unsigned c16 = (b->acc >> (b->nbit - 16)) & 0xFFFFu;
    for (len = 0; len < 16; len++) {
      int c = (int)(c16 >> (15 - len));
      if (h->mincode[len] >= 0 && c >= h->mincode[len] && c < h->mincode[len] + h->bits[len]) {
        int idx = h->off[len] + c;
        if (idx < 0 || idx >= h->nval) return -1;
        b->nbit -= len + 1;
        return h->val[idx];
      }
    }
    return -1;
  }
  /* perto de um marcador ou do fim: um bit por vez */
  for (len = 0; len < 16; len++) {
    int bit = jbits(b, 1);
    if (bit < 0) return bit;
    code = (code << 1) | bit;
    if (h->mincode[len] >= 0 && code >= h->mincode[len] &&
        code < h->mincode[len] + h->bits[len]) {
      int idx = h->off[len] + code;
      if (idx < 0 || idx >= h->nval) return -1;
      return h->val[idx];
    }
  }
  return -1;
}

static int jextend(int v, int n) {
  if (n < 1) return v;
  if (v < (1 << (n - 1))) v -= (1 << n) - 1;
  return v;
}

/* IDCT inteira separavel (colunas e depois linhas, como a islow do libjpeg):
 * 32 bits, constantes em ponto fixo de 12 bits ja calculadas (o cc nao dobra
 * constantes de ponto flutuante). Entrada: coeficientes ja multiplicados pela
 * tabela de quantizacao, em ordem natural. Grava 8x8 em out (passo stride). */
#define JIDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                              \
  p2 = s2;                                                                    \
  p3 = s6;                                                                    \
  p1 = (p2 + p3) * 2217;                                                      \
  t2 = p1 + p3 * -7567;                                                       \
  t3 = p1 + p2 * 3135;                                                        \
  p2 = s0;                                                                    \
  p3 = s4;                                                                    \
  t0 = (p2 + p3) * 4096;                                                      \
  t1 = (p2 - p3) * 4096;                                                      \
  x0 = t0 + t3;                                                               \
  x3 = t0 - t3;                                                               \
  x1 = t1 + t2;                                                               \
  x2 = t1 - t2;                                                               \
  t0 = s7;                                                                    \
  t1 = s5;                                                                    \
  t2 = s3;                                                                    \
  t3 = s1;                                                                    \
  p3 = t0 + t2;                                                               \
  p4 = t1 + t3;                                                               \
  p1 = t0 + t3;                                                               \
  p2 = t1 + t2;                                                               \
  p5 = (p3 + p4) * 4816;                                                      \
  t0 = t0 * 1223;                                                             \
  t1 = t1 * 8410;                                                             \
  t2 = t2 * 12586;                                                            \
  t3 = t3 * 6149;                                                             \
  p1 = p5 + p1 * -3685;                                                       \
  p2 = p5 + p2 * -10497;                                                      \
  p3 = p3 * -8034;                                                            \
  p4 = p4 * -1597;                                                            \
  t3 += p1 + p4;                                                              \
  t2 += p2 + p3;                                                              \
  t1 += p2 + p4;                                                              \
  t0 += p1 + p3;

#define JPUT(o, v)                                                            \
  r = (v) >> 17;                                                              \
  o = (u8)((unsigned)r < 256u ? r : r < 0 ? 0 : 255);

static void idct_put(const int* in, u8* out, int stride) {
  int i, val[64], r;
  int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;
  int* v = val;
  const int* d = in;
  for (i = 0; i < 8; i++, d++, v++) {
    if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 && d[40] == 0 &&
        d[48] == 0 && d[56] == 0) {
      int dc = d[0] * 4;
      v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
      continue;
    }
    JIDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
    x0 += 512;
    x1 += 512;
    x2 += 512;
    x3 += 512;
    v[0] = (x0 + t3) >> 10;
    v[56] = (x0 - t3) >> 10;
    v[8] = (x1 + t2) >> 10;
    v[48] = (x1 - t2) >> 10;
    v[16] = (x2 + t1) >> 10;
    v[40] = (x2 - t1) >> 10;
    v[24] = (x3 + t0) >> 10;
    v[32] = (x3 - t0) >> 10;
  }
  for (i = 0, v = val; i < 8; i++, v += 8, out += stride) {
    if ((v[1] | v[2] | v[3] | v[4] | v[5] | v[6] | v[7]) == 0) {
      /* so o termo DC na linha: (v0 * 4096 + 65536 + (128 << 17)) >> 17 */
      int c = ((v[0] + 16) >> 5) + 128;
      u8 px = (u8)((unsigned)c < 256u ? c : c < 0 ? 0 : 255);
      out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = px;
      continue;
    }
    JIDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
    x0 += 16842752;   /* 65536 (arredonda) + (128 << 17) (nivel 128) */
    x1 += 16842752;
    x2 += 16842752;
    x3 += 16842752;
    JPUT(out[0], x0 + t3)
    JPUT(out[7], x0 - t3)
    JPUT(out[1], x1 + t2)
    JPUT(out[6], x1 - t2)
    JPUT(out[2], x2 + t1)
    JPUT(out[5], x2 - t1)
    JPUT(out[3], x3 + t0)
    JPUT(out[4], x3 - t0)
  }
}

/* YCbCr -> RGB em tabela, com as mesmas contas (e arredondamentos) de antes. */
static int jcr_r[256], jcb_b[256], jcb_g[256], jcr_g[256];
static int jcor_ok;

static void jcor_init(void) {
  int i;
  if (jcor_ok) return;
  for (i = 0; i < 256; i++) {
    jcr_r[i] = (359 * (i - 128)) / 256;
    jcb_b[i] = (454 * (i - 128)) / 256;
    jcb_g[i] = 88 * (i - 128);
    jcr_g[i] = 183 * (i - 128);
  }
  jcor_ok = 1;
}

/* Bloco so com DC: o mesmo valor da idct_put, sem a conta. */
static void idct_dc(int dc, u8* out, int stride) {
  int c = ((dc + 4) >> 3) + 128, y;
  u8 px = (u8)((unsigned)c < 256u ? c : c < 0 ? 0 : 255);
  for (y = 0; y < 8; y++, out += stride)
    out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = px;
}

static int refine_one(jbit_t* jb, short* c, int al) {
  int bit = jbits(jb, 1);
  if (bit < 0) return -1;
  if (bit && (*c & (short)(1 << al)) == 0) {
    if (*c >= 0) *c += (short)(1 << al);
    else *c -= (short)(1 << al);
  }
  return 0;
}

/* um bloco de um scan progressivo. -2 = marcador de restart. */
static int prog_block(jbit_t* jb, short* coef, const jhuff_t* hdc, const jhuff_t* hac,
                      int* pred, int Ss, int Se, int Ah, int Al, int* eob) {
  int zz;
  if (Ss == 0) {
    if (Ah == 0) {
      int sym = jhuff_dec(jb, hdc);
      int extra;
      if (sym == -2) return -2;
      if (sym < 0 || sym > 11) return -1;
      extra = sym ? jbits(jb, sym) : 0;
      if (extra == -2) return -2;
      if (extra < 0) return -1;
      *pred += jextend(extra, sym);
      {
        int mag = *pred;
        int sh;
        for (sh = 0; sh < Al && sh < 14; sh++) mag *= 2;
        if (mag > 32767) mag = 32767;
        if (mag < -32768) mag = -32768;
        coef[0] = (short)mag;
      }
    } else if (refine_one(jb, coef, Al) < 0) return -1;
  }
  if (Se < Ss || Se < 1) return 0;
  if (Ah == 0) {
    if (*eob > 0) { (*eob)--; return 0; }
    for (zz = Ss; zz <= Se; ) {
      int sym = jhuff_dec(jb, hac);
      int run, sz;
      if (sym == -2) return -2;
      if (sym < 0) return -1;
      run = sym >> 4;
      sz = sym & 15;
      if (sz == 0) {
        if (run == 15) { zz += 16; continue; }
        *eob = 1 << run;
        if (run) {
          int ex = jbits(jb, run);
          if (ex < 0) return -1;
          *eob += ex;
        }
        (*eob)--;
        return 0;
      }
      zz += run;
      if (zz > Se || zz >= 64) return -1;
      {
        int ex = jbits(jb, sz);
        if (ex < 0) return -1;
        coef[ZIG[zz]] = (short)(jextend(ex, sz) << Al);
      }
      zz++;
    }
    return 0;
  }
  if (*eob > 0) {
    for (zz = Ss; zz <= Se; zz++)
      if (coef[ZIG[zz]] && refine_one(jb, &coef[ZIG[zz]], Al) < 0) return -1;
    (*eob)--;
    return 0;
  }
  for (zz = Ss; zz <= Se; ) {
    int sym = jhuff_dec(jb, hac);
    int run, sz, bit;
    if (sym == -2) return -2;
    if (sym < 0) return -1;
    run = sym >> 4;
    sz = sym & 15;
    if (sz) {
      if (sz != 1) return -1;
      bit = jbits(jb, 1);
      if (bit < 0) return -1;
      for (;;) {
        if (zz > Se) return -1;
        if (coef[ZIG[zz]]) {
          if (refine_one(jb, &coef[ZIG[zz]], Al) < 0) return -1;
          zz++;
        } else if (run > 0) {
          run--;
          zz++;
        } else {
          coef[ZIG[zz]] = (short)(bit ? (1 << Al) : -(1 << Al));
          zz++;
          break;
        }
      }
    } else if (run == 15) {
      int left = 16;
      while (left > 0 && zz <= Se) {
        if (coef[ZIG[zz]]) {
          if (refine_one(jb, &coef[ZIG[zz]], Al) < 0) return -1;
        } else left--;
        zz++;
      }
    } else {
      *eob = 1 << run;
      if (run) {
        int ex = jbits(jb, run);
        if (ex < 0) return -1;
        *eob += ex;
      }
      for (; zz <= Se; zz++)
        if (coef[ZIG[zz]] && refine_one(jb, &coef[ZIG[zz]], Al) < 0) return -1;
      (*eob)--;
      return 0;
    }
  }
  return 0;
}

static int jpeg_decode(const u8* data, int len, img_t* o) {
  int i, w = 0, h = 0, ncomp = 0, restart = 0, prog = 0;
  int qt[4][64];
  jhuff_t dc[4], ac[4];
  int have_qt[4], have_dc[4], have_ac[4];
  int cid[3], hs[3], vs[3], tq[3], tdc[3], tac[3];
  int hmax = 1, vmax = 1;
  u8* plane[3];
  short* coef[3];
  int pw[3], ph[3], pstride[3], nbx[3], nby[3];
  jbit_t jb;
  int pred[3];
  int mbx, mby, mbw = 0, mbh = 0;
  if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return -1;
  memset(have_qt, 0, sizeof have_qt);
  memset(have_dc, 0, sizeof have_dc);
  memset(have_ac, 0, sizeof have_ac);
  memset(dc, 0, sizeof dc);
  memset(ac, 0, sizeof ac);
  plane[0] = plane[1] = plane[2] = 0;
  coef[0] = coef[1] = coef[2] = 0;
  i = 2;
  while (i + 4 <= len) {
    int m, seglen;
    if (data[i] != 0xFF) { i++; continue; }
    while (i < len && data[i] == 0xFF) i++;
    if (i >= len) break;
    m = data[i++];
    if (m == 0xD9) break;
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
    if (i + 2 > len) goto fail;
    seglen = (data[i] << 8) | data[i + 1];
    if (seglen < 2 || i + seglen > len) goto fail;
    if (m == 0xDB) {
      int p = i + 2;
      int end = i + seglen;
      while (p < end) {
        int info = data[p++];
        int id = info & 15;
        int prec = info >> 4;
        int k;
        if (id > 3 || prec > 1) { goto fail; }
        if (p + 64 * (prec + 1) > end) goto fail;
        for (k = 0; k < 64; k++) {
          int v = data[p++];
          if (prec) v = (v << 8) | data[p++];
          qt[id][ZIG[k]] = v;
        }
        have_qt[id] = 1;
      }
    } else if (m == 0xC4) {
      int p = i + 2;
      int end = i + seglen;
      while (p < end) {
        int info = data[p++];
        int id = info & 15;
        int acs = (info >> 4) & 1;
        jhuff_t* hh = acs ? &ac[id] : &dc[id];
        int k, nv = 0;
        if (id > 3) goto fail;
        if (p + 16 > end) goto fail;
        for (k = 0; k < 16; k++) { hh->bits[k] = data[p++]; nv += hh->bits[k]; }
        if (nv > 256 || p + nv > end) goto fail;
        for (k = 0; k < nv; k++) hh->val[k] = data[p++];
        jhuff_build(hh);
        if (acs) have_ac[id] = 1;
        else have_dc[id] = 1;
      }
    } else if (m == 0xC0 || m == 0xC2) {
      const u8* p = data + i + 2;
      int c;
      prog = (m == 0xC2);
      if (seglen < 8 || p[0] != 8) goto fail;
      h = (p[1] << 8) | p[2];
      w = (p[3] << 8) | p[4];
      ncomp = p[5];
      if (ncomp < 1 || ncomp > 3 || seglen < 8 + ncomp * 3) goto fail;
      if (sample_step(w, h) < 0) goto fail;
      for (c = 0; c < ncomp; c++) {
        cid[c] = p[6 + c * 3];
        hs[c] = p[7 + c * 3] >> 4;
        vs[c] = p[7 + c * 3] & 15;
        tq[c] = p[8 + c * 3];
        if (hs[c] < 1 || hs[c] > 2 || vs[c] < 1 || vs[c] > 2 || tq[c] > 3) goto fail;
        if (hs[c] > hmax) hmax = hs[c];
        if (vs[c] > vmax) vmax = vs[c];
      }
    } else if (m == 0xC1) {
      goto fail;
    } else if (m == 0xDD && seglen >= 4) {
      restart = (data[i + 2] << 8) | data[i + 3];
    } else if (m == 0xDA) {
      const u8* p = data + i + 2;
      int c, ns, Ss = 0, Se = 63, Ah = 0, Al = 0;
      int scan_c[3];
      if (ncomp < 1) goto fail;
      ns = p[0];
      if (ns < 1 || ns > ncomp || seglen < 6 + ns * 2) goto fail;
      if (prog) {
        Ss = p[1 + ns * 2];
        Se = p[2 + ns * 2];
        Ah = p[3 + ns * 2] >> 4;
        Al = p[3 + ns * 2] & 15;
      } else if (ns != ncomp) goto fail;
      for (c = 0; c < ns; c++) {
        int id = p[1 + c * 2];
        int k, found = -1;
        for (k = 0; k < ncomp; k++) if (cid[k] == id) found = k;
        if (found < 0) goto fail;
        scan_c[c] = found;
        tdc[found] = p[2 + c * 2] >> 4;
        tac[found] = p[2 + c * 2] & 15;
        if (tdc[found] > 3 || tac[found] > 3) goto fail;
        if (!have_dc[tdc[found]] || !have_qt[tq[found]]) goto fail;
        if (Se > 0 && !have_ac[tac[found]]) goto fail;
      }
      i += seglen;
      if (!prog) goto scan;
      if (!mbw) {
        int comp;
        mbw = (w + hmax * 8 - 1) / (hmax * 8);
        mbh = (h + vmax * 8 - 1) / (vmax * 8);
        for (comp = 0; comp < ncomp; comp++) {
          int nb;
          nbx[comp] = mbw * hs[comp];
          nby[comp] = mbh * vs[comp];
          nb = nbx[comp] * nby[comp];
          if (nb < 1 || nb > 200000) goto fail;
          coef[comp] = (short*)calloc((size_t)nb * 64, sizeof(short));
          if (!coef[comp]) goto fail;
        }
      }
      memset(&jb, 0, sizeof jb);
      jb.p = data + i;
      jb.n = len - i;
      pred[0] = pred[1] = pred[2] = 0;
      {
        int eob = 0, nleft = restart, rc;
        if (ns == 1) {
          int comp = scan_c[0], bx, by;
          for (by = 0; by < nby[comp]; by++) {
            for (bx = 0; bx < nbx[comp]; bx++) {
              if (restart && nleft == 0) {
                pred[comp] = 0;
                eob = 0;
                jb.nbit = 0;
                nleft = restart;
              }
              if (restart) nleft--;
              rc = prog_block(&jb, coef[comp] + ((size_t)by * nbx[comp] + bx) * 64,
                              &dc[tdc[comp]], &ac[tac[comp]], &pred[comp],
                              Ss, Se, Ah, Al, &eob);
              if (rc == -2) {
                pred[comp] = 0;
                eob = 0;
                jb.nbit = 0;
                rc = prog_block(&jb, coef[comp] + ((size_t)by * nbx[comp] + bx) * 64,
                                &dc[tdc[comp]], &ac[tac[comp]], &pred[comp],
                                Ss, Se, Ah, Al, &eob);
              }
              if (rc < 0) goto fail;
            }
          }
        } else {
          for (mby = 0; mby < mbh; mby++) {
            for (mbx = 0; mbx < mbw; mbx++) {
              int si;
              if (restart && nleft == 0) {
                pred[0] = pred[1] = pred[2] = 0;
                eob = 0;
                jb.nbit = 0;
                nleft = restart;
              }
              if (restart) nleft--;
              for (si = 0; si < ns; si++) {
                int comp = scan_c[si], by, bx;
                for (by = 0; by < vs[comp]; by++) {
                  for (bx = 0; bx < hs[comp]; bx++) {
                    int ix = (mby * vs[comp] + by) * nbx[comp] + (mbx * hs[comp] + bx);
                    rc = prog_block(&jb, coef[comp] + (size_t)ix * 64,
                                    &dc[tdc[comp]], &ac[tac[comp]], &pred[comp],
                                    Ss, Se, Ah, Al, &eob);
                    if (rc < 0) goto fail;
                  }
                }
              }
            }
          }
        }
      }
      i = (int)(jb.p + jb.i - data);
      continue;
    }
    i += seglen;
  }
  if (!prog || !coef[0]) return -1;
  mbw = (w + hmax * 8 - 1) / (hmax * 8);
  mbh = (h + vmax * 8 - 1) / (vmax * 8);
  for (i = 0; i < ncomp; i++) {
    int by, bx, nbxx = mbw * hs[i], nbyy = mbh * vs[i];
    pw[i] = nbxx * 8;
    ph[i] = nbyy * 8;
    pstride[i] = pw[i];
    plane[i] = (u8*)malloc((size_t)pw[i] * (size_t)ph[i]);
    if (!plane[i]) goto fail;
    memset(plane[i], 128, (size_t)pw[i] * (size_t)ph[i]);
    for (by = 0; by < nbyy; by++) {
      for (bx = 0; bx < nbxx; bx++) {
        short* src = coef[i] + ((size_t)by * nbxx + bx) * 64;
        const int* q = qt[tq[i]];
        int coefi[64], k, nz = 0;
        u8* dst = plane[i] + (size_t)(by * 8) * pstride[i] + bx * 8;
        coefi[0] = (int)src[0] * q[0];
        for (k = 1; k < 64; k++) {
          coefi[k] = (int)src[k] * q[k];
          nz |= coefi[k];
        }
        if (nz) idct_put(coefi, dst, pstride[i]);
        else idct_dc(coefi[0], dst, pstride[i]);
      }
    }
  }
  goto color;
scan: {
  const u8* sp = data + i;
  int sn = len - i;
  mbw = (w + hmax * 8 - 1) / (hmax * 8);
  mbh = (h + vmax * 8 - 1) / (vmax * 8);
  for (i = 0; i < ncomp; i++) {
    pw[i] = mbw * hs[i] * 8;
    ph[i] = mbh * vs[i] * 8;
    pstride[i] = pw[i];
    plane[i] = (u8*)malloc((size_t)pw[i] * (size_t)ph[i]);
    if (!plane[i]) goto fail;
    memset(plane[i], 128, (size_t)pw[i] * (size_t)ph[i]);
  }
  memset(&jb, 0, sizeof jb);
  jb.p = sp;
  jb.n = sn;
  }
  pred[0] = pred[1] = pred[2] = 0;
  {
    int nleft = restart;
    for (mby = 0; mby < mbh; mby++) {
      for (mbx = 0; mbx < mbw; mbx++) {
        int c;
        if (restart && nleft == 0) {
          pred[0] = pred[1] = pred[2] = 0;
          jb.nbit = 0;
          nleft = restart;
        }
        if (restart) nleft--;
        for (c = 0; c < ncomp; c++) {
          int by, bx;
          for (by = 0; by < vs[c]; by++) {
            for (bx = 0; bx < hs[c]; bx++) {
              int coef[64], zz, nz = 0;
              int sym, extra;
              const int* q = qt[tq[c]];
              const jhuff_t* hac = &ac[tac[c]];
              u8* dst;
              memset(coef, 0, sizeof coef);
              sym = jhuff_dec(&jb, &dc[tdc[c]]);
              if (sym == -2) { pred[0] = pred[1] = pred[2] = 0; jb.nbit = 0; sym = jhuff_dec(&jb, &dc[tdc[c]]); }
              if (sym < 0 || sym > 11) goto fail;
              extra = sym ? jbits(&jb, sym) : 0;
              if (extra == -2) goto fail;
              if (extra < 0) goto fail;
              pred[c] += jextend(extra, sym);
              coef[0] = pred[c] * q[0];
              zz = 1;
              while (zz < 64) {
                int run, sz, k;
                sym = jhuff_dec(&jb, hac);
                if (sym < 0) goto fail;
                if (sym == 0) break;
                run = sym >> 4;
                sz = sym & 15;
                if (sz == 0) {
                  if (run == 15) zz += 16;
                  else goto fail;
                } else {
                  zz += run;
                  if (zz >= 64) goto fail;
                  extra = jbits(&jb, sz);
                  if (extra < 0) goto fail;
                  k = ZIG[zz];
                  coef[k] = jextend(extra, sz) * q[k];
                  nz = 1;
                  zz++;
                }
              }
              dst = plane[c] + (size_t)((mby * vs[c] + by) * 8) * pstride[c] +
                    (mbx * hs[c] + bx) * 8;
              if (nz) idct_put(coef, dst, pstride[c]);
              else idct_dc(coef[0], dst, pstride[c]);
            }
          }
        }
      }
    }
  }
color:
  {
    int step = sample_step(w, h);
    int ow = step > 0 ? (w + step - 1) / step : 0;
    int oh = step > 0 ? (h + step - 1) / step : 0;
    int y, x, c;
    int* xi;
    if (alloc_img(o, ow, oh) < 0) goto fail;
    /* coluna de cada componente para cada x da saida: sem divisao no laco */
    xi = (int*)malloc((size_t)ow * 3 * sizeof(int));
    if (!xi) goto fail;
    for (c = 0; c < ncomp; c++) {
      for (x = 0; x < ow; x++) {
        int sx = x * step;
        if (sx >= w) sx = w - 1;
        xi[c * ow + x] = sx * hs[c] / hmax;
      }
    }
    jcor_init();
    for (y = 0; y < oh; y++) {
      int sy = y * step;
      u8* d = o->rgba + (size_t)y * (size_t)ow * 4;
      const u8* p0;
      const int* x0 = xi;
      if (sy >= h) sy = h - 1;
      p0 = plane[0] + (size_t)(sy * vs[0] / vmax) * pstride[0];
      if (ncomp >= 3) {
        const u8* p1 = plane[1] + (size_t)(sy * vs[1] / vmax) * pstride[1];
        const u8* p2 = plane[2] + (size_t)(sy * vs[2] / vmax) * pstride[2];
        const int* x1 = xi + ow;
        const int* x2 = xi + 2 * ow;
        for (x = 0; x < ow; x++, d += 4) {
          int Y = p0[x0[x]], cb = p1[x1[x]], cr = p2[x2[x]], v;
          v = Y + jcr_r[cr];
          d[0] = (u8)((unsigned)v < 256u ? v : v < 0 ? 0 : 255);
          v = Y - (jcb_g[cb] + jcr_g[cr]) / 256;
          d[1] = (u8)((unsigned)v < 256u ? v : v < 0 ? 0 : 255);
          v = Y + jcb_b[cb];
          d[2] = (u8)((unsigned)v < 256u ? v : v < 0 ? 0 : 255);
          d[3] = 255;
        }
      } else {
        for (x = 0; x < ow; x++, d += 4) {
          u8 Y = p0[x0[x]];
          d[0] = d[1] = d[2] = Y;
          d[3] = 255;
        }
      }
    }
    free(xi);
  }
  free(plane[0]);
  free(plane[1]);
  free(plane[2]);
  free(coef[0]);
  free(coef[1]);
  free(coef[2]);
  return 0;
fail:
  free(plane[0]);
  free(plane[1]);
  free(plane[2]);
  free(coef[0]);
  free(coef[1]);
  free(coef[2]);
  img_free(o);
  return -1;
}

/* ---------- ICO (BMP ou PNG embutido) ---------- */
static int ico_bmp(const u8* p, int n, int bw, int bh, img_t* o) {
  int hs, width, height, bpp, x, y, stride, and_stride;
  const u8* pal = 0;
  const u8* bits;
  const u8* mask;
  int npal = 0;
  if (n < 40) return -1;
  hs = le32(p);
  if (hs < 40 || hs > n) return -1;
  width = le32(p + 4);
  height = le32(p + 8);
  bpp = le16(p + 14);
  if (width < 0) width = -width;
  if (height < 0) height = -height;
  height /= 2;
  if (!bw) bw = width;
  if (!bh) bh = height;
  if (width < 1 || height < 1 ||
      (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32))
    return -1;
  if (le32(p + 16) != 0 && le32(p + 16) != 3) return -1;
  if (bpp <= 8) {
    npal = 1 << bpp;
    if (le32(p + 32)) npal = le32(p + 32);
    pal = p + hs;
    if (hs + npal * 4 > n) return -1;
  }
  bits = p + hs + npal * 4;
  stride = ((width * bpp + 31) / 32) * 4;
  and_stride = ((width + 31) / 32) * 4;
  if ((bits - p) + stride * height + and_stride * height > n) return -1;
  mask = bits + stride * height;
  if (alloc_img(o, bw < width ? bw : width, bh < height ? bh : height) < 0) return -1;
  width = o->w;
  height = o->h;
  for (y = 0; y < height; y++) {
    const u8* row = bits + (height - 1 - y) * stride;
    const u8* arow = mask + (height - 1 - y) * and_stride;
    for (x = 0; x < width; x++) {
      int r = 0, g = 0, b = 0, a = 255;
      u8* d = o->rgba + ((size_t)y * (size_t)width + (size_t)x) * 4;
      if (bpp == 32) {
        b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; a = row[x * 4 + 3];
        if (a == 0) a = ((arow[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 255;
      } else if (bpp == 24) {
        b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2];
        if ((arow[x >> 3] >> (7 - (x & 7))) & 1) a = 0;
      } else {
        int v, sh;
        if (bpp == 8) v = row[x];
        else if (bpp == 4) { v = row[x >> 1]; v = (x & 1) ? (v & 15) : (v >> 4); }
        else { v = (row[x >> 3] >> (7 - (x & 7))) & 1; }
        if (v < 0 || v >= npal) v = 0;
        b = pal[v * 4]; g = pal[v * 4 + 1]; r = pal[v * 4 + 2];
        if ((arow[x >> 3] >> (7 - (x & 7))) & 1) a = 0;
        (void)sh;
      }
      d[0] = (u8)r; d[1] = (u8)g; d[2] = (u8)b; d[3] = (u8)a;
    }
  }
  return 0;
}

static int ico_decode(const u8* data, int len, img_t* o) {
  int count, i, best = -1, best_area = -1;
  if (len < 6 || data[0] != 0 || data[1] != 0) return -1;
  if (data[2] != 1 && data[2] != 2) return -1;
  count = le16(data + 4);
  if (count < 1 || 6 + count * 16 > len) return -1;
  for (i = 0; i < count; i++) {
    const u8* e = data + 6 + i * 16;
    int w = e[0] ? e[0] : 256;
    int h = e[1] ? e[1] : 256;
    int bytes = le32(e + 8);
    int off = le32(e + 12);
    int area = w * h;
    if (off < 0 || bytes < 0 || off + bytes > len) continue;
    if (w > MAXS || h > MAXS) continue;
    if (area > best_area) { best_area = area; best = i; }
  }
  if (best < 0) return -1;
  {
    const u8* e = data + 6 + best * 16;
    int bytes = le32(e + 8);
    int off = le32(e + 12);
    const u8* p = data + off;
    if (bytes >= 8 && p[0] == 137 && p[1] == 80) return png_decode(p, bytes, o);
    return ico_bmp(p, bytes, e[0] ? e[0] : 256, e[1] ? e[1] : 256, o);
  }
}

/* ---------- GIF (primeiro quadro) ---------- */
static int gif_bits(const u8* s, int n, int* sp, unsigned* acc, int* nbit, int nb) {
  int v = 0, k;
  for (k = 0; k < nb; k++) {
    if (*nbit == 0) {
      if (*sp >= n) return -1;
      *acc = s[(*sp)++];
      *nbit = 8;
    }
    v |= (int)((*acc & 1u) << k);
    *acc >>= 1;
    (*nbit)--;
  }
  return v;
}

static int gif_lzw(const u8* src, int n, int mincs, u8* out, int outn) {
  int* prefix;
  u8* suffix;
  u8* stack;
  int clear, eoi, codesize, next, prev, sp, nbit, outi, i;
  unsigned acc;
  if (mincs < 2 || mincs > 8) return -1;
  prefix = (int*)malloc(4096 * sizeof(int));
  suffix = (u8*)malloc(4096);
  stack = (u8*)malloc(4096);
  if (!prefix || !suffix || !stack) {
    free(prefix);
    free(suffix);
    free(stack);
    return -1;
  }
  clear = 1 << mincs;
  eoi = clear + 1;
  for (i = 0; i < clear; i++) {
    prefix[i] = -1;
    suffix[i] = (u8)i;
  }
  codesize = mincs + 1;
  next = eoi + 1;
  prev = -1;
  sp = 0;
  nbit = 0;
  acc = 0;
  outi = 0;
  while (outi < outn) {
    int code = gif_bits(src, n, &sp, &acc, &nbit, codesize);
    int incode, nstk, t;
    u8 first;
    if (code < 0) break;
    if (code == clear) {
      codesize = mincs + 1;
      next = eoi + 1;
      prev = -1;
      continue;
    }
    if (code == eoi) break;
    if (code > next || (code == next && prev < 0)) { outi = -1; goto lzw_done; }
    incode = code;
    nstk = 0;
    if (code == next) {
      t = prev;
      while (t >= clear) {
        if (nstk >= 4096 || t < 0) { outi = -1; goto lzw_done; }
        stack[nstk++] = suffix[t];
        t = prefix[t];
      }
      if (t < 0 || t >= clear) { outi = -1; goto lzw_done; }
      first = suffix[t];
      if (outi < outn) out[outi++] = first;
      while (nstk > 0 && outi < outn) out[outi++] = stack[--nstk];
      if (outi < outn) out[outi++] = first;
    } else {
      t = code;
      while (t >= clear) {
        if (nstk >= 4096 || t < 0) { outi = -1; goto lzw_done; }
        stack[nstk++] = suffix[t];
        t = prefix[t];
      }
      if (t < 0 || t >= clear) { outi = -1; goto lzw_done; }
      first = suffix[t];
      if (outi < outn) out[outi++] = first;
      while (nstk > 0 && outi < outn) out[outi++] = stack[--nstk];
    }
    if (prev >= 0 && next < 4096) {
      prefix[next] = prev;
      suffix[next] = first;
      next++;
      if (codesize < 12 && next == (1 << codesize)) codesize++;
    }
    prev = incode;
  }
lzw_done:
  free(prefix);
  free(suffix);
  free(stack);
  return outi;
}

static int gif_skip_blocks(const u8* data, int len, int i) {
  while (i < len) {
    int sz = data[i++];
    if (!sz) break;
    if (i + sz > len) return -1;
    i += sz;
  }
  return i;
}

static int gif_decode(const u8* data, int len, img_t* o) {
  int i, packed, gsize = 0, trans = -1;
  u8 gpal[256 * 3];
  if (len < 13) return -1;
  if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) return -1;
  packed = data[10];
  i = 13;
  if (packed & 0x80) {
    gsize = 1 << ((packed & 7) + 1);
    if (i + gsize * 3 > len) return -1;
    memcpy(gpal, data + i, (size_t)gsize * 3);
    i += gsize * 3;
  }
  while (i < len) {
    int b = data[i++];
    if (b == 0x3B) break;
    if (b == 0x21) {
      int label;
      if (i >= len) return -1;
      label = data[i++];
      if (label == 0xF9 && i < len && data[i] >= 4 && i + 5 <= len && (data[i + 1] & 1))
        trans = data[i + 4];
      i = gif_skip_blocks(data, len, i);
      if (i < 0) return -1;
      continue;
    }
    if (b != 0x2C) return -1;
    {
      int w, h, pk, mincs, inter, npal, step, ow, oh, y, x;
      u8 pal[256 * 3];
      u8* idx;
      u8* comp;
      int cn = 0, cap = 4096;
      const u8* use;
      int ncol;
      if (i + 9 > len) return -1;
      w = le16(data + i + 4);
      h = le16(data + i + 6);
      pk = data[i + 8];
      i += 9;
      if (w < 1 || h < 1 || sample_step(w, h) < 0) return -1;
      memcpy(pal, gpal, (size_t)gsize * 3);
      npal = gsize;
      if (pk & 0x80) {
        int lsz = 1 << ((pk & 7) + 1);
        if (i + lsz * 3 > len) return -1;
        memcpy(pal, data + i, (size_t)lsz * 3);
        npal = lsz;
        i += lsz * 3;
      }
      if (i >= len) return -1;
      mincs = data[i++];
      comp = (u8*)malloc(4096);
      if (!comp) return -1;
      while (i < len) {
        int sz = data[i++];
        u8* nb;
        if (!sz) break;
        if (i + sz > len) { free(comp); return -1; }
        if (cn + sz > cap) {
          cap = cap * 2 + sz;
          if (cap > 4 * 1024 * 1024) { free(comp); return -1; }
          nb = (u8*)realloc(comp, (size_t)cap);
          if (!nb) { free(comp); return -1; }
          comp = nb;
        }
        memcpy(comp + cn, data + i, (size_t)sz);
        cn += sz;
        i += sz;
      }
      if ((size_t)w * (size_t)h > 8u * 1024u * 1024u) { free(comp); return -1; }
      idx = (u8*)calloc((size_t)w * (size_t)h, 1);
      if (!idx || gif_lzw(comp, cn, mincs, idx, w * h) <= 0) {
        free(comp);
        free(idx);
        return -1;
      }
      free(comp);
      inter = (pk & 0x40) != 0;
      if (inter) {
        u8* tmp = (u8*)malloc((size_t)w * (size_t)h);
        int starts[4], steps[4], pass, src, yy;
        if (!tmp) { free(idx); return -1; }
        memcpy(tmp, idx, (size_t)w * (size_t)h);
        starts[0] = 0; starts[1] = 4; starts[2] = 2; starts[3] = 1;
        steps[0] = 8; steps[1] = 8; steps[2] = 4; steps[3] = 2;
        src = 0;
        for (pass = 0; pass < 4; pass++) {
          for (yy = starts[pass]; yy < h; yy += steps[pass]) {
            if (src < h) memcpy(idx + yy * w, tmp + src * w, (size_t)w);
            src++;
          }
        }
        free(tmp);
      }
      step = sample_step(w, h);
      ow = (w + step - 1) / step;
      oh = (h + step - 1) / step;
      if (alloc_img(o, ow, oh) < 0) { free(idx); return -1; }
      use = pal;
      ncol = npal;
      for (y = 0; y < oh; y++) {
        int sy = y * step;
        if (sy >= h) sy = h - 1;
        for (x = 0; x < ow; x++) {
          int sx = x * step;
          int ix, a = 255, r = 0, g = 0, b = 0;
          u8* d;
          if (sx >= w) sx = w - 1;
          ix = idx[sy * w + sx];
          if (ix == trans) a = 0;
          if (ix < ncol) {
            r = use[ix * 3];
            g = use[ix * 3 + 1];
            b = use[ix * 3 + 2];
          }
          d = o->rgba + ((size_t)y * (size_t)ow + (size_t)x) * 4;
          d[0] = (u8)r; d[1] = (u8)g; d[2] = (u8)b; d[3] = (u8)a;
        }
      }
      free(idx);
      return 0;
    }
  }
  return -1;
}

__export int img_decode(const void* data, int len, img_t* out) {
  const u8* p = (const u8*)data;
  if (!out) return -1;
  out->w = out->h = 0;
  out->rgba = 0;
  if (!p || len < 8) return -1;
  if (p[0] == 137 && p[1] == 80 && p[2] == 78 && p[3] == 71) return png_decode(p, len, out);
  if (p[0] == 0xFF && p[1] == 0xD8) return jpeg_decode(p, len, out);
  if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F') return gif_decode(p, len, out);
  if (p[0] == 0 && p[1] == 0 && (p[2] == 1 || p[2] == 2)) return ico_decode(p, len, out);
  return -1;
}
