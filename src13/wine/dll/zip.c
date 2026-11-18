/*
 * zip.c - Leitor de ZIP (com ZIP64) e inflate, para pacotes .msix/.appx.
 *
 * O arquivo inteiro esta na memoria (o .msix baixado fica em RAM, file.c).
 * zip_open acha o diretorio central; zip_next percorre as entradas;
 * zip_extract devolve o conteudo descomprimido num bloco novo (k_malloc).
 * Metodos: 0 (guardado) e 8 (deflate).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "zip.h"

#define R16(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8))
#define R32(p) (R16(p) | (R16((p) + 2) << 16))
#define R64(p) ((uint64_t)R32(p) | ((uint64_t)R32((p) + 4) << 32))

/* ---------- inflate (RFC 1951) ---------- */
struct inf {
  const uint8_t* in;
  const uint8_t* end;
  uint32_t bits;
  int nbits;
  uint8_t* out;
  size_t outn, outcap;
  int err;
};
struct huff {
  uint16_t count[16];
  uint16_t symbol[288];
};

static int getbits(struct inf* s, int n) {
  uint32_t v;
  while (s->nbits < n) {
    if (s->in >= s->end) {
      s->err = 1;
      return 0;
    }
    s->bits |= (uint32_t)*s->in++ << s->nbits;
    s->nbits += 8;
  }
  v = s->bits & ((1u << n) - 1u);
  s->bits >>= n;
  s->nbits -= n;
  return (int)v;
}

