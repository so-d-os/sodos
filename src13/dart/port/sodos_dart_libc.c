/* sodos_dart_libc.c - funcoes de libc que a Dart VM usa e o port nao tem.
 *
 * So entram no build do Dart, com os nomes sodos_* (o sodos_dart_compat.h
 * renomeia por macro: open64 -> sodos_open64, openat64 -> sodos_openat64...),
 * porque o port do Firefox ja tem versoes que ignoram o dirfd.
 *
 * dirfd: AT_FDCWD, um fd de diretorio (sodos_dirfd_new, em sodos_epoll.c: o
 * posix-lite da EISDIR para pasta) ou qualquer um com caminho absoluto.
 */
#define SODOS_DART_NO_RENAME 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#define UTIME_NOW_ ((1l << 30) - 1l)
#define UTIME_OMIT_ ((1l << 30) - 2l)
#define PATHN 1024

int utimes(const char* path, const void* times); /* sodos_linux_abi.c */
long syscall(long number, ...);                  /* c_compiler/lib/misc.c */
ssize_t getrandom(void* buf, size_t n, unsigned int flags); /* posix-lite */
int sodos_dirfd_new(const char* path);           /* sodos_epoll.c */
const char* sodos_dirfd_path(int fd);

/* Caminho absoluto no SODOS: "X:\..." ou "X:/..." (e "/..." para o Unix). */
static int absolute(const char* p) {
  return p && (p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':'));
}

/* dirfd + path -> out. 0 ok, -1 com errno. */
static int resolve(int dirfd, const char* path, char* out) {
  const char* base;
  size_t bl, pl;
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  if (dirfd == AT_FDCWD || absolute(path)) {
    if (strlen(path) >= PATHN) {
      errno = ENAMETOOLONG;
      return -1;
    }
    strcpy(out, path);
    return 0;
  }
  base = sodos_dirfd_path(dirfd);
  if (!base) {
    errno = EBADF;
    return -1;
  }
  bl = strlen(base);
  pl = strlen(path);
  if (bl + 1 + pl >= PATHN) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(out, base, bl);
  if (!pl || (pl == 1 && path[0] == '.')) {
    out[bl] = 0;
    return 0;
  }
  if (bl && out[bl - 1] != '/' && out[bl - 1] != '\\') out[bl++] = '/';
  memcpy(out + bl, path, pl + 1);
  return 0;
}

