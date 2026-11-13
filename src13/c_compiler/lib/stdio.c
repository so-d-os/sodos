/* stdio.c - <stdio.h> sobre k_fload/k_fsave (arquivo inteiro na memoria). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <so.h>
#include "unixdrv.h"

#define F_READ 1
#define F_WRITE 2
#define F_APPEND 4
#define F_DIRTY 8

#define K_STDIN 0
#define K_STDOUT 1
#define K_STDERR 2
#define K_MEM 3
#define K_RANDOM 4

struct __so_file {
  int kind;
  int flags;
  char* buf;
  unsigned size;
  unsigned cap;
  unsigned pos;
  int eof;
  int err;
  int unget;
  struct __so_file* next;
  char path[FILENAME_MAX];
  int fd; /* K_RANDOM: fd da posix-lite que gera os bytes em cada read */
};

struct io_state {
  FILE* open_list;
  char inbuf[256];
  int inpos;
  int inlen;
};

static FILE std_files[3] = {
  { K_STDIN, F_READ, 0, 0, 0, 0, 0, 0, -1, 0, "" },
  { K_STDOUT, F_WRITE, 0, 0, 0, 0, 0, 0, -1, 0, "" },
  { K_STDERR, F_WRITE, 0, 0, 0, 0, 0, 0, -1, 0, "" },
};

static struct io_state io;
static struct io_state io_saved[8];
static int io_depth;

static char obuf[512];
static int olen;

FILE* __so_stdio(int which) {
  if (which < 0 || which > 2) return NULL;
  return &std_files[which];
}

/* Espelho na serial (COM1) da saida do programa: ajuda a depurar portes que
 * ainda nao tem console proprio. */
#ifdef __x86_64__
/* Ultimos 8 KB escritos no console: o registro de crash do port (Firefox)
 * guarda isso junto da pilha, com as mensagens que vieram antes do erro. */
static char con_tail[8192];
static unsigned con_pos;

int __so_console_tail(char* out, int max) {
  unsigned n = con_pos < sizeof con_tail ? con_pos : (unsigned)sizeof con_tail;
  unsigned i, start = con_pos - n;
  if (!out || max <= 0) return 0;
  if (n > (unsigned)max) {
    start += n - (unsigned)max;
    n = (unsigned)max;
  }
  for (i = 0; i < n; i++) out[i] = con_tail[(start + i) % sizeof con_tail];
  return (int)n;
}

static void ser_write(const char* s, int n) {
  int i;
  if (!s) return;
  for (i = 0; i < n; i++) {
    __asm__ volatile("outb %0, %1" :: "a"((unsigned char)s[i]), "d"((unsigned short)0x3F8));
    con_tail[con_pos++ % sizeof con_tail] = s[i];
  }
}
#else
static void ser_write(const char* s, int n) { (void)s; (void)n; }
#endif

void __so_stdio_flush_out(void) {
  if (olen > 0) {
    ser_write(obuf, olen);
    k_write(obuf, olen);
  }
  olen = 0;
}

/* system(): NULL guarda o estado e limpa; outro valor restaura. */
void* __so_stdio_swap(void* st) {
  if (!st) {
    if (io_depth >= 8) return (void*)1;
    io_saved[io_depth++] = io;
    memset(&io, 0, sizeof(io));
    return (void*)1;
  }
  if (io_depth > 0) io = io_saved[--io_depth];
  return NULL;
}

static int mem_save(FILE* f) {
  if (!(f->flags & F_DIRTY)) return 0;
  f->flags &= ~F_DIRTY;
  if (k_fsave(f->path, f->buf ? f->buf : "", (int)f->size) < 0) {
    f->err = 1;
    errno = EIO;
    return EOF;
  }
  return 0;
}

void __so_stdio_flush_all(void) {
  FILE* f;
  for (f = io.open_list; f; f = f->next)
    if (f->flags & F_WRITE) mem_save(f);
  __so_stdio_flush_out();
}

