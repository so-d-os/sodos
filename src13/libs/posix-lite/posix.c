/* posix-lite - subset POSIX sobre k_* (Fase 1 do port MC).
 * Compila como POSIX.DLL pelo cc do SODOS. Arquivos abertos com open()
 * ficam inteiros na memoria e sao gravados no close/fsync. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <so.h>
#include "../../c_compiler/lib/unixdrv.h"

#define FD_MAX 64
#define DIR_MAX 8
#undef PATH_MAX
#define PATH_MAX 260
#define LIST_MAX 16384
#define FILE_CAP (64 * 1024 * 1024)

/* Um arquivo aberto: os bytes ficam na memoria e o fsync/close grava o
 * arquivo inteiro (k_fsave). Todos os fds do mesmo caminho dividem o mesmo
 * objeto. Antes cada open tinha a sua copia: o SQLite abre o mesmo banco em
 * varias conexoes (places, favicons, leitura assincrona), cada uma gravava a
 * sua copia por cima da outra e o banco ficava "malformed". */
struct fobj {
  int refs;          /* fds que apontam para ele; 0 = livre */
  int size;
  int dirty;
  int gone;          /* unlink com o arquivo aberto: nao grava mais */
  char path[PATH_MAX];
  char* data;
};

struct fd_ent {
  int used;
  int flags;
  int pos;
  int rnd; /* /dev/urandom: cada read devolve bytes pseudo-aleatorios */
  struct fobj* f;
};

struct __so_DIR {
  int used;
  int pos;
  char list[LIST_MAX];
  struct dirent cur;
};

static struct fd_ent fds[FD_MAX];
static struct fobj fobjs[FD_MAX];

static int path_eq(const char* a, const char* b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x == '/') x = '\\';
    if (y == '/') y = '\\';
    if (x != y) return 0;
    if (!x) return 1;
  }
}

/* Objeto ja aberto para este caminho (que nao foi apagado), ou 0. */
static struct fobj* fobj_find(const char* full) {
  int i;
  for (i = 0; i < FD_MAX; i++)
    if (fobjs[i].refs > 0 && !fobjs[i].gone && path_eq(fobjs[i].path, full)) return &fobjs[i];
  return 0;
}

static struct fobj* fobj_new(const char* full) {
  int i;
  for (i = 0; i < FD_MAX; i++) {
    if (fobjs[i].refs == 0) {
      memset(&fobjs[i], 0, sizeof(fobjs[i]));
      strncpy(fobjs[i].path, full, PATH_MAX - 1);
      return &fobjs[i];
    }
  }
  return 0;
}

static int fobj_flush(struct fobj* o) {
  if (!o->dirty || o->gone) return 0;
  if (k_fsave(o->path, o->data ? o->data : "", o->size) < 0) return -1;
  o->dirty = 0;
  return 0;
}

static void fobj_release(struct fobj* o) {
  if (--o->refs > 0) return;
  free(o->data);
  o->data = 0;
  o->refs = 0;
}

/* Garante espaco para need bytes (+ NUL). */
static int fobj_grow(struct fobj* o, int need) {
  char* nb;
  if (need > FILE_CAP) return -1;
  if (o->data && need <= o->size) return 0;
  nb = (char*)realloc(o->data, (size_t)need + 1);
  if (!nb) return -1;
  o->data = nb;
  return 0;
}
static struct __so_DIR dirs[DIR_MAX];

static int is_abs_dos(const char* p) {
  return p && p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/' || p[2] == 0);
}

static void norm_sep(char* s) {
  for (; *s; s++)
    if (*s == '/') *s = '\\';
}

static void join_cwd(const char* path, char* out, int max) {
  char cwd[PATH_MAX];
  int i = 0, j = 0;
  if (!path) path = "";
  if (path[0] == '/' && ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')) &&
      (path[2] == '/' || path[2] == 0)) {
    out[j++] = (char)unix_drive(path[1]);
    out[j++] = ':';
    i = 2;
    if (!path[2]) out[j++] = '\\';
    while (path[i] && j < max - 1) out[j++] = path[i++];
    out[j] = 0;
    norm_sep(out);
    return;
  }
  if (is_abs_dos(path) || path[0] == '\\' || path[0] == '/') {
    while (path[i] && j < max - 1) out[j++] = path[i++];
    out[j] = 0;
    norm_sep(out);
    return;
  }
  k_pwd(cwd, (int)sizeof(cwd));
  while (cwd[i] && j < max - 1) out[j++] = cwd[i++];
  if (j > 0 && out[j - 1] != '\\' && j < max - 1) out[j++] = '\\';
  i = 0;
  while (path[i] && j < max - 1) out[j++] = path[i++];
  out[j] = 0;
  norm_sep(out);
}

