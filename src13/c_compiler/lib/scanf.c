/* scanf.c - familia scanf: %d %i %u %o %x %X %p %c %s %[...] %n %%, com
 * h/hh/l/ll/j/z/t/L, largura e '*'; %f %e %g %a leem com strtod. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) && defined(__x86_64__) && !defined(__SSE2__)
#define NO_FLOAT 1
#endif

struct src {
  FILE* f;
  const char* s;
  int count;   /* caracteres consumidos (%n) */
  int eof;
};

static int src_get(struct src* r) {
  int c;
  if (r->f) c = fgetc(r->f);
  else c = *r->s ? (unsigned char)*r->s++ : EOF;
  if (c == EOF) r->eof = 1;
  else r->count++;
  return c;
}

static void src_unget(struct src* r, int c) {
  if (c == EOF) return;
  r->count--;
  if (r->f) ungetc(c, r->f);
  else r->s--;
}

static int is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static int digit_val(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

static void store_int(void* p, int size, unsigned long long v) {
  if (!p) return;
  if (size == 1) *(char*)p = (char)v;
  else if (size == 2) *(short*)p = (short)v;
  else if (size == 8) *(long long*)p = (long long)v;
  else *(int*)p = (int)v;
}

/* Le um token numerico (ate width) em buf: sinal, 0x, digitos validos da base. */
static int read_int_text(struct src* r, char* buf, int width, int base) {
  int n = 0, c = src_get(r);
  if (width <= 0 || width > 70) width = 70;
  if ((c == '+' || c == '-') && n < width) {
    buf[n++] = (char)c;
    c = src_get(r);
  }
  if ((base == 0 || base == 16) && c == '0' && n < width) {
    buf[n++] = (char)c;
    c = src_get(r);
    if ((c == 'x' || c == 'X') && n < width) {
      int c2 = src_get(r);
      if (digit_val(c2) < 16) {
        buf[n++] = (char)c;
        c = c2;
        base = 16;
      } else {
        src_unget(r, c2);
      }
    } else if (base == 0) {
      base = 8;
    }
  }
  if (base == 0) base = 10;
  while (n < width && c != EOF && digit_val(c) < base) {
    buf[n++] = (char)c;
    c = src_get(r);
  }
  src_unget(r, c);
  buf[n] = 0;
  return n;
}

#ifndef NO_FLOAT
static int read_float_text(struct src* r, char* buf, int width) {
  int n = 0, c = src_get(r), digits = 0, hex = 0;
  if (width <= 0 || width > 300) width = 300;
  if ((c == '+' || c == '-') && n < width) {
    buf[n++] = (char)c;
    c = src_get(r);
  }
  /* inf / nan */
  if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
    while (n < width && c != EOF && ((c | 32) >= 'a' && (c | 32) <= 'z')) {
      buf[n++] = (char)c;
      c = src_get(r);
    }
    src_unget(r, c);
    buf[n] = 0;
    return n;
  }
  if (c == '0' && n < width) {
    buf[n++] = (char)c;
    digits = 1;
    c = src_get(r);
    if ((c == 'x' || c == 'X') && n < width) {
      hex = 1;
      buf[n++] = (char)c;
      c = src_get(r);
    }
  }
  while (n < width && c != EOF && digit_val(c) < (hex ? 16 : 10)) {
    buf[n++] = (char)c;
    digits = 1;
    c = src_get(r);
  }
  if (c == '.' && n < width) {
    buf[n++] = (char)c;
    c = src_get(r);
    while (n < width && c != EOF && digit_val(c) < (hex ? 16 : 10)) {
      buf[n++] = (char)c;
      digits = 1;
      c = src_get(r);
    }
  }
  if (digits && n < width && (hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E'))) {
    buf[n++] = (char)c;
    c = src_get(r);
    if ((c == '+' || c == '-') && n < width) {
      buf[n++] = (char)c;
      c = src_get(r);
    }
    while (n < width && c >= '0' && c <= '9') {
      buf[n++] = (char)c;
      c = src_get(r);
    }
  }
  src_unget(r, c);
  buf[n] = 0;
  return digits ? n : 0;
}
#endif

static int vscan(struct src* r, const char* fmt, va_list ap) {
  int assigned = 0;
  char buf[320];
  while (*fmt) {
    int suppress = 0, width = 0, size = 4, c;
    char conv;
    if (is_space((unsigned char)*fmt)) {
      while (is_space((unsigned char)*fmt)) fmt++;
      c = src_get(r);
      while (is_space(c)) c = src_get(r);
      src_unget(r, c);
      continue;
    }
    if (*fmt != '%' || fmt[1] == '%') {
      if (*fmt == '%') {
        fmt++;
        c = src_get(r);
        while (is_space(c)) c = src_get(r);
      } else {
        c = src_get(r);
      }
      if (c != (unsigned char)*fmt) {
        src_unget(r, c);
        break;
      }
      fmt++;
      continue;
    }
    fmt++;
    if (*fmt == '*') {
      suppress = 1;
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    for (;;) {
      if (*fmt == 'h') size = size == 2 ? 1 : 2;
      else if (*fmt == 'l') size = size == 8 || size == 5 ? 8 : 5; /* 5 = l (long/double) */
      else if (*fmt == 'L' || *fmt == 'q' || *fmt == 'j') size = 8;
      else if (*fmt == 'z' || *fmt == 't') size = 4;
      else break;
      fmt++;
    }
    conv = *fmt;
    if (!conv) break;
    fmt++;
    if (conv == 'n') {
      if (!suppress) store_int(va_arg(ap, void*), size == 5 ? 4 : size, (unsigned long long)r->count);
      continue;
    }
    /* tudo menos %c e %[ pula espacos */
    if (conv != 'c' && conv != '[') {
      c = src_get(r);
      while (is_space(c)) c = src_get(r);
      if (c == EOF) return assigned ? assigned : -1;
      src_unget(r, c);
    }
    switch (conv) {
      case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
        int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
        unsigned long long v;
        if (!read_int_text(r, buf, width, base)) goto done;
        if (buf[0] == '-' || buf[0] == '+') {
          if (!buf[1]) goto done;
        }
        v = strtoull(buf[0] == '-' || buf[0] == '+' ? buf + 1 : buf, NULL, base);
        if (buf[0] == '-') v = (unsigned long long)-(long long)v;
        if (conv == 'p') size = 4;
        if (!suppress) {
          store_int(va_arg(ap, void*), size == 5 ? 4 : size, v);
          assigned++;
        }
        break;
      }
      case 'f': case 'e': case 'g': case 'a': case 'E': case 'G': case 'A': case 'F': {
#ifdef NO_FLOAT
        goto done;
#else
        double d;
        if (!read_float_text(r, buf, width)) goto done;
        d = strtod(buf, NULL);
        if (!suppress) {
          if (size == 5 || size == 8) *va_arg(ap, double*) = d;
          else *va_arg(ap, float*) = (float)d;
          assigned++;
        }
        break;
#endif
      }
      case 'c': {
        char* out = suppress ? NULL : va_arg(ap, char*);
        int i;
        if (width <= 0) width = 1;
        for (i = 0; i < width; i++) {
          c = src_get(r);
          if (c == EOF) {
            if (i == 0) return assigned ? assigned : -1;
            break;
          }
          if (out) out[i] = (char)c;
        }
        if (!suppress) assigned++;
        break;
      }
      case 's': {
        char* out = suppress ? NULL : va_arg(ap, char*);
        int i = 0;
        c = src_get(r);
        while (c != EOF && !is_space(c) && (width <= 0 || i < width)) {
          if (out) out[i] = (char)c;
          i++;
          c = src_get(r);
        }
        src_unget(r, c);
        if (out) out[i] = 0;
        if (!suppress) assigned++;
        break;
      }
      case '[': {
        char set[256];
        int neg = 0, i = 0;
        char* out;
        memset(set, 0, sizeof(set));
        if (*fmt == '^') {
          neg = 1;
          fmt++;
        }
        if (*fmt == ']') set[(unsigned char)*fmt++] = 1;
        while (*fmt && *fmt != ']') {
          if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
            int a = (unsigned char)fmt[0], b = (unsigned char)fmt[2];
            for (; a <= b; a++) set[a] = 1;
            fmt += 3;
          } else {
            set[(unsigned char)*fmt++] = 1;
          }
        }
        if (*fmt == ']') fmt++;
        out = suppress ? NULL : va_arg(ap, char*);
        c = src_get(r);
        while (c != EOF && (set[c] != neg) && (width <= 0 || i < width)) {
          if (out) out[i] = (char)c;
          i++;
          c = src_get(r);
        }
        src_unget(r, c);
        if (i == 0) goto done;
        if (out) out[i] = 0;
        if (!suppress) assigned++;
        break;
      }
      default:
        goto done;
    }
  }
done:
  if (r->eof && assigned == 0) return -1;
  return assigned;
}

int vsscanf(const char* s, const char* fmt, va_list ap) {
  struct src r;
  r.f = NULL;
  r.s = s;
  r.count = 0;
  r.eof = 0;
  return vscan(&r, fmt, ap);
}

int vfscanf(FILE* f, const char* fmt, va_list ap) {
  struct src r;
  r.f = f;
  r.s = NULL;
  r.count = 0;
  r.eof = 0;
  return vscan(&r, fmt, ap);
}

int vscanf(const char* fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int sscanf(const char* s, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsscanf(s, fmt, ap);
  va_end(ap);
  return r;
}

int fscanf(FILE* f, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vfscanf(f, fmt, ap);
  va_end(ap);
  return r;
}

int scanf(const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vfscanf(stdin, fmt, ap);
  va_end(ap);
  return r;
}
