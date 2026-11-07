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
#include <errno.h>
#include <sys/select.h>
#include <sys/ioctl.h>
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
static char cwd_rel[200]; /* relativo a root_dir; vazio = raiz D:\ */

/* ---------- caminhos ---------- */
static void slash_to_host(char* s) {
  for (; *s; s++)
    if (*s == '\\') *s = '/';
}

static const char* host_path(const char* p, char* out, int max) {
  char cand[3][256];
  char rel[256];
  const char* n = p;
  int i;
  struct stat st;
  {
    int abs = (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/';
    if (n[0] && n[1] == ':') n += 2;
    while (*n == '\\' || *n == '/') n++;
    if (!abs && cwd_rel[0]) {
      if (n[0]) snprintf(rel, sizeof rel, "%s/%s", cwd_rel, n);
      else snprintf(rel, sizeof rel, "%s", cwd_rel);
    } else {
      snprintf(rel, sizeof rel, "%s", n);
    }
  }
  slash_to_host(rel);
  snprintf(cand[0], 256, "%s/%s", root_dir, rel);
  snprintf(cand[1], 256, "%s/", root_dir);
  snprintf(cand[2], 256, "%s/", root_dir);
  for (i = 0; rel[i]; i++) {
    char u[2] = { (char)toupper((unsigned char)rel[i]), 0 };
    char l[2] = { (char)tolower((unsigned char)rel[i]), 0 };
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

/* k_shell_capture desvia a saida para ca */
static char* cap_buf;
static int cap_len, cap_max, cap_cut;
static void cap_put(const char* s, int n) {
  while (n-- > 0) {
    if (cap_len < cap_max - 1) cap_buf[cap_len++] = *s;
    else cap_cut = 1;
    s++;
  }
}
static void k_putc(int c) {
  char ch = (char)c;
  if (cap_buf) { cap_put(&ch, 1); return; }
  fwrite(&ch, 1, 1, stdout);
  if (ch == '\n') fflush(stdout);
}
static void k_write(const char* s, int n) {
  if (cap_buf) { if (s) cap_put(s, n); return; }
  if (s && n > 0) fwrite(s, 1, (size_t)n, stdout);
  fflush(stdout);
}
static void k_puts(const char* s) {
  if (cap_buf) { if (s) cap_put(s, (int)strlen(s)); return; }
  if (s) fputs(s, stdout);
  fflush(stdout);
}
/* Tela de celulas de k_vput. Com SOX_SCREEN=pref, cada k_getc grava a tela
 * em pref-N.bin (4000 bytes, caractere e cor), para conferir o desenho. */
static unsigned short vscreen[25 * 80];
static int vscreen_used;
static int vscreen_frame;

static void vscreen_dump(void) {
  const char* pref = getenv("SOX_SCREEN");
  char name[300];
  FILE* f;
  if (!pref || !vscreen_used) return;
  snprintf(name, sizeof name, "%s-%d.bin", pref, vscreen_frame++);
  f = fopen(name, "wb");
  if (!f) return;
  fwrite(vscreen, 2, 25 * 80, f);
  fclose(f);
}

/* Com SOX_KEYS="DOWN,F5,ENTER,x" as teclas vem dali (nomes ou um caractere);
 * no fim, 26 (fim de arquivo). */
static int key_token(const char* t, int n) {
  static const struct { const char* name; int code; } names[] = {
    {"UP", 0x100}, {"DOWN", 0x101}, {"LEFT", 0x102}, {"RIGHT", 0x103},
    {"PGUP", 0x104}, {"PGDN", 0x105}, {"HOME", 0x106}, {"END", 0x107},
    {"DEL", 0x108}, {"INS", 0x109}, {"ENTER", '\n'}, {"TAB", '\t'},
    {"ESC", 27}, {"BS", 8}, {"SPACE", ' '}, {"COMMA", ','},
  };
  unsigned i;
  if (n == 1) return (unsigned char)t[0];
  if (n >= 2 && t[0] == 'F' && t[1] >= '1' && t[1] <= '9') return 0x140 + atoi(t + 1);
  for (i = 0; i < sizeof names / sizeof names[0]; i++)
    if ((int)strlen(names[i].name) == n && strncmp(names[i].name, t, (size_t)n) == 0)
      return names[i].code;
  return '?';
}

static int k_getc(void) {
  static const char* keys;
  static int keys_init;
  int c;
  vscreen_dump();
  if (!keys_init) {
    keys = getenv("SOX_KEYS");
    keys_init = 1;
  }
  if (keys) {
    const char* e;
    if (!*keys) return 26;
    e = strchr(keys, ',');
    if (!e) e = keys + strlen(keys);
    c = key_token(keys, (int)(e - keys));
    keys = *e ? e + 1 : e;
    return c;
  }
  c = getchar();
  return c == EOF ? 26 : c;
}

static unsigned k_ms(void);

/* Nao espera. Com SOX_KEYS entrega uma tecla a cada 200 ms (o resto e -1),
 * para um laco de jogo andar entre as telas gravadas; sem ele, olha o stdin. */
static int k_trygetc(void) {
  static unsigned next;
  struct timeval tv = {0, 0};
  fd_set fds;
  if (getenv("SOX_KEYS")) {
    unsigned now = k_ms();
    if (next && (int)(now - next) < 0) return -1;
    next = now + 200;
    return k_getc();
  }
  FD_ZERO(&fds);
  FD_SET(0, &fds);
  if (select(1, &fds, 0, 0, &tv) <= 0) return -1;
  return k_getc();
}

static int k_vput(int row, int col, const unsigned short* cells, int n) {
  int pos;
  if (row < 0 || row >= 25 || col < 0 || col >= 80 || !cells) return 0;
  pos = row * 80 + col;
  if (n > 2000 - pos) n = 2000 - pos;
  if (n > 0) memcpy(vscreen + pos, cells, (size_t)n * 2);
  vscreen_used = 1;
  return 0;
}
static void k_cursor(int row, int col) { (void)row; (void)col; }
static unsigned char vfont[256][16];
static int k_font_get(int ch, unsigned char* glyph) {
  if (ch < 0 || ch > 255 || !glyph) return -1;
  memcpy(glyph, vfont[ch], 16);
  return 0;
}
static int k_font_set(int ch, const unsigned char* glyph) {
  if (ch < 0 || ch > 255 || !glyph) return -1;
  memcpy(vfont[ch], glyph, 16);
  return 0;
}
static void k_font_reset(void) { memset(vfont, 0, sizeof vfont); }
static int k_blink(int on) { (void)on; return 0; }
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
/* Como o kernel (fat_write_file), cria as pastas do caminho. */
static void make_parents(char* hp) {
  char* p;
  for (p = hp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    mkdir(hp, 0755);
    *p = '/';
  }
}
static int k_fsave(const char* path, const char* data, int size) {
  char hp[300];
  FILE* f = fopen(host_path(path, hp, sizeof hp), "wb");
  if (!f) {
    make_parents(hp);
    f = fopen(hp, "wb");
  }
  if (!f) return -1;
  if (size > 0 && fwrite(data, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}
static int k_fread(const char* path, unsigned offset, char* buf, int len) {
  char hp[300];
  FILE* f = fopen(host_path(path, hp, sizeof hp), "rb");
  int n;
  if (!f || len < 1) {
    if (f) fclose(f);
    return -1;
  }
  if (fseek(f, (long)offset, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  n = (int)fread(buf, 1, (size_t)len, f);
  fclose(f);
  return n;
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
static int k_dirlist(const char* path, char* out, int max) {
  char hp[300];
  DIR* d;
  struct dirent* e;
  int pos = 0;
  if (!out || max < 2) return -1;
  out[0] = 0;
  host_path(path && path[0] ? path : "D:\\", hp, sizeof hp);
  d = opendir(hp);
  if (!d) return -1;
  while ((e = readdir(d)) != NULL) {
    char sub[400];
    struct stat st;
    int l;
    if (e->d_name[0] == '.') continue;
    l = (int)strlen(e->d_name);
    if (pos + l + 3 >= max) break;
    memcpy(out + pos, e->d_name, (size_t)l);
    pos += l;
    snprintf(sub, sizeof sub, "%s/%s", hp, e->d_name);
    if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) out[pos++] = '\\';
    out[pos++] = '\n';
  }
  out[pos] = 0;
  closedir(d);
  return pos > 0 ? 1 : 0;
}
static void k_pwd(char* out, int max) {
  int i = 0, j = 0;
  if (!out || max < 4) return;
  out[i++] = 'D';
  out[i++] = ':';
  out[i++] = '\\';
  while (cwd_rel[j] && i < max - 1) {
    char c = cwd_rel[j++];
    out[i++] = (c == '/') ? '\\' : c;
  }
  out[i] = 0;
}
static int k_chdir(const char* path) {
  char hp[300];
  struct stat st;
  const char* n;
  if (!path || !path[0]) return 0;
  host_path(path, hp, sizeof hp);
  if (stat(hp, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
  n = path;
  if (n[0] && n[1] == ':') n += 2;
  while (*n == '\\' || *n == '/') n++;
  if (path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':')) {
    strncpy(cwd_rel, n, sizeof(cwd_rel) - 1);
  } else if (cwd_rel[0]) {
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s/%s", cwd_rel, n);
    strncpy(cwd_rel, tmp, sizeof(cwd_rel) - 1);
  } else {
    strncpy(cwd_rel, n, sizeof(cwd_rel) - 1);
  }
  cwd_rel[sizeof(cwd_rel) - 1] = 0;
  slash_to_host(cwd_rel);
  return 0;
}
struct k_dirent {
  char name[32];
  unsigned size;
  unsigned short date;
  unsigned short time;
  unsigned char dir;
  unsigned char pad[3];
};
static int k_dir(const char* path, struct k_dirent* out, int max) {
  char hp[300];
  DIR* d;
  struct dirent* e;
  int n = 0;
  host_path(path && path[0] ? path : "D:\\", hp, sizeof hp);
  d = opendir(hp);
  if (!d) return -1;
  while ((e = readdir(d)) != NULL && n < max) {
    char sub[600];
    struct stat st;
    struct tm tm;
    if (e->d_name[0] == '.') continue;
    snprintf(sub, sizeof sub, "%s/%s", hp, e->d_name);
    if (stat(sub, &st) != 0) continue;
    memset(&out[n], 0, sizeof out[n]);
    snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
    out[n].dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out[n].size = out[n].dir ? 0 : (unsigned)st.st_size;
    if (localtime_r(&st.st_mtime, &tm)) {
      out[n].date = (unsigned short)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
      out[n].time = (unsigned short)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    }
    n++;
  }
  closedir(d);
  return n;
}
static int k_fdelete(const char* path) {
  char hp[300];
  struct stat st;
  host_path(path, hp, sizeof hp);
  if (stat(hp, &st) != 0) return -1;
  if (S_ISDIR(st.st_mode)) return -3;
  return unlink(hp) == 0 ? 0 : -2;
}
static int k_mkdir(const char* path) {
  char hp[300];
  struct stat st;
  host_path(path, hp, sizeof hp);
  if (stat(hp, &st) == 0) return 1;
  return mkdir(hp, 0755) == 0 ? 0 : -1;
}
static int k_rmdir(const char* path) {
  char hp[300];
  host_path(path, hp, sizeof hp);
  return rmdir(hp) == 0 ? 0 : -2;
}
static int k_dfree(int letter, unsigned* total_kb, unsigned* free_kb) {
  (void)letter;
  if (total_kb) *total_kb = 2u * 1024u * 1024u;
  if (free_kb) *free_kb = 1234567u;
  return 0;
}
static int k_drive(void) { return 'D'; }
static int k_set_drive(int letter) { return letter == 'D' || letter == 'd' ? 0 : -1; }
static int k_exe_drive(void) { return 'D'; }

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
  /* MAP_32BIT so usa os primeiros 2 GB: blocos grandes (modelos do LLAMA)
   * vao para 2..4 GB, que ponteiros de 32 bits ainda alcancam. */
  if (mem == MAP_FAILED) {
    unsigned long a;
    for (a = 0x80000000ul; a + total + sizeof(struct lowblk) <= 0xFFFF0000ul; a += 0x4000000ul) {
      mem = mmap((void*)a, total + sizeof(struct lowblk), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (mem != MAP_FAILED) break;
    }
  }
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

/* TOP: uma maquina inventada de 4 GB (3 GB abaixo de 4 GB, buraco, 1 GB acima). */
struct k_proc {
  char name[32];
  unsigned pid, kind, state, base, image_kb, data_kb, refs, pad;
};
static int k_ps(struct k_proc* out, int max) {
  static const struct k_proc fake[] = {
    { "MC.EXE", 3, 1, 'E', 0x01800000u, 92, 1100, 1, 0 },
    { "TOP.EXE", 4, 1, 'R', 0x01A00000u, 40, 1090, 1, 0 },
    { "LIBC.DLL", 0, 2, 'S', 0x01700000u, 120, 0, 2, 0 },
    { "KERNEL", 0, 3, 'S', 0x01000000u, 4200, 380, 0, 0 },
    { "BUFFERS", 0, 3, 'S', 0x02000000u, 0, 24000, 0, 0 },
    { "MEM>4GB", 0, 3, 'S', 0, 0, 0, 0, 0 },
    { "REDE", 0, 4, 'A', 0, 0, 0, 0, 0 },
    { "HTTPD", 0, 4, 'A', 0, 0, 0, 8797, 0 },
  };
  int n = (int)(sizeof fake / sizeof fake[0]), i;
  if (n > max) n = max;
  for (i = 0; i < n; i++) out[i] = fake[i];
  return n;
}
static int k_memmap(unsigned* kb, int cells, unsigned* cell_kb, unsigned pid) {
  unsigned long long span = 5ull << 30, cell;
  int n, c;
  if (!kb || !cell_kb || cells <= 0) return 0;
  if (cells > 2048) cells = 2048;
  cell = *cell_kb ? (unsigned long long)*cell_kb << 10 : 1ull << 20;
  if (!*cell_kb) while ((span + cell - 1) / cell > (unsigned long long)cells) cell <<= 1;
  n = (int)((span + cell - 1) / cell);
  if (n > cells) n = cells;
  memset(kb, 0, (size_t)n * 8 * 4);
  for (c = 0; c < n; c++) {
    unsigned long long a = (unsigned long long)c * cell, e = a + cell;
    unsigned* k = kb + c * 8;
    unsigned ck = (unsigned)(cell >> 10);
    if (a >= 0xC0000000ull && a < 0x100000000ull) k[6] = ck;
    else if (a < 0x01800000ull) k[1] = ck;
    else if (a < 0x01A00000ull) { k[2] = ck / 8; k[3] = ck / 8; k[4] = ck / 2; k[0] = ck - k[2] - k[3] - k[4]; }
    else if (a < 0x03800000ull) { k[5] = ck * 3 / 4; k[0] = ck - k[5]; }
    else if (a >= 0x20000000ull && a < 0x24000000ull) k[4] = ck;
    else k[0] = ck;
    if (pid && a < 0x01A00000ull && e > 0x01800000ull) k[7] = ck / 2;
    if (pid == 4 && a >= 0x20000000ull && a < 0x24000000ull) k[7] = ck;
  }
  *cell_kb = (unsigned)(cell >> 10);
  return n;
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
static int k_time(void) {
  time_t t = time(0);
  struct tm tm;
  if (!gmtime_r(&t, &tm)) return -1;
  return tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec;
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
/* libsodos: ceder o turno e consultar o buffer de recepcao sem bloquear. */
static void k_yield(void) {}
static int k_net_pending(void) {
  int avail = 0;
  char c;
  ssize_t r;
  if (sock < 0) return -1;
  if (ioctl(sock, FIONREAD, &avail) != 0) return -1;
  if (avail > 0) return avail;
  r = recv(sock, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  if (r == 0) return -1;                       /* o par fechou */
  if (r > 0) return 1;
  return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

/* No SO isto e o prompt. Aqui: echo imprime, o resto vira k_exec. */
static int k_ci_start(const char* s, const char* p) {
  while (*p) {
    char a = *s++;
    char b = *p++;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return 1;
}

static int k_shell(char* line) {
  char buf[512];
  char* argv[16];
  char* p;
  int argc = 0;
  int rc;
  if (!line) return 1;
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  p = buf;
  while (*p == ' ' || *p == '\t') p++;
  if (k_ci_start(p, "echo") && (p[4] == 0 || p[4] == ' ' || p[4] == '\t')) {
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    k_puts(p);
    k_putc('\n');
    return 0;
  }
  while (*p && argc < 16) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
  }
  if (argc == 0) return 0;
  rc = k_exec(argv[0], argc, argv);
  if (rc <= -1000) {
    k_puts("Bad command or file name\n");
    return 1;
  }
  return rc;
}

static int k_shell_capture(char* line, char* out, int max, int* len) {
  char* sbuf = cap_buf;
  int slen = cap_len, smax = cap_max, scut = cap_cut;
  int rc;
  if (len) *len = 0;
  if (!out || max < 1) return k_shell(line);
  fflush(stdout);
  cap_buf = out;
  cap_len = 0;
  cap_max = max;
  cap_cut = 0;
  rc = k_shell(line);
  out[cap_len] = 0;
  if (len) *len = cap_cut ? -cap_len : cap_len;
  cap_buf = sbuf;
  cap_len = slen;
  cap_max = smax;
  cap_cut = scut;
  return rc;
}

/* Sem SOX_GUI a janela nunca abre (como no shell do DOS).
 * SOX_GUI=800x600 abre uma janela em memoria; cada k_gui_present grava
 * SOX_GUI_OUT (padrao gui) como .ppm. SOX_GUI_EVENTS e um roteiro:
 * "wait:500,click:120:80,drag:130:90,up,key:a,key:ENTER,wheel:3:x:y,close".
 * wait so passa em gui_event com timeout; no fim do roteiro vem CLOSE. */
static unsigned char* gui_fb;
static int gui_w, gui_h, gui_frames;
static const char* gui_script;
static unsigned gui_wait_end;
static int gui_wait_on;

static int k_gui_open(const char* t) {
  const char* g = getenv("SOX_GUI");
  (void)t;
  if (!g || sscanf(g, "%dx%d", &gui_w, &gui_h) != 2 || gui_w < 1 || gui_h < 1) return -1;
  free(gui_fb);
  gui_fb = (unsigned char*)calloc((size_t)gui_w * gui_h, 3);
  gui_script = getenv("SOX_GUI_EVENTS");
  return gui_fb ? 0 : -1;
}
static void k_gui_nop(void) {}
static void k_gui_close(void) {
  free(gui_fb);
  gui_fb = 0;
}
static void k_gui_size(int* w, int* h) { *w = gui_fb ? gui_w : 0; *h = gui_fb ? gui_h : 0; }
static void k_gui_font(int* cw, int* ch) { *cw = 8; *ch = 16; }
static void k_gui_present(void) {
  const char* pref = getenv("SOX_GUI_OUT");
  char name[300];
  FILE* f;
  if (!gui_fb) return;
  snprintf(name, sizeof name, "%s.ppm", pref ? pref : "gui");
  f = fopen(name, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", gui_w, gui_h);
  fwrite(gui_fb, 3, (size_t)gui_w * gui_h, f);
  fclose(f);
  gui_frames++;
}
static void k_gui_fill(int x, int y, int w, int h, unsigned rgb) {
  int yy, xx;
  if (!gui_fb) return;
  for (yy = y; yy < y + h; yy++)
    for (xx = x; xx < x + w; xx++) {
      unsigned char* d;
      if (xx < 0 || yy < 0 || xx >= gui_w || yy >= gui_h) continue;
      d = gui_fb + ((size_t)yy * gui_w + xx) * 3;
      d[0] = (unsigned char)(rgb >> 16);
      d[1] = (unsigned char)(rgb >> 8);
      d[2] = (unsigned char)rgb;
    }
}
static int k_gui_event(int* ev, int ms) {
  const char* e;
  char tok[64];
  int n;
  ev[0] = ev[1] = ev[2] = ev[3] = 0;
  if (!gui_fb) {
    ev[0] = 3;
    return -1;
  }
  for (;;) {
    if (!gui_script || !*gui_script) {
      ev[0] = 3;
      return 3;
    }
    e = strchr(gui_script, ',');
    if (!e) e = gui_script + strlen(gui_script);
    n = (int)(e - gui_script);
    if (n >= (int)sizeof tok) n = (int)sizeof tok - 1;
    memcpy(tok, gui_script, (size_t)n);
    tok[n] = 0;
    if (strncmp(tok, "wait:", 5) == 0) {
      /* tempo real: o programa pode esperar com timeout 0 e k_ms */
      int left, step;
      if (!gui_wait_on) {
        gui_wait_end = k_ms() + (unsigned)atoi(tok + 5);
        gui_wait_on = 1;
      }
      left = (int)(gui_wait_end - k_ms());
      if (ms < 0 || left <= 0) {
        gui_wait_on = 0;
        gui_script = *e ? e + 1 : e;
        continue;
      }
      step = ms < left ? ms : left;
      usleep((useconds_t)(step > 0 ? step : 1) * 1000);
      return 0;
    }
    gui_script = *e ? e + 1 : e;
    if (sscanf(tok, "click:%d:%d", &ev[1], &ev[2]) == 2) {
      ev[0] = 1;
      ev[3] = 1;
    } else if (sscanf(tok, "drag:%d:%d", &ev[1], &ev[2]) == 2) {
      ev[0] = 5;
    } else if (strcmp(tok, "up") == 0) {
      ev[0] = 4;
    } else if (sscanf(tok, "wheel:%d", &ev[3]) == 1) {
      /* wheel:N ou wheel:N:x:y (sem x:y, no meio da janela) */
      ev[0] = 6;
      ev[1] = gui_w / 2;
      ev[2] = gui_h / 2;
      sscanf(tok, "wheel:%d:%d:%d", &ev[3], &ev[1], &ev[2]);
    } else if (strncmp(tok, "key:", 4) == 0) {
      static const struct { const char* name; int code; } names[] = {
        {"UP", 0x101}, {"DOWN", 0x102}, {"LEFT", 0x103}, {"RIGHT", 0x104},
        {"PGUP", 0x105}, {"PGDN", 0x106}, {"HOME", 0x107}, {"END", 0x108},
        {"DEL", 0x109}, {"ENTER", 13}, {"TAB", 9}, {"ESC", 27}, {"BS", 8},
        {"SPACE", ' '}, {"COMMA", ','},
      };
      unsigned i;
      ev[0] = 2;
      ev[3] = (unsigned char)tok[4];
      for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(tok + 4, names[i].name) == 0) ev[3] = names[i].code;
    } else {
      ev[0] = 3;
    }
    return ev[0];
  }
}
static void k_gui_pixels(int x, int y, int w, int h, const void* p, int stride) {
  int yy, xx;
  if (!gui_fb || !p) return;
  for (yy = 0; yy < h; yy++) {
    const unsigned char* row = (const unsigned char*)p + (size_t)yy * stride;
    if (y + yy < 0 || y + yy >= gui_h) continue;
    for (xx = 0; xx < w; xx++) {
      const unsigned char* q = row + xx * 4;
      unsigned char* d;
      if (x + xx < 0 || x + xx >= gui_w || q[3] < 128) continue;
      d = gui_fb + ((size_t)(y + yy) * gui_w + x + xx) * 3;
      d[0] = q[0];
      d[1] = q[1];
      d[2] = q[2];
    }
  }
}

#define E(n) { #n, (void*)n }
static const struct {
  const char* name;
  void* fn;
} ktab[] = {
  E(k_putc), E(k_write), E(k_puts), E(k_getc), E(k_trygetc), E(k_readline), E(k_clear), E(k_yield),
  E(k_fsize), E(k_fload), E(k_fread), E(k_fsave), E(k_flist), E(k_dirlist), E(k_pwd), E(k_chdir),
  E(k_drive), E(k_set_drive), E(k_exe_drive), E(k_dir), E(k_fdelete), E(k_mkdir), E(k_rmdir), E(k_dfree),
  E(k_vput), E(k_cursor), E(k_font_get), E(k_font_set), E(k_font_reset), E(k_blink),
  E(k_malloc), E(k_free), E(k_realloc), E(k_msize), E(k_meminfo), E(k_ps), E(k_memmap),
  E(k_exit), E(k_exec), E(k_shell), E(k_shell_capture), E(k_last_error), E(k_dll_load), E(k_dll_sym), E(k_dll_free),
  E(k_rdtsc), E(k_ms), E(k_date), E(k_time), E(k_random),
  E(k_net_ready), E(k_dns), E(k_tcp_connect), E(k_tcp_send), E(k_tcp_recv), E(k_net_pending), E(k_tcp_close),
  E(k_gui_open), E(k_gui_size), E(k_gui_font), E(k_gui_event),
  E(k_gui_close), { "k_gui_title", (void*)k_gui_nop },
  E(k_gui_fill), { "k_gui_text", (void*)k_gui_nop },
  E(k_gui_present),
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
