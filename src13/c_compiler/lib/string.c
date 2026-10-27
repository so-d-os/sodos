/* string.c - <string.h> */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void* memcpy(void* d, const void* s, size_t n) {
  char* a = (char*)d;
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
