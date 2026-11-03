/* posix_extra.c - o que o NetSurf usa e a LIBCX/POSIXX do SODOS nao tem.
 * Sem links simbolicos, sem regex, sem zlib: as versoes aqui cobrem o uso
 * real do NetSurf (ler arquivos, listar pastas, argumentos). */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <zlib.h>

int isascii(int c) { return c >= 0 && c < 128; }

void setbuf(FILE* f, char* buf) {
  (void)f;
  (void)buf;
}

int uname(struct utsname* u) {
  memset(u, 0, sizeof(*u));
  strcpy(u->sysname, "SODOS");
  strcpy(u->nodename, "sodos");
  strcpy(u->release, "1.0");
  strcpy(u->version, "1");
  strcpy(u->machine, "x86_64");
  return 0;
}

/* --- arquivos ----------------------------------------------------------- */

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
  (void)flags;
  if (dirfd != AT_FDCWD && path[0] != '/' && path[0] != '\\' && path[1] != ':') {
    errno = ENOSYS;
    return -1;
  }
  return stat(path, st);
}

int lstat(const char* path, struct stat* st) { return stat(path, st); }

int chmod(const char* path, mode_t mode) {
  (void)mode;
  return access(path, F_OK);
}

mode_t umask(mode_t mask) {
  (void)mask;
  return 022;
}

