/* iconv.c - conversao de charsets para o NetSurf no SODOS.
 * Suporta UTF-8, ASCII, ISO-8859-1, ISO-8859-15, Windows-1252,
 * UTF-16/UTF-16BE/UTF-16LE e UCS-4/UTF-32 (BE/LE). "//TRANSLIT" troca o
 * que nao cabe por '?'. Sem estado alem do BOM do UTF-16/32 de entrada. */
#include <errno.h>
#include <iconv.h>
#include <stdlib.h>
#include <string.h>

enum { CS_UTF8, CS_ASCII, CS_LATIN1, CS_LATIN9, CS_CP1252,
       CS_UTF16, CS_UTF16BE, CS_UTF16LE, CS_UCS4, CS_UCS4BE, CS_UCS4LE };

struct conv {
  int from, to;
  int translit;
  int in_le;      /* UTF-16/UCS-4 sem sufixo: BOM decide (padrao BE) */
  int out_bom;    /* falta escrever o BOM na saida */
};

/* 0x80-0x9F do Windows-1252 (0 = indefinido) */
static const unsigned short cp1252_hi[32] = {
  0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
  0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};

/* posicoes do ISO-8859-15 que diferem do Latin-1 */
static const struct { unsigned char b; unsigned short u; } latin9[8] = {
  { 0xA4, 0x20AC }, { 0xA6, 0x0160 }, { 0xA8, 0x0161 }, { 0xB4, 0x017D },
  { 0xB8, 0x017E }, { 0xBC, 0x0152 }, { 0xBD, 0x0153 }, { 0xBE, 0x0178 }
};

static int cs_lookup(const char* name, int* translit) {
  char n[40];
  int i = 0;
  const char* p;
  *translit = 0;
  for (p = name; *p && i < (int)sizeof(n) - 1; p++) {
    char c = *p;
    if (c == '/') {
      if (strstr(p, "TRANSLIT") || strstr(p, "translit")) *translit = 1;
      break;
    }
    if (c == '-' || c == '_' || c == ' ') continue;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    n[i++] = c;
  }
  n[i] = 0;
  if (!strcmp(n, "UTF8")) return CS_UTF8;
  if (!strcmp(n, "ASCII") || !strcmp(n, "USASCII") || !strcmp(n, "ANSIX3.41968") ||
      !strcmp(n, "646"))
    return CS_ASCII;
  if (!strcmp(n, "ISO88591") || !strcmp(n, "LATIN1") || !strcmp(n, "L1") ||
      !strcmp(n, "ISO885911987") || !strcmp(n, "CP819") || !strcmp(n, "IBM819"))
    return CS_LATIN1;
  if (!strcmp(n, "ISO885915") || !strcmp(n, "LATIN9") || !strcmp(n, "LATIN0"))
    return CS_LATIN9;
  if (!strcmp(n, "WINDOWS1252") || !strcmp(n, "CP1252") || !strcmp(n, "MS1252"))
    return CS_CP1252;
  if (!strcmp(n, "UTF16")) return CS_UTF16;
  if (!strcmp(n, "UTF16BE")) return CS_UTF16BE;
  if (!strcmp(n, "UTF16LE")) return CS_UTF16LE;
  if (!strcmp(n, "UCS4") || !strcmp(n, "UTF32") || !strcmp(n, "ISO10646UCS4"))
    return CS_UCS4;
  if (!strcmp(n, "UCS4BE") || !strcmp(n, "UTF32BE")) return CS_UCS4BE;
  if (!strcmp(n, "UCS4LE") || !strcmp(n, "UTF32LE") || !strcmp(n, "UCS4INTERNAL"))
    return CS_UCS4LE;
  return -1;
}

iconv_t iconv_open(const char* tocode, const char* fromcode) {
  struct conv* c;
  int tr_to, tr_from;
  int to = cs_lookup(tocode, &tr_to);
  int from = cs_lookup(fromcode, &tr_from);
  if (to < 0 || from < 0) {
    errno = EINVAL;
    return (iconv_t)-1;
  }
  c = (struct conv*)calloc(1, sizeof(*c));
  if (!c) {
    errno = ENOMEM;
    return (iconv_t)-1;
  }
  c->from = from;
  c->to = to;
  c->translit = tr_to;
  c->in_le = 0;
  c->out_bom = (to == CS_UTF16 || to == CS_UCS4);
  return (iconv_t)c;
}

