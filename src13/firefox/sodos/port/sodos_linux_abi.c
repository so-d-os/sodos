/* sodos_linux_abi.c - ABI glibc x86_64 que o std do Rust e o Gecko esperam.
 *
 * O alvo x86_64-sodos compila como Linux (target JSON com os=linux, env=gnu).
 * Aqui ficam os simbolos do glibc que a LIBC64 nao tem. Onde o layout difere
 * (stat64, dirent64) ha traducao; o que o kernel ainda nao oferece, como
 * sockets e processos, devolve erro: o programa liga e falha em tempo de
 * execucao so se usar o recurso.
 */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8

/* Aqui stat64 e o layout do glibc, para o std do Rust. */
#define SODOS_NO_STAT64_ALIAS
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <time.h>

extern void k_exit(int code);
extern void sodos_sleep_ms(unsigned ms);

#define FAIL_ERRNO(e) \
  do {                \
    errno = (e);      \
    return -1;        \
  } while (0)

/* ---- basicos ------------------------------------------------------------ */

int* __errno_location(void) { return &errno; }

void _exit(int code) {
  k_exit(code);
  for (;;) {
  }
}

static char* g_empty_env[1] = {0};
char** environ = g_empty_env;

const char* gnu_get_libc_version(void) { return "2.36"; }
int __res_init(void) { return 0; }

int __xpg_strerror_r(int err, char* buf, size_t n) {
  if (n) {
    const char* s = strerror(err);
    strncpy(buf, s ? s : "erro", n - 1);
    buf[n - 1] = 0;
  }
  return 0;
}

int gethostname(char* name, size_t n) {
  if (n) {
    strncpy(name, "sodos", n - 1);
    name[n - 1] = 0;
  }
  return 0;
}

int getpagesize(void) { return 4096; }
unsigned alarm(unsigned seconds) {
  (void)seconds;
  return 0;
}

/* ---- arquivos ----------------------------------------------------------- */

int open64(const char* path, int flags, ...) {
  va_list ap;
  int mode;
  va_start(ap, flags);
  mode = va_arg(ap, int);
  va_end(ap);
  return open(path, flags & ~(O_CLOEXEC | O_NOFOLLOW), mode);
}

int openat64(int dirfd, const char* path, int flags, ...) {
  va_list ap;
  int mode;
  (void)dirfd;
  va_start(ap, flags);
  mode = va_arg(ap, int);
  va_end(ap);
  return open(path, flags & ~(O_CLOEXEC | O_NOFOLLOW), mode);
}

long lseek64(int fd, long off, int whence) { return lseek(fd, off, whence); }
int ftruncate64(int fd, long len) { return ftruncate(fd, len); }
int fdatasync(int fd) { return fsync(fd); }
void* mmap64(void* addr, size_t len, int prot, int flags, int fd, long off) {
  return mmap(addr, len, prot, flags, fd, off);
}

ssize_t pread(int fd, void* buf, size_t n, off_t off) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  ssize_t r;
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  r = read(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  ssize_t r;
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  r = write(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}

ssize_t pread64(int fd, void* buf, size_t n, long off) { return pread(fd, buf, n, off); }
ssize_t pwrite64(int fd, const void* buf, size_t n, long off) { return pwrite(fd, buf, n, off); }

struct so_iovec {
  void* iov_base;
  size_t iov_len;
};

ssize_t readv(int fd, const struct so_iovec* v, int cnt) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < cnt; i++) {
    ssize_t r = read(fd, v[i].iov_base, v[i].iov_len);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < v[i].iov_len) break;
  }
  return total;
}

ssize_t writev(int fd, const struct so_iovec* v, int cnt) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < cnt; i++) {
    ssize_t r = write(fd, v[i].iov_base, v[i].iov_len);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < v[i].iov_len) break;
  }
  return total;
}

ssize_t preadv(int fd, const struct so_iovec* v, int cnt, long off) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < cnt; i++) {
    ssize_t r = pread(fd, v[i].iov_base, v[i].iov_len, off + total);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < v[i].iov_len) break;
  }
  return total;
}

ssize_t pwritev(int fd, const struct so_iovec* v, int cnt, long off) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < cnt; i++) {
    ssize_t r = pwrite(fd, v[i].iov_base, v[i].iov_len, off + total);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < v[i].iov_len) break;
  }
  return total;
}