/* "/dev/urandom" e "/dev/random": nao existem no SODOS; criamos um fd
 * sintetico que devolve bytes pseudo-aleatorios em cada read. */
static int is_dev_random(const char* full) {
  const char* p = full;
  char low[PATH_MAX];
  int i;
  if (!p) return 0;
  if (p[0] && p[1] == ':') p += 2;
  while (*p == '\\' || *p == '/') p++;
  for (i = 0; p[i] && i < PATH_MAX - 1; i++) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == '/') c = '\\';
    low[i] = c;
  }
  low[i] = 0;
  return strcmp(low, "dev\\urandom") == 0 || strcmp(low, "dev\\random") == 0;
}

static unsigned long long rng_state;

static unsigned long long rng_next(void) {
  if (!rng_state) {
    rng_state = 0x9E3779B97F4A7C15ull ^ (unsigned long long)k_ms();
    rng_state ^= (unsigned long long)(unsigned int)k_time() * 0x2545F4914F6CDD1Dull;
    rng_state ^= (unsigned long long)(unsigned int)(size_t)&rng_state << 17;
    if (!rng_state) rng_state = 0x123456789ABCDEFull;
  }
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1Dull;
}

static void rng_fill(void* buf, size_t n) {
  unsigned char* p = (unsigned char*)buf;
  size_t i = 0;
  while (i < n) {
    unsigned long long v = rng_next();
    int k;
    for (k = 0; k < 8 && i < n; k++, i++) p[i] = (unsigned char)(v >> (8 * k));
  }
}

static int path_is_dir(const char* path) {
  char list[8];
  int n;
  if (!path || !path[0]) return 0;
  n = (int)strlen(path);
  if (n >= 2 && path[n - 1] == '\\') return 1;
  if (is_abs_dos(path) && path[2] == 0) return 1;
  n = k_dirlist(path, list, (int)sizeof(list));
  return n >= 0;
}

__export int chdir(const char* path) {
  char full[PATH_MAX];
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  join_cwd(path, full, PATH_MAX);
  if (k_chdir(full) != 0) {
    errno = ENOENT;
    return -1;
  }
  errno = 0;
  return 0;
}

__export char* getcwd(char* buf, size_t size) {
  char tmp[64];
  if (!buf || size < 2) {
    errno = EINVAL;
    return 0;
  }
  k_pwd(tmp, (int)sizeof(tmp));
  if (strlen(tmp) >= size) {
    errno = ERANGE;
    return 0;
  }
  strcpy(buf, tmp);
  return buf;
}

__export int isatty(int fd) {
  return fd == 0 || fd == 1 || fd == 2;
}

static int err_from_k(int r) {
  if (r == -1) return ENOENT;
  if (r == -2) return EINVAL;
  if (r == -3) return EISDIR;
  return EACCES;
}

__export int unlink(const char* path) {
  char full[PATH_MAX];
  int r;
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  join_cwd(path, full, PATH_MAX);
  r = k_fdelete(full);
  {
    /* Aberto em algum fd: continua legivel por ele, mas nao volta ao disco. */
    struct fobj* o = fobj_find(full);
    if (o) {
      o->gone = 1;
      o->dirty = 0;
      if (r < 0) r = 0;
    }
  }
  if (r < 0) {
    errno = err_from_k(r);
    return -1;
  }
  return 0;
}

__export int rmdir(const char* path) {
  char full[PATH_MAX];
  int r;
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  join_cwd(path, full, PATH_MAX);
  r = k_rmdir(full);
  if (r == -2) errno = EEXIST; /* ENOTEMPTY */
  else if (r == -3) errno = ENOTDIR;
  else if (r < 0) errno = ENOENT;
  return r < 0 ? -1 : 0;
}