int iconv_close(iconv_t cd) {
  if (cd == (iconv_t)-1) return -1;
  free(cd);
  return 0;
}

/* le um codepoint; devolve bytes usados, 0 = entrada incompleta, -1 = invalida */
static int dec(struct conv* c, const unsigned char* s, size_t n, unsigned* cp) {
  unsigned u, u2;
  int le;
  switch (c->from) {
  case CS_UTF8: {
    int len, i;
    u = s[0];
    if (u < 0x80) { *cp = u; return 1; }
    if (u < 0xC2) return -1;
    if (u < 0xE0) { len = 2; u &= 0x1F; }
    else if (u < 0xF0) { len = 3; u &= 0x0F; }
    else if (u < 0xF5) { len = 4; u &= 0x07; }
    else return -1;
    if (n < (size_t)len) {
      for (i = 1; i < (int)n; i++)
        if ((s[i] & 0xC0) != 0x80) return -1;
      return 0;
    }
    for (i = 1; i < len; i++) {
      if ((s[i] & 0xC0) != 0x80) return -1;
      u = (u << 6) | (s[i] & 0x3F);
    }
    if ((len == 3 && u < 0x800) || (len == 4 && (u < 0x10000 || u > 0x10FFFF)) ||
        (u >= 0xD800 && u <= 0xDFFF))
      return -1;
    *cp = u;
    return len;
  }
  case CS_ASCII:
    if (s[0] >= 0x80) return -1;
    *cp = s[0];
    return 1;
  case CS_LATIN1:
    *cp = s[0];
    return 1;
  case CS_LATIN9: {
    int i;
    *cp = s[0];
    for (i = 0; i < 8; i++)
      if (latin9[i].b == s[0]) *cp = latin9[i].u;
    return 1;
  }
  case CS_CP1252:
    if (s[0] >= 0x80 && s[0] < 0xA0) {
      if (!cp1252_hi[s[0] - 0x80]) return -1;
      *cp = cp1252_hi[s[0] - 0x80];
    } else {
      *cp = s[0];
    }
    return 1;
  case CS_UTF16:
  case CS_UTF16BE:
  case CS_UTF16LE:
    if (n < 2) return 0;
    le = c->from == CS_UTF16LE || (c->from == CS_UTF16 && c->in_le);
    u = le ? (unsigned)(s[0] | s[1] << 8) : (unsigned)(s[0] << 8 | s[1]);
    if (u >= 0xDC00 && u <= 0xDFFF) return -1;
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (n < 4) return 0;
      u2 = le ? (unsigned)(s[2] | s[3] << 8) : (unsigned)(s[2] << 8 | s[3]);
      if (u2 < 0xDC00 || u2 > 0xDFFF) return -1;
      *cp = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00);
      return 4;
    }
    *cp = u;
    return 2;
  default: /* UCS-4 */
    if (n < 4) return 0;
    le = c->from == CS_UCS4LE || (c->from == CS_UCS4 && c->in_le);
    u = le ? ((unsigned)s[0] | (unsigned)s[1] << 8 | (unsigned)s[2] << 16 | (unsigned)s[3] << 24)
           : ((unsigned)s[0] << 24 | (unsigned)s[1] << 16 | (unsigned)s[2] << 8 | (unsigned)s[3]);
    if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) return -1;
    *cp = u;
    return 4;
  }
}

