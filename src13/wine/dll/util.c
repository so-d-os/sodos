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

void wlogx(const char* a, uint64_t v) {
  char h[19];
  int i;
  h[0] = '0';
  h[1] = 'x';
  for (i = 0; i < 16; i++) h[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  h[18] = 0;
  wlog(a, h);
}

void wsay(const char* a, const char* b) {
  wlog(a, b);
  k_puts("wine: ");
  if (a) k_puts(a);
  if (b) k_puts(b);
  k_puts("\n");
}

static void con_raw(const char* s, size_t n) {
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

static void clock_(struct wproc* p) {
  while (__sync_lock_test_and_set(&p->clk, 1)) k_thread_yield();
}
static void cunlock(struct wproc* p) { __sync_lock_release(&p->clk); }

/* Da tarefa principal: direto. De uma thread do Windows: fica no buffer e a
 * tarefa principal escreve (wcon_flush), na mesma ordem. */
void wcon_write(const char* s, size_t n) {
  struct wproc* p = wp();
  if (!wthread()) {
    wcon_flush();
    con_raw(s, n);
    return;
  }
  for (;;) {
    clock_(p);
    if (p->cn + n <= 65536 || !p->cn) {
      if (p->cn + n > p->ccap) {
        uint32_t nc = p->ccap ? p->ccap : 4096;
        char* nb;
        while (nc < p->cn + n) nc *= 2;
        nb = (char*)k_realloc(p->cbuf, nc);
        if (!nb) {
          cunlock(p);
          return;
        }
        p->cbuf = nb;
        p->ccap = nc;
      }
      memcpy(p->cbuf + p->cn, s, n);
      p->cn += (uint32_t)n;
      cunlock(p);
      k_futex_wake(&p->exit_req, -1);
      return;
    }
    cunlock(p);
    k_futex_wake(&p->exit_req, -1);
    if (p->exit_req) return;
    k_thread_yield();
  }
}

void wcon_flush(void) {
  struct wproc* p = wp();
  char* b;
  uint32_t n;
  if (!p->cn) goto input;
  clock_(p);
  b = p->cbuf;
  n = p->cn;
  p->cbuf = 0;
  p->cn = 0;
  p->ccap = 0;
  cunlock(p);
  con_raw(b, n);
  k_free(b);
input:
  /* leitura do teclado pedida por uma thread */
  if (p->in_req == 1) {
    int len = k_readline(p->in_line, (int)sizeof(p->in_line) - 1);
    p->in_len = len;
    p->in_req = 2;
    k_futex_wake(&p->in_req, -1);
  } else if (p->in_req == 3) {
    p->in_len = k_getc();
    p->in_req = 4;
    k_futex_wake(&p->in_req, -1);
  }
}

int wcon_readline(char* buf, int max) {
  struct wproc* p = wp();
  int len;
  if (!wthread()) {
    wcon_flush();
    return k_readline(buf, max);
  }
  p->in_req = 1;
  k_futex_wake(&p->exit_req, -1);
  while (p->in_req != 2 && !p->exit_req) k_futex_wait(&p->in_req, 1, 50);
  len = p->in_req == 2 ? p->in_len : -1;
  p->in_req = 0;
  if (len < 0) return -1;
  if (len >= max) len = max - 1;
  memcpy(buf, p->in_line, (size_t)len);
  buf[len] = 0;
  return len;
}

int wcon_getc(void) {
  struct wproc* p = wp();
  int c;
  if (!wthread()) {
    wcon_flush();
    return k_getc();
  }
  p->in_req = 3;
  k_futex_wake(&p->exit_req, -1);
  while (p->in_req != 4 && !p->exit_req) k_futex_wait(&p->in_req, 3, 50);
  c = p->in_req == 4 ? p->in_len : -1;
  p->in_req = 0;
  return c;
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
  if ((uintptr_t)s < 0x10000 || (intptr_t)s == -1) return 0; /* ATOM, LPSTR_TEXTCALLBACK */
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
