/* soxrun.c - Roda .EXE SOX no Linux (long mode, abaixo de 4 GB) para testar
 * cc/link/libc sem subir o QEMU. Imita as funcoes "KERNEL".
 *
 *   soxrun [-C dir] PROG.EXE [args...]
 *
 * Arquivos: "D:NOME" vira "NOME" relativo a dir (padrao: pasta atual);
 * tenta o nome exato, depois maiusculo, depois minusculo. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sox.h"

#define MAXMOD 32

struct module {
  int used, refs, is_dll;
  char name[32];
  unsigned char* base;
  unsigned size;
  unsigned entry;
  struct sox_export* exports;
  unsigned nexports;
  char* strtab;
  int deps[16];
  int ndeps;
};

static struct module mods[MAXMOD];
static char last_err[128];
static const char* root_dir = ".";

/* ---------- caminhos ---------- */
static const char* host_path(const char* p, char* out, int max) {
  char cand[3][256];
  const char* n = p;
  int i;
  struct stat st;
  if (n[0] && n[1] == ':') n += 2;
  while (*n == '\\' || *n == '/') n++;
  snprintf(cand[0], 256, "%s/%s", root_dir, n);
  snprintf(cand[1], 256, "%s/", root_dir);
  snprintf(cand[2], 256, "%s/", root_dir);
  for (i = 0; n[i]; i++) {
    char u[2] = { (char)toupper((unsigned char)n[i]), 0 };
    char l[2] = { (char)tolower((unsigned char)n[i]), 0 };
    strncat(cand[1], u, 255 - strlen(cand[1]));
    strncat(cand[2], l, 255 - strlen(cand[2]));
  }
  for (i = 0; i < 3; i++) {
    if (stat(cand[i], &st) == 0) break;
  }
  /* arquivo novo: maiusculo, como no FAT */
  strncpy(out, cand[i < 3 ? i : 1], (size_t)max - 1);
  out[max - 1] = 0;
  return out;
}

/* ---------- KERNEL ---------- */
static jmp_buf* exit_jmp[16];
static int exit_code[16];
static int exec_depth;

static void k_putc(int c) {
  char ch = (char)c;
  fwrite(&ch, 1, 1, stdout);
  if (ch == '\n') fflush(stdout);
}
static void k_write(const char* s, int n) {
  if (s && n > 0) fwrite(s, 1, (size_t)n, stdout);
  fflush(stdout);
}
static void k_puts(const char* s) {
  if (s) fputs(s, stdout);
  fflush(stdout);
}
static int k_getc(void) {
  int c = getchar();
  return c == EOF ? 26 : c;
}
static int k_readline(char* buf, int max) {
  int n;
  fflush(stdout);
  if (!fgets(buf, max, stdin)) {
    buf[0] = 26;
    buf[1] = 0;
    return 1;
  }
  n = (int)strlen(buf);
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  return n;
}
static void k_clear(void) { printf("\033[2J\033[H"); }