/* grava um codepoint; devolve bytes, -1 = sem espaco, -2 = nao representavel */
static int enc(struct conv* c, unsigned cp, unsigned char* o, size_t n) {
  int i;
  switch (c->to) {
  case CS_UTF8:
    if (cp < 0x80) {
      if (n < 1) return -1;
      o[0] = (unsigned char)cp;
      return 1;
    }
    if (cp < 0x800) {
      if (n < 2) return -1;
      o[0] = (unsigned char)(0xC0 | cp >> 6);
      o[1] = (unsigned char)(0x80 | (cp & 0x3F));
      return 2;
    }
    if (cp < 0x10000) {
      if (n < 3) return -1;
      o[0] = (unsigned char)(0xE0 | cp >> 12);
      o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
      o[2] = (unsigned char)(0x80 | (cp & 0x3F));
      return 3;
    }
    if (n < 4) return -1;
    o[0] = (unsigned char)(0xF0 | cp >> 18);
    o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
  case CS_ASCII:
  case CS_LATIN1:
  case CS_LATIN9:
  case CS_CP1252: {
    int b = -1;
    if (cp < 0x80 || (cp < 0x100 && c->to == CS_LATIN1)) b = (int)cp;
    else if (c->to == CS_LATIN9) {
      b = cp < 0x100 ? (int)cp : -1;
      for (i = 0; i < 8; i++) {
        if (latin9[i].u == cp) b = latin9[i].b;
        else if (latin9[i].b == cp) b = -1;
      }
    } else if (c->to == CS_CP1252) {
      if (cp >= 0xA0 && cp < 0x100) b = (int)cp;
      for (i = 0; i < 32; i++)
        if (cp1252_hi[i] && cp1252_hi[i] == cp) b = 0x80 + i;
    }
    if (b < 0) return -2;
    if (n < 1) return -1;
    o[0] = (unsigned char)b;
    return 1;
  }
  case CS_UTF16:
  case CS_UTF16BE:
  case CS_UTF16LE: {
    int le = c->to == CS_UTF16LE;
    unsigned w[2];
    int k = 1;
    if (cp >= 0x10000) {
      w[0] = 0xD800 + ((cp - 0x10000) >> 10);
      w[1] = 0xDC00 + ((cp - 0x10000) & 0x3FF);
      k = 2;
    } else {
      w[0] = cp;
    }
    if (n < (size_t)(2 * k)) return -1;
    for (i = 0; i < k; i++) {
      o[2 * i] = (unsigned char)(le ? w[i] : w[i] >> 8);
      o[2 * i + 1] = (unsigned char)(le ? w[i] >> 8 : w[i]);
    }
    return 2 * k;
  }
  default: {
    int le = c->to == CS_UCS4LE;
    if (n < 4) return -1;
    for (i = 0; i < 4; i++) o[i] = (unsigned char)(cp >> (le ? 8 * i : 24 - 8 * i));
    return 4;
  }
  }
}

size_t iconv(iconv_t cd, char** inbuf, size_t* inleft, char** outbuf, size_t* outleft) {
  struct conv* c = (struct conv*)cd;
  size_t count = 0;
  const unsigned char* s;
  unsigned char* o;
  if (cd == (iconv_t)-1) {
    errno = EBADF;
    return (size_t)-1;
  }
  if (!inbuf || !*inbuf) {
    /* reset do estado */
    c->in_le = 0;
    c->out_bom = (c->to == CS_UTF16 || c->to == CS_UCS4);
    return 0;
  }
  s = (const unsigned char*)*inbuf;
  o = (unsigned char*)*outbuf;
  if (c->out_bom) {
    unsigned char bom[4];
    int k;
    if (c->to == CS_UTF16) {
      bom[0] = 0xFE; bom[1] = 0xFF; k = 2;
    } else {
      bom[0] = 0; bom[1] = 0; bom[2] = 0xFE; bom[3] = 0xFF; k = 4;
    }
    if (*outleft < (size_t)k) {
      errno = E2BIG;
      return (size_t)-1;
    }
    memcpy(o, bom, (size_t)k);
    o += k;
    *outleft -= (size_t)k;
    *outbuf = (char*)o;
    c->out_bom = 0;
  }
  while (*inleft > 0) {
    unsigned cp = 0;
    int r, w;
    r = dec(c, s, *inleft, &cp);
    if (r == 0) {
      errno = EINVAL;
      return (size_t)-1;
    }
    if (r < 0) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    /* BOM no inicio de UTF-16/UCS-4 sem endian: decide e descarta */
    if ((c->from == CS_UTF16 || c->from == CS_UCS4) && (cp == 0xFEFF || cp == 0xFFFE0000u ||
        cp == 0xFFFE)) {
      if (cp != 0xFEFF) c->in_le = 1;
      s += r;
      *inleft -= (size_t)r;
      *inbuf = (char*)s;
      continue;
    }
    w = enc(c, cp, o, *outleft);
    if (w == -2) {
      if (!c->translit) {
        errno = EILSEQ;
        return (size_t)-1;
      }
      w = enc(c, '?', o, *outleft);
      count++;
    }
    if (w < 0) {
      errno = E2BIG;
      return (size_t)-1;
    }
    s += r;
    *inleft -= (size_t)r;
    o += w;
    *outleft -= (size_t)w;
    *inbuf = (char*)s;
    *outbuf = (char*)o;
  }
  return count;
}