/* Layout do struct stat64 do glibc x86_64 (144 bytes). */
struct glibc_stat64 {
  uint64_t st_dev, st_ino, st_nlink;
  uint32_t st_mode, st_uid, st_gid, pad0;
  uint64_t st_rdev;
  int64_t st_size, st_blksize, st_blocks;
  int64_t st_atime_sec, st_atime_nsec;
  int64_t st_mtime_sec, st_mtime_nsec;
  int64_t st_ctime_sec, st_ctime_nsec;
  int64_t unused[3];
};

static void to_stat64(const struct stat* s, struct glibc_stat64* o) {
  memset(o, 0, sizeof(*o));
  o->st_dev = (uint64_t)s->st_dev;
  o->st_ino = (uint64_t)s->st_ino;
  o->st_nlink = 1;
  o->st_mode = (uint32_t)s->st_mode;
  o->st_size = (int64_t)s->st_size;
  o->st_blksize = 512;
  o->st_blocks = ((int64_t)s->st_size + 511) / 512;
  o->st_atime_sec = (int64_t)s->st_atime;
  o->st_mtime_sec = (int64_t)s->st_mtime;
  o->st_ctime_sec = (int64_t)s->st_ctime;
}

int stat64(const char* path, struct glibc_stat64* out) {
  struct stat s;
  if (stat(path, &s) < 0) return -1;
  to_stat64(&s, out);
  return 0;
}

int lstat64(const char* path, struct glibc_stat64* out) { return stat64(path, out); }

int fstat64(int fd, struct glibc_stat64* out) {
  struct stat s;
  if (fstat(fd, &s) < 0) return -1;
  to_stat64(&s, out);
  return 0;
}

int fstatat64(int dirfd, const char* path, struct glibc_stat64* out, int flags) {
  (void)dirfd;
  (void)flags;
  return stat64(path, out);
}

int lstat(const char* path, struct stat* st) { return stat(path, st); }

struct glibc_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[256];
};

struct glibc_dirent64* readdir64(DIR* d) {
  static struct glibc_dirent64 e;
  struct dirent* r = readdir(d);
  if (!r) return NULL;
  memset(&e, 0, sizeof(e));
  e.d_ino = 1;
  e.d_reclen = sizeof(e);
  e.d_type = r->d_type;
  strncpy(e.d_name, r->d_name, sizeof(e.d_name) - 1);
  return &e;
}

/* FAT nao tem dono nem permissao: aceitar e o comportamento correto. */
int chmod(const char* path, mode_t mode) {
  (void)path;
  (void)mode;
  return 0;
}
int fchmod(int fd, mode_t mode) {
  (void)fd;
  (void)mode;
  return 0;
}
int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
  (void)dirfd;
  (void)path;
  (void)mode;
  (void)flags;
  return 0;
}
int chown(const char* path, uid_t uid, gid_t gid) {
  (void)path;
  (void)uid;
  (void)gid;
  return 0;
}
int fchown(int fd, uid_t uid, gid_t gid) {
  (void)fd;
  (void)uid;
  (void)gid;
  return 0;
}
int lchown(const char* path, uid_t uid, gid_t gid) {
  (void)path;
  (void)uid;
  (void)gid;
  return 0;
}
int flock(int fd, int op) {
  (void)fd;
  (void)op;
  return 0;
}
mode_t umask(mode_t mask) {
  (void)mask;
  return 022;
}

int unlinkat(int dirfd, const char* path, int flags) {
  (void)dirfd;
  return (flags & 0x200) ? rmdir(path) : unlink(path);
}
int mkdirat(int dirfd, const char* path, mode_t mode) {
  (void)dirfd;
  return mkdir(path, mode);
}

/* ---- tempo e sinais ----------------------------------------------------- */

struct so_timespec64 {
  int64_t tv_sec;
  int64_t tv_nsec;
};

/* flags & 1 = TIMER_ABSTIME: req e um instante do relogio clk, nao uma
 * duracao (o glib e o QEMU dormem assim). Tratar como duracao fazia a
 * thread dormir o uptime inteiro. */
