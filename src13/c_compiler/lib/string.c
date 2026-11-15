/* string.c - <string.h> */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#if __SIZEOF_POINTER__ == 8
/* A pagina 0 e mapeada e guarda a IDT do kernel: um ponteiro nulo em memset ou
 * memcpy derrubaria a maquina sem mensagem. Melhor parar com a pilha na serial. */
extern void k_exit(int code);
extern void sodos_dump_stack(void) __attribute__((weak));
static void null_dest(void) {
  if (sodos_dump_stack) sodos_dump_stack();
  k_exit(126);
}
#define CHECK_DEST(d, n)                          \
  do {                                            \
    if ((n) && (unsigned long)(d) < 0x100000) null_dest(); \
  } while (0)
#else
#define CHECK_DEST(d, n)
#endif

void* memcpy(void* d, const void* s, size_t n) {
  char* a = (char*)d;
  CHECK_DEST(d, n);
#if __SIZEOF_POINTER__ == 8
  /* O Firefox e compilado com -ffreestanding (sem builtins): ate as copias de
   * 16 bytes dos shaders do SWGL chegam aqui. Pequenas: cargas de 8 bytes
   * sobrepostas (sem laco de byte). Grandes: rep movsb (ERMS), que tem custo
   * fixo de partida alto mas e a mais rapida para blocos medios e grandes. */
  typedef unsigned long long __attribute__((may_alias, aligned(1))) u64u;
  typedef unsigned __attribute__((may_alias, aligned(1))) u32u;
  if (n <= 16) {
    if (n >= 8) {
      u64u x = *(const u64u*)s, y = *(const u64u*)((const char*)s + n - 8);
      *(u64u*)a = x;
      *(u64u*)(a + n - 8) = y;
    } else if (n >= 4) {
      u32u x = *(const u32u*)s, y = *(const u32u*)((const char*)s + n - 4);
      *(u32u*)a = x;
      *(u32u*)(a + n - 4) = y;
    } else {
      const char* b0 = (const char*)s;
      while (n--) *a++ = *b0++;
    }
    return d;
  }
  if (n <= 128) {
    const char* b0 = (const char*)s;
    size_t i;
    u64u last = *(const u64u*)(b0 + n - 8);
    for (i = 0; i + 8 <= n; i += 8) *(u64u*)(a + i) = *(const u64u*)(b0 + i);
    *(u64u*)(a + n - 8) = last;
    return d;
  }
  {
    void* dd = d;
    __asm__ volatile("rep movsb" : "+D"(dd), "+S"(s), "+c"(n) : : "memory");
    return d;
  }
#endif
  const char* b = (const char*)s;
  while (n >= 4) {
    *(unsigned*)a = *(const unsigned*)b;
    a += 4;
    b += 4;
    n -= 4;
  }
  while (n--) *a++ = *b++;
  return d;
}

void* memmove(void* d, const void* s, size_t n) {
  char* a = (char*)d;
  const char* b = (const char*)s;
  if (a == b || n == 0) return d;
  if (a < b || a >= b + n) return memcpy(d, s, n);
  a += n;
  b += n;
  while (n--) *--a = *--b;
  return d;
}

void* memset(void* d, int c, size_t n) {
  unsigned char* a = (unsigned char*)d;
  unsigned v = (unsigned char)c;
  CHECK_DEST(d, n);
#if __SIZEOF_POINTER__ == 8
  if (n < 128) {
    typedef unsigned long long __attribute__((may_alias, aligned(1))) u64u;
    unsigned long long w = (unsigned long long)(unsigned char)c * 0x0101010101010101ULL;
    size_t i;
    if (n < 8) {
      while (n--) *a++ = (unsigned char)c;
      return d;
    }
    for (i = 0; i + 8 <= n; i += 8) *(u64u*)(a + i) = w;
    *(u64u*)(a + n - 8) = w;
    return d;
  }
  {
    void* dd = d;
    __asm__ volatile("rep stosb" : "+D"(dd), "+c"(n) : "a"(c) : "memory");
    return d;
  }
#endif
  v |= v << 8;
  v |= v << 16;
  while (n && ((unsigned)a & 3)) {
    *a++ = (unsigned char)c;
    n--;
  }
  while (n >= 4) {
    *(unsigned*)a = v;
    a += 4;
    n -= 4;
  }
  while (n--) *a++ = (unsigned char)c;
  return d;
}

int memcmp(const void* x, const void* y, size_t n) {
  const unsigned char* a = (const unsigned char*)x;
  const unsigned char* b = (const unsigned char*)y;
  size_t i;
  for (i = 0; i < n; i++)
    if (a[i] != b[i]) return a[i] - b[i];
  return 0;
}

/* BSD; o core/compiler_builtins do Rust usa bcmp. */
int bcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }

