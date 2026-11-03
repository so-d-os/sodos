/* printf.c - familia printf. %f/%e/%g/%a arredondam os digitos exatos do
 * double (fpconv.c) meio para o par, como o glibc. */
#include <stdio.h>
#include <string.h>

/* gcc -mx32 -mno-sse (LIBCX): sem double; o argumento nem vem na pilha. */
#if defined(__GNUC__) && defined(__x86_64__) && !defined(__SSE2__)
#define NO_FLOAT 1
#endif

struct sink {
  FILE* f;      /* destino arquivo, ou */
  char* buf;    /* destino memoria */
  size_t cap;
  size_t len;   /* total que seria escrito */
};

static void emit(struct sink* s, char c) {
  if (s->f) fputc(c, s->f);
  else if (s->len + 1 < s->cap) s->buf[s->len] = c;
  s->len++;
}

static void emit_n(struct sink* s, char c, int n) {
  while (n-- > 0) emit(s, c);
}

#ifndef NO_FLOAT
int __fp_digits(double v, char* out, int* nd);

#define FP_DIG 800
#define FP_OUT 1200

/* Arredonda d[0..nd) para keep digitos (meio para o par). Devolve o novo exp. */
static int fp_round(char* d, int* nd, int exp, int keep) {
  int i, up;
  if (keep >= *nd) return exp;
  if (keep < 0) {
    *nd = 0;
    return exp;
  }
  if (d[keep] > '5') up = 1;
  else if (d[keep] < '5') up = 0;
  else {
    up = 0;
    for (i = keep + 1; i < *nd; i++)
      if (d[i] != '0') up = 1;
    if (!up) up = keep > 0 && ((d[keep - 1] - '0') & 1);
  }
  *nd = keep;
  if (up) {
    for (i = keep - 1; i >= 0; i--) {
      if (d[i] < '9') {
        d[i]++;
        break;
      }
      d[i] = '0';
    }
    if (i < 0) {
      d[0] = '1';
      *nd = 1;
      exp++;
    }
  }
  while (*nd > 0 && d[*nd - 1] == '0') (*nd)--;
  return exp;
}

static char fp_dig(const char* d, int nd, int i) { return i >= 0 && i < nd ? d[i] : '0'; }

/* corpo sem sinal nem padding; devolve o tamanho */
static int fp_fixed(char* o, const char* d, int nd, int exp, int prec, int alt) {
  int n = 0, i;
  if (exp <= 0) o[n++] = '0';
  else
    for (i = 0; i < exp; i++) o[n++] = fp_dig(d, nd, i);
  if (prec > 0 || alt) o[n++] = '.';
  for (i = 0; i < prec; i++) o[n++] = fp_dig(d, nd, exp + i);
  return n;
}

static int fp_expo(char* o, const char* d, int nd, int exp, int prec, int alt, int upper) {
  int n = 0, i, x = nd ? exp - 1 : 0;
  char t[8];
  int k = 0;
  o[n++] = fp_dig(d, nd, 0);
  if (prec > 0 || alt) o[n++] = '.';
  for (i = 1; i <= prec; i++) o[n++] = fp_dig(d, nd, i);
  o[n++] = upper ? 'E' : 'e';
  o[n++] = x < 0 ? '-' : '+';
  if (x < 0) x = -x;
  do {
    t[k++] = (char)('0' + x % 10);
    x /= 10;
  } while (x);
  if (k < 2) t[k++] = '0';
  while (k) o[n++] = t[--k];
  return n;
}

/* %g sem '#': tira zeros da fracao e o ponto que sobrar */
static int fp_strip(char* o, int n) {
  int dot = -1, e = n, i;
  for (i = 0; i < n; i++) {
    if (o[i] == '.') dot = i;
    if (o[i] == 'e' || o[i] == 'E') {
      e = i;
      break;
    }
  }
  if (dot < 0) return n;
  i = e;
  while (i > dot + 1 && o[i - 1] == '0') i--;
  if (i == dot + 1) i = dot;
  memmove(o + i, o + e, (size_t)(n - e));
  return i + (n - e);
}