static void huff_build(struct huff* h, const uint8_t* len, int n) {
  uint16_t offs[16];
  int i;
  for (i = 0; i < 16; i++) h->count[i] = 0;
  for (i = 0; i < n; i++) h->count[len[i]]++;
  h->count[0] = 0;
  offs[1] = 0;
  for (i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
  for (i = 0; i < n; i++)
    if (len[i]) h->symbol[offs[len[i]]++] = (uint16_t)i;
}

static int huff_decode(struct inf* s, const struct huff* h) {
  int code = 0, first = 0, index = 0, len;
  for (len = 1; len <= 15; len++) {
    int count;
    code |= getbits(s, 1);
    if (s->err) return -1;
    count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  s->err = 1;
  return -1;
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                       33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void inf_codes(struct inf* s, const struct huff* lc, const struct huff* dc) {
  for (;;) {
    int sym = huff_decode(s, lc);
    if (s->err) return;
    if (sym < 256) {
      if (s->outn >= s->outcap) {
        s->err = 2;
        return;
      }
      s->out[s->outn++] = (uint8_t)sym;
    } else if (sym == 256) {
      return;
    } else {
      int len, dist, ds;
      sym -= 257;
      if (sym >= 29) {
        s->err = 1;
        return;
      }
      len = len_base[sym] + getbits(s, len_extra[sym]);
      ds = huff_decode(s, dc);
      if (s->err || ds >= 30) {
        s->err = 1;
        return;
      }
      dist = dist_base[ds] + getbits(s, dist_extra[ds]);
      if (s->err || (size_t)dist > s->outn || s->outn + (size_t)len > s->outcap) {
        s->err = s->err ? s->err : 2;
        return;
      }
      {
        uint8_t* d = s->out + s->outn;
        const uint8_t* f = d - dist;
        int i;
        for (i = 0; i < len; i++) d[i] = f[i];
        s->outn += (size_t)len;
      }
    }
  }
}

/* Descomprime deflate cru. Devolve bytes gerados ou -1. */
long zip_inflate(const uint8_t* in, size_t inlen, uint8_t* out, size_t outcap) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  struct inf s;
  struct huff* lc = (struct huff*)k_malloc(sizeof(struct huff) * 2);
  struct huff* dc = lc + 1;
  int last;
  if (!lc) return -1;
  memset(&s, 0, sizeof(s));
  s.in = in;
  s.end = in + inlen;
  s.out = out;
  s.outcap = outcap;
  do {
    int type;
    last = getbits(&s, 1);
    type = getbits(&s, 2);
    if (s.err) break;
    if (type == 0) {
      uint32_t len;
      s.bits = 0;
      s.nbits = 0;
      if (s.in + 4 > s.end) {
        s.err = 1;
        break;
      }
      len = R16(s.in);
      s.in += 4;
      if (s.in + len > s.end || s.outn + len > s.outcap) {
        s.err = 1;
        break;
      }
      memcpy(s.out + s.outn, s.in, len);
      s.in += len;
      s.outn += len;
    } else if (type == 1) {
      uint8_t l[288 + 30];
      int i;
      for (i = 0; i < 144; i++) l[i] = 8;
      for (; i < 256; i++) l[i] = 9;
      for (; i < 280; i++) l[i] = 7;
      for (; i < 288; i++) l[i] = 8;
      huff_build(lc, l, 288);
      for (i = 0; i < 30; i++) l[i] = 5;
      huff_build(dc, l, 30);
      inf_codes(&s, lc, dc);
    } else if (type == 2) {
      uint8_t l[288 + 32];
      int nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1, ncode = getbits(&s, 4) + 4, i;
      if (s.err || nlen > 286 || ndist > 30) {
        s.err = 1;
        break;
      }
      for (i = 0; i < 19; i++) l[i] = 0;
      for (i = 0; i < ncode; i++) l[order[i]] = (uint8_t)getbits(&s, 3);
      huff_build(lc, l, 19);
      for (i = 0; i < nlen + ndist && !s.err;) {
        int sym = huff_decode(&s, lc);
        if (s.err) break;
        if (sym < 16) {
          l[i++] = (uint8_t)sym;
        } else {
          int prev = 0, rep;
          if (sym == 16) {
            if (!i) {
              s.err = 1;
              break;
            }
            prev = l[i - 1];
            rep = 3 + getbits(&s, 2);
          } else if (sym == 17) {
            rep = 3 + getbits(&s, 3);
          } else {
            rep = 11 + getbits(&s, 7);
          }
          if (i + rep > nlen + ndist) {
            s.err = 1;
            break;
          }
          while (rep--) l[i++] = (uint8_t)prev;
        }
      }
      if (s.err) break;
      huff_build(lc, l, nlen);
      huff_build(dc, l + nlen, ndist);
      inf_codes(&s, lc, dc);
    } else {
      s.err = 1;
    }
  } while (!last && !s.err);
  k_free(lc);
  return s.err ? -1 : (long)s.outn;
}

/* ---------- ZIP ---------- */
int zip_open(struct zip* z, const uint8_t* data, uint64_t size) {
  uint64_t i, lo;
  const uint8_t* e = 0;
  memset(z, 0, sizeof(*z));
  z->data = data;
  z->size = size;
  if (size < 22) return 0;
  lo = size > 0x10016 ? size - 0x10016 : 0;
  for (i = size - 22;; i--) {
    if (R32(data + i) == 0x06054B50u) {
      e = data + i;
      break;
    }
    if (i == lo) break;
  }
  if (!e) return 0;
  z->count = R16(e + 10);
  z->cd_size = R32(e + 12);
  z->cd_off = R32(e + 16);
  /* ZIP64: localizador 20 bytes antes do fim do diretorio */
  if ((z->count == 0xFFFF || z->cd_off == 0xFFFFFFFFu || z->cd_size == 0xFFFFFFFFu) && e - data >= 20 &&
      R32(e - 20) == 0x07064B50u) {
    uint64_t off64 = R64(e - 20 + 8);
    if (off64 + 56 <= size && R32(data + off64) == 0x06064B50u) {
      z->count = R64(data + off64 + 32);
      z->cd_size = R64(data + off64 + 40);
      z->cd_off = R64(data + off64 + 48);
    }
  }
  if (z->cd_off + z->cd_size > size) return 0;
  z->pos = z->cd_off;
  return 1;
}

int zip_next(struct zip* z, struct zip_entry* en) {
  const uint8_t* c;
  uint32_t nlen, xlen, clen, n;
  if (z->index >= z->count || z->pos + 46 > z->cd_off + z->cd_size) return 0;
  c = z->data + z->pos;
  if (R32(c) != 0x02014B50u) return 0;
  memset(en, 0, sizeof(*en));
  en->method = (int)R16(c + 10);
  en->crc = R32(c + 16);
  en->csize = R32(c + 20);
  en->usize = R32(c + 24);
  nlen = R16(c + 28);
  xlen = R16(c + 30);
  clen = R16(c + 32);
  en->local = R32(c + 42);
  n = nlen < sizeof(en->name) - 1 ? nlen : (uint32_t)sizeof(en->name) - 1;
  memcpy(en->name, c + 46, n);
  en->name[n] = 0;
  {
    /* campo extra 0x0001: tamanhos e offset de 64 bits, so os que estouraram */
    const uint8_t* x = c + 46 + nlen;
    const uint8_t* xe = x + xlen;
    while (x + 4 <= xe) {
      uint32_t id = R16(x), sz = R16(x + 2);
      const uint8_t* v = x + 4;
      if (id == 1) {
        if (en->usize == 0xFFFFFFFFu && v + 8 <= xe) {
          en->usize = R64(v);
          v += 8;
        }
        if (en->csize == 0xFFFFFFFFu && v + 8 <= xe) {
          en->csize = R64(v);
          v += 8;
        }
        if (en->local == 0xFFFFFFFFu && v + 8 <= xe) en->local = R64(v);
      }
      x += 4 + sz;
    }
  }
  en->is_dir = nlen && en->name[n - 1] == '/';
  z->pos += 46 + nlen + xlen + clen;
  z->index++;
  return 1;
}

/* Conteudo descomprimido num bloco novo (k_free depois); 0 se falhou. */
uint8_t* zip_extract(struct zip* z, const struct zip_entry* en) {
  const uint8_t* l;
  const uint8_t* src;
  uint8_t* out;
  if (en->local + 30 > z->size) return 0;
  l = z->data + en->local;
  if (R32(l) != 0x04034B50u) return 0;
  src = l + 30 + R16(l + 26) + R16(l + 28);
  if ((uint64_t)(src - z->data) + en->csize > z->size || en->usize > 0x7FF00000u) return 0;
  out = (uint8_t*)k_malloc((unsigned)en->usize + 1u);
  if (!out) return 0;
  if (en->method == 0) {
    memcpy(out, src, (size_t)en->usize);
  } else if (en->method == 8) {
    if (zip_inflate(src, (size_t)en->csize, out, (size_t)en->usize) != (long)en->usize) {
      k_free(out);
      return 0;
    }
  } else {
    k_free(out);
    return 0;
  }
  return out;
}