int clock_nanosleep(int clk, int flags, const struct so_timespec64* req, void* rem) {
  int64_t want = req->tv_sec * 1000 + req->tv_nsec / 1000000;
  (void)rem;
  if (flags & 1) {
    struct timespec now;
    clock_gettime(clk, &now);
    want -= (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (want <= 0) {
      sched_yield();
      return 0;
    }
  }
  sodos_sleep_ms(want > 0 ? (unsigned)want : 0);
  return 0;
}

int sigaltstack(const stack_t* ss, stack_t* old) {
  (void)ss;
  (void)old;
  return 0;
}

int sched_getaffinity(pid_t pid, size_t n, cpu_set_t* mask) {
  (void)pid;
  if (n) {
    memset(mask, 0, n);
    mask->bits[0] = 1;
  }
  return 0;
}

int sched_getcpu(void) { return 0; }
int sched_setaffinity(pid_t pid, size_t n, const cpu_set_t* set) {
  (void)pid;
  (void)n;
  (void)set;
  return 0;
}
int dladdr(const void* addr, void* info) {
  (void)addr;
  (void)info;
  return 0;
}
int truncate(const char* path, off_t len) {
  int fd = open(path, O_WRONLY, 0);
  int r;
  if (fd < 0) return -1;
  r = ftruncate(fd, len);
  close(fd);
  return r;
}
int utimes(const char* path, const void* times) {
  (void)path;
  (void)times;
  return 0;
}
ssize_t readahead(int fd, long off, size_t n) {
  (void)fd;
  (void)off;
  (void)n;
  return 0;
}
int pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void)) {
  (void)a;
  (void)b;
  (void)c;
  return 0;
}
int getpriority(int which, unsigned who) {
  (void)which;
  (void)who;
  return 0;
}
int setpriority(int which, unsigned who, int prio) {
  (void)which;
  (void)who;
  (void)prio;
  return 0;
}

pid_t gettid(void) { return (pid_t)(uintptr_t)pthread_self(); }

char* strsep(char** s, const char* delim) {
  char* start = *s;
  char* p;
  if (!start) return NULL;
  p = start + strcspn(start, delim);
  if (*p) {
    *p = 0;
    *s = p + 1;
  } else {
    *s = NULL;
  }
  return start;
}

extern ssize_t getrandom(void* buf, size_t n, unsigned int flags);
void arc4random_buf(void* buf, size_t n) { getrandom(buf, n, 0); }
unsigned int arc4random(void) {
  unsigned int v = 0;
  getrandom(&v, sizeof(v), 0);
  return v;
}

int faccessat(int dirfd, const char* path, int mode, int flags) {
  (void)dirfd;
  (void)flags;
  return access(path, mode);
}

long random(void) { return rand(); }
void srandom(unsigned int seed) { srand(seed); }

/* ---- usuario ------------------------------------------------------------ */

int getpwuid_r(unsigned uid, void* pw, char* buf, size_t n, void** out) {
  (void)uid;
  (void)pw;
  (void)buf;
  (void)n;
  *out = NULL;
  return ENOENT;
}

int getpwnam_r(const char* name, void* pw, char* buf, size_t n, void** out) {
  (void)name;
  (void)pw;
  (void)buf;
  (void)n;
  *out = NULL;
  return ENOENT;
}

/* ---- ainda sem suporte no kernel ---------------------------------------- */

#define ENOSYS_STUB(name, ...)     \
  int name(__VA_ARGS__) {          \
    errno = ENOSYS;                \
    return -1;                     \
  }