/* O buffer passa a ser do FILE. fclose libera. */
FILE* fopen_mem(char* data, int size) {
  FILE* f;
  if (!data || size < 0) return NULL;
  f = (FILE*)calloc(1, sizeof(FILE));
  if (!f) return NULL;
  f->kind = K_MEM;
  f->flags = F_READ;
  f->buf = data;
  f->size = (unsigned)size;
  f->cap = (unsigned)size;
  f->unget = -1;
  f->fd = -1;
  f->next = io.open_list;
  io.open_list = f;
  return f;
}

/* Caminho no estilo Unix de programa portado: "/D/SODOS/X.TTF" vira
 * "D:\\SODOS\\X.TTF", como na posix-lite. */
static void dos_path(const char* path, char* out) {
  int i = 0, j = 0;
  if (path[0] == '/' && ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')) &&
      (path[2] == '/' || path[2] == 0)) {
    out[j++] = (char)unix_drive(path[1]);
    out[j++] = ':';
    if (!path[2]) out[j++] = '\\';
    i = 2;
  }
  for (; path[i] && j < FILENAME_MAX - 1; i++) out[j++] = path[i] == '/' ? '\\' : path[i];
  out[j] = 0;
}

#if __SIZEOF_POINTER__ == 8
/* open/read/close sao da posix-lite, que nem todo programa liga (HELLO64C,
 * testes da libc): referencias fracas, e sem ela o fopen do /dev/urandom so
 * falha com EIO em vez de quebrar o link. */
#pragma weak open
#pragma weak read
#pragma weak close
/* "/dev/urandom" e "/dev/random" nao existem no SODOS. No LP64 o fopen cria o
 * FILE sobre um fd da posix-lite, que gera bytes pseudo-aleatorios em cada
 * read: e assim que o NSS (fopen + fileno + read) semeia o DRBG. */
static int dev_random_path(const char* p) {
  char low[32];
  int i = 0;
  if (!p) return 0;
  if (p[0] && p[1] == ':') p += 2;
  while (*p == '\\' || *p == '/') p++;
  for (; p[i] && i < (int)sizeof(low) - 1; i++) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == '/') c = '\\';
    low[i] = c;
  }
  low[i] = 0;
  return strcmp(low, "dev\\urandom") == 0 || strcmp(low, "dev\\random") == 0;
}
#endif

FILE* fopen(const char* upath, const char* mode) {
  FILE* f;
  int fl = 0;
  int load = 0;
  char path[FILENAME_MAX];
  if (upath && strlen(upath) < FILENAME_MAX) dos_path(upath, path);
  else upath = NULL;
  if (!upath || !mode) {
    errno = EINVAL;
    return NULL;
  }
  if (mode[0] == 'r') {
    fl = F_READ;
    load = 1;
  } else if (mode[0] == 'w') {
    fl = F_WRITE | F_DIRTY;
  } else if (mode[0] == 'a') {
    fl = F_WRITE | F_APPEND | F_DIRTY;
    load = 2;
  } else {
    errno = EINVAL;
    return NULL;
  }
  if (strchr(mode, '+')) fl |= F_READ | F_WRITE;
  if (strlen(path) >= FILENAME_MAX) {
    errno = EINVAL;
    return NULL;
  }
#if __SIZEOF_POINTER__ == 8
  if (dev_random_path(path)) {
    int rfd = open ? open("/dev/urandom", O_RDONLY) : -1;
    if (rfd < 0) {
      errno = EIO;
      return NULL;
    }
    f = (FILE*)calloc(1, sizeof(FILE));
    if (!f) {
      close(rfd);
      errno = ENOMEM;
      return NULL;
    }
    f->kind = K_RANDOM;
    f->flags = F_READ;
    f->fd = rfd;
    f->unget = -1;
    f->next = io.open_list;
    io.open_list = f;
    return f;
  }
#endif
  f = (FILE*)calloc(1, sizeof(FILE));
  if (!f) return NULL;
  f->kind = K_MEM;
  f->flags = fl;
  f->unget = -1;
  f->fd = -1;
  strcpy(f->path, path);
  if (load) {
    int sz = k_fsize(path);
    if (sz < 0) {
      if (load == 1) {
        free(f);
        errno = ENOENT;
        return NULL;
      }
      sz = 0;
    }
    f->cap = (unsigned)sz + 1;
    f->buf = (char*)malloc(f->cap);
    if (!f->buf) {
      free(f);
      errno = ENOMEM;
      return NULL;
    }
    if (sz > 0 && k_fload(path, f->buf, (int)f->cap) != sz) {
      free(f->buf);
      free(f);
      errno = EIO;
      return NULL;
    }
    f->size = (unsigned)sz;
    if (load == 2) f->pos = f->size;
  }
  f->next = io.open_list;
  io.open_list = f;
  return f;
}