static int k_fsize(const char* path) {
  char hp[300];
  struct stat st;
  if (stat(host_path(path, hp, sizeof hp), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  return (int)st.st_size;
}
static int k_fload(const char* path, char* buf, int max) {
  char hp[300];
  FILE* f = fopen(host_path(path, hp, sizeof hp), "rb");
  int n;
  if (!f) return -1;
  if (max < 2) {
    fclose(f);
    return -1;
  }
  /* igual ao file_load do kernel: no maximo max-1 bytes + NUL */
  n = (int)fread(buf, 1, (size_t)(max - 1), f);
  buf[n] = 0;
  fclose(f);
  return n;
}
static int k_fsave(const char* path, const char* data, int size) {
  char hp[300];
  FILE* f = fopen(host_path(path, hp, sizeof hp), "wb");
  if (!f) return -1;
  if (size > 0 && fwrite(data, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}
static int k_flist(int letter, char* out, int max) {
  DIR* d = opendir(root_dir);
  struct dirent* e;
  int n = 0, pos = 0;
  (void)letter;
  if (!d) return -1;
  while ((e = readdir(d)) != NULL) {
    int l = (int)strlen(e->d_name);
    if (e->d_name[0] == '.') continue;
    n++;
    if (out && pos + l + 2 < max) {
      memcpy(out + pos, e->d_name, (size_t)l);
      pos += l;
      out[pos++] = '\n';
    }
  }
  if (out && max > 0) out[pos < max ? pos : max - 1] = 0;
  closedir(d);
  return n;
}
static int k_drive(void) { return 'D'; }
static int k_set_drive(int letter) { return letter == 'D' || letter == 'd' ? 0 : -1; }

static void* low_alloc(size_t n);
static void low_free(void* p);

struct lowblk {
  size_t size;
  size_t magic;
};
#define LOW_MAGIC 0x314C4F57u

static void* k_malloc(unsigned n) { return low_alloc(n ? n : 1); }
static void k_free(void* p) { low_free(p); }
static void* k_realloc(void* p, unsigned n) {
  struct lowblk* b;
  void* q;
  size_t old;
  if (!p) return k_malloc(n);
  if (!n) {
    k_free(p);
    return NULL;
  }
  b = (struct lowblk*)p - 1;
  if (b->magic != LOW_MAGIC) return NULL;
  if (b->size >= n) return p;
  old = b->size;
  q = k_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, old);
  k_free(p);
  return q;
}
static unsigned k_msize(void* p) {
  struct lowblk* b;
  if (!p) return 0;
  b = (struct lowblk*)p - 1;
  if (b->magic != LOW_MAGIC) return 0;
  return (unsigned)b->size;
}

/* Ponteiros do programa tem 32 bits. A pilha, o argv e o malloc ficam
 * abaixo de 4 GB, como no kernel. */
static unsigned char* low_stack;
static size_t low_stack_n;

static void* low_alloc(size_t n) {
  struct lowblk* b;
  size_t total = (n + 15u) & ~(size_t)15u;
  void* mem = mmap(NULL, total + sizeof(struct lowblk), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (mem == MAP_FAILED) return NULL;
  b = (struct lowblk*)mem;
  b->size = total;
  b->magic = LOW_MAGIC;
  return b + 1;
}

static void low_free(void* p) {
  struct lowblk* b;
  if (!p) return;
  b = (struct lowblk*)p - 1;
  if (b->magic != LOW_MAGIC) return;
  b->magic = 0;
  munmap(b, b->size + sizeof(struct lowblk));
}

static int rsp_on_low_stack(void) {
  unsigned long sp;
  if (!low_stack) return 0;
  __asm__ volatile ("movq %%rsp, %0" : "=r"(sp));
  return sp >= (unsigned long)low_stack && sp < (unsigned long)low_stack + low_stack_n;
}

/* Chama codigo gerado. Se a pilha ja e a baixa, segue nela (k_exec aninhado). */
static int call_user(void* fn, int a0, void* a1) {
  int rc;
  unsigned char* top;
  if (rsp_on_low_stack()) return ((int (*)(int, void*))fn)(a0, a1);
  if (!low_stack) {
    low_stack_n = 8u << 20;
    low_stack = (unsigned char*)mmap(NULL, low_stack_n, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (low_stack == MAP_FAILED) {
      low_stack = NULL;
      fprintf(stderr, "soxrun: sem pilha abaixo de 4 GB\n");
      return -1005;
    }
  }
  top = low_stack + low_stack_n;
  __asm__ volatile (
    "movq %1, %%r10\n\t"
    "movq %4, %%r11\n\t"
    "pushq %%r12\n\t"
    "movq %%rsp, %%r12\n\t"
    "movq %%r11, %%rsp\n\t"
    "andq $-16, %%rsp\n\t"
    "call *%%r10\n\t"
    "movq %%r12, %%rsp\n\t"
    "popq %%r12\n\t"
    : "=a"(rc)
    : "r"(fn), "D"(a0), "S"(a1), "r"(top)
    : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc"
  );
  return rc;
}

/* O programa enxerga ponteiros de 32 bits. wide=1: argv do Linux (8 bytes). */
static void* low_argv(int argc, char** argv, int wide) {
  int i;
  size_t bytes = (size_t)(argc + 1) * 4u;
  unsigned* out;
  char* s;
  for (i = 0; i < argc; i++) {
    const char* src = "";
    if (argv) {
      if (wide) src = argv[i] ? argv[i] : "";
      else {
        unsigned p = ((unsigned*)argv)[i];
        src = p ? (const char*)(unsigned long)p : "";
      }
    }
    bytes += strlen(src) + 1;
  }
  out = (unsigned*)k_malloc((unsigned)bytes);
  if (!out) return NULL;
  s = (char*)(out + (argc + 1));
  for (i = 0; i < argc; i++) {
    const char* src = "";
    if (argv) {
      if (wide) src = argv[i] ? argv[i] : "";
      else {
        unsigned p = ((unsigned*)argv)[i];
        src = p ? (const char*)(unsigned long)p : "";
      }
    }
    strcpy(s, src);
    out[i] = (unsigned)(unsigned long)s;
    s += strlen(s) + 1;
  }
  out[argc] = 0;
  return out;
}
static void k_meminfo(unsigned* total, unsigned* freeb) {
  if (total) *total = 256u << 20;
  if (freeb) *freeb = 128u << 20;
}

static void k_exit(int code) {
  if (exec_depth <= 0) exit(code);
  exit_code[exec_depth - 1] = code;
  longjmp(*exit_jmp[exec_depth - 1], 1);
}

static int run_exe(const char* path, int argc, char** argv, int wide);
static int k_exec(const char* path, int argc, char** argv) { return run_exe(path, argc, argv, 0); }
static const char* k_last_error(void) { return last_err; }

static int load(const char* req, int want_dll);
static void release(int id);
static int k_dll_load(const char* name) {
  int id = load(name, 1);
  return id < 0 ? 0 : id + 1;
}
static void* find_export(struct module* m, const char* name);
static void* k_dll_sym(int h, const char* name) {
  if (h < 1 || h > MAXMOD || !mods[h - 1].used) return NULL;
  return find_export(&mods[h - 1], name);
}
static void k_dll_free(int h) {
  if (h >= 1 && h <= MAXMOD) release(h - 1);
}

static unsigned k_rdtsc(unsigned* hi) {
  unsigned lo, h;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(h));
  if (hi) *hi = h;
  return lo;
}
static unsigned k_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
static int k_date(void) {
  time_t t = time(0);
  struct tm tm;
  if (!gmtime_r(&t, &tm)) return 0;
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
static void k_random(unsigned char* out, int n) {
  FILE* f = fopen("/dev/urandom", "rb");
  if (f) {
    if (fread(out, 1, (size_t)n, f) != (size_t)n) memset(out, 0x5A, (size_t)n);
    fclose(f);
  }
}

static int sock = -1;
static int k_net_ready(void) { return 1; }
static int k_dns(const char* host, unsigned* ip) {
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  if (getaddrinfo(host, NULL, &hints, &res) != 0) return -1;
  *ip = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(res);
  return 0;
}
static int k_tcp_connect(const char* host, int port) {
  unsigned ip;
  struct sockaddr_in a;
  if (sock >= 0) close(sock);
  sock = -1;
  if (k_dns(host, &ip) != 0) return -1;
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return -1;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(ip);
  if (connect(sock, (struct sockaddr*)&a, sizeof a) != 0) {
    close(sock);
    sock = -1;
    return -2;
  }
  return 0;
}
static int k_tcp_send(const void* d, int n) {
  int sent = 0;
  if (sock < 0) return -1;
  while (sent < n) {
    ssize_t r = send(sock, (const char*)d + sent, (size_t)(n - sent), 0);
    if (r <= 0) return -1;
    sent += (int)r;
  }
  return sent;
}
static int k_tcp_recv(void* d, int cap) {
  ssize_t r;
  if (sock < 0 || cap < 2) return -1;
  /* como o net.c: no maximo cap-1 bytes + NUL; 0 = conexao fechada */
  r = recv(sock, d, (size_t)(cap - 1), 0);
  if (r < 0) return -1;
  ((char*)d)[r] = 0;
  return (int)r;
}
static void k_tcp_close(void) {
  if (sock >= 0) close(sock);
  sock = -1;
}

/* sem WIN: a janela nunca abre (como no shell do DOS) */
static int k_gui_open(const char* t) { (void)t; return -1; }
static void k_gui_nop(void) {}
static void k_gui_size(int* w, int* h) { *w = 0; *h = 0; }
static void k_gui_font(int* cw, int* ch) { *cw = 8; *ch = 16; }
static int k_gui_event(int* ev, int ms) { (void)ms; ev[0] = 3; return -1; }
static void k_gui_pixels(int x, int y, int w, int h, const void* p, int stride) {
  (void)x; (void)y; (void)w; (void)h; (void)p; (void)stride;
}

#define E(n) { #n, (void*)n }
static const struct {
  const char* name;
  void* fn;
} ktab[] = {
  E(k_putc), E(k_write), E(k_puts), E(k_getc), E(k_readline), E(k_clear),
  E(k_fsize), E(k_fload), E(k_fsave), E(k_flist), E(k_drive), E(k_set_drive),
  E(k_malloc), E(k_free), E(k_realloc), E(k_msize), E(k_meminfo),
  E(k_exit), E(k_exec), E(k_last_error), E(k_dll_load), E(k_dll_sym), E(k_dll_free),
  E(k_rdtsc), E(k_ms), E(k_date), E(k_random),
  E(k_net_ready), E(k_dns), E(k_tcp_connect), E(k_tcp_send), E(k_tcp_recv), E(k_tcp_close),
  E(k_gui_open), E(k_gui_size), E(k_gui_font), E(k_gui_event),
  { "k_gui_close", (void*)k_gui_nop }, { "k_gui_title", (void*)k_gui_nop },
  { "k_gui_fill", (void*)k_gui_nop }, { "k_gui_text", (void*)k_gui_nop },
  { "k_gui_present", (void*)k_gui_nop },
  E(k_gui_pixels),
};

static void* kfind(const char* n) {
  unsigned i;
  for (i = 0; i < sizeof ktab / sizeof ktab[0]; i++)
    if (strcmp(ktab[i].name, n) == 0) return ktab[i].fn;
  return NULL;
}

/* ---------- loader ---------- */
static void base_name(const char* path, char* out, int max) {
  const char* b = path;
  const char* p;
  int i = 0;
  for (p = path; *p; p++)
    if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
  while (*b && i < max - 1) out[i++] = (char)toupper((unsigned char)*b++);
  out[i] = 0;
}

static void* find_export(struct module* m, const char* name) {
  unsigned i;
  for (i = 0; i < m->nexports; i++)
    if (strcmp(m->strtab + m->exports[i].name, name) == 0) return m->base + m->exports[i].value;
  return NULL;
}

static void unload(int id) {
  struct module* m = &mods[id];
  int i;
  if (m->is_dll && m->entry != SOX_NO_ENTRY) call_user(m->base + m->entry, SOX_DLL_DETACH, NULL);
  for (i = 0; i < m->ndeps; i++) release(m->deps[i]);
  munmap(m->base, m->size ? m->size : 1);
  free(m->exports);
  free(m->strtab);
  m->used = 0;
}

static void release(int id) {
  if (id < 0 || id >= MAXMOD || !mods[id].used) return;
  if (--mods[id].refs <= 0) unload(id);
}

static int load(const char* req, int want_dll) {
  char bname[32], hp[300];
  FILE* f;
  long fsz;
  unsigned char* file;
  struct sox_hdr* h;
  struct module* m;
  int id;
  unsigned i, total;
  base_name(req, bname, sizeof bname);
  if (want_dll) {
    for (id = 0; id < MAXMOD; id++)
      if (mods[id].used && mods[id].is_dll && strcmp(mods[id].name, bname) == 0) {
        mods[id].refs++;
        return id;
      }
  }
  f = fopen(host_path(req, hp, sizeof hp), "rb");
  if (!f) {
    snprintf(last_err, sizeof last_err, "nao achei %s", req);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  fsz = ftell(f);
  fseek(f, 0, SEEK_SET);
  file = (unsigned char*)malloc((size_t)fsz + 1);
  if (fread(file, 1, (size_t)fsz, f) != (size_t)fsz) fsz = 0;
  fclose(f);
  h = (struct sox_hdr*)file;
  if (fsz < (long)sizeof *h || h->magic != SOX_MAGIC ||
      ((h->flags & SOX_FLAG_DLL) != 0) != (want_dll != 0)) {
    snprintf(last_err, sizeof last_err, "nao e %s SOX: %s", want_dll ? "DLL" : "EXE", bname);
    free(file);
    return -1;
  }
  for (id = 0; id < MAXMOD && mods[id].used; id++) {}
  if (id == MAXMOD) {
    free(file);
    return -1;
  }
  m = &mods[id];
  memset(m, 0, sizeof *m);
  total = h->image_size + h->bss_size;
  m->base = (unsigned char*)mmap(NULL, total ? total : 1, PROT_READ | PROT_WRITE | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (m->base == MAP_FAILED) {
    free(file);
    return -1;
  }
  m->used = 1;
  m->refs = 1;
  m->is_dll = want_dll;
  strcpy(m->name, bname);
  m->size = total;
  m->entry = h->entry;
  memcpy(m->base, file + h->off_image, h->image_size);
  m->strtab = (char*)malloc(h->strtab_size + 1);
  memcpy(m->strtab, file + h->off_strtab, h->strtab_size);
  m->strtab[h->strtab_size] = 0;
  m->nexports = h->nexports;
  m->exports = (struct sox_export*)malloc(h->nexports * sizeof(struct sox_export) + 1);
  memcpy(m->exports, file + h->off_exports, h->nexports * sizeof(struct sox_export));
  for (i = 0; i < h->nrelocs; i++) {
    unsigned off = ((unsigned*)(file + h->off_relocs))[i];
    *(unsigned*)(m->base + off) += (unsigned)(unsigned long)m->base;
  }
  for (i = 0; i < h->nimports; i++) {
    struct sox_import* im = (struct sox_import*)(file + h->off_imports) + i;
    const char* lib = (const char*)file + h->off_strtab + im->lib;
    const char* fn = (const char*)file + h->off_strtab + im->name;
    void* addr = NULL;
    if (strcasecmp(lib, "KERNEL") == 0) {
      addr = kfind(fn);
    } else {
      char b[32];
      int k, dep = -1;
      base_name(lib, b, sizeof b);
      for (k = 0; k < m->ndeps; k++)
        if (strcmp(mods[m->deps[k]].name, b) == 0) dep = m->deps[k];
      if (dep < 0) {
        dep = load(lib, 1);
        if (dep < 0) {
          free(file);
          m->entry = SOX_NO_ENTRY;
          unload(id);
          return -1;
        }
        m->deps[m->ndeps++] = dep;
      }
      addr = find_export(&mods[dep], fn);
    }
    if (!addr) {
      snprintf(last_err, sizeof last_err, "falta %s!%s", lib, fn);
      free(file);
      m->entry = SOX_NO_ENTRY;
      unload(id);
      return -1;
    }
    *(unsigned*)(m->base + im->slot) = (unsigned)(unsigned long)addr;
  }
  free(file);
  if (want_dll && m->entry != SOX_NO_ENTRY &&
      call_user(m->base + m->entry, SOX_DLL_ATTACH, NULL) == 0) {
    snprintf(last_err, sizeof last_err, "DllMain recusou %s", bname);
    m->entry = SOX_NO_ENTRY;
    unload(id);
    return -1;
  }
  return id;
}

static int run_exe(const char* path, int argc, char** argv, int wide) {
  jmp_buf jb;
  int id, rc;
  char p2[256];
  char** av;
  if (exec_depth >= 16) return -1004;
  id = load(path, 0);
  if (id < 0 && !strchr(path, '.')) {
    snprintf(p2, sizeof p2, "%s.EXE", path);
    id = load(p2, 0);
  }
  if (id < 0) return -1003;
  av = low_argv(argc, argv, wide);
  if (!av) return -1005;
  exit_jmp[exec_depth] = &jb;
  exec_depth++;
  if (setjmp(jb) == 0) {
    rc = call_user(mods[id].base + mods[id].entry, argc, av);
  } else {
    rc = exit_code[exec_depth - 1];
  }
  exec_depth--;
  k_free(av);
  release(id);
  return rc;
}

int main(int argc, char** argv) {
  int a = 1, rc;
  if (a + 1 < argc && strcmp(argv[a], "-C") == 0) {
    root_dir = argv[a + 1];
    a += 2;
  }
  if (a >= argc) {
    fprintf(stderr, "uso: soxrun [-C dir] PROG.EXE [args...]\n");
    return 2;
  }
  rc = run_exe(argv[a], argc - a, argv + a, 1);
  fflush(stdout);
  if (rc <= -1000 && rc >= -1004) {
    fprintf(stderr, "soxrun: %s\n", last_err);
    return 127;
  }
  return rc & 255;
}
