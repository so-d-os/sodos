/*
 * sodos_posix_extra.c - POSIX que faltava para o QEMU: velocidades e controle
 * do termios (o SODOS nao tem porta serial para processos; tcgetattr ja
 * falha, entao estes so mexem na struct ou devolvem ENOTTY) e glob() sem
 * curingas.
 */
#include <errno.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>

speed_t cfgetispeed(const struct termios* t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios* t) { return t->c_ospeed; }

int cfsetispeed(struct termios* t, speed_t s)
{
  t->c_ispeed = s;
  return 0;
}

int cfsetospeed(struct termios* t, speed_t s)
{
  t->c_ospeed = s;
  t->c_cflag = (t->c_cflag & ~(tcflag_t)CBAUD) | (s & CBAUD);
  return 0;
}

int cfsetspeed(struct termios* t, speed_t s)
{
  cfsetispeed(t, s);
  return cfsetospeed(t, s);
}

void cfmakeraw(struct termios* t)
{
  t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t->c_oflag &= ~(tcflag_t)OPOST;
  t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
  t->c_cflag |= CS8;
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}

int tcflush(int fd, int q) { (void)fd; (void)q; errno = ENOTTY; return -1; }
int tcdrain(int fd) { (void)fd; errno = ENOTTY; return -1; }
int tcsendbreak(int fd, int d) { (void)fd; (void)d; errno = ENOTTY; return -1; }
int tcflow(int fd, int a) { (void)fd; (void)a; errno = ENOTTY; return -1; }

int glob(const char* pattern, int flags, int (*errfunc)(const char*, int), glob_t* g)
{
  struct stat st;
  char** v;
  (void)errfunc;
  if (!(flags & GLOB_APPEND)) {
    g->gl_pathc = 0;
    g->gl_pathv = NULL;
  }
  if (strpbrk(pattern, "*?[") || stat(pattern, &st) != 0) {
    if (!(flags & GLOB_NOCHECK)) return GLOB_NOMATCH;
  }
  v = realloc(g->gl_pathv, (g->gl_pathc + 2) * sizeof *v);
  if (!v) return GLOB_NOSPACE;
  g->gl_pathv = v;
  v[g->gl_pathc] = strdup(pattern);
  if (!v[g->gl_pathc]) return GLOB_NOSPACE;
  g->gl_pathc++;
  v[g->gl_pathc] = NULL;
  return 0;
}

void globfree(glob_t* g)
{
  size_t i;
  for (i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[i]);
  free(g->gl_pathv);
  g->gl_pathv = NULL;
  g->gl_pathc = 0;
}

/* O SODOS nao cria processos nem threads com clone(2). */
int clone(int (*fn)(void*), void* stack, int flags, void* arg, ...)
{
  (void)fn;
  (void)stack;
  (void)flags;
  (void)arg;
  errno = ENOSYS;
  return -1;
}

/* ---- getopt (POSIX, sem permutacao) ------------------------------------- */
#include <stdio.h>

char* optarg;
int optind = 1, opterr = 1, optopt;
static int optpos = 1;

int getopt(int argc, char* const argv[], const char* opts)
{
  const char* p;
  char c;
  optarg = NULL;
  if (optind == 0) { optind = 1; optpos = 1; }
  if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1])
    return -1;
  if (argv[optind][1] == '-' && !argv[optind][2]) {
    optind++;
    return -1;
  }
  c = argv[optind][optpos++];
  p = strchr(opts, c);
  if (!p || c == ':') {
    optopt = c;
    if (opterr && *opts != ':') fprintf(stderr, "%s: opcao invalida -- '%c'\n", argv[0], c);
    if (!argv[optind][optpos]) { optind++; optpos = 1; }
    return '?';
  }
  if (p[1] == ':') {
    if (argv[optind][optpos]) {
      optarg = &argv[optind][optpos];
    } else if (p[2] != ':') {
      if (optind + 1 >= argc) {
        optopt = c;
        optind++;
        optpos = 1;
        if (opterr && *opts != ':') fprintf(stderr, "%s: falta argumento -- '%c'\n", argv[0], c);
        return *opts == ':' ? ':' : '?';
      }
      optarg = argv[++optind];
    }
    optind++;
    optpos = 1;
  } else if (!argv[optind][optpos]) {
    optind++;
    optpos = 1;
  }
  return c;
}

/* ---- stdio, arquivos, rede ---------------------------------------------- */
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/statfs.h>

/* Threads cooperativas: um FILE nunca e disputado no meio de uma chamada. */
void flockfile(FILE* f) { (void)f; }
void funlockfile(FILE* f) { (void)f; }
int ftrylockfile(FILE* f) { (void)f; return 0; }

int fstatfs(int fd, struct statfs* buf)
{
  (void)fd;
  memset(buf, 0, sizeof *buf);
  buf->f_type = MSDOS_SUPER_MAGIC; /* o SODOS so tem FAT */
  buf->f_bsize = 512;
  buf->f_namelen = 255;
  return 0;
}

int posix_fadvise(int fd, off_t off, off_t len, int advice)
{
  (void)fd; (void)off; (void)len; (void)advice;
  return 0; /* conselho: ignorar e valido */
}

int inet_aton(const char* cp, struct in_addr* inp)
{
  unsigned long part[4];
  int n = 0;
  const char* p = cp;
  while (n < 4) {
    char* end;
    if (*p < '0' || *p > '9') return 0;
    part[n++] = strtoul(p, &end, 0);
    p = end;
    if (*p != '.') break;
    p++;
  }
  if (*p && *p != ' ' && *p != '\t' && *p != '\n') return 0;
  {
    unsigned long v;
    switch (n) {
    case 1: v = part[0]; break;
    case 2: if (part[0] > 255 || part[1] > 0xffffff) return 0; v = part[0] << 24 | part[1]; break;
    case 3: if (part[0] > 255 || part[1] > 255 || part[2] > 0xffff) return 0;
            v = part[0] << 24 | part[1] << 16 | part[2]; break;
    default:
      if (part[0] > 255 || part[1] > 255 || part[2] > 255 || part[3] > 255) return 0;
      v = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3];
    }
    if (v > 0xffffffffUL) return 0;
    if (inp) inp->s_addr = htonl((uint32_t)v);
  }
  return 1;
}

/* ---- sem suporte no SODOS (pty, processos, contas, paginas) ------------- */
struct termios;
struct winsize;
struct passwd;

int openpty(int* amaster, int* aslave, char* name, const struct termios* t, const struct winsize* w)
{
  (void)amaster; (void)aslave; (void)name; (void)t; (void)w;
  errno = ENOSYS;
  return -1;
}

char* ptsname(int fd) { (void)fd; errno = ENOSYS; return NULL; }
int daemon(int nochdir, int noclose) { (void)nochdir; (void)noclose; errno = ENOSYS; return -1; }
int initgroups(const char* user, unsigned group) { (void)user; (void)group; errno = ENOSYS; return -1; }
struct passwd* getpwnam(const char* name) { (void)name; return NULL; }

int mincore(void* addr, size_t len, unsigned char* vec)
{
  (void)addr; (void)len; (void)vec;
  errno = ENOSYS;
  return -1;
}

/* ---- math --------------------------------------------------------------- */
/* Em double o produto de dois float e exato, entao so ha um arredondamento a
 * mais no fim (o fma de float de verdade arredondaria uma vez). */
float fmaf(float a, float b, float c) { return (float)((double)a * b + c); }