static int fp_hex(char* o, double v, int prec, int upper) {
  unsigned long long bits, m;
  int e, n = 0, i, nd;
  const char* hx = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char t[8];
  int k = 0;
  memcpy(&bits, &v, 8);
  e = (int)((bits >> 52) & 0x7FF);
  m = bits & 0xFFFFFFFFFFFFFULL;
  o[n++] = '0';
  o[n++] = upper ? 'X' : 'x';
  if (e == 0 && m == 0) {
    o[n++] = '0';
    e = 0;
  } else {
    int lead = 1;
    if (e == 0) {
      /* subnormal: 0x0.xxxp-1022, como o glibc */
      e = -1022;
      lead = 0;
    } else {
      e -= 1023;
    }
    if (prec >= 0 && prec < 13) {
      int drop = (13 - prec) * 4;
      unsigned long long half = 1ULL << (drop - 1);
      unsigned long long rest = m & ((half << 1) - 1);
      unsigned long long full = (m | ((unsigned long long)lead << 52)) >> drop;
      if (rest > half || (rest == half && (full & 1))) full++;
      o[n++] = (char)('0' + (int)(full >> (52 - drop)));
      m = (full << drop) & 0xFFFFFFFFFFFFFULL;
    } else {
      o[n++] = (char)('0' + lead);
    }
    nd = 13;
    if (prec < 0)
      while (nd > 0 && !((m >> ((13 - nd) * 4)) & 15)) nd--;
    else
      nd = prec;
    if (nd > 0) o[n++] = '.';
    for (i = 0; i < nd; i++) o[n++] = i < 13 ? hx[(m >> ((12 - i) * 4)) & 15] : '0';
  }
  o[n++] = upper ? 'P' : 'p';
  o[n++] = e < 0 ? '-' : '+';
  if (e < 0) e = -e;
  do {
    t[k++] = (char)('0' + e % 10);
    e /= 10;
  } while (e);
  while (k) o[n++] = t[--k];
  return n;
}

static void fmt_float(struct sink* s, int c, double v, int width, int prec, int left, int zero,
                      int plus, int space, int alt) {
  static char d[FP_DIG];
  static char o[FP_OUT];
  unsigned long long bits;
  int neg, nd = 0, exp = 1, n, upper = c == 'F' || c == 'E' || c == 'G' || c == 'A';
  int lc = upper ? c + 32 : c;
  char sign = 0;
  int pad, i;
  memcpy(&bits, &v, 8);
  neg = (int)(bits >> 63);
  if (neg) sign = '-';
  else if (plus) sign = '+';
  else if (space) sign = ' ';
  if (((bits >> 52) & 0x7FF) == 0x7FF) {
    const char* w = (bits & 0xFFFFFFFFFFFFFULL) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
    memcpy(o, w, 3);
    n = 3;
    zero = 0;
  } else if (lc == 'a') {
    n = fp_hex(o, v, prec, upper);
  } else {
    if (prec < 0) prec = 6;
    if (prec > 700) prec = 700;
    if (bits << 1) exp = __fp_digits(neg ? -v : v, d, &nd);
    if (lc == 'f') {
      exp = fp_round(d, &nd, exp, exp + prec);
      n = fp_fixed(o, d, nd, exp, prec, alt);
    } else if (lc == 'e') {
      exp = fp_round(d, &nd, exp, prec + 1);
      if (!nd) exp = 1;
      n = fp_expo(o, d, nd, exp, prec, alt, upper);
    } else {
      int p = prec ? prec : 1, x;
      exp = fp_round(d, &nd, exp, p);
      x = nd ? exp - 1 : 0;
      if (p > x && x >= -4) n = fp_fixed(o, d, nd, exp, p - 1 - x, alt);
      else n = fp_expo(o, d, nd, nd ? exp : 1, p - 1, alt, upper);
      if (!alt) n = fp_strip(o, n);
    }
  }
  pad = width - n - (sign ? 1 : 0);
  if (!left && !zero) emit_n(s, ' ', pad);
  if (sign) emit(s, sign);
  if (!left && zero) {
    /* 0x de %a vem antes dos zeros */
    i = 0;
    if (lc == 'a' && n >= 2) {
      emit(s, o[0]);
      emit(s, o[1]);
      i = 2;
    }
    emit_n(s, '0', pad);
    for (; i < n; i++) emit(s, o[i]);
  } else {
    for (i = 0; i < n; i++) emit(s, o[i]);
  }
  if (left) emit_n(s, ' ', pad);
}
#endif