ENOSYS_STUB(setitimer, int a, const void* b, void* c)
ENOSYS_STUB(getitimer, int a, void* b)
ENOSYS_STUB(getifaddrs, void** a)
ENOSYS_STUB(res_init, void)
ENOSYS_STUB(res_ninit, void* a)
ENOSYS_STUB(res_query, const char* a, int b, int c, unsigned char* d, int e)
ENOSYS_STUB(res_nquery, void* a, const char* b, int c, int d, unsigned char* e, int f)
ENOSYS_STUB(symlink, const char* a, const char* b)
ENOSYS_STUB(link, const char* a, const char* b)
ENOSYS_STUB(linkat, int a, const char* b, int c, const char* d, int e)
ENOSYS_STUB(renameat, int a, const char* b, int c, const char* d)
ENOSYS_STUB(mkfifo, const char* a, mode_t b)
ENOSYS_STUB(chroot, const char* a)
ENOSYS_STUB(dirfd, void* a)
ENOSYS_STUB(futimens, int a, const void* b)
ENOSYS_STUB(utimensat, int a, const char* b, const void* c, int d)
ENOSYS_STUB(ioctl, int a, unsigned long b, ...)
ENOSYS_STUB(pause, void)
ENOSYS_STUB(killpg, int a, int b)
ENOSYS_STUB(setgid, unsigned a)
ENOSYS_STUB(setuid, unsigned a)
ENOSYS_STUB(setgroups, size_t a, const unsigned* b)
ENOSYS_STUB(setpgid, int a, int b)
ENOSYS_STUB(setsid, void)
ENOSYS_STUB(waitid, int a, unsigned b, void* c, int d)
ENOSYS_STUB(socket, int a, int b, int c)
ENOSYS_STUB(bind, int a, const void* b, unsigned c)
ENOSYS_STUB(connect, int a, const void* b, unsigned c)
ENOSYS_STUB(listen, int a, int b)
ENOSYS_STUB(accept4, int a, void* b, unsigned* c, int d)
ENOSYS_STUB(getpeername, int a, void* b, unsigned* c)
ENOSYS_STUB(getsockname, int a, void* b, unsigned* c)
ENOSYS_STUB(getsockopt, int a, int b, int c, void* d, unsigned* e)
ENOSYS_STUB(setsockopt, int a, int b, int c, const void* d, unsigned e)
ENOSYS_STUB(shutdown, int a, int b)
ENOSYS_STUB(dl_iterate_phdr, void* a, void* b)

#define ENOSYS_SSIZE(name, ...)    \
  ssize_t name(__VA_ARGS__) {      \
    errno = ENOSYS;                \
    return -1;                     \
  }

ENOSYS_SSIZE(recvfrom, int a, void* b, size_t c, int d, void* e, unsigned* f)
ENOSYS_SSIZE(sendto, int a, const void* b, size_t c, int d, const void* e, unsigned f)
ENOSYS_SSIZE(sendfile64, int a, int b, long* c, size_t d)
ENOSYS_SSIZE(splice, int a, long* b, int c, long* d, size_t e, unsigned f)

void res_nclose(void* a) { (void)a; }
void freeifaddrs(void* a) { (void)a; }
void* getmntent_r(void* f, void* m, char* buf, int n) {
  (void)f;
  (void)m;
  (void)buf;
  (void)n;
  return NULL;
}
char* hasmntopt(const void* m, const char* opt) {
  (void)m;
  (void)opt;
  return NULL;
}

void* fdopendir(int fd) {
  (void)fd;
  errno = ENOSYS;
  return NULL;
}

/* getaddrinfo: EAI_SYSTEM, com errno. */
int getaddrinfo(const char* a, const char* b, const void* c, void** d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  errno = ENOSYS;
  return -11;
}
void freeaddrinfo(void* a) { (void)a; }
const char* gai_strerror(int e) {
  (void)e;
  return "getaddrinfo indisponivel no SODOS";
}

/* posix_spawn devolve o erro, sem usar errno. */
#define SPAWN_NOP(name, ...) \
  int name(__VA_ARGS__) { return 0; }
SPAWN_NOP(posix_spawn_file_actions_init, void* a)
SPAWN_NOP(posix_spawn_file_actions_destroy, void* a)
SPAWN_NOP(posix_spawn_file_actions_adddup2, void* a, int b, int c)
SPAWN_NOP(posix_spawnattr_init, void* a)
SPAWN_NOP(posix_spawnattr_destroy, void* a)
SPAWN_NOP(posix_spawnattr_setflags, void* a, short b)
SPAWN_NOP(posix_spawnattr_setpgroup, void* a, int b)
SPAWN_NOP(posix_spawnattr_setsigdefault, void* a, const void* b)
int posix_spawnp(int* pid, const char* file, const void* acts, const void* attr, char* const* argv,
                 char* const* envp) {
  (void)pid;
  (void)file;
  (void)acts;
  (void)attr;
  (void)argv;
  (void)envp;
  return ENOSYS;
}

/* ---- unwinder: o alvo e panic=abort, nao ha excecoes nem backtrace ------- */

enum { URC_END_OF_STACK = 5 };

