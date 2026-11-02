/* zinf.c - inflate (RFC 1950/1951) que diz quantos bytes de entrada usou:
 * no pack do git os objetos vem um atras do outro, sem tamanho comprimido. */
#include "git.h"

#define HMAX 15

typedef struct {
  short count[HMAX + 1];
  short symbol[320];
} huff_t;

typedef struct {
  const unsigned char* p;
  int n;
  int i;
  unsigned buf;
  int nbit;
} bits_t;

static int need(bits_t* b, int n) {
  while (b->nbit < n) {
    if (b->i >= b->n) return -1;
    b->buf |= (unsigned)b->p[b->i++] << b->nbit;
    b->nbit += 8;
  }
  return 0;
}

static int bits_n(bits_t* b, int n) {
  int v;
  if (n == 0) return 0;
  if (need(b, n) < 0) return -1;
  v = (int)(b->buf & ((1u << n) - 1u));
  b->buf >>= n;
  b->nbit -= n;
  return v;
}

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
    if (lens[sym]) h->symbol[offs[lens[sym]]++] = (short)sym;
  return 0;
}

static int huff_dec(bits_t* b, const huff_t* h) {
  int len, code = 0, first = 0, index = 0;
  for (len = 1; len <= HMAX; len++) {
    int count;
    if (!b->nbit && need(b, 1) < 0) return -1;
    code |= (int)(b->buf & 1u);
    b->buf >>= 1;
    b->nbit--;
    count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

static const short len_base[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short len_extra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short dist_base[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short dist_extra[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static huff_t fix_lit, fix_dist;
static int fix_ready;

static int codes(bits_t* b, unsigned char* dst, int n, int dlen, const huff_t* lit, const huff_t* dist) {
  for (;;) {
    int sym = huff_dec(b, lit);
    if (sym < 0) return -1;
    if (sym < 256) {
      if (n >= dlen) return -1;
      dst[n++] = (unsigned char)sym;
    } else if (sym == 256) {
      return n;
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
      if (back > n || n + len > dlen) return -1;
      for (i = 0; i < len; i++) dst[n + i] = dst[n - back + i];
      n += len;
    } else {
      return -1;
    }
  }
}

int zinflate(const unsigned char* src, int slen, unsigned char* dst, int dlen, int* used) {
  bits_t b;
  int nout = 0;
  if (slen < 2 || (src[0] & 15) != 8 || ((src[0] << 8) | src[1]) % 31) return -1;
  memset(&b, 0, sizeof b);
  b.p = src + 2;
  b.n = slen - 2;
  for (;;) {
    int bfinal = bits_n(&b, 1);
    int btype = bits_n(&b, 2);
    if (bfinal < 0 || btype < 0) return -1;
    if (btype == 0) {
      int len, nlen;
      b.buf = 0;
      b.nbit = 0;
      if (b.i + 4 > b.n) return -1;
      len = b.p[b.i] | (b.p[b.i + 1] << 8);
      nlen = b.p[b.i + 2] | (b.p[b.i + 3] << 8);
      b.i += 4;
      if ((len ^ 0xFFFF) != nlen || b.i + len > b.n || nout + len > dlen) return -1;
      memcpy(dst + nout, b.p + b.i, (size_t)len);
      nout += len;
      b.i += len;
    } else if (btype == 1 || btype == 2) {
      static huff_t lit, dist;
      int lens[320];
      int i;
      const huff_t* L = &lit;
      const huff_t* D = &dist;
      if (btype == 1) {
        if (!fix_ready) {
          for (i = 0; i < 288; i++) lens[i] = i <= 143 ? 8 : (i <= 255 ? 9 : (i <= 279 ? 7 : 8));
          huff_build(&fix_lit, lens, 288);
          for (i = 0; i < 30; i++) lens[i] = 5;
          huff_build(&fix_dist, lens, 30);
          fix_ready = 1;
        }
        L = &fix_lit;
        D = &fix_dist;
      } else {
        static const unsigned char order[19] = {
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
        if (nlit > 286 || ndist > 30) return -1;
        memset(clen, 0, sizeof clen);
        for (i = 0; i < nlen; i++) {
          int v = bits_n(&b, 3);
          if (v < 0) return -1;
          clen[order[i]] = v;
        }
        if (huff_build(&cl, clen, 19) < 0) return -1;
        i = 0;
        while (i < nlit + ndist) {
          int s = huff_dec(&b, &cl), rep, v = 0, k;
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
      nout = codes(&b, dst, nout, dlen, L, D);
      if (nout < 0) return -1;
    } else {
      return -1;
    }
    if (bfinal) break;
  }
  /* bytes inteiros que sobraram no buffer de bits voltam; depois vem o adler32 */
  b.i -= b.nbit / 8;
  if (b.i + 4 > b.n) return -1;
  if (used) *used = 2 + b.i + 4;
  return nout;
}
