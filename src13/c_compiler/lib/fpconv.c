/* fpconv.c - double <-> texto, exato (bignum de 32 bits por limb).
 * __fp_digits da os digitos decimais exatos de um double (printf arredonda
 * em cima deles); strtod arredonda certo (meio para o par), como o glibc.
 * gcc -mx32 -mno-sse (LIBCX) nao tem double: o arquivo fica vazio. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(__GNUC__) && defined(__x86_64__) && !defined(__SSE2__)
typedef int fpconv_sem_double;
#else

typedef unsigned long long u64;

/* 5^1100 tem ~2560 bits; o numerador do strtod ganha mais 2800 de digitos */
#define NL 200

typedef struct {
  unsigned d[NL];   /* little endian */
  int n;            /* limbs usados (sem zeros no topo) */
} Big;

static void big_trim(Big* b) {
  while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
}

static void big_set(Big* b, u64 v) {
  b->d[0] = (unsigned)v;
  b->d[1] = (unsigned)(v >> 32);
  b->n = 2;
  big_trim(b);
}

/* b = b * m + add */
static void big_mul_small(Big* b, unsigned m, unsigned add) {
  u64 carry = add;
  int i;
  for (i = 0; i < b->n; i++) {
    u64 t = (u64)b->d[i] * m + carry;
    b->d[i] = (unsigned)t;
    carry = t >> 32;
  }
  if (carry && b->n < NL) b->d[b->n++] = (unsigned)carry;
}

static void big_mul_pow5(Big* b, int k) {
  static const unsigned p5[] = { 1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125,
                                 9765625, 48828125, 244140625, 1220703125 };
  while (k >= 13) {
    big_mul_small(b, 1220703125u, 0);
    k -= 13;
  }
  if (k) big_mul_small(b, p5[k], 0);
}

static void big_shl(Big* b, int s) {
  int w = s / 32, r = s % 32, i;
  if (b->n == 0) return;
  if (b->n + w + 1 > NL) w = NL - b->n - 1; /* nao acontece com os limites do strtod */
  b->d[b->n] = 0;
  if (r) {
    for (i = b->n; i > 0; i--) b->d[i] = (b->d[i] << r) | (b->d[i - 1] >> (32 - r));
    b->d[0] <<= r;
  }
  b->n++;
  if (w) {
    for (i = b->n - 1; i >= 0; i--) b->d[i + w] = b->d[i];
    for (i = 0; i < w; i++) b->d[i] = 0;
    b->n += w;
  }
  big_trim(b);
}

static void big_shr1(Big* b) {
  int i;
  for (i = 0; i < b->n; i++) {
    b->d[i] >>= 1;
    if (i + 1 < b->n) b->d[i] |= b->d[i + 1] << 31;
  }
  big_trim(b);
}

static int big_bitlen(Big* b) {
  unsigned top;
  int n;
  if (b->n == 0) return 0;
  top = b->d[b->n - 1];
  n = (b->n - 1) * 32;
  while (top) {
    n++;
    top >>= 1;
  }
  return n;
}

static int big_bit(Big* b, int i) {
  if (i < 0 || i / 32 >= b->n) return 0;
  return (b->d[i / 32] >> (i % 32)) & 1;
}

/* algum bit abaixo da posicao i? */
static int big_any_below(Big* b, int i) {
  int w = i / 32, k;
  if (i <= 0) return 0;
  for (k = 0; k < w && k < b->n; k++)
    if (b->d[k]) return 1;
  if (w < b->n && (i % 32) && (b->d[w] & ((1u << (i % 32)) - 1))) return 1;
  return 0;
}

/* bits [lo, lo+cnt) com cnt <= 64 */
static u64 big_bits(Big* b, int lo, int cnt) {
  u64 v = 0;
  int i;
  for (i = cnt - 1; i >= 0; i--) v = (v << 1) | (u64)big_bit(b, lo + i);
  return v;
}

static int big_cmp(Big* a, Big* b) {
  int i;
  if (a->n != b->n) return a->n < b->n ? -1 : 1;
  for (i = a->n - 1; i >= 0; i--)
    if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
  return 0;
}

/* a -= b (a >= b) */
static void big_sub(Big* a, Big* b) {
  u64 borrow = 0;
  int i;
  for (i = 0; i < a->n; i++) {
    u64 t = (u64)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
    a->d[i] = (unsigned)t;
    borrow = (t >> 32) ? 1 : 0;
  }
  big_trim(a);
}

static unsigned big_divmod_small(Big* b, unsigned div) {
  u64 rem = 0;
  int i;
  for (i = b->n - 1; i >= 0; i--) {
    u64 cur = (rem << 32) | b->d[i];
    b->d[i] = (unsigned)(cur / div);
    rem = cur % div;
  }
  big_trim(b);
  return (unsigned)rem;
}

static double from_bits(u64 bits) {
  double d;
  memcpy(&d, &bits, 8);
  return d;
}

