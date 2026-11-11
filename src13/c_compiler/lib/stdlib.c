/* stdlib.c - malloc, conversoes, qsort, exit, system.
 *
 * A LIBC.DLL e uma so na memoria: se um programa roda outro (system), os
 * dois usam as mesmas variaveis daqui. Por isso system() guarda o estado
 * (listas livres do malloc, atexit, arquivos abertos), roda o filho com
 * estado limpo e restaura depois. O kernel devolve a memoria do filho. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

#ifdef __x86_64__
static inline void dbg_mark(unsigned char c) {
  __asm__ volatile("outb %0, %1" :: "a"(c), "d"((unsigned short)0x3F8));
}
static void dbg_txt(const char* s) {
  while (*s) dbg_mark((unsigned char)*s++);
}
static void dbg_u32(unsigned v) {
  char b[9];
  int i;
  for (i = 0; i < 8; i++) {
    int d = (int)((v >> (28 - 4 * i)) & 0xFu);
    b[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
  }
  b[8] = 0;
  dbg_txt(b);
}
#else
#define dbg_mark(c) ((void)0)
#define dbg_txt(s) ((void)0)
#define dbg_u32(v) ((void)0)
#endif
#include <stdio.h>
#include <so.h>

void __so_stdio_flush_all(void);
void __so_stdio_flush_out(void);
void* __so_stdio_swap(void* st);

/* ---------- malloc ----------
 * Blocos pequenos (ate 2048) em classes de potencia de 2, cortados de pedacos
 * de 64KB do kernel; o resto vai direto para k_malloc. Cabecalho de 8 bytes. */
#define NCLASS 8
#define CHUNK 65536
#define BIG_CLASS 0xFF
#define HDR_MAGIC 0x4D41u

struct hdr {
  unsigned short cls;
  unsigned short magic;
  unsigned size; /* bytes uteis */
  unsigned pad;  /* deslocamento ate o bloco original do k_malloc */
  unsigned pad2; /* fecha o cabecalho em 16 bytes (alinhamento do ponteiro) */
};

struct heap_state {
  void* free_list[NCLASS];
  char* cur;
  unsigned left;
  void (*atexit_fn[32])(void);
  int natexit;
  unsigned rand_state;
};

static struct heap_state hs;
static int err_value;
static unsigned nchunks = 1; /* 1 = primeira alocacao ainda nao feita */

int* __so_errno(void) { return &err_value; }

#define class_size(c) (16u << (c))

void* malloc(size_t n) {
  struct hdr* h;
  int c = 0;
  if (n == 0) n = 1;
  while (c < NCLASS && class_size(c) < n) c++;
  if (c == NCLASS) {
    char* raw = (char*)k_malloc(n + 2 * sizeof(struct hdr) + 16u);
    if (!raw) {
      dbg_txt("!MBIG n=");
      dbg_u32((unsigned)n);
      dbg_txt(" ch=");
      dbg_u32(nchunks);
      dbg_txt("\n");
      err_value = ENOMEM;
      return NULL;
    }
    /* k_malloc devolve 4-alinhado: alinhamos o bloco a 16 e guardamos o
     * deslocamento para o free achar o bloco original. */
    h = (struct hdr*)(((uintptr_t)raw + sizeof(struct hdr) + 15u) &
                      ~(uintptr_t)15u);
    if (n >= 0x2000u) {
      dbg_txt("!AB n=");
      dbg_u32((unsigned)n);
      dbg_txt(" p=");
      dbg_u32((unsigned)(uintptr_t)(h + 1));
      dbg_txt("\n");
    }
    h->cls = BIG_CLASS;
    h->magic = HDR_MAGIC;
    h->size = n;
    h->pad = (unsigned)((char*)h - raw);
    return h + 1;
  }
  if (hs.free_list[c]) {
    void** p = (void**)hs.free_list[c];
    hs.free_list[c] = *p;
    h = (struct hdr*)p - 1;
    h->magic = HDR_MAGIC;
    return p;
  }
  {
    unsigned need = class_size(c) + sizeof(struct hdr);
    if (hs.left < need) {
      char* p = (char*)k_malloc(CHUNK + 16u);
      if (!p) {
        dbg_txt("!MCHK ch=");
        dbg_u32(nchunks);
        dbg_txt("\n");
        hs.left = 0;
        err_value = ENOMEM;
        return NULL;
      }
      nchunks++;
      hs.cur = (char*)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
      hs.left = CHUNK;
    }
    h = (struct hdr*)hs.cur;
    hs.cur += need;
    hs.left -= need;
    h->cls = (unsigned short)c;
    h->magic = HDR_MAGIC;
    h->size = class_size(c);
    return h + 1;
  }
}