int fclose(FILE* f) {
  FILE** pp;
  int rc = 0;
  if (!f) return EOF;
#if __SIZEOF_POINTER__ == 8
  if (f->kind == K_RANDOM) {
    close(f->fd);
    for (pp = &io.open_list; *pp; pp = &(*pp)->next) {
      if (*pp == f) {
        *pp = f->next;
        break;
      }
    }
    free(f);
    return 0;
  }
#endif
  if (f->kind != K_MEM) {
    fflush(f);
    return 0;
  }
  if (f->flags & F_WRITE) rc = mem_save(f);
  for (pp = &io.open_list; *pp; pp = &(*pp)->next) {
    if (*pp == f) {
      *pp = f->next;
      break;
    }
  }
  free(f->buf);
  free(f);
  return rc;
}

int fflush(FILE* f) {
  if (!f) {
    __so_stdio_flush_all();
    return 0;
  }
  if (f->kind == K_STDOUT || f->kind == K_STDERR) {
    __so_stdio_flush_out();
    return 0;
  }
  if (f->kind == K_MEM && (f->flags & F_WRITE)) return mem_save(f);
  return 0;
}

static int grow(FILE* f, unsigned need) {
  unsigned nc;
  char* nb;
  if (need <= f->cap) return 0;
  nc = f->cap ? f->cap : 256;
  while (nc < need) nc *= 2;
  nb = (char*)realloc(f->buf, nc);
  if (!nb) {
    f->err = 1;
    errno = ENOMEM;
    return -1;
  }
  f->buf = nb;
  f->cap = nc;
  return 0;
}

static int fill_stdin(void) {
  int n;
  __so_stdio_flush_out();
  n = k_readline(io.inbuf, (int)sizeof(io.inbuf) - 2);
  if (n < 0 || (n == 1 && io.inbuf[0] == 26)) return -1;
  io.inbuf[n++] = '\n';
  io.inpos = 0;
  io.inlen = n;
  return 0;
}

int fgetc(FILE* f) {
  if (!f) return EOF;
  if (f->unget >= 0) {
    int c = f->unget;
    f->unget = -1;
    return c;
  }
  if (f->kind == K_STDIN) {
    if (io.inpos >= io.inlen && fill_stdin() < 0) {
      f->eof = 1;
      return EOF;
    }
    return (unsigned char)io.inbuf[io.inpos++];
  }
#if __SIZEOF_POINTER__ == 8
  if (f->kind == K_RANDOM) {
    unsigned char c;
    if (read(f->fd, &c, 1) != 1) {
      f->eof = 1;
      return EOF;
    }
    return c;
  }
#endif
  if (f->kind != K_MEM || !(f->flags & F_READ)) {
    f->err = 1;
    return EOF;
  }
  if (f->pos >= f->size) {
    f->eof = 1;
    return EOF;
  }
  return (unsigned char)f->buf[f->pos++];
}

int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE* f) {
  if (!f || c == EOF) return EOF;
  f->unget = c & 255;
  f->eof = 0;
  return c;
}