__export int mkdir(const char* path, mode_t mode) {
  char full[PATH_MAX];
  int r;
  (void)mode;
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  join_cwd(path, full, PATH_MAX);
  r = k_mkdir(full);
  if (r == 1) {
    errno = EEXIST;
    return -1;
  }
  if (r < 0) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

__export int access(const char* path, int mode) {
  struct stat st;
  (void)mode;
  return stat(path, &st);
}

__export pid_t getpid(void) { return 1; }
__export pid_t getppid(void) { return 0; }
__export uid_t getuid(void) { return 0; }
__export uid_t geteuid(void) { return 0; }
__export gid_t getgid(void) { return 0; }
__export gid_t getegid(void) { return 0; }

#if defined(__GNUC__) && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
extern void sodos_sleep_ms(unsigned ms) __attribute__((weak));
#endif

__export int usleep(unsigned us) {
  unsigned start = k_ms(), ms = us / 1000;
#if defined(__GNUC__) && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
  /* LP64: a espera ocupada prenderia as threads cooperativas da libsodos. */
  if (sodos_sleep_ms) {
    sodos_sleep_ms(ms);
    return 0;
  }
#endif
  while (k_ms() - start < ms) {}
  return 0;
}

__export unsigned sleep(unsigned s) {
  usleep(s * 1000000u);
  return 0;
}

__export long sysconf(int name) {
  if (name == _SC_PAGESIZE) return 4096;
  if (name == _SC_CLK_TCK) return 1000;
  if (name == _SC_OPEN_MAX) return FD_MAX;
  errno = EINVAL;
  return -1;
}

/* data/hora FAT da entrada -> time_t (0 se nao tiver) */
static time_t file_mtime(const char* full) {
  static struct k_dirent ents[256];
  char dir[PATH_MAX];
  const char* name;
  int n, i, k;
  strncpy(dir, full, PATH_MAX - 1);
  dir[PATH_MAX - 1] = 0;
  k = (int)strlen(dir);
  while (k > 0 && dir[k - 1] == '\\') dir[--k] = 0;
  while (k > 0 && dir[k - 1] != '\\' && dir[k - 1] != ':') k--;
  name = full + k;
  if (!*name) return 0;
  dir[k] = 0;
  if (k == 0) k_pwd(dir, PATH_MAX);
  n = k_dir(dir, ents, 256);
  for (i = 0; i < n; i++) {
    if (strcasecmp(ents[i].name, name) == 0) {
      struct tm tm;
      unsigned d = ents[i].date, t = ents[i].time;
      if (!d) return 0;
      memset(&tm, 0, sizeof(tm));
      tm.tm_mday = (int)(d & 31);
      tm.tm_mon = (int)((d >> 5) & 15) - 1;
      tm.tm_year = (int)(d >> 9) + 80;
      tm.tm_hour = (int)(t >> 11);
      tm.tm_min = (int)((t >> 5) & 63);
      tm.tm_sec = (int)(t & 31) * 2;
      return mktime(&tm);
    }
  }
  return 0;
}

/* Numero de inode a partir do caminho (sem diferenciar maiusculas): a FAT nao
 * tem inode, e quem compara st_dev/st_ino para saber se dois descritores sao o
 * mesmo arquivo (as travas do SQLite) nao pode ver zero em todos. */
static unsigned long path_ino(const char* full) {
  unsigned long h = 1469598103u;
  for (; *full; full++) {
    char c = *full;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    h = (h ^ (unsigned char)c) * 16777619u;
  }
  return h ? h : 1;
}

static int fd_alloc(void) {
  int i;
  for (i = 3; i < FD_MAX; i++)
    if (!fds[i].used) return i;
  return -1;
}

__export int open(const char* path, int flags, ...) {
  char full[PATH_MAX];
  int fd, sz, mode = 0;
  struct fd_ent* e;
  struct fobj* o;
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    (void)mode;
  }
  join_cwd(path, full, PATH_MAX);
  if (is_dev_random(full)) {
    fd = fd_alloc();
    if (fd < 0) {
      errno = ENOMEM;
      return -1;
    }
    e = &fds[fd];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->flags = flags;
    e->rnd = 1;
    errno = 0;
    return fd;
  }
  o = fobj_find(full);
  if (o) {
    /* Ja aberto: o mesmo conteudo (inclusive o que ainda nao foi gravado). */
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
      errno = EEXIST;
      return -1;
    }
    fd = fd_alloc();
    if (fd < 0) {
      errno = ENOMEM;
      return -1;
    }
    e = &fds[fd];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->flags = flags;
    e->f = o;
    o->refs++;
    if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR))) {
      o->size = 0;
      if (o->data) o->data[0] = 0;
      o->dirty = 1;
    }
    e->pos = (flags & O_APPEND) ? o->size : 0;
    errno = 0;
    return fd;
  }
  if (path_is_dir(full)) {
    errno = EISDIR;
    return -1;
  }
  sz = k_fsize(full);
  if (sz >= 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
    errno = EEXIST;
    return -1;
  }
  if (sz < 0) {
    if (!(flags & O_CREAT)) {
      errno = ENOENT;
      return -1;
    }
    if (k_fsave(full, "", 0) < 0) {
      errno = EIO;
      return -1;
    }
    sz = 0;
  }
  if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR))) sz = 0;
  if (sz > FILE_CAP) {
    errno = EIO;
    return -1;
  }
  fd = fd_alloc();
  o = fobj_new(full);
  if (fd < 0 || !o) {
    errno = ENOMEM;
    return -1;
  }
  o->data = (char*)malloc((size_t)sz + 1);
  if (!o->data) {
    errno = ENOMEM;
    return -1;
  }
  if (sz > 0 && k_fload(full, o->data, sz + 1) != sz) {
    free(o->data);
    o->data = 0;
    errno = EIO;
    return -1;
  }
  o->data[sz] = 0;
  o->size = sz;
  o->dirty = (flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR));
  o->refs = 1;
  e = &fds[fd];
  memset(e, 0, sizeof(*e));
  e->used = 1;
  e->flags = flags;
  e->f = o;
  e->pos = (flags & O_APPEND) ? sz : 0;
  errno = 0;
  return fd;
}