/* B * 2^bexp, arredondado para o double mais proximo; sticky = havia mais
 * alguma coisa abaixo de B (vale mais que B, nunca menos). */
static double big_to_double(Big* b, int sticky, int bexp) {
  int bl = big_bitlen(b);
  int e, q, drop;
  u64 m, bits;
  if (bl == 0) return 0.0;
  e = bl - 1 + bexp;
  if (e > 1023) {
    errno = ERANGE;
    return from_bits(0x7FF0000000000000ULL);
  }
  if (e < -1076) {
    errno = ERANGE;
    return 0.0;
  }
  q = e - 52;
  if (q < -1074) q = -1074;
  drop = q - bexp;
  if (drop <= 0) {
    m = big_bits(b, 0, bl) << -drop;
  } else {
    int half = big_bit(b, drop - 1);
    int rest = sticky || big_any_below(b, drop - 1);
    m = bl > drop ? big_bits(b, drop, bl - drop) : 0;
    if (half && (rest || (m & 1))) m++;
  }
  /* normal: (q+1075)<<52 | frac; subnormal: m; o carry vira o expoente */
  bits = ((u64)(q + 1074) << 52) + m;
  if (bits >= 0x7FF0000000000000ULL) {
    errno = ERANGE;
    bits = 0x7FF0000000000000ULL;
  }
  if (bits == 0) errno = ERANGE;
  return from_bits(bits);
}

static Big g_a, g_b, g_q;

/* Digitos decimais exatos de v (> 0, finito), sem zeros nas pontas:
 * v = 0.d1d2d3... * 10^exp. Grava em out (cabe 800) e devolve exp. */
int __fp_digits(double v, char* out, int* nd) {
  u64 bits, m;
  int e, e2, n = 0, exp10, i;
  unsigned chunks[100];
  int nch = 0;
  Big* b = &g_a;
  memcpy(&bits, &v, 8);
  e = (int)((bits >> 52) & 0x7FF);
  m = bits & 0xFFFFFFFFFFFFFULL;
  if (e == 0) e = 1;
  else m |= 1ULL << 52;
  e2 = e - 1075;
  if (m == 0) {
    *nd = 0;
    return 1;
  }
  while (!(m & 1)) {
    m >>= 1;
    e2++;
  }
  big_set(b, m);
  if (e2 >= 0) big_shl(b, e2);
  else big_mul_pow5(b, -e2);
  while (b->n) chunks[nch++] = big_divmod_small(b, 1000000000u);
  /* primeiro bloco sem zeros a esquerda */
  {
    char tmp[10];
    unsigned c = chunks[nch - 1];
    int k = 0;
    while (c) {
      tmp[k++] = (char)('0' + c % 10);
      c /= 10;
    }
    while (k) out[n++] = tmp[--k];
  }
  for (i = nch - 2; i >= 0; i--) {
    unsigned c = chunks[i];
    int k;
    for (k = 8; k >= 0; k--) {
      out[n + k] = (char)('0' + c % 10);
      c /= 10;
    }
    n += 9;
  }
  exp10 = n + (e2 >= 0 ? 0 : e2);
  while (n > 0 && out[n - 1] == '0') n--;
  *nd = n;
  return exp10;
}