char* fgets(char* s, int n, FILE* f) {
  int i = 0;
  if (n <= 0) return NULL;
  while (i < n - 1) {
    int c = fgetc(f);
    if (c == EOF) break;
    s[i++] = (char)c;
    if (c == '\n') break;
  }
  if (i == 0) return NULL;
  s[i] = 0;
  return s;
}

size_t fread(void* buf, size_t sz, size_t n, FILE* f) {
  size_t total = sz * n;
  size_t got = 0;
  char* d = (char*)buf;
  if (!f || total == 0) return 0;
#if __SIZEOF_POINTER__ == 8
  if (f->kind == K_RANDOM) {
    ssize_t n = read(f->fd, d, total);
    if (n <= 0) {
      f->eof = 1;
      return 0;
    }
    return (size_t)n / sz;
  }
#endif
  if (f->kind == K_MEM && f->unget < 0) {
    unsigned avail;
    if (!(f->flags & F_READ)) {
      f->err = 1;
      return 0;
    }
    avail = f->pos < f->size ? f->size - f->pos : 0;
    got = total < avail ? total : avail;
    memcpy(d, f->buf + f->pos, got);
    f->pos += got;
    if (got < total) f->eof = 1;
    return got / sz;
  }
  while (got < total) {
    int c = fgetc(f);
    if (c == EOF) break;
    d[got++] = (char)c;
  }
  return got / sz;
}

static void out_char(char c) {
  if (olen >= (int)sizeof(obuf)) __so_stdio_flush_out();
  obuf[olen++] = c;
  if (c == '\n') __so_stdio_flush_out();
}

size_t fwrite(const void* buf, size_t sz, size_t n, FILE* f) {
  size_t total = sz * n;
  const char* s = (const char*)buf;
  size_t i;
  if (!f || total == 0) return 0;
  if (f->kind == K_STDOUT) {
    for (i = 0; i < total; i++) out_char(s[i]);
    return n;
  }
  if (f->kind == K_STDERR) {
    __so_stdio_flush_out();
    ser_write(s, (int)total);
    k_write(s, (int)total);
    return n;
  }
  if (f->kind != K_MEM || !(f->flags & F_WRITE)) {
    f->err = 1;
    return 0;
  }
  if (f->flags & F_APPEND) f->pos = f->size;
  if (grow(f, f->pos + total) < 0) return 0;
  if (f->pos > f->size) memset(f->buf + f->size, 0, f->pos - f->size);
  memcpy(f->buf + f->pos, s, total);
  f->pos += total;
  if (f->pos > f->size) f->size = f->pos;
  f->flags |= F_DIRTY;
  return n;
}

int fputc(int c, FILE* f) {
  char ch = (char)c;
  if (f && f->kind == K_STDOUT) {
    out_char(ch);
    return c & 255;
  }
  return fwrite(&ch, 1, 1, f) == 1 ? (c & 255) : EOF;
}

int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char* s, FILE* f) {
  size_t n = strlen(s);
  return fwrite(s, 1, n, f) == n || n == 0 ? 0 : EOF;
}

int puts(const char* s) {
  if (fputs(s, stdout) == EOF) return EOF;
  return putchar('\n') == EOF ? EOF : 0;
}

int fseek(FILE* f, long off, int whence) {
  long base;
  if (!f || f->kind != K_MEM) return -1;
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = (long)f->pos;
  else if (whence == SEEK_END) base = (long)f->size;
  else return -1;
  if (base + off < 0) return -1;
  f->pos = (unsigned)(base + off);
  f->eof = 0;
  f->unget = -1;
  return 0;
}

long ftell(FILE* f) {
  if (!f || f->kind != K_MEM) return -1;
  return (long)f->pos - (f->unget >= 0 ? 1 : 0);
}

void rewind(FILE* f) {
  fseek(f, 0, SEEK_SET);
  if (f) f->err = 0;
}