__export int creat(const char* path, mode_t mode) {
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)mode);
}

#if __SIZEOF_POINTER__ == 8
/* sodos_pipe.c: pipes e socketpair em memoria, com descritores a partir de 2000. */
extern int sodos_pipe_close(int fd);
extern ssize_t sodos_pipe_read(int fd, void* buf, size_t n);
extern ssize_t sodos_pipe_write(int fd, const void* buf, size_t n);
#define PIPE_FD(fd) ((fd) >= 2000)
/* sodos_shm.c: memoria compartilhada (shm_open), descritores a partir de 3000. */
extern int sodos_shm_ftruncate(int fd, off_t len);
extern int sodos_shm_fstat(int fd, struct stat* st);
extern off_t sodos_shm_lseek(int fd, off_t off, int whence);
extern int sodos_shm_dup(int fd);
#define SHM_FD(fd) ((fd) >= 3000)
#else
#define PIPE_FD(fd) 0
#define SHM_FD(fd) 0
#define sodos_shm_ftruncate(fd, len) (-1)
#define sodos_shm_fstat(fd, st) (-1)
#define sodos_shm_lseek(fd, off, whence) (-1)
#define sodos_shm_dup(fd) (-1)
#define sodos_pipe_close(fd) (-1)
#define sodos_pipe_read(fd, buf, n) (-1)
#define sodos_pipe_write(fd, buf, n) (-1)
#endif

__export int close(int fd) {
  struct fd_ent* e;
  int r = 0;
  if (PIPE_FD(fd)) return sodos_pipe_close(fd);
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (e->f) {
    /* O ultimo a fechar grava; um fd que escreveu grava ja (como antes). */
    if ((e->flags & (O_WRONLY | O_RDWR)) || e->f->refs == 1) r = fobj_flush(e->f);
    fobj_release(e->f);
  }
  e->used = 0;
  e->f = 0;
  if (r < 0) {
    errno = EIO;
    return -1;
  }
  errno = 0;
  return 0;
}