int _Unwind_Backtrace(void* fn, void* arg) {
  (void)fn;
  (void)arg;
  return URC_END_OF_STACK;
}
int _Unwind_RaiseException(void* e) {
  (void)e;
  return URC_END_OF_STACK;
}
void _Unwind_DeleteException(void* e) { (void)e; }
void* _Unwind_FindEnclosingFunction(void* ip) {
  (void)ip;
  return NULL;
}
unsigned long _Unwind_GetCFA(void* c) {
  (void)c;
  return 0;
}
unsigned long _Unwind_GetDataRelBase(void* c) {
  (void)c;
  return 0;
}
unsigned long _Unwind_GetTextRelBase(void* c) {
  (void)c;
  return 0;
}
unsigned long _Unwind_GetIP(void* c) {
  (void)c;
  return 0;
}
unsigned long _Unwind_GetIPInfo(void* c, int* before) {
  (void)c;
  if (before) *before = 0;
  return 0;
}
void* _Unwind_GetLanguageSpecificData(void* c) {
  (void)c;
  return NULL;
}
unsigned long _Unwind_GetRegionStart(void* c) {
  (void)c;
  return 0;
}
void _Unwind_SetGR(void* c, int r, unsigned long v) {
  (void)c;
  (void)r;
  (void)v;
}
void _Unwind_SetIP(void* c, unsigned long v) {
  (void)c;
  (void)v;
}

/* ---- libm: o que falta, sobre as versoes double ------------------------- */

float acoshf(float x) { return (float)acosh(x); }
float asinhf(float x) { return (float)asinh(x); }
float atanhf(float x) { return (float)atanh(x); }
float exp2f(float x) { return (float)exp2(x); }
float log1pf(float x) { return (float)log1p(x); }
long lrintf(float x) { return lrint(x); }
long long llrintf(float x) { return (long long)lrint(x); }
long lroundf(float x) { return lround(x); }
float nanf(const char* s) {
  (void)s;
  return __builtin_nanf("");
}
float nextafterf(float x, float y) {
  union {
    float f;
    uint32_t u;
  } v;
  if (x != x || y != y) return x + y;
  if (x == y) return y;
  v.f = x;
  if (x == 0.0f) {
    v.u = 1;
    return y > 0.0f ? v.f : -v.f;
  }
  if ((x < y) == (x > 0.0f)) v.u++;
  else v.u--;
  return v.f;
}
void sincos(double x, double* s, double* c) {
  *s = sin(x);
  *c = cos(x);
}
void sincosf(float x, float* s, float* c) {
  *s = (float)sin(x);
  *c = (float)cos(x);
}
#undef ldexpl
#undef frexpl
long double ldexpl(long double x, int e) { return (long double)ldexp((double)x, e); }
long double frexpl(long double x, int* e) { return (long double)frexp((double)x, e); }
int fegetround(void) { return 0; /* FE_TONEAREST */ }

/* ---- libc: arquivos e texto --------------------------------------------- */

int iswspace(unsigned int c) {
  return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x2003 || c == 0x3000;
}

int mkstemp(char* tmpl) {
  static unsigned counter;
  size_t n = strlen(tmpl);
  int tries;
  if (n < 6) FAIL_ERRNO(EINVAL);
  for (tries = 0; tries < 100; tries++) {
    unsigned v = ++counter * 2654435761u;
    int i, fd;
    for (i = 0; i < 6; i++) {
      tmpl[n - 6 + i] = (char)('A' + v % 26);
      v /= 26;
    }
    fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 || errno != EEXIST) return fd;
  }
  return -1;
}

ssize_t getdelim(char** line, size_t* cap, int delim, FILE* f) {
  size_t len = 0;
  int c;
  if (!*line || !*cap) {
    *cap = 128;
    *line = (char*)malloc(*cap);
    if (!*line) return -1;
  }
  while ((c = fgetc(f)) != EOF) {
    if (len + 2 > *cap) {
      char* p = (char*)realloc(*line, *cap * 2);
      if (!p) return -1;
      *line = p;
      *cap *= 2;
    }
    (*line)[len++] = (char)c;
    if (c == delim) break;
  }
  if (!len) return -1;
  (*line)[len] = 0;
  return (ssize_t)len;
}
ssize_t getline(char** line, size_t* cap, FILE* f) { return getdelim(line, cap, '\n', f); }

char* strptime(const char* s, const char* fmt, struct tm* tm) {
  (void)s;
  (void)fmt;
  (void)tm;
  return NULL;
}

void* mremap(void* old, size_t oldsz, size_t newsz, int flags, ...) {
  (void)old;
  (void)oldsz;
  (void)newsz;
  (void)flags;
  errno = ENOMEM;
  return MAP_FAILED;
}

/* ---- rede: bytes e nomes (a conexao real ainda nao passa por aqui) ------- */

uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }

uint32_t inet_addr(const char* s) {
  uint32_t r = 0;
  int i;
  for (i = 0; i < 4; i++) {
    unsigned v = 0;
    if (*s < '0' || *s > '9') return 0xffffffffu;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    if (v > 255) return 0xffffffffu;
    r |= v << (i * 8);
    if (i < 3 && *s++ != '.') return 0xffffffffu;
  }
  return *s ? 0xffffffffu : r;
}

static char* ipv4_text(uint32_t a, char* buf, size_t n) {
  snprintf(buf, n, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, (a >> 24) & 255);
  return buf;
}
char* inet_ntoa(uint32_t a) {
  static char buf[16];
  return ipv4_text(a, buf, sizeof(buf));
}
const char* inet_ntop(int af, const void* src, char* dst, unsigned n) {
  if (af != 2 /* AF_INET */) {
    errno = EAFNOSUPPORT;
    return NULL;
  }
  return ipv4_text(*(const uint32_t*)src, dst, n);
}

int h_errno;
void* gethostbyname(const char* name) {
  (void)name;
  h_errno = 3; /* NO_RECOVERY */
  return NULL;
}
void* gethostbyaddr(const void* a, unsigned n, int t) {
  (void)a;
  (void)n;
  (void)t;
  h_errno = 3;
  return NULL;
}
void* getprotobyname(const char* a) {
  (void)a;
  return NULL;
}
void* getprotobynumber(int a) {
  (void)a;
  return NULL;
}
void* getservbyname(const char* a, const char* b) {
  (void)a;
  (void)b;
  return NULL;
}
int getnameinfo(const void* a, unsigned b, char* c, unsigned d, char* e, unsigned f, int g) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  (void)g;
  return -4; /* EAI_FAIL */
}
unsigned if_nametoindex(const char* a) {
  (void)a;
  return 0;
}
void* getpwuid(unsigned a) {
  (void)a;
  return NULL;
}
void* getgrgid(unsigned a) {
  (void)a;
  return NULL;
}

ENOSYS_STUB(accept, int a, void* b, unsigned* c)
ENOSYS_STUB(epoll_create1, int a)
ENOSYS_STUB(epoll_ctl, int a, int b, int c, void* d)
ENOSYS_STUB(epoll_wait, int a, void* b, int c, int d)
ENOSYS_STUB(eventfd, unsigned a, int b)
ENOSYS_STUB(fallocate, int a, int b, long c, long d)
ENOSYS_STUB(recvmmsg, int a, void* b, unsigned c, int d, void* e)
ENOSYS_STUB(semget, int a, int b, int c)
ENOSYS_STUB(semop, int a, void* b, size_t c)
ENOSYS_STUB(semctl, int a, int b, int c, ...)
ENOSYS_STUB(shmget, int a, size_t b, int c)
ENOSYS_STUB(shmdt, const void* a)
ENOSYS_STUB(shmctl, int a, int b, void* c)
ENOSYS_STUB(ftok, const char* a, int b)
ENOSYS_STUB(shm_open, const char* a, int b, unsigned c)
ENOSYS_STUB(shm_unlink, const char* a)
ENOSYS_STUB(statfs, const char* a, void* b)
ENOSYS_STUB(sysinfo, void* a)
ENOSYS_SSIZE(sendfile, int a, int b, long* c, size_t d)

void* shmat(int a, const void* b, int c) {
  (void)a;
  (void)b;
  (void)c;
  errno = ENOSYS;
  return (void*)-1;
}
int posix_fallocate(int fd, long off, long len) {
  (void)fd;
  (void)off;
  (void)len;
  return ENOSYS;
}
void* setmntent(const char* a, const char* b) {
  (void)a;
  (void)b;
  return NULL;
}
int endmntent(void* a) {
  (void)a;
  return 1;
}

/* select: ainda sem descritores que fiquem prontos; so respeita o prazo. */
struct so_timeval64 {
  long tv_sec;
  long tv_usec;
};
int select(int n, void* r, void* w, void* e, struct so_timeval64* t) {
  (void)n;
  (void)r;
  (void)w;
  (void)e;
  if (t) sodos_sleep_ms((unsigned)(t->tv_sec * 1000 + t->tv_usec / 1000));
  return 0;
}

/* ---- sistema ------------------------------------------------------------ */

