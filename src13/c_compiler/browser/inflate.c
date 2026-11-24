/* inflate.c - descompacta o corpo HTTP com Content-Encoding gzip ou deflate
 * (RFC 1950, 1951 e 1952). Decodificador canonico no estilo do puff.c de Mark
 * Adler: le um bit de cada vez, sem tabelas grandes. A saida inteira fica na
 * memoria, entao as referencias para tras copiam da propria saida.
 *
 * Com gzip, o HTML e o CSS chegam de 4 a 6 vezes menores: na rede do SO o
 * tempo de carga e quase todo transferencia. */
#include <stdlib.h>
#include <string.h>
#include "browser.h"

#define OUT_MAX (12 * 1024 * 1024)
#define MAXBITS 15

typedef struct {
  const unsigned char* in;
  int in_len, pos;
  unsigned bitbuf;
  int bitcnt;
  unsigned char* out;
  int out_len, out_cap;
  int err;
} inf_t;

typedef struct {
  short count[MAXBITS + 1];
  short sym[288];
} huff_t;

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const short clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static huff_t fixed_len, fixed_dist;
static int fixed_ready;

/* need bits do fluxo, o primeiro no bit baixo. Sem entrada: err = 1 e 0. */
static int bits(inf_t* s, int need) {
  unsigned val = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->pos >= s->in_len) {
      s->err = 1;
      return 0;
    }
    val |= (unsigned)s->in[s->pos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = val >> need;
  s->bitcnt -= need;
  return (int)(val & ((1u << need) - 1));
}

/* garante lugar para mais n bytes na saida (e um NUL no fim) */
static int reserve(inf_t* s, int n) {
  int nc;
  unsigned char* nb;
  if (s->out_len + n <= s->out_cap) return 0;
  nc = s->out_cap ? s->out_cap : 65536;
  while (nc < s->out_len + n) nc *= 2;
  if (nc > OUT_MAX) nc = OUT_MAX;
  if (s->out_len + n > nc) {
    s->err = 2;
    return -1;
  }
  nb = (unsigned char*)realloc(s->out, (size_t)nc + 1);
  if (!nb) {
    s->err = 2;
    return -1;
  }
  s->out = nb;
  s->out_cap = nc;
  return 0;
}

/* tabela canonica a partir dos comprimentos; < 0 se sobra codigo demais,
 * > 0 se o conjunto e incompleto (aceito so em alguns casos), 0 se completo */
