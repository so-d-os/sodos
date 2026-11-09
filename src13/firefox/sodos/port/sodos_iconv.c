/*
 * sodos_iconv.c - iconv minimo: decodifica a origem em code points e codifica
 * no destino. Cobre o que o glib (g_convert, g_locale_*) pede no SODOS.
 */
#include <errno.h>
#include <iconv.h>
#include <stdlib.h>
#include <string.h>

enum { CS_UTF8, CS_ASCII, CS_LATIN1, CS_UTF16BE, CS_UTF16LE, CS_UTF32BE, CS_UTF32LE };

struct sodos_iconv {
  int from, to;
};

static int lower_eq(const char* a, const char* b)
{
  for (; *a && *b; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return 0;
  }
  return *a == *b;
}

static int charset(const char* name)
{
  char n[32];
  size_t i, j = 0;
  if (!name) return -1;
  /* sem sufixos //TRANSLIT e //IGNORE, que aqui nao mudam nada */
  for (i = 0; name[i] && name[i] != '/' && j + 1 < sizeof n; i++) n[j++] = name[i];
  n[j] = 0;
  if (!n[0] || lower_eq(n, "utf-8") || lower_eq(n, "utf8")) return CS_UTF8;
  if (lower_eq(n, "ascii") || lower_eq(n, "us-ascii") || lower_eq(n, "ansi_x3.4-1968") ||
      lower_eq(n, "c") || lower_eq(n, "posix"))
    return CS_ASCII;
  if (lower_eq(n, "iso-8859-1") || lower_eq(n, "iso8859-1") || lower_eq(n, "latin1") ||
      lower_eq(n, "l1"))
    return CS_LATIN1;
  if (lower_eq(n, "utf-16") || lower_eq(n, "utf-16be") || lower_eq(n, "ucs-2") ||
      lower_eq(n, "ucs-2be"))
    return CS_UTF16BE;
  if (lower_eq(n, "utf-16le") || lower_eq(n, "ucs-2le")) return CS_UTF16LE;
  if (lower_eq(n, "utf-32") || lower_eq(n, "utf-32be") || lower_eq(n, "ucs-4") ||
      lower_eq(n, "ucs-4be"))
    return CS_UTF32BE;
  if (lower_eq(n, "utf-32le") || lower_eq(n, "ucs-4le")) return CS_UTF32LE;
  return -1;
}

iconv_t iconv_open(const char* tocode, const char* fromcode)
{
  struct sodos_iconv* cd;
  int to = charset(tocode), from = charset(fromcode);
  if (to < 0 || from < 0) {
    errno = EINVAL;
    return (iconv_t)-1;
  }
  cd = malloc(sizeof *cd);
  if (!cd) {
    errno = ENOMEM;
    return (iconv_t)-1;
  }
  cd->from = from;
  cd->to = to;
  return cd;
}

int iconv_close(iconv_t cd)
{
  if (cd == (iconv_t)-1 || !cd) {
    errno = EBADF;
    return -1;
  }
  free(cd);
  return 0;
}

/* Le um code point; devolve bytes consumidos, 0 se incompleto, -1 se invalido. */
static int decode(int cs, const unsigned char* p, size_t n, unsigned* cp)
{
  unsigned c, c2;
  switch (cs) {
  case CS_ASCII:
    if (n < 1) return 0;
    if (p[0] > 0x7f) return -1;
    *cp = p[0];
    return 1;
  case CS_LATIN1:
    if (n < 1) return 0;
    *cp = p[0];
    return 1;
  case CS_UTF8: {
    int len, i;
    if (n < 1) return 0;
    c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xe0) == 0xc0) { len = 2; c &= 0x1f; }
    else if ((c & 0xf0) == 0xe0) { len = 3; c &= 0x0f; }
    else if ((c & 0xf8) == 0xf0) { len = 4; c &= 0x07; }
    else return -1;
    if (n < (size_t)len) return 0;
    for (i = 1; i < len; i++) {
      if ((p[i] & 0xc0) != 0x80) return -1;
      c = (c << 6) | (p[i] & 0x3f);
    }
    if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) || (len == 4 && c < 0x10000) ||
        c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return -1;
    *cp = c;
    return len;
  }
  case CS_UTF16BE:
  case CS_UTF16LE:
    if (n < 2) return 0;
    c = cs == CS_UTF16BE ? (unsigned)(p[0] << 8 | p[1]) : (unsigned)(p[1] << 8 | p[0]);
    if (c >= 0xdc00 && c <= 0xdfff) return -1;
    if (c >= 0xd800 && c <= 0xdbff) {
      if (n < 4) return 0;
      c2 = cs == CS_UTF16BE ? (unsigned)(p[2] << 8 | p[3]) : (unsigned)(p[3] << 8 | p[2]);
      if (c2 < 0xdc00 || c2 > 0xdfff) return -1;
      *cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
      return 4;
    }
    *cp = c;
    return 2;
  default: /* UTF-32 */
    if (n < 4) return 0;
    c = cs == CS_UTF32BE ? ((unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3])
                         : ((unsigned)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]);
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -1;
    *cp = c;
    return 4;
  }
}