/* Teclado em modo raw (tcsetattr sem ICANON, c_compiler/lib/misc.c): teclas
 * que o poll ja tirou do kernel esperam aqui o read(0). Converte para o que
 * um terminal manda: Enter = CR, Backspace = DEL e setas = ESC [ A..D. */
extern int __so_stdin_raw;
#if defined(__GNUC__) && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
extern int sched_yield(void) __attribute__((weak));
#endif
static unsigned char kin[64];
static int kin_h, kin_t;

static void kin_put(int c) {
  if (((kin_h + 1) & 63) != kin_t) {
    kin[kin_h] = (unsigned char)c;
    kin_h = (kin_h + 1) & 63;
  }
}

static void kin_seq(const char* s) {
  while (*s) kin_put(*s++);
}

/* Le o que houver no teclado, sem esperar. Devolve quantos bytes ha. */
__export int sodos_stdin_ready(void) {
  int c;
  while (((kin_h + 8) & 63) != kin_t && (c = k_trygetc()) > 0) {
    switch (c) {
      case '\n': kin_put('\r'); break;
      case '\b': kin_put(0x7f); break;
      case 0x100: kin_seq("\033[A"); break;  /* KEY_UP */
      case 0x101: kin_seq("\033[B"); break;  /* KEY_DOWN */
      case 0x102: kin_seq("\033[D"); break;  /* KEY_LEFT */
      case 0x103: kin_seq("\033[C"); break;  /* KEY_RIGHT */
      default:
        if (c < 0x100) kin_put(c);
    }
  }
  return (kin_h - kin_t) & 63;
}

__export ssize_t read(int fd, void* buf, size_t n) {
  if (PIPE_FD(fd)) return sodos_pipe_read(fd, buf, n);
  struct fd_ent* e;
  size_t left;
  if (fd == 0 && __so_stdin_raw) {
    size_t got = 0;
    /* LP64: as threads sao cooperativas; ceder so ao kernel as prenderia. */
    while (!sodos_stdin_ready()) {
#if defined(__GNUC__) && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
      if (sched_yield) sched_yield(); else
#endif
      k_yield();
    }
    while (got < n && kin_t != kin_h) {
      ((char*)buf)[got++] = (char)kin[kin_t];
      kin_t = (kin_t + 1) & 63;
    }
    return (ssize_t)got;
  }
  if (fd == 0) {
    /* stdin da libc: linha a linha */
    size_t got = 0;
    while (got < n) {
      int c = fgetc(stdin);
      if (c == EOF) break;
      ((char*)buf)[got++] = (char)c;
      if (c == '\n') break;
    }
    return (ssize_t)got;
  }
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (e->rnd) {
    rng_fill(buf, n);
    e->pos += (int)n;
    return (ssize_t)n;
  }
  if (!e->f || e->pos >= e->f->size) return 0;
  left = (size_t)(e->f->size - e->pos);
  if (n > left) n = left;
  memcpy(buf, e->f->data + e->pos, n);
  e->pos += (int)n;
  return (ssize_t)n;
}

/* Em modo raw (programa de terminal, ex.: QEMU com -serial stdio) a saida
 * traz sequencias ANSI que o console do SODOS nao interpreta. Tira as
 * sequencias ESC [ ... e ESC x; "ESC [ 2 J" limpa a tela. */
static int ansi_st;          /* 0 texto, 1 depois do ESC, 2 dentro do CSI */
static char ansi_arg[16];
static int ansi_n;

static void con_write_ansi(const char* s, size_t n) {
  size_t i, run = 0;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (ansi_st == 0) {
      if (c != 27) {
        run++;
        continue;
      }
      if (run) k_write(s + i - run, (int)run);
      run = 0;
      ansi_st = 1;
    } else if (ansi_st == 1) {
      ansi_st = c == '[' ? 2 : 0;
      ansi_n = 0;
    } else if (c >= 0x40 && c <= 0x7e) {
      if (c == 'J' && ansi_n == 1 && ansi_arg[0] == '2') k_clear();
      ansi_st = 0;
    } else if (ansi_n < (int)sizeof(ansi_arg)) {
      ansi_arg[ansi_n++] = (char)c;
    }
  }
  if (run) k_write(s + n - run, (int)run);
}