struct so_utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
int uname(struct so_utsname* u) {
  memset(u, 0, sizeof(*u));
  strcpy(u->sysname, "SODOS");
  strcpy(u->nodename, "sodos");
  strcpy(u->release, "13");
  strcpy(u->machine, "x86_64");
  return 0;
}

struct so_rlimit {
  unsigned long cur, max;
};
int getrlimit(int r, struct so_rlimit* l) {
  (void)r;
  l->cur = l->max = ~0UL;
  return 0;
}
int setrlimit(int r, const struct so_rlimit* l) {
  (void)r;
  (void)l;
  return 0;
}

/* ---- pthreads que faltam ------------------------------------------------- */

int pthread_kill(pthread_t t, int sig) {
  (void)t;
  (void)sig;
  return 0;
}
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) {
  (void)how;
  (void)set;
  if (old) memset(old, 0, sizeof(*old));
  return 0;
}
int pthread_getcpuclockid(pthread_t t, int* clk) {
  (void)t;
  (void)clk;
  return ENOENT;
}
int pthread_getschedparam(pthread_t t, int* policy, struct sched_param* p) {
  (void)t;
  *policy = 0;
  p->sched_priority = 0;
  return 0;
}
int pthread_setschedparam(pthread_t t, int policy, const struct sched_param* p) {
  (void)t;
  (void)policy;
  (void)p;
  return 0;
}
int pthread_setaffinity_np(pthread_t t, size_t n, const cpu_set_t* s) {
  (void)t;
  (void)n;
  (void)s;
  return 0;
}
int pthread_mutexattr_setpshared(pthread_mutexattr_t* a, int shared) {
  (void)a;
  (void)shared;
  return 0;
}
int sem_trywait(sem_t* s);
int sem_timedwait(sem_t* s, const struct timespec* abstime) {
  (void)abstime;
  while (sem_trywait(s) != 0) {
    /* Sem relogio absoluto aqui: cede a CPU e tenta de novo uma vez. */
    sched_yield();
    if (sem_trywait(s) == 0) return 0;
    errno = ETIMEDOUT;
    return -1;
  }
  return 0;
}

/* Sem shell Unix para preencher o ambiente: valores padrao do SODOS. */
__attribute__((constructor(102))) static void sodos_default_env(void) {
  setenv("HOME", "/D/USER/DEFAULT", 0);
  setenv("TMPDIR", "/D/TEMP", 0);
  setenv("USER", "sodos", 0);
}

/* ---- depuracao ---------------------------------------------------------- */

extern char __executable_start[] __attribute__((weak));
extern char etext[] __attribute__((weak));

/* Lista na serial os enderecos de codigo da pilha (toolchain: simbolizar com o ELF). */
void sodos_dump_stack(void) {
  uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
  int i, shown = 0;
  fprintf(stderr, "[stk] base %lx\n", (unsigned long)__executable_start);
  for (i = 0; i < 4000 && shown < 40; i++) {
    uintptr_t v = sp[i];
    if ((char*)v >= __executable_start && (char*)v < etext) {
      fprintf(stderr, "[stk] %lx\n", (unsigned long)v);
      shown++;
    }
  }
}

/* ---- simbolos do Rust ---------------------------------------------------- */

extern ssize_t getrandom(void* buf, size_t n, unsigned int flags);
/* Result<(), getrandom::Error>: 0 e Ok no registro de retorno. */
unsigned int __getrandom_v03_custom(void* dest, size_t len) {
  getrandom(dest, len, 0);
  return 0;
}
/* DMABuf e Vulkan nao existem aqui (o lado C++ esta fora do build). */
int wgpu_server_get_dma_buf_fd(void* parent, uint64_t id) {
  (void)parent;
  (void)id;
  return -1;
}
const void* wgpu_server_get_vk_image_handle(void* parent, uint64_t id) {
  (void)parent;
  (void)id;
  return NULL;
}

/* libevent: sem epoll, o init falha e ele cai no proximo backend. */
static void* no_epoll_init(void* base) {
  (void)base;
  return NULL;
}
const struct {
  const char* name;
  void* (*init)(void*);
  void* add;
  void* del;
  void* dispatch;
  void* dealloc;
  int need_reinit;
  int features;
  size_t fdinfo_len;
} epollops = {"epoll", no_epoll_init, NULL, NULL, NULL, NULL, 0, 0, 0};

#endif /* LP64 */