/* ---------- strtod ---------- */
#define MAXD 800

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int match_word(const char* s, const char* w) {
  while (*w) {
    if (lower((unsigned char)*s) != *w) return 0;
    s++;
    w++;
  }
  return 1;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = lower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static double parse_hex(const char* p, const char** endp) {
  u64 m = 0;
  int e2 = 0, sticky = 0, seen = 0, d;
  while ((d = hexval((unsigned char)*p)) >= 0) {
    seen = 1;
    if (m < (1ULL << 59)) m = m * 16 + (u64)d;
    else {
      e2 += 4;
      if (d) sticky = 1;
    }
    p++;
  }
  if (*p == '.') {
    p++;
    while ((d = hexval((unsigned char)*p)) >= 0) {
      seen = 1;
      if (m < (1ULL << 59)) {
        m = m * 16 + (u64)d;
        e2 -= 4;
      } else if (d) {
        sticky = 1;
      }
      p++;
    }
  }
  if (!seen) {
    *endp = NULL;
    return 0;
  }
  if (lower((unsigned char)*p) == 'p') {
    const char* q = p + 1;
    int neg = 0, ex = 0;
    if (*q == '+' || *q == '-') neg = *q++ == '-';
    if (*q >= '0' && *q <= '9') {
      while (*q >= '0' && *q <= '9') {
        if (ex < 100000) ex = ex * 10 + (*q - '0');
        q++;
      }
      e2 += neg ? -ex : ex;
      p = q;
    }
  }
  *endp = p;
  big_set(&g_a, m);
  return big_to_double(&g_a, sticky, e2);
}

double strtod(const char* s, char** end) {
  static char dig[MAXD];
  const char* p = s;
  int neg = 0, nd = 0, e10 = 0, seen = 0, trunc = 0;
  double r;
  while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
  if (*p == '+' || *p == '-') neg = *p++ == '-';
  if (match_word(p, "inf")) {
    p += match_word(p, "infinity") ? 8 : 3;
    r = from_bits(0x7FF0000000000000ULL);
    goto done;
  }
  if (match_word(p, "nan")) {
    p += 3;
    if (*p == '(') {
      const char* q = p + 1;
      while ((*q >= '0' && *q <= '9') || (lower((unsigned char)*q) >= 'a' && lower((unsigned char)*q) <= 'z') ||
             *q == '_')
        q++;
      if (*q == ')') p = q + 1;
    }
    r = from_bits(0x7FF8000000000000ULL);
    goto done;
  }
  if (p[0] == '0' && lower((unsigned char)p[1]) == 'x') {
    const char* q;
    r = parse_hex(p + 2, &q);
    if (q) {
      p = q;
      goto done;
    }
    /* "0x" sem digitos: vale o 0 */
    p++;
    r = 0;
    goto done;
  }
  for (; *p >= '0' && *p <= '9'; p++) {
    seen = 1;
    if (nd == 0 && *p == '0') continue;
    if (nd < MAXD) dig[nd++] = *p;
    else {
      e10++;
      if (*p != '0') trunc = 1;
    }
  }
  if (*p == '.') {
    const char* q = p + 1;
    if (seen || (*q >= '0' && *q <= '9')) {
      for (p = q; *p >= '0' && *p <= '9'; p++) {
        seen = 1;
        if (nd == 0 && *p == '0') {
          e10--;
          continue;
        }
        if (nd < MAXD) {
          dig[nd++] = *p;
          e10--;
        } else if (*p != '0') {
          trunc = 1;
        }
      }
    }
  }
  if (!seen) {
    if (end) *end = (char*)s;
    return 0;
  }
  if (lower((unsigned char)*p) == 'e') {
    const char* q = p + 1;
    int eneg = 0, ex = 0;
    if (*q == '+' || *q == '-') eneg = *q++ == '-';
    if (*q >= '0' && *q <= '9') {
      while (*q >= '0' && *q <= '9') {
        if (ex < 100000) ex = ex * 10 + (*q - '0');
        q++;
      }
      e10 += eneg ? -ex : ex;
      p = q;
    }
  }
  while (nd > 0 && dig[nd - 1] == '0') {
    nd--;
    e10++;
  }
  if (nd == 0) {
    r = 0;
    goto done;
  }
  if (nd + e10 > 310) {
    errno = ERANGE;
    r = from_bits(0x7FF0000000000000ULL);
    goto done;
  }
  if (nd + e10 < -343) {
    errno = ERANGE;
    r = 0;
    goto done;
  }
  /* caminho rapido: inteiro exato em double e potencia de 10 exata */
  if (nd <= 15 && !trunc && e10 >= -22 && e10 <= 22) {
    static const double p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    u64 v = 0;
    int i;
    for (i = 0; i < nd; i++) v = v * 10 + (u64)(dig[i] - '0');
    r = (double)(long long)v;
    if (e10 < 0) r = r / p10[-e10];
    else r = r * p10[e10];
    goto done;
  }
  {
    Big* b = &g_a;
    int i = 0;
    b->n = 0;
    while (i < nd) {
      unsigned chunk = 0, mul = 1;
      int k;
      for (k = 0; k < 9 && i < nd; k++, i++) {
        chunk = chunk * 10 + (unsigned)(dig[i] - '0');
        mul *= 10;
      }
      if (b->n == 0) big_set(b, chunk);
      else big_mul_small(b, mul, chunk);
    }
    if (e10 >= 0) {
      big_mul_pow5(b, e10);
      r = big_to_double(b, trunc, e10);
    } else {
      /* D / 5^k com quociente de 66+ bits; o resto vira sticky */
      Big* m = &g_b;
      Big* q = &g_q;
      int k = -e10, s, shift, j;
      big_set(m, 1);
      big_mul_pow5(m, k);
      s = big_bitlen(m) - big_bitlen(b) + 66;
      if (s < 0) s = 0;
      big_shl(b, s);
      shift = big_bitlen(b) - big_bitlen(m);
      if (shift < 0) shift = 0;
      big_shl(m, shift);
      memset(q->d, 0, sizeof(q->d));
      q->n = shift / 32 + 1;
      for (j = shift; j >= 0; j--) {
        if (big_cmp(b, m) >= 0) {
          big_sub(b, m);
          q->d[j / 32] |= 1u << (j % 32);
        }
        big_shr1(m);
      }
      big_trim(q);
      r = big_to_double(q, trunc || b->n != 0, e10 - s);
    }
  }
done:
  if (end) *end = (char*)p;
  return neg ? -r : r;
}

float strtof(const char* s, char** end) { return (float)strtod(s, end); }
long double strtold(const char* s, char** end) { return strtod(s, end); }
double atof(const char* s) { return strtod(s, NULL); }

#endif
