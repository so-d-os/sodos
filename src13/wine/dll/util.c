/* util.c - Utilidades do WINE.DLL: console, nomes, UTF-16, stubs. */
#include <so.h>
#include <string.h>
#include "wine_int.h"

void wlog(const char* a, const char* b) {
  k_dbg("[wine] ", 7);
  if (a) k_dbg(a, (int)strlen(a));
  if (b) k_dbg(b, (int)strlen(b));
  k_dbg("\n", 1);
}

void wsay(const char* a, const char* b) {
  wlog(a, b);
  k_puts("wine: ");
  if (a) k_puts(a);
  if (b) k_puts(b);
  k_puts("\n");
}

void wcon_write(const char* s, size_t n) {
  size_t i = 0, start = 0;
  /* O console do SODOS quebra linha com \n; o \r de \r\n sairia como lixo. */
  for (i = 0; i < n; i++) {
    if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') {
      if (i > start) k_write(s + start, (int)(i - start));
      start = i + 1;
    }
  }
  if (n > start) k_write(s + start, (int)(n - start));
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

void wlower(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = lc(s[i]);
    i++;
  }
  d[i] = 0;
}

void wbase(char* d, const char* path, int max) {
  const char* b = path;
  const char* p;
  for (p = path; *p; p++)
    if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
  wlower(d, b, max);
}

int wstreq(const char* a, const char* b) {
  while (*a && lc(*a) == lc(*b)) {
    a++;
    b++;
  }
  return lc(*a) == lc(*b);
}

size_t wstrlen16(const wchar16* s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

int wutf16_to_8(const wchar16* s, int n, char* d, int max) {
  int i = 0, o = 0;
  while (n < 0 ? s[i] != 0 : i < n) {
    uint32_t c = s[i++];
    char tmp[4];
    int k, len;
    if (c >= 0xD800 && c < 0xDC00 && (n < 0 ? s[i] != 0 : i < n) && s[i] >= 0xDC00 && s[i] < 0xE000)
      c = 0x10000u + ((c - 0xD800u) << 10) + (uint32_t)(s[i++] - 0xDC00u);
    if (c < 0x80) {
      tmp[0] = (char)c;
      len = 1;
    } else if (c < 0x800) {
      tmp[0] = (char)(0xC0 | (c >> 6));
      tmp[1] = (char)(0x80 | (c & 0x3F));
      len = 2;
    } else if (c < 0x10000) {
      tmp[0] = (char)(0xE0 | (c >> 12));
      tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
      tmp[2] = (char)(0x80 | (c & 0x3F));
      len = 3;
    } else {
      tmp[0] = (char)(0xF0 | (c >> 18));
      tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F));
      tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F));
      tmp[3] = (char)(0x80 | (c & 0x3F));
      len = 4;
    }
    if (d) {
      if (o + len > max) break;
      for (k = 0; k < len; k++) d[o + k] = tmp[k];
    }
    o += len;
  }
  return o;
}

int wutf8_to_16(const char* s8, int n, wchar16* d, int max) {
  const uint8_t* s = (const uint8_t*)s8;
  int i = 0, o = 0;
  while (n < 0 ? s[i] != 0 : i < n) {
    uint32_t c = s[i++];
    int more = 0;
    if (c >= 0xF0 && c < 0xF8) {
      c &= 7;
      more = 3;
    } else if (c >= 0xE0) {
      c &= 15;
      more = 2;
    } else if (c >= 0xC0) {
      c &= 31;
      more = 1;
    }
    /* Byte que nao e UTF-8 valido: Latin-1, como o console do SODOS. */
    while (more-- > 0 && (n < 0 ? s[i] != 0 : i < n) && (s[i] & 0xC0) == 0x80) c = (c << 6) | (s[i++] & 0x3Fu);
    if (c >= 0x10000) {
      if (d) {
        if (o + 2 > max) break;
        d[o] = (wchar16)(0xD800 + ((c - 0x10000) >> 10));
        d[o + 1] = (wchar16)(0xDC00 + ((c - 0x10000) & 0x3FF));
      }
      o += 2;
    } else {
      if (d) {
        if (o + 1 > max) break;
        d[o] = (wchar16)c;
      }
      o++;
    }
  }
  return o;
}

void* wzalloc(size_t n) {
  void* p = k_malloc((unsigned)(n ? n : 1));
  if (p) memset(p, 0, n);
  return p;
}

char* wdup16to8(const wchar16* s) {
  int n;
  char* d;
  if (!s) return 0;
  n = wutf16_to_8(s, -1, 0, 0);
  d = (char*)wzalloc((size_t)n + 1);
  if (d) wutf16_to_8(s, -1, d, n);
  return d;
}

wchar16* wdup8to16(const char* s) {
  int n;
  wchar16* d;
  if (!s) return 0;
  n = wutf8_to_16(s, -1, 0, 0);
  d = (wchar16*)wzalloc(((size_t)n + 1) * 2);
  if (d) wutf8_to_16(s, -1, d, n);
  return d;
}

void WINAPI wine_unimplemented(const char* what) {
  wsay("funcao nao implementada: ", what);
  wexit(0xC0000139);   /* STATUS_ENTRYPOINT_NOT_FOUND */
}

/* Stub para um import que nao existe: o programa so cai se chamar.
 *   48 B9 <imm64>   mov rcx, "dll!nome"
 *   48 B8 <imm64>   mov rax, wine_unimplemented
 *   FF E0           jmp rax                                  */
void* wine_make_stub(const char* dll, const char* name) {
  size_t a = strlen(dll), b = strlen(name);
  uint8_t* p = (uint8_t*)k_malloc((unsigned)(24 + a + b + 2));
  char* txt;
  uint64_t v;
  int i;
  if (!p) return 0;
  txt = (char*)p + 24;
  memcpy(txt, dll, a);
  txt[a] = '!';
  memcpy(txt + a + 1, name, b + 1);
  p[0] = 0x48;
  p[1] = 0xB9;
  v = (uint64_t)(uintptr_t)txt;
  for (i = 0; i < 8; i++) p[2 + i] = (uint8_t)(v >> (8 * i));
  p[10] = 0x48;
  p[11] = 0xB8;
  v = (uint64_t)(uintptr_t)wine_unimplemented;
  for (i = 0; i < 8; i++) p[12 + i] = (uint8_t)(v >> (8 * i));
  p[20] = 0xFF;
  p[21] = 0xE0;
  return p;
}