static int is_dir(const char* p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- abrir -------------------------------------------------------------- */

static int open_full(const char* full, int flags, int mode) {
  if (flags & O_DIRECTORY) {
    struct stat st;
    if (stat(full, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
    return sodos_dirfd_new(full);
  }
  /* abrir pasta sem O_DIRECTORY so para leitura tambem vale no Linux */
  if (!(flags & (O_WRONLY | O_RDWR | O_CREAT)) && is_dir(full)) return sodos_dirfd_new(full);
  return open(full, flags & ~(O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY), mode);
}

int sodos_openat64(int dirfd, const char* path, int flags, ...) {
  char full[PATHN];
  va_list ap;
  int mode;
  va_start(ap, flags);
  mode = va_arg(ap, int);
  va_end(ap);
  if (resolve(dirfd, path, full) < 0) return -1;
  return open_full(full, flags, mode);
}

int sodos_open64(const char* path, int flags, ...) {
  char full[PATHN];
  va_list ap;
  int mode;
  va_start(ap, flags);
  mode = va_arg(ap, int);
  va_end(ap);
  if (resolve(AT_FDCWD, path, full) < 0) return -1;
  return open_full(full, flags, mode);
}

/* fdopendir fica com o fd: o closedir do Linux o fecharia; aqui ele e
 * fechado ja, porque o DIR do posix-lite nao guarda fd. */
DIR* sodos_fdopendir(int fd) {
  const char* p = sodos_dirfd_path(fd);
  DIR* d;
  if (!p) {
    errno = EBADF;
    return NULL;
  }
  d = opendir(p);
  if (d) close(fd);
  return d;
}

/* ---- *at ---------------------------------------------------------------- */

int sodos_fstatat(int dirfd, const char* path, struct stat* st, int flags) {
  char full[PATHN];
  if ((flags & AT_EMPTY_PATH) && path && !path[0]) {
    const char* p = sodos_dirfd_path(dirfd);
    return p ? stat(p, st) : fstat(dirfd, st);
  }
  if (resolve(dirfd, path, full) < 0) return -1;
  return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(full, st) : stat(full, st);
}

int sodos_unlinkat(int dirfd, const char* path, int flags) {
  char full[PATHN];
  if (resolve(dirfd, path, full) < 0) return -1;
  return (flags & 0x200) ? rmdir(full) : unlink(full); /* AT_REMOVEDIR */
}

int sodos_mkdirat(int dirfd, const char* path, unsigned int mode) {
  char full[PATHN];
  if (resolve(dirfd, path, full) < 0) return -1;
  return mkdir(full, (mode_t)mode);
}

int sodos_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
  char a[PATHN], b[PATHN];
  if (resolve(olddirfd, oldpath, a) < 0 || resolve(newdirfd, newpath, b) < 0) return -1;
  return rename(a, b);
}

long sodos_readlinkat(int dirfd, const char* path, char* buf, unsigned long n) {
  char full[PATHN];
  if (resolve(dirfd, path, full) < 0) return -1;
  return (long)readlink(full, buf, n);
}

/* FAT: sem links simbolicos. */
int sodos_symlinkat(const char* target, int newdirfd, const char* linkpath) {
  (void)target;
  (void)newdirfd;
  (void)linkpath;
  errno = EPERM;
  return -1;
}

/* UTIME_OMIT num campo mantem o valor atual (lido do stat); UTIME_NOW = agora. */
int sodos_utimensat(int dirfd, const char* path, const struct timespec t[2], int flags) {
  struct {
    long tv_sec, tv_usec;
  } tv[2];
  char full[PATHN];
  struct stat st;
  int i;
  (void)flags;
  if (resolve(dirfd, path, full) < 0) return -1;
  if (!t) return utimes(full, 0);
  if (stat(full, &st) != 0) return -1;
  for (i = 0; i < 2; i++) {
    if (t[i].tv_nsec == UTIME_NOW_) {
      tv[i].tv_sec = (long)time(0);
      tv[i].tv_usec = 0;
    } else if (t[i].tv_nsec == UTIME_OMIT_) {
      tv[i].tv_sec = (long)(i == 0 ? st.st_atime : st.st_mtime);
      tv[i].tv_usec = 0;
    } else {
      tv[i].tv_sec = (long)t[i].tv_sec;
      tv[i].tv_usec = t[i].tv_nsec / 1000;
    }
  }
  return utimes(full, tv);
}

/* ---- syscall: getrandom vai ao posix-lite, o resto a libc ---------------- */

long sodos_dart_syscall(long number, ...) {
  va_list ap;
  long a, b, c, d;
  va_start(ap, number);
  a = va_arg(ap, long);
  b = va_arg(ap, long);
  c = va_arg(ap, long);
  d = va_arg(ap, long);
  va_end(ap);
  if (number == 318) /* __NR_getrandom */
    return (long)getrandom((void*)a, (size_t)b, (unsigned)c);
  return syscall(number, a, b, c, d);
}

/* ---- getcwd no estilo POSIX ---------------------------------------------- */

char* sodos_dart_getcwd(char* buf, size_t size) {
  char* dos = getcwd(NULL, 0);
  char tmp[PATHN];
  size_t i = 0, j = 0;
  if (!dos) return NULL;
  if (dos[0] && dos[1] == ':') {
    char d = dos[0];
    if (d >= 'A' && d <= 'Z') d = (char)(d - 'A' + 'a');
    tmp[j++] = '/';
    tmp[j++] = d;
    i = 2;
  }
  for (; dos[i] && j < PATHN - 1; i++) tmp[j++] = dos[i] == '\\' ? '/' : dos[i];
  while (j > 2 && tmp[j - 1] == '/') j--; /* "/d/" -> "/d" (sem barra final) */
  tmp[j] = 0;
  free(dos);
  if (!buf) {
    if (size && j >= size) {
      errno = ERANGE;
      return NULL;
    }
    buf = (char*)malloc(size ? size : j + 1);
    if (!buf) {
      errno = ENOMEM;
      return NULL;
    }
  } else if (j >= size) {
    errno = ERANGE;
    return NULL;
  }
  memcpy(buf, tmp, j + 1);
  return buf;
}

/* ---- pilha por thread ---------------------------------------------------- */
/* O pthread_attr_t do port nao tem onde guardar a thread (o layout e contrato
 * com o Firefox ja compilado). O getattr lembra, por thread, qual attr
 * preencheu e de qual thread; o getstack pergunta ao port
 * (sodos_thread_stack). Sem resposta (thread principal), vale o do port. */
#include <pthread.h>
int sodos_thread_stack(pthread_t t, void** lo, size_t* size); /* sodos_thread.c */

static __thread const void* last_attr;
static __thread pthread_t last_thread;

int sodos_dart_getattr_np(pthread_t t, pthread_attr_t* a) {
  int r = pthread_getattr_np(t, a);
  last_attr = a;
  last_thread = t;
  return r;
}

int sodos_dart_attr_getstack(const pthread_attr_t* a, void** addr, size_t* size) {
  if (a == last_attr && sodos_thread_stack(last_thread, addr, size) == 0) return 0;
  return pthread_attr_getstack(a, addr, size);
}

/* ---- inotify: o kernel nao avisa mudancas -------------------------------- */

int inotify_init1(int flags) {
  (void)flags;
  errno = ENOSYS;
  return -1;
}
int inotify_init(void) { return inotify_init1(0); }
int inotify_add_watch(int fd, const char* path, uint32_t mask) {
  (void)fd;
  (void)path;
  (void)mask;
  errno = EBADF;
  return -1;
}
int inotify_rm_watch(int fd, int wd) {
  (void)fd;
  (void)wd;
  errno = EBADF;
  return -1;
}

/* ---- inet_pton ---------------------------------------------------------- */

static int pton4(const char* s, unsigned char out[4]) {
  int i;
  for (i = 0; i < 4; i++) {
    unsigned v = 0;
    int digits = 0;
    if (i && *s++ != '.') return 0;
    while (*s >= '0' && *s <= '9') {
      if (digits && v == 0) return 0; /* sem zero a esquerda, como o glibc */
      v = v * 10 + (unsigned)(*s++ - '0');
      if (++digits > 3 || v > 255) return 0;
    }
    if (!digits) return 0;
    out[i] = (unsigned char)v;
  }
  return *s == 0;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* RFC 4291: grupos hex, um "::" e IPv4 no fim opcional. */
static int pton6(const char* s, unsigned char out[16]) {
  unsigned char buf[16];
  int n = 0, gap = -1;
  memset(buf, 0, sizeof(buf));
  if (s[0] == ':') {
    if (s[1] != ':') return 0;
    s++;
  }
  while (*s) {
    if (*s == ':') {
      if (gap >= 0) return 0;
      gap = n;
      s++;
      if (!*s) break;
      continue;
    }
    {
      const char* g = s;
      unsigned v = 0;
      int d = 0, h;
      while ((h = hexv(*s)) >= 0) {
        v = (v << 4) | (unsigned)h;
        s++;
        if (++d > 4) return 0;
      }
      if (*s == '.') {
        if (n > 12 || !pton4(g, buf + n)) return 0;
        n += 4;
        break;
      }
      if (!d || n > 14) return 0;
      buf[n++] = (unsigned char)(v >> 8);
      buf[n++] = (unsigned char)v;
      if (*s == ':') {
        s++;
        if (!*s) return 0;
        if (*s == ':') continue;
      } else if (*s) {
        return 0;
      }
    }
  }
  if (gap >= 0) {
    int tail = n - gap;
    if (n == 16) return 0;
    memmove(buf + 16 - tail, buf + gap, (size_t)tail);
    memset(buf + gap, 0, (size_t)(16 - n));
  } else if (n != 16) {
    return 0;
  }
  memcpy(out, buf, 16);
  return 1;
}

int inet_pton(int af, const char* src, void* dst) {
  if (af == 2) return pton4(src, (unsigned char*)dst);   /* AF_INET */
  if (af == 10) return pton6(src, (unsigned char*)dst);  /* AF_INET6 */
  errno = EAFNOSUPPORT;
  return -1;
}

/* ---- string ------------------------------------------------------------- */

char* strcasestr(const char* hay, const char* needle) {
  size_t n = strlen(needle);
  if (!n) return (char*)hay;
  for (; *hay; hay++)
    if (strncasecmp(hay, needle, n) == 0) return (char*)hay;
  return NULL;
}

void bzero(void* p, unsigned long n) { memset(p, 0, n); }