int unlinkat(int dirfd, const char* path, int flags) {
  (void)dirfd;
  return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int openat(int dirfd, const char* path, int flags, ...) {
  (void)dirfd;
  return open(path, flags);
}

int fcntl(int fd, int cmd, ...) {
  (void)fd;
  (void)cmd;
  return 0;
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

/* mmap: copia o arquivo para a memoria (so leitura de verdade) */
void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
  char* p;
  size_t got = 0;
  (void)addr;
  (void)prot;
  if (len == 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  p = (char*)malloc(len);
  if (!p) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  if (flags & MAP_ANONYMOUS) {
    memset(p, 0, len);
    return p;
  }
  if (lseek(fd, off, SEEK_SET) < 0) {
    free(p);
    return MAP_FAILED;
  }
  while (got < len) {
    ssize_t r = read(fd, p + got, len - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  if (got < len) memset(p + got, 0, len - got);
  return p;
}

int munmap(void* addr, size_t len) {
  (void)len;
  free(addr);
  return 0;
}

int dirfd(DIR* d) {
  (void)d;
  return AT_FDCWD;
}

int alphasort(const struct dirent** a, const struct dirent** b) {
  return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char* dir, struct dirent*** list, int (*sel)(const struct dirent*),
            int (*cmp)(const struct dirent**, const struct dirent**)) {
  DIR* d = opendir(dir);
  struct dirent* e;
  struct dirent** v = NULL;
  int n = 0, cap = 0;
  if (!d) return -1;
  while ((e = readdir(d)) != NULL) {
    struct dirent* c;
    if (sel && !sel(e)) continue;
    if (n == cap) {
      struct dirent** nv;
      cap = cap ? cap * 2 : 32;
      nv = (struct dirent**)realloc(v, (size_t)cap * sizeof(*v));
      if (!nv) break;
      v = nv;
    }
    c = (struct dirent*)malloc(sizeof(*c));
    if (!c) break;
    *c = *e;
    v[n++] = c;
  }
  closedir(d);
  if (cmp && n > 1)
    qsort(v, (size_t)n, sizeof(*v), (int (*)(const void*, const void*))cmp);
  *list = v;
  return n;
}

char* basename(char* path) {
  char* p;
  size_t n;
  if (!path || !*path) return ".";
  n = strlen(path);
  while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) path[--n] = 0;
  p = path + n;
  while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
  return p;
}

char* dirname(char* path) {
  char* p;
  if (!path || !*path) return ".";
  p = path + strlen(path);
  while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
  if (p == path) return ".";
  if (p - 1 == path) {
    p[0] = 0;
    return path;
  }
  p[-1] = 0;
  return path;
}

int select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv) {
  (void)n;
  if (r) FD_ZERO(r);
  if (w) FD_ZERO(w);
  if (e) FD_ZERO(e);
  if (tv) usleep((unsigned)(tv->tv_sec * 1000000 + tv->tv_usec));
  return 0;
}

/* --- rede (so parse de enderecos) ---------------------------------------- */

int inet_aton(const char* cp, struct in_addr* a) {
  unsigned v[4];
  int i = 0;
  const char* p = cp;
  while (i < 4) {
    unsigned x = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      x = x * 10 + (unsigned)(*p++ - '0');
      if (++digits > 3 || x > 255) return 0;
    }
    if (!digits) return 0;
    v[i++] = x;
    if (i < 4) {
      if (*p != '.') return 0;
      p++;
    }
  }
  if (*p) return 0;
  /* ordem de rede (big-endian) */
  a->s_addr = v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24;
  return 1;
}

int inet_pton(int af, const char* src, void* dst) {
  if (af == AF_INET) return inet_aton(src, (struct in_addr*)dst);
  if (af == AF_INET6) {
    /* basta reconhecer se parece IPv6: o NetSurf so testa "e um IP?" */
    const char* p;
    int colons = 0;
    for (p = src; *p; p++) {
      if (*p == ':') colons++;
      else if (!isxdigit((unsigned char)*p) && *p != '.') return 0;
    }
    if (colons < 2) return 0;
    memset(dst, 0, 16);
    return 1;
  }
  errno = EAFNOSUPPORT;
  return -1;
}

/* --- regex: indisponivel ------------------------------------------------- */

int regcomp(regex_t* re, const char* pat, int flags) {
  (void)pat;
  (void)flags;
  re->re_nsub = 0;
  re->priv = NULL;
  return REG_ESPACE;
}

int regexec(const regex_t* re, const char* s, size_t n, regmatch_t* m, int flags) {
  (void)re;
  (void)s;
  (void)n;
  (void)m;
  (void)flags;
  return REG_NOMATCH;
}

size_t regerror(int err, const regex_t* re, char* buf, size_t size) {
  const char* msg = "regex indisponivel no SODOS";
  (void)err;
  (void)re;
  if (size) {
    strncpy(buf, msg, size - 1);
    buf[size - 1] = 0;
  }
  return strlen(msg) + 1;
}

void regfree(regex_t* re) { (void)re; }

/* --- zlib: so arquivos sem compressao ------------------------------------ */

gzFile gzopen(const char* path, const char* mode) { return (gzFile)fopen(path, mode); }

char* gzgets(gzFile f, char* buf, int len) { return fgets(buf, len, (FILE*)f); }

int gzclose(gzFile f) { return fclose((FILE*)f); }

int inflateInit2_(z_streamp s, int bits, const char* ver, int size) {
  (void)s;
  (void)bits;
  (void)ver;
  (void)size;
  return Z_MEM_ERROR;
}

int inflate(z_streamp s, int flush) {
  (void)s;
  (void)flush;
  return Z_DATA_ERROR;
}

int inflateEnd(z_streamp s) {
  (void)s;
  return Z_OK;
}

/* --- getopt -------------------------------------------------------------- */

char* optarg;
int optind = 1, opterr = 1, optopt;
static int optpos;

int getopt(int argc, char* const argv[], const char* opts) {
  const char* a;
  const char* o;
  optarg = NULL;
  if (optind >= argc) return -1;
  a = argv[optind];
  if (optpos == 0) {
    if (a[0] != '-' || a[1] == 0) return -1;
    if (a[1] == '-' && a[2] == 0) {
      optind++;
      return -1;
    }
    optpos = 1;
  }
  optopt = a[optpos++];
  o = strchr(opts, optopt);
  if (!o || optopt == ':') {
    if (!a[optpos]) {
      optind++;
      optpos = 0;
    }
    return '?';
  }
  if (o[1] == ':') {
    if (a[optpos]) optarg = (char*)a + optpos;
    else if (optind + 1 < argc) optarg = argv[++optind];
    else {
      optind++;
      optpos = 0;
      return o[2] == ':' ? optopt : '?';
    }
    optind++;
    optpos = 0;
  } else if (!a[optpos]) {
    optind++;
    optpos = 0;
  }
  return optopt;
}

int getopt_long(int argc, char* const argv[], const char* opts, const struct option* lo,
                int* idx) {
  const char* a;
  int i;
  if (optind >= argc || optpos != 0) return getopt(argc, argv, opts);
  a = argv[optind];
  if (a[0] != '-' || a[1] != '-' || a[2] == 0) return getopt(argc, argv, opts);
  a += 2;
  for (i = 0; lo && lo[i].name; i++) {
    size_t n = strlen(lo[i].name);
    if (strncmp(a, lo[i].name, n) != 0 || (a[n] != 0 && a[n] != '=')) continue;
    optind++;
    optarg = NULL;
    if (lo[i].has_arg != no_argument) {
      if (a[n] == '=') optarg = (char*)a + n + 1;
      else if (lo[i].has_arg == required_argument && optind < argc) optarg = argv[optind++];
    }
    if (idx) *idx = i;
    if (lo[i].flag) {
      *lo[i].flag = lo[i].val;
      return 0;
    }
    return lo[i].val;
  }
  optind++;
  return '?';
}