__export ssize_t write(int fd, const void* buf, size_t n) {
  if (PIPE_FD(fd)) return sodos_pipe_write(fd, buf, n);
  struct fd_ent* e;
  struct fobj* o;
  int need;
  if (fd == 1 || fd == 2) {
    if (__so_stdin_raw) con_write_ansi((const char*)buf, n);
    else k_write((const char*)buf, (int)n);
    return (ssize_t)n;
  }
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (!(e->flags & (O_WRONLY | O_RDWR))) {
    errno = EACCES;
    return -1;
  }
  o = e->f;
  if (!o) {
    errno = EBADF;
    return -1;
  }
  if (e->flags & O_APPEND) e->pos = o->size;
  need = e->pos + (int)n;
  if (need > FILE_CAP) {
    errno = EIO;
    return -1;
  }
  if (fobj_grow(o, need) < 0) {
    errno = ENOMEM;
    return -1;
  }
  /* escrever alem do fim deixa um buraco de zeros, como no Unix */
  if (e->pos > o->size) memset(o->data + o->size, 0, (size_t)(e->pos - o->size));
  memcpy(o->data + e->pos, buf, n);
  e->pos += (int)n;
  if (e->pos > o->size) o->size = e->pos;
  o->data[o->size] = 0;
  o->dirty = 1;
  return (ssize_t)n;
}

__export off_t lseek(int fd, off_t off, int whence) {
  struct fd_ent* e;
  int np;
  if (SHM_FD(fd)) return sodos_shm_lseek(fd, off, whence);
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (whence == SEEK_SET) np = off;
  else if (whence == SEEK_CUR) np = e->pos + off;
  else if (whence == SEEK_END) np = (e->f ? e->f->size : 0) + off;
  else {
    errno = EINVAL;
    return -1;
  }
  if (np < 0) {
    errno = EINVAL;
    return -1;
  }
  e->pos = np;
  return e->pos;
}

/* grava o que mudou sem fechar */
__export int fsync(int fd) {
  struct fd_ent* e;
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return fd >= 0 && fd < 3 ? 0 : -1;
  }
  e = &fds[fd];
  /* Z: e o ramdisk: gravar no fsync so copiaria o arquivo inteiro de uma RAM
   * para outra. O close grava (e um processo que cai perde so o ramdisk). */
  if (e->f && (e->f->path[0] == 'Z' || e->f->path[0] == 'z') && e->f->path[1] == ':') return 0;
  if (e->f && fobj_flush(e->f) < 0) {
    errno = EIO;
    return -1;
  }
  return 0;
}

__export int ftruncate(int fd, off_t len) {
  struct fd_ent* e;
  struct fobj* o;
  if (SHM_FD(fd)) return sodos_shm_ftruncate(fd, len);
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used || len < 0) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  o = e->f;
  if (!o || !(e->flags & (O_WRONLY | O_RDWR))) {
    errno = EACCES;
    return -1;
  }
  if (len > o->size) {
    if (fobj_grow(o, (int)len) < 0) {
      errno = len > FILE_CAP ? EIO : ENOMEM;
      return -1;
    }
    memset(o->data + o->size, 0, (size_t)(len - o->size));
  }
  o->size = (int)len;
  o->data[len] = 0;
  o->dirty = 1;
  return 0;
}

__export int dup(int fd) {
  int nfd;
  struct fd_ent* e;
  if (SHM_FD(fd)) return sodos_shm_dup(fd);
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  /* mesmo arquivo (mesmo conteudo); a posicao continua separada */
  nfd = fd_alloc();
  if (nfd < 0) {
    errno = ENOMEM;
    return -1;
  }
  e = &fds[nfd];
  *e = fds[fd];
  if (e->f) e->f->refs++;
  return nfd;
}