/* Escreve um code point; devolve bytes, 0 se nao cabe, -1 se nao representavel. */
static int encode(int cs, unsigned c, unsigned char* p, size_t n)
{
  switch (cs) {
  case CS_ASCII:
  case CS_LATIN1:
    if (c > (cs == CS_ASCII ? 0x7fu : 0xffu)) return -1;
    if (n < 1) return 0;
    p[0] = (unsigned char)c;
    return 1;
  case CS_UTF8:
    if (c < 0x80) {
      if (n < 1) return 0;
      p[0] = (unsigned char)c;
      return 1;
    }
    if (c < 0x800) {
      if (n < 2) return 0;
      p[0] = (unsigned char)(0xc0 | c >> 6);
      p[1] = (unsigned char)(0x80 | (c & 0x3f));
      return 2;
    }
    if (c < 0x10000) {
      if (n < 3) return 0;
      p[0] = (unsigned char)(0xe0 | c >> 12);
      p[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3f));
      p[2] = (unsigned char)(0x80 | (c & 0x3f));
      return 3;
    }
    if (n < 4) return 0;
    p[0] = (unsigned char)(0xf0 | c >> 18);
    p[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3f));
    p[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3f));
    p[3] = (unsigned char)(0x80 | (c & 0x3f));
    return 4;
  case CS_UTF16BE:
  case CS_UTF16LE: {
    unsigned u[2];
    int k, i;
    if (c >= 0x10000) {
      u[0] = 0xd800 + ((c - 0x10000) >> 10);
      u[1] = 0xdc00 + ((c - 0x10000) & 0x3ff);
      k = 2;
    } else {
      u[0] = c;
      k = 1;
    }
    if (n < (size_t)(2 * k)) return 0;
    for (i = 0; i < k; i++) {
      unsigned char hi = (unsigned char)(u[i] >> 8), lo = (unsigned char)u[i];
      p[2 * i] = cs == CS_UTF16BE ? hi : lo;
      p[2 * i + 1] = cs == CS_UTF16BE ? lo : hi;
    }
    return 2 * k;
  }
  default:
    if (n < 4) return 0;
    if (cs == CS_UTF32BE) {
      p[0] = (unsigned char)(c >> 24); p[1] = (unsigned char)(c >> 16);
      p[2] = (unsigned char)(c >> 8); p[3] = (unsigned char)c;
    } else {
      p[3] = (unsigned char)(c >> 24); p[2] = (unsigned char)(c >> 16);
      p[1] = (unsigned char)(c >> 8); p[0] = (unsigned char)c;
    }
    return 4;
  }
}

size_t iconv(iconv_t cdv, char** inbuf, size_t* inbytesleft, char** outbuf,
             size_t* outbytesleft)
{
  struct sodos_iconv* cd = cdv;
  if (cdv == (iconv_t)-1 || !cd) {
    errno = EBADF;
    return (size_t)-1;
  }
  /* inbuf NULL: volta ao estado inicial (nao ha estado aqui). */
  if (!inbuf || !*inbuf) return 0;
  while (*inbytesleft > 0) {
    unsigned cp;
    int r = decode(cd->from, (const unsigned char*)*inbuf, *inbytesleft, &cp);
    int w;
    if (r == 0) { errno = EINVAL; return (size_t)-1; }
    if (r < 0) { errno = EILSEQ; return (size_t)-1; }
    w = encode(cd->to, cp, (unsigned char*)*outbuf, *outbytesleft);
    if (w == 0) { errno = E2BIG; return (size_t)-1; }
    if (w < 0) { errno = EILSEQ; return (size_t)-1; }
    *inbuf += r;
    *inbytesleft -= (size_t)r;
    *outbuf += w;
    *outbytesleft -= (size_t)w;
  }
  return 0;
}