static int construct(huff_t* h, const short* length, int n) {
  int sym, len, left;
  short offs[MAXBITS + 1];
  for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
  for (sym = 0; sym < n; sym++) h->count[length[sym]]++;
  if (h->count[0] == n) return 0;
  left = 1;
  for (len = 1; len <= MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return left;
  }
  offs[1] = 0;
  for (len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
  for (sym = 0; sym < n; sym++)
    if (length[sym] != 0) h->sym[offs[length[sym]]++] = (short)sym;
  return left;
}

static int decode(inf_t* s, const huff_t* h) {
  int len, code = 0, first = 0, index = 0, count;
  for (len = 1; len <= MAXBITS; len++) {
    if (!s->bitcnt) {
      if (s->pos >= s->in_len) {
        s->err = 1;
        return -1;
      }
      s->bitbuf = s->in[s->pos++];
      s->bitcnt = 8;
    }
    code |= (int)(s->bitbuf & 1);
    s->bitbuf >>= 1;
    s->bitcnt--;
    count = h->count[len];
    if (code - count < first) return h->sym[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  s->err = 3;
  return -1;
}

static int codes(inf_t* s, const huff_t* lencode, const huff_t* distcode) {
  for (;;) {
    int sym = decode(s, lencode);
    if (sym < 0) return -1;
    if (sym < 256) {
      if (reserve(s, 1)) return -1;
      s->out[s->out_len++] = (unsigned char)sym;
    } else if (sym == 256) {
      return 0;
    } else {
      int len, dist, k;
      unsigned char* o;
      const unsigned char* f;
      sym -= 257;
      if (sym >= 29) {
        s->err = 4;
        return -1;
      }
      len = lbase[sym] + bits(s, lext[sym]);
      sym = decode(s, distcode);
      if (sym < 0) return -1;
      if (sym >= 30) {
        s->err = 4;
        return -1;
      }
      dist = dbase[sym] + bits(s, dext[sym]);
      if (s->err) return -1;
      if (dist > s->out_len) {
        s->err = 5;
        return -1;
      }
      if (reserve(s, len)) return -1;
      o = s->out + s->out_len;
      f = o - dist;
      for (k = 0; k < len; k++) o[k] = f[k];
      s->out_len += len;
    }
  }
}

static int stored(inf_t* s) {
  int len, nlen;
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->pos + 4 > s->in_len) {
    s->err = 1;
    return -1;
  }
  len = s->in[s->pos] | (s->in[s->pos + 1] << 8);
  nlen = s->in[s->pos + 2] | (s->in[s->pos + 3] << 8);
  if (len != (~nlen & 0xFFFF)) {
    s->err = 6;
    return -1;
  }
  s->pos += 4;
  if (s->pos + len > s->in_len) {
    s->err = 1;
    return -1;
  }
  if (reserve(s, len)) return -1;
  memcpy(s->out + s->out_len, s->in + s->pos, (size_t)len);
  s->pos += len;
  s->out_len += len;
  return 0;
}

static void build_fixed(void) {
  short lengths[288];
  int i;
  for (i = 0; i < 144; i++) lengths[i] = 8;
  for (; i < 256; i++) lengths[i] = 9;
  for (; i < 280; i++) lengths[i] = 7;
  for (; i < 288; i++) lengths[i] = 8;
  construct(&fixed_len, lengths, 288);
  for (i = 0; i < 30; i++) lengths[i] = 5;
  construct(&fixed_dist, lengths, 30);
  fixed_ready = 1;
}

static int dynamic(inf_t* s) {
  int nlen, ndist, ncode, index, rc;
  short lengths[320];
  huff_t lencode, distcode;
  nlen = bits(s, 5) + 257;
  ndist = bits(s, 5) + 1;
  ncode = bits(s, 4) + 4;
  if (s->err || nlen > 286 || ndist > 30) return -1;
  for (index = 0; index < ncode; index++) lengths[clorder[index]] = (short)bits(s, 3);
  for (; index < 19; index++) lengths[clorder[index]] = 0;
  if (s->err) return -1;
  if (construct(&lencode, lengths, 19) != 0) return -1;
  index = 0;
  while (index < nlen + ndist) {
    int symbol = decode(s, &lencode);
    int len = 0;
    if (symbol < 0) return -1;
    if (symbol < 16) {
      lengths[index++] = (short)symbol;
      continue;
    }
    if (symbol == 16) {
      if (index == 0) return -1;
      len = lengths[index - 1];
      symbol = 3 + bits(s, 2);
    } else if (symbol == 17) {
      symbol = 3 + bits(s, 3);
    } else {
      symbol = 11 + bits(s, 7);
    }
    if (s->err || index + symbol > nlen + ndist) return -1;
    while (symbol--) lengths[index++] = (short)len;
  }
  if (lengths[256] == 0) return -1;
  rc = construct(&lencode, lengths, nlen);
  if (rc < 0 || (rc > 0 && nlen - lencode.count[0] != 1)) return -1;
  rc = construct(&distcode, lengths + nlen, ndist);
  if (rc < 0 || (rc > 0 && ndist - distcode.count[0] != 1)) return -1;
  return codes(s, &lencode, &distcode);
}

static int run(inf_t* s) {
  int last;
  do {
    int type, rc;
    last = bits(s, 1);
    type = bits(s, 2);
    if (s->err) return -1;
    if (type == 0) {
      rc = stored(s);
    } else if (type == 1) {
      if (!fixed_ready) build_fixed();
      rc = codes(s, &fixed_len, &fixed_dist);
    } else if (type == 2) {
      rc = dynamic(s);
    } else {
      return -1;
    }
    if (rc != 0) return -1;
  } while (!last);
  return 0;
}

/* tamanho do cabecalho gzip (RFC 1952), 0 se nao e gzip, -1 se cortado */
static int gzip_header(const unsigned char* b, int n) {
  int p = 10, flg;
  if (n < 18 || b[0] != 0x1F || b[1] != 0x8B || b[2] != 8) return 0;
  flg = b[3];
  if (flg & 4) { /* FEXTRA */
    if (p + 2 > n) return -1;
    p += 2 + (b[p] | (b[p + 1] << 8));
  }
  if (flg & 8) { /* FNAME */
    while (p < n && b[p]) p++;
    p++;
  }
  if (flg & 16) { /* FCOMMENT */
    while (p < n && b[p]) p++;
    p++;
  }
  if (flg & 2) p += 2; /* FHCRC */
  return p < n ? p : -1;
}

int inflate_body(const unsigned char* in, int n, char** out, int* out_len) {
  inf_t s;
  int skip;
  *out = 0;
  *out_len = 0;
  if (!in || n <= 0) return -1;
  memset(&s, 0, sizeof(s));
  skip = gzip_header(in, n);
  if (skip < 0) return -1;
  /* deflate do HTTP e quase sempre zlib (RFC 1950); alguns servidores mandam cru */
  if (!skip && n >= 2 && (in[0] & 0x0F) == 8 && ((in[0] << 8) | in[1]) % 31 == 0) skip = 2;
  s.in = in + skip;
  s.in_len = n - skip;
  if (run(&s) != 0 && (s.err == 2 || s.out_len == 0)) {
    /* sem memoria, ou nada saiu: falhou. Cortado no meio: fica o que veio. */
    free(s.out);
    return -1;
  }
  if (!s.out && reserve(&s, 1) != 0) return -1;
  s.out[s.out_len] = 0; /* reserve deixa sempre um byte a mais para o NUL */
  *out = (char*)s.out;
  *out_len = s.out_len;
  return 0;
}