__export int fstat(int fd, struct stat* st) {
  struct fd_ent* e;
  if (SHM_FD(fd)) return sodos_shm_fstat(fd, st);
  if (!st) {
    errno = EINVAL;
    return -1;
  }
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
  st->st_size = e->f ? e->f->size : 0;
  st->st_dev = 1;
  st->st_ino = e->f ? path_ino(e->f->path) : (unsigned long)(fd + 1);
  st->st_mtime = e->f && !e->f->gone ? file_mtime(e->f->path) : 0;
  st->st_atime = st->st_mtime;
  st->st_ctime = st->st_mtime;
#if __SIZEOF_POINTER__ == 8
  st->st_nlink = 1;
  st->st_blksize = 512;
  st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = st->st_mtime;
#endif
  return 0;
}

__export int stat(const char* path, struct stat* st) {
  char full[PATH_MAX];
  int sz;
  if (!path || !st) {
    errno = EINVAL;
    return -1;
  }
  join_cwd(path, full, PATH_MAX);
  memset(st, 0, sizeof(*st));
  st->st_dev = 1;
  st->st_ino = path_ino(full);
  /* Arquivo primeiro: e o caso comum e poupa listar a pasta. */
  sz = k_fsize(full);
  {
    /* Aberto e ainda nao gravado: o tamanho de verdade e o da memoria. */
    struct fobj* o = fobj_find(full);
    if (o) sz = o->size;
  }
  if (sz < 0) {
    if (path_is_dir(full)) {
      st->st_mode = S_IFDIR | S_IRWXU;
      st->st_size = 0;
      return 0;
    }
    errno = ENOENT;
    return -1;
  }
  st->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
  st->st_size = sz;
  st->st_mtime = file_mtime(full);
  st->st_atime = st->st_mtime;
  st->st_ctime = st->st_mtime;
#if __SIZEOF_POINTER__ == 8
  st->st_nlink = 1;
  st->st_blksize = 512;
  st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = st->st_mtime;
#endif
  return 0;
}

__export DIR* opendir(const char* path) {
  char full[PATH_MAX];
  int i, n;
  struct __so_DIR* d;
  if (!path) {
    errno = EINVAL;
    return 0;
  }
  join_cwd(path, full, PATH_MAX);
  for (i = 0; i < DIR_MAX; i++)
    if (!dirs[i].used) break;
  if (i >= DIR_MAX) {
    errno = ENOMEM;
    return 0;
  }
  d = &dirs[i];
  memset(d, 0, sizeof(*d));
  n = k_dirlist(full, d->list, LIST_MAX);
  if (n < 0) {
    errno = ENOTDIR;
    return 0;
  }
  d->used = 1;
  d->pos = 0;
  return d;
}

__export struct dirent* readdir(DIR* dp) {
  struct __so_DIR* d = (struct __so_DIR*)dp;
  int i = 0;
  char* p;
  if (!d || !d->used) {
    errno = EBADF;
    return 0;
  }
  p = d->list + d->pos;
  while (*p == '\n') {
    p++;
    d->pos++;
  }
  if (!*p) return 0;
  memset(&d->cur, 0, sizeof(d->cur));
  while (*p && *p != '\n' && i < 31) d->cur.d_name[i++] = *p++;
  d->cur.d_name[i] = 0;
  if (i > 0 && d->cur.d_name[i - 1] == '\\') {
    d->cur.d_name[i - 1] = 0;
    d->cur.d_type = DT_DIR;
  } else {
    d->cur.d_type = DT_REG;
  }
  if (*p == '\n') p++;
  d->pos = (int)(p - d->list);
  return &d->cur;
}

__export void rewinddir(DIR* dp) {
  struct __so_DIR* d = (struct __so_DIR*)dp;
  if (d && d->used) d->pos = 0;
}

__export int closedir(DIR* dp) {
  struct __so_DIR* d = (struct __so_DIR*)dp;
  if (!d || !d->used) {
    errno = EBADF;
    return -1;
  }
  d->used = 0;
  return 0;
}

/* getrandom(2): o SODOS nao tem entropia do kernel; usamos o PRNG do shim. */
__export ssize_t getrandom(void* buf, size_t n, unsigned int flags) {
  (void)flags;
  if (!buf && n) {
    errno = EINVAL;
    return -1;
  }
  rng_fill(buf, n);
  return (ssize_t)n;
}

__export void arc4random_buf(void* buf, size_t n) { rng_fill(buf, n); }

__export unsigned int arc4random(void) { return (unsigned int)(rng_next() >> 16); }