int feof(FILE* f) { return f ? f->eof : 1; }
int ferror(FILE* f) { return f ? f->err : 1; }

void clearerr(FILE* f) {
  if (f) {
    f->eof = 0;
    f->err = 0;
  }
}

int remove(const char* path) {
  int r;
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  r = k_fdelete(path);
  if (r == -3) r = k_rmdir(path);
  if (r < 0) {
    errno = r == -1 ? ENOENT : r == -2 ? EINVAL : EACCES;
    return -1;
  }
  return 0;
}

/* k_frename troca so a entrada de diretorio (mesmo disco FAT). Entre
 * unidades, ou numa unidade sem rename, copia o arquivo inteiro e apaga o
 * antigo. */
int rename(const char* ufrom, const char* uto) {
  int sz, r;
  char* buf;
  char from[FILENAME_MAX];
  char to[FILENAME_MAX];
  if (!ufrom || !uto || strlen(ufrom) >= FILENAME_MAX || strlen(uto) >= FILENAME_MAX) {
    errno = EINVAL;
    return -1;
  }
  dos_path(ufrom, from);
  dos_path(uto, to);
  r = k_frename(from, to);
  if (r == 0) return 0;
  if (r != -5 && r != -6 && r != -7) {
    errno = r == -1 ? ENOENT : r == -3 ? EISDIR : r == -2 ? EINVAL : EACCES;
    return -1;
  }
  sz = k_fsize(from);
  if (sz < 0) {
    errno = ENOENT;
    return -1;
  }
  buf = (char*)malloc((size_t)sz + 1);
  if (!buf) {
    errno = ENOMEM;
    return -1;
  }
  if (k_fload(from, buf, sz + 1) != sz || k_fsave(to, buf, sz) < 0) {
    free(buf);
    errno = EIO;
    return -1;
  }
  free(buf);
  if (k_fdelete(from) < 0) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

char* tmpnam(char* s) {
  static char name[L_tmpnam];
  static unsigned seq;
  char* out = s ? s : name;
  do {
    seq++;
    sprintf(out, "TMP%05u.TMP", (k_ms() + seq * 7919u) % 100000u);
  } while (k_fsize(out) >= 0);
  return out;
}

FILE* tmpfile(void) {
  char name[L_tmpnam];
  return fopen(tmpnam(name), "w+");
}

void perror(const char* s) {
  if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
  else fprintf(stderr, "%s\n", strerror(errno));
}

/* ---------- funcoes que a libc++ espera do <cstdio> ---------- */

int setvbuf(FILE* f, char* buf, int mode, size_t size) {
  (void)f;
  (void)buf;
  (void)mode;
  (void)size;
  return 0; /* o SODOS nao tem buffer de stdio configuravel */
}

void setbuf(FILE* f, char* buf) { (void)f; (void)buf; }

FILE* freopen(const char* path, const char* mode, FILE* f) {
  if (f) fclose(f);
  return fopen(path, mode);
}

int fgetpos(FILE* f, fpos_t* pos) {
  long p;
  if (!pos) return -1;
  p = ftell(f);
  if (p < 0) return -1;
  *pos = (fpos_t)p;
  return 0;
}

int fsetpos(FILE* f, const fpos_t* pos) {
  if (!pos) return -1;
  return fseek(f, (long)*pos, SEEK_SET);
}

FILE* fdopen(int fd, const char* mode) { (void)fd; (void)mode; errno = EINVAL; return NULL; }
int fseeko(FILE* f, off_t off, int whence) { return fseek(f, (long)off, whence); }
off_t ftello(FILE* f) { return (off_t)ftell(f); }

int fileno(FILE* f) {
  if (!f) return -1;
  if (f->kind == K_STDIN) return 0;
  if (f->kind == K_STDOUT) return 1;
  if (f->kind == K_STDERR) return 2;
#if __SIZEOF_POINTER__ == 8
  if (f->kind == K_RANDOM) return f->fd;
#endif
  return -1;
}