static int format(struct sink* s, const char* fmt, va_list ap) {
  char tmp[72];
  while (*fmt) {
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
    int width = 0, prec = -1, lng = 0;
    int base = 10, sgn = 0, upper = 0;
    unsigned long long v = 0;
    const char* str;
    const char* prefix = "";
    int len, i, pad;
    char c = *fmt++;
    if (c != '%') {
      emit(s, c);
      continue;
    }
    for (;; fmt++) {
      if (*fmt == '-') left = 1;
      else if (*fmt == '0') zero = 1;
      else if (*fmt == '+') plus = 1;
      else if (*fmt == ' ') space = 1;
      else if (*fmt == '#') alt = 1;
      else break;
    }
    if (*fmt == '*') {
      width = va_arg(ap, int);
      if (width < 0) {
        left = 1;
        width = -width;
      }
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    }
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
      }
    }
    for (;; fmt++) {
      if (*fmt == 'l') lng++;
      else if (*fmt == 'L' || *fmt == 'q') lng = 2;
      else if (*fmt == 'h' || *fmt == 'z' || *fmt == 't' || *fmt == 'j') {
        if (*fmt == 'j') lng = 2;
      } else break;
    }
    c = *fmt;
    if (!c) break;
    fmt++;
    switch (c) {
      case '%':
        emit(s, '%');
        continue;
      case 'c':
        tmp[0] = (char)va_arg(ap, int);
        str = tmp;
        len = 1;
        goto put_str;
      case 's':
        str = va_arg(ap, const char*);
        if (!str) str = "(null)";
        len = 0;
        while (str[len] && (prec < 0 || len < prec)) len++;
      put_str:
        pad = width - len;
        if (!left) emit_n(s, ' ', pad);
        for (i = 0; i < len; i++) emit(s, str[i]);
        if (left) emit_n(s, ' ', pad);
        continue;
      case 'n':
        *va_arg(ap, int*) = (int)s->len;
        continue;
      case 'f': case 'e': case 'g': case 'a': case 'F': case 'E': case 'G': case 'A':
#ifdef NO_FLOAT
        emit(s, '?');
#else
        fmt_float(s, c, va_arg(ap, double), width, prec, left, zero, plus, space, alt);
#endif
        continue;
      case 'p':
        v = (unsigned)va_arg(ap, void*);
        base = 16;
        prefix = "0x";
        prec = 8;
        break;
      case 'd': case 'i':
        sgn = 1;
        if (lng >= 2) {
          long long x = va_arg(ap, long long);
          if (x < 0) {
            v = (unsigned long long)-x;
            prefix = "-";
          } else v = (unsigned long long)x;
        } else {
          int x = va_arg(ap, int);
          if (x < 0) {
            v = (unsigned)-x;
            prefix = "-";
          } else v = (unsigned)x;
        }
        if (!*prefix) prefix = plus ? "+" : space ? " " : "";
        break;
      case 'u': case 'x': case 'X': case 'o':
        if (lng >= 2) v = va_arg(ap, unsigned long long);
        else v = va_arg(ap, unsigned);
        if (c == 'x' || c == 'X') {
          base = 16;
          upper = c == 'X';
          if (alt && v) prefix = upper ? "0X" : "0x";
        } else if (c == 'o') {
          base = 8;
          if (alt) prefix = "0";
        }
        break;
      default:
        emit(s, '%');
        emit(s, c);
        continue;
    }
    (void)sgn;
    /* numero */
    {
      const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
      int n = 0;
      int plen = (int)strlen(prefix);
      int zeros;
      if (v >> 32) {
        while (v) {
          tmp[n++] = digs[v % (unsigned)base];
          v /= (unsigned)base;
        }
      } else {
        unsigned w = (unsigned)v;
        while (w) {
          tmp[n++] = digs[w % (unsigned)base];
          w /= (unsigned)base;
        }
      }
      if (n == 0 && prec != 0) tmp[n++] = '0';
      zeros = prec > n ? prec - n : 0;
      if (prec < 0 && zero && !left && width > n + plen) zeros = width - n - plen;
      pad = width - n - plen - zeros;
      if (!left) emit_n(s, ' ', pad);
      for (i = 0; i < plen; i++) emit(s, prefix[i]);
      emit_n(s, '0', zeros);
      while (n > 0) emit(s, tmp[--n]);
      if (left) emit_n(s, ' ', pad);
    }
  }
  return (int)s->len;
}

int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
  struct sink s;
  int r;
  s.f = NULL;
  s.buf = buf;
  s.cap = n;
  s.len = 0;
  r = format(&s, fmt, ap);
  if (n > 0) buf[s.len < n ? s.len : n - 1] = 0;
  return r;
}

int vsprintf(char* buf, const char* fmt, va_list ap) {
  return vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
  struct sink s;
  s.f = f;
  s.buf = NULL;
  s.cap = 0;
  s.len = 0;
  return format(&s, fmt, ap);
}

int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return r;
}

int fprintf(FILE* f, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vfprintf(f, fmt, ap);
  va_end(ap);
  return r;
}

int sprintf(char* buf, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsprintf(buf, fmt, ap);
  va_end(ap);
  return r;
}

int snprintf(char* buf, size_t n, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