void* memchr(const void* s, int c, size_t n) {
  const unsigned char* p = (const unsigned char*)s;
  size_t i;
  for (i = 0; i < n; i++)
    if (p[i] == (unsigned char)c) return (void*)(p + i);
  return NULL;
}

size_t strlen(const char* s) {
  const char* p = s;
  while (*p) p++;
  return (size_t)(p - s);
}

size_t strnlen(const char* s, size_t max) {
  size_t n = 0;
  while (n < max && s[n]) n++;
  return n;
}

char* strcpy(char* d, const char* s) {
  char* r = d;
  while ((*d++ = *s++) != 0) {}
  return r;
}

char* strncpy(char* d, const char* s, size_t n) {
  size_t i = 0;
  for (; i < n && s[i]; i++) d[i] = s[i];
  for (; i < n; i++) d[i] = 0;
  return d;
}

char* strcat(char* d, const char* s) {
  strcpy(d + strlen(d), s);
  return d;
}

char* strncat(char* d, const char* s, size_t n) {
  char* p = d + strlen(d);
  while (n-- && *s) *p++ = *s++;
  *p = 0;
  return d;
}

int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
  }
  return 0;
}

int strcasecmp(const char* a, const char* b) {
  int x, y;
  do {
    x = tolower((unsigned char)*a++);
    y = tolower((unsigned char)*b++);
  } while (x && x == y);
  return x - y;
}

int strncasecmp(const char* a, const char* b, size_t n) {
  int x = 0, y = 0;
  while (n--) {
    x = tolower((unsigned char)*a++);
    y = tolower((unsigned char)*b++);
    if (!x || x != y) break;
  }
  return x - y;
}

char* strchr(const char* s, int c) {
  for (;; s++) {
    if (*s == (char)c) return (char*)s;
    if (!*s) return NULL;
  }
}

char* strrchr(const char* s, int c) {
  const char* r = NULL;
  for (;; s++) {
    if (*s == (char)c) r = s;
    if (!*s) return (char*)r;
  }
}

char* strstr(const char* h, const char* n) {
  size_t ln = strlen(n);
  if (!ln) return (char*)h;
  for (; *h; h++)
    if (*h == *n && strncmp(h, n, ln) == 0) return (char*)h;
  return NULL;
}

size_t strspn(const char* s, const char* accept) {
  size_t n = 0;
  while (s[n] && strchr(accept, s[n])) n++;
  return n;
}

size_t strcspn(const char* s, const char* reject) {
  size_t n = 0;
  while (s[n] && !strchr(reject, s[n])) n++;
  return n;
}

char* strpbrk(const char* s, const char* accept) {
  s += strcspn(s, accept);
  return *s ? (char*)s : NULL;
}

char* strtok(char* s, const char* delim) {
  static char* save;
  char* t;
  if (!s) s = save;
  if (!s) return NULL;
  s += strspn(s, delim);
  if (!*s) {
    save = NULL;
    return NULL;
  }
  t = s + strcspn(s, delim);
  if (*t) {
    *t = 0;
    save = t + 1;
  } else {
    save = NULL;
  }
  return s;
}

char* strdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* p = (char*)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

char* strndup(const char* s, size_t n) {
  size_t l = strnlen(s, n);
  char* p = (char*)malloc(l + 1);
  if (!p) return NULL;
  memcpy(p, s, l);
  p[l] = 0;
  return p;
}

char* strerror(int e) {
  switch (e) {
    case 0: return "Sucesso";
    case 2: return "Arquivo nao encontrado";
    case 5: return "Erro de E/S";
    case 12: return "Memoria insuficiente";
    case 13: return "Acesso negado";
    case 17: return "Arquivo ja existe";
    case 22: return "Argumento invalido";
    case 34: return "Fora do intervalo";
  }
  return "Erro desconhecido";
}

/* Sem locale: colacao e ordenacao byte a byte. */
int strcoll(const char* a, const char* b) { return strcmp(a, b); }

size_t strxfrm(char* d, const char* s, size_t n) {
  size_t l = strlen(s);
  if (d && n) {
    size_t c = l < n - 1 ? l : n - 1;
    memcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}

int strerror_r(int e, char* buf, size_t n) {
  const char* s = strerror(e);
  size_t l;
  if (!buf || !n) return -1;
  l = strlen(s);
  if (l >= n) l = n - 1;
  memcpy(buf, s, l);
  buf[l] = 0;
  return 0;
}

char* strtok_r(char* s, const char* delim, char** save) {
  char* p = s ? s : *save;
  if (!p) return NULL;
  while (*p && strchr(delim, *p)) p++;
  if (!*p) { *save = NULL; return NULL; }
  char* start = p;
  while (*p && !strchr(delim, *p)) p++;
  if (*p) { *p = 0; *save = p + 1; } else *save = NULL;
  return start;
}