/* C11 aligned_alloc: o malloc do SODOS ja devolve 16-alinhado. Para
 * alinhamentos maiores nao ha garantia; a libc++ usa em tipos over-aligned. */
void* aligned_alloc(size_t align, size_t n) {
  (void)align;
  return malloc(n);
}

void _Exit(int code) {
  k_exit(code);
  for (;;) {}
}

void quick_exit(int code) { _Exit(code); }

int at_quick_exit(void (*fn)(void)) { return atexit(fn); }

/* Sem locale multibyte: cada byte e um caractere. */
int mblen(const char* s, size_t n) {
  if (!s) return 0;
  return (n && *s) ? 1 : 0;
}

void free(void* p) {
  struct hdr* h;
  if (!p) return;
  h = (struct hdr*)p - 1;
  if (h->magic != HDR_MAGIC) return; /* ponteiro invalido ou free duplo */
  h->magic = 0;
  if (h->cls == BIG_CLASS) {
    if (h->size >= 0x2000u) {
      dbg_txt("!FB p=");
      dbg_u32((unsigned)(uintptr_t)p);
      dbg_txt(" n=");
      dbg_u32(h->size);
      dbg_txt("\n");
    }
    k_free((char*)h - h->pad);
    return;
  }
  *(void**)p = hs.free_list[h->cls];
  hs.free_list[h->cls] = p;
}

void* calloc(size_t n, size_t sz) {
  size_t t = n * sz;
  void* p;
  if (sz && t / sz != n) return NULL;
  p = malloc(t);
  if (p) memset(p, 0, t);
  return p;
}

void* realloc(void* p, size_t n) {
  struct hdr* h;
  void* q;
  if (!p) return malloc(n);
  if (n == 0) {
    free(p);
    return NULL;
  }
  h = (struct hdr*)p - 1;
  if (h->magic != HDR_MAGIC) return NULL;
  if (n <= h->size) return p;
  q = malloc(n);
  if (!q) return NULL;
  memcpy(q, p, h->size);
  free(p);
  return q;
}

/* ---------- inicio e fim ---------- */
void __so_libc_start(void) {}

#ifdef SODOS_DYNAMIC
/* libc.so.6: atexit, destrutores C++ e .fini_array das bibliotecas saem pela
 * mesma fila, em libs/dso/start.c. */
int __cxa_atexit(void (*fn)(void*), void* arg, void* dso);
void __so_run_exit(void);

int atexit(void (*fn)(void)) { return __cxa_atexit((void (*)(void*))(void*)fn, 0, 0); }

void exit(int code) {
  __so_run_exit();
  __so_stdio_flush_all();
  k_exit(code);
  for (;;) {}
}
#else
int atexit(void (*fn)(void)) {
  if (hs.natexit >= 32) return -1;
  hs.atexit_fn[hs.natexit++] = fn;
  return 0;
}

void exit(int code) {
  while (hs.natexit > 0) hs.atexit_fn[--hs.natexit]();
  __so_stdio_flush_all();
  k_exit(code);
}
#endif

void abort(void) {
  __so_stdio_flush_out();
#ifdef __GNUC__
  {
    /* Quem chamou: subtraia o endereco de abort e some o de abort no ELF
     * (SODOS_KEEP_ELF=1 no link) para achar a funcao com nm/addr2line. */
    char b[96];
    snprintf(b, sizeof b, "abort() chamado de %p (abort em %p)\n",
             __builtin_return_address(0), (void*)abort);
    k_puts(b);
  }
#else
  k_puts("abort()\n");
#endif
  k_exit(134);
}

void __so_assert(const char* expr, const char* file, int line) {
  fprintf(stderr, "%s:%d: assert falhou: %s\n", file, line, expr);
  abort();
}

/* Roda outro programa: "NOME arg1 arg2" (aspas agrupam). Devolve o exit code
 * ou -1 se nao conseguiu carregar. */
