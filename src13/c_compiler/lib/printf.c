/* printf.c - familia printf (sem ponto flutuante: %f/%e/%g imprimem '?'). */
#include <stdio.h>
#include <string.h>

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
      case 'f': case 'e': case 'g': case 'F': case 'E': case 'G':
        (void)va_arg(ap, long long);
        emit(s, '?');
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