int system(const char* cmd) {
  char buf[512];
  char* argv[32];
  int argc = 0;
  char* p;
  struct heap_state saved;
  void* io;
  int rc;
  if (!cmd) return 1;
  strncpy(buf, cmd, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  p = buf;
  while (*p && argc < 31) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (*p == '"') {
      argv[argc++] = ++p;
      while (*p && *p != '"') p++;
    } else {
      argv[argc++] = p;
      while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (*p) *p++ = 0;
  }
  argv[argc] = NULL;
  if (argc == 0) return 0;

  __so_stdio_flush_out();
  saved = hs;
  memset(&hs, 0, sizeof(hs));
  hs.rand_state = saved.rand_state;
  io = __so_stdio_swap(NULL);
  rc = k_exec(argv[0], argc, argv);
  __so_stdio_flush_out();
  __so_stdio_swap(io);
  hs = saved;
  if (rc <= -1000 && rc >= -1004) {
    fprintf(stderr, "%s: %s\n", argv[0], k_last_error());
    return -1;
  }
  return rc;
}

/* ---------- conversoes ---------- */
static int digit_val(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

unsigned long long strtoull(const char* s, char** end, int base) {
  const char* p = s;
  unsigned long long v = 0;
  int neg = 0;
  int any = 0;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '-') {
    neg = 1;
    p++;
  } else if (*p == '+') {
    p++;
  }
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
      digit_val(p[2]) < 16) {
    p += 2;
    base = 16;
  } else if (base == 0 && p[0] == '0') {
    base = 8;
  } else if (base == 0) {
    base = 10;
  }
  for (;; p++) {
    int d = digit_val(*p);
    if (d >= base) break;
    v = v * (unsigned)base + (unsigned)d;
    any = 1;
  }
  if (end) *end = (char*)(any ? p : s);
  return neg ? (unsigned long long)(-(long long)v) : v;
}

long long strtoll(const char* s, char** end, int base) {
  return (long long)strtoull(s, end, base);
}

unsigned long strtoul(const char* s, char** end, int base) {
  return (unsigned long)strtoull(s, end, base);
}

long strtol(const char* s, char** end, int base) {
  return (long)strtoull(s, end, base);
}

int atoi(const char* s) { return (int)strtol(s, NULL, 10); }
long atol(const char* s) { return strtol(s, NULL, 10); }
long long atoll(const char* s) { return strtoll(s, NULL, 10); }

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }

int rand(void) {
  hs.rand_state = hs.rand_state * 1103515245u + 12345u;
  return (int)((hs.rand_state >> 16) & 0x7FFF);
}

void srand(unsigned seed) { hs.rand_state = seed; }

/* ---------- qsort (heapsort: sem recursao, n log n sempre) ---------- */
static void swap_bytes(char* a, char* b, size_t n) {
  while (n--) {
    char t = *a;
    *a++ = *b;
    *b++ = t;
  }
}

static void sift(char* base, size_t root, size_t n, size_t sz,
                 int (*cmp)(const void*, const void*)) {
  for (;;) {
    size_t child = root * 2 + 1;
    if (child >= n) return;
    if (child + 1 < n && cmp(base + child * sz, base + (child + 1) * sz) < 0) child++;
    if (cmp(base + root * sz, base + child * sz) >= 0) return;
    swap_bytes(base + root * sz, base + child * sz, sz);
    root = child;
  }
}

void qsort(void* b, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
  char* base = (char*)b;
  size_t i;
  if (n < 2) return;
  for (i = n / 2; i > 0; i--) sift(base, i - 1, n, sz, cmp);
  for (i = n - 1; i > 0; i--) {
    swap_bytes(base, base + i * sz, sz);
    sift(base, 0, i, sz, cmp);
  }
}

void* bsearch(const void* key, const void* b, size_t n, size_t sz,
              int (*cmp)(const void*, const void*)) {
  const char* base = (const char*)b;
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int r = cmp(key, base + mid * sz);
    if (r == 0) return (void*)(base + mid * sz);
    if (r < 0) hi = mid;
    else lo = mid + 1;
  }
  return NULL;
}

void* memalign(size_t align, size_t n) { return aligned_alloc(align, n); }

int posix_memalign(void** out, size_t align, size_t n) { void* p = aligned_alloc(align, n); if (!p) return 12; *out = p; return 0; }
