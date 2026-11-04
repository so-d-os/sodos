/* shim.c - o que falta na LIBCX/POSIXX/libsodos para o NSPR compilar e ligar.
 *
 * Tudo aqui e a cola do alvo SODOS: sinais que nao existem, fcntl/lockf,
 * select sobre poll, dlopen sobre k_dll_*, conversao de enderecos e soquetes
 * (que ficam para o M4; por enquanto devolvem ENOSYS em vez de derrubar).
 */
#include <so.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <poll.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sodos.h"

/* ---------- sinais (nao ha entrega de sinal no SODOS) ---------- */

int sigemptyset(sigset_t* set) {
  if (!set) return -1;
  memset(set, 0, sizeof(*set));
  return 0;
}

int sigfillset(sigset_t* set) {
  if (!set) return -1;
  memset(set, 0xff, sizeof(*set));
  return 0;
}

int sigaddset(sigset_t* set, int sig) {
  if (!set || sig < 1 || sig >= NSIG) return -1;
  set->val[(sig - 1) / 32] |= 1u << ((sig - 1) % 32);
  return 0;
}

int sigdelset(sigset_t* set, int sig) {
  if (!set || sig < 1 || sig >= NSIG) return -1;
  set->val[(sig - 1) / 32] &= ~(1u << ((sig - 1) % 32));
  return 0;
}

int sigismember(const sigset_t* set, int sig) {
  if (!set || sig < 1 || sig >= NSIG) return -1;
  return (set->val[(sig - 1) / 32] >> ((sig - 1) % 32)) & 1;
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
  (void)sig;
  if (old) memset(old, 0, sizeof(*old));
  (void)act;
  return 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* old) {
  (void)how;
  (void)set;
  if (old) memset(old, 0, sizeof(*old));
  return 0;
}

/* ---------- fcntl/lockf ---------- */

int fcntl(int fd, int cmd, ...) {
  (void)fd;
  switch (cmd) {
    case F_GETFD: return 0;
    case F_SETFD: return 0;
    case F_GETFL: return O_RDWR;
    case F_SETFL: return 0;
    default: errno = ENOSYS; return -1;
  }
}

int lockf(int fd, int cmd, off_t len) {
  (void)fd;
  (void)cmd;
  (void)len;
  return 0;
}

/* ---------- select sobre poll ---------- */

int select(int nfds, fd_set* rd, fd_set* wr, fd_set* ex,
           struct timeval* timeout) {
  struct pollfd pfd[FD_SETSIZE];
  int map[FD_SETSIZE];
  int n = 0, i, rc, ready = 0;
  int timeout_ms = -1;

  if (nfds < 0 || nfds > FD_SETSIZE) {
    errno = EINVAL;
    return -1;
  }
  if (timeout) timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);

  for (i = 0; i < nfds; i++) {
    short ev = 0;
    if (rd && FD_ISSET(i, rd)) ev |= POLLIN;
    if (wr && FD_ISSET(i, wr)) ev |= POLLOUT;
    if (ex && FD_ISSET(i, ex)) ev |= POLLPRI;
    if (!ev) continue;
    /* Arquivo comum esta sempre pronto; o poll da libsodos so conhece
     * o soquete. */
    if (i < SODOS_SOCK_FD_BASE) {
      pfd[n].fd = i;
      pfd[n].events = 0;
      pfd[n].revents = ev;
      map[n] = i;
      n++;
      continue;
    }
    pfd[n].fd = i;
    pfd[n].events = ev;
    pfd[n].revents = 0;
    map[n] = i;
    n++;
  }

  rc = poll(pfd, (unsigned)n, timeout_ms);
  if (rc < 0) return -1;

  if (rd) FD_ZERO(rd);
  if (wr) FD_ZERO(wr);
  if (ex) FD_ZERO(ex);

  for (i = 0; i < n; i++) {
    short re = pfd[i].revents;
    int fd = map[i];
    if (!re) {
      if (pfd[i].events == 0 && pfd[i].revents) re = pfd[i].revents;
      else continue;
    }
    if ((re & (POLLIN | POLLHUP | POLLERR)) && rd) {
      FD_SET(fd, rd);
      ready++;
    }
    if ((re & POLLOUT) && wr) {
      FD_SET(fd, wr);
      ready++;
    }
    if ((re & POLLPRI) && ex) {
      FD_SET(fd, ex);
      ready++;
    }
  }
  return ready;
}

/* ---------- byte order / inet ---------- */

uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }
uint32_t htonl(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}
uint32_t ntohl(uint32_t v) { return htonl(v); }

int inet_aton(const char* cp, struct in_addr* inp) {
  unsigned a, b, c, d;
  char tail;
  if (!cp || !inp) return 0;
  if (sscanf(cp, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
  if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
  inp->s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
  return 1;
}

in_addr_t inet_addr(const char* cp) {
  struct in_addr a;
  if (!inet_aton(cp, &a)) return INADDR_NONE;
  return a.s_addr;
}

char* inet_ntoa(struct in_addr in) {
  static char buf[16];
  uint32_t h = ntohl(in.s_addr);
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (h >> 24) & 255, (h >> 16) & 255,
           (h >> 8) & 255, h & 255);
  return buf;
}

int inet_pton(int af, const char* src, void* dst) {
  if (af != AF_INET || !dst) return 0;
  return inet_aton(src, (struct in_addr*)dst) ? 1 : 0;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
  struct in_addr a;
  const char* s;
  if (af != AF_INET || !src || !dst) return NULL;
  a = *(const struct in_addr*)src;
  s = inet_ntoa(a);
  if (strlen(s) + 1 > (size_t)size) return NULL;
  strcpy(dst, s);
  return dst;
}

/* ---------- DNS ---------- */

int h_errno;

struct hostent* gethostbyname(const char* name) {
  static struct hostent he;
  static char* addrs[2];
  static struct in_addr addr;
  static char hname[256];
  uint32_t ip = 0;

  if (!name || k_dns(name, &ip) != 0) {
    h_errno = HOST_NOT_FOUND;
    return NULL;
  }
  strncpy(hname, name, sizeof(hname) - 1);
  hname[sizeof(hname) - 1] = 0;
  addr.s_addr = htonl(ip);
  addrs[0] = (char*)&addr;
  addrs[1] = NULL;
  he.h_name = hname;
  he.h_aliases = NULL;
  he.h_addrtype = AF_INET;
  he.h_length = 4;
  he.h_addr_list = addrs;
  return &he;
}

struct hostent* gethostbyaddr(const void* addr, socklen_t len, int type) {
  (void)addr;
  (void)len;
  (void)type;
  h_errno = HOST_NOT_FOUND;
  return NULL;
}

struct servent* getservbyname(const char* name, const char* proto) {
  (void)name;
  (void)proto;
  return NULL;
}

struct servent* getservbyport(int port, const char* proto) {
  (void)port;
  (void)proto;
  return NULL;
}

struct protoent* getprotobyname(const char* name) {
  static struct protoent pe;
  if (!name) return NULL;
  if (strcmp(name, "tcp") == 0) pe.p_proto = 6;
  else if (strcmp(name, "udp") == 0) pe.p_proto = 17;
  else if (strcmp(name, "ip") == 0) pe.p_proto = 0;
  else return NULL;
  pe.p_name = (char*)name;
  pe.p_aliases = NULL;
  return &pe;
}

struct protoent* getprotobynumber(int proto) {
  static struct protoent pe;
  pe.p_name = proto == 6 ? "tcp" : proto == 17 ? "udp" : proto == 0 ? "ip" : "";
  pe.p_aliases = NULL;
  pe.p_proto = proto;
  return &pe;
}

int gethostname(char* name, size_t len) {
  if (!name || len < 6) return -1;
  strcpy(name, "sodos");
  return 0;
}

int getnameinfo(const struct sockaddr* addr, socklen_t addrlen, char* host,
                socklen_t hostlen, char* serv, socklen_t servlen, int flags) {
  (void)addrlen;
  (void)flags;
  if (addr && addr->sa_family == AF_INET) {
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    if (host && hostlen) {
      const char* s = inet_ntoa(in->sin_addr);
      if (strlen(s) + 1 > (size_t)hostlen) return -1;
      strcpy(host, s);
    }
    if (serv && servlen) {
      snprintf(serv, servlen, "%u", ntohs(in->sin_port));
    }
    return 0;
  }
  return -1;
}

/* ---------- dlopen sobre o loader SOX ---------- */

void* dlopen(const char* file, int mode) {
  int h;
  (void)mode;
  /* dlopen(NULL) = o programa principal; o NSPR usa para achar simbolos. */
  if (!file) return (void*)(intptr_t)1;
  h = k_dll_load(file);
  return h > 0 ? (void*)(intptr_t)h : NULL;
}

void* dlsym(void* handle, const char* name) {
  if (!handle || handle == (void*)(intptr_t)1) return NULL;
  return k_dll_sym((int)(intptr_t)handle, name);
}

int dlclose(void* handle) {
  if (handle && handle != (void*)(intptr_t)1) k_dll_free((int)(intptr_t)handle);
  return 0;
}

char* dlerror(void) { return NULL; }

/* ---------- soquetes (M4) ---------- */

static int no_socket(void) {
  errno = ENOSYS;
  return -1;
}

int socket(int domain, int type, int protocol) {
  (void)domain;
  (void)type;
  (void)protocol;
  return no_socket();
}

int bind(int fd, const struct sockaddr* addr, socklen_t len) {
  (void)fd;
  (void)addr;
  (void)len;
  return no_socket();
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
  (void)fd;
  (void)addr;
  (void)len;
  return no_socket();
}

int listen(int fd, int backlog) {
  (void)fd;
  (void)backlog;
  return no_socket();
}

int accept(int fd, struct sockaddr* addr, socklen_t* len) {
  (void)fd;
  (void)addr;
  (void)len;
  return no_socket();
}

int send(int fd, const void* buf, size_t n, int flags) {
  (void)fd;
  (void)buf;
  (void)n;
  (void)flags;
  return no_socket();
}

int recv(int fd, void* buf, size_t n, int flags) {
  (void)fd;
  (void)buf;
  (void)n;
  (void)flags;
  return no_socket();
}

int sendto(int fd, const void* buf, size_t n, int flags,
           const struct sockaddr* to, socklen_t tolen) {
  (void)fd;
  (void)buf;
  (void)n;
  (void)flags;
  (void)to;
  (void)tolen;
  return no_socket();
}

int recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* from,
             socklen_t* fromlen) {
  (void)fd;
  (void)buf;
  (void)n;
  (void)flags;
  (void)from;
  (void)fromlen;
  return no_socket();
}

int shutdown(int fd, int how) {
  (void)fd;
  (void)how;
  return no_socket();
}

int setsockopt(int fd, int level, int opt, const void* val, socklen_t len) {
  (void)fd;
  (void)level;
  (void)opt;
  (void)val;
  (void)len;
  return no_socket();
}

int getsockopt(int fd, int level, int opt, void* val, socklen_t* len) {
  (void)fd;
  (void)level;
  (void)opt;
  (void)val;
  (void)len;
  return no_socket();
}

int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
  (void)fd;
  (void)addr;
  (void)len;
  return no_socket();
}

int getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
  (void)fd;
  (void)addr;
  (void)len;
  return no_socket();
}

int ioctl(int fd, unsigned long request, ...) {
  (void)fd;
  (void)request;
  return no_socket();
}

/* ---------- sistema/recursos ---------- */

int uname(struct utsname* u) {
  if (!u) return -1;
  memset(u, 0, sizeof(*u));
  strcpy(u->sysname, "SODOS");
  strcpy(u->nodename, "sodos");
  strcpy(u->release, "1.0");
  strcpy(u->version, "src13");
  strcpy(u->machine, "x86_64");
  return 0;
}

int getpagesize(void) { return 4096; }

int sched_get_priority_min(int policy) {
  (void)policy;
  return 0;
}

int sched_get_priority_max(int policy) {
  (void)policy;
  return 0;
}

int getrusage(int who, struct rusage* usage) {  (void)who;
  if (usage) memset(usage, 0, sizeof(*usage));
  return 0;
}

int getrlimit(int resource, struct rlimit* rlim) {
  if (!rlim) return -1;
  rlim->rlim_cur = (resource == RLIMIT_NOFILE) ? 256 : (4u << 20);
  rlim->rlim_max = rlim->rlim_cur;
  return 0;
}

int setrlimit(int resource, const struct rlimit* rlim) {
  (void)resource;
  (void)rlim;
  return 0;
}

ssize_t readv(int fd, const struct iovec* iov, int cnt) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < cnt; i++) {
    ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
    if (n < 0) return total ? total : -1;
    total += n;
    if ((size_t)n < iov[i].iov_len) break;
  }
  return total;
}

ssize_t writev(int fd, const struct iovec* iov, int cnt) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < cnt; i++) {
    ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (n < 0) return total ? total : -1;
    total += n;
  }
  return total;
}

/* ---------- processos/pipe: o SODOS ainda nao tem fork nem pipe ---------- */

int flock(int fd, int operation) {
  (void)fd;
  (void)operation;
  return 0;
}

int fork(void) {
  errno = ENOSYS;
  return -1;
}

pid_t wait(int* status) {
  if (status) *status = 0;
  errno = ECHILD;
  return -1;
}

pid_t waitpid(pid_t pid, int* status, int options) {
  (void)pid;
  (void)options;
  if (status) *status = 0;
  errno = ECHILD;
  return -1;
}

int kill(pid_t pid, int sig) {
  (void)pid;
  (void)sig;
  errno = ESRCH;
  return -1;
}

int pipe(int fds[2]) {
  (void)fds;
  errno = ENOSYS;
  return -1;
}

int dup2(int oldfd, int newfd) {
  (void)oldfd;
  (void)newfd;
  errno = ENOSYS;
  return -1;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
  (void)domain;
  (void)type;
  (void)protocol;
  (void)sv;
  errno = ENOSYS;
  return -1;
}

ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
  (void)out_fd;
  (void)in_fd;
  (void)offset;
  (void)count;
  errno = ENOSYS;
  return -1;
}

/* Ambiente e processos: o SODOS nao tem argv/envp global nem exec com
 * ambiente; o NSPR so precisa dos simbolos para ligar. */
char* __sodos_env_empty[] = { NULL };
char** environ = __sodos_env_empty;

void _exit(int code) {
  k_exit(code);
  for (;;) {}
}

int execv(const char* path, char* const argv[]) {
  (void)path;
  (void)argv;
  errno = ENOSYS;
  return -1;
}

int execve(const char* path, char* const argv[], char* const envp[]) {
  (void)path;
  (void)argv;
  (void)envp;
  errno = ENOSYS;
  return -1;
}

int sigwait(const sigset_t* set, int* sig) {
  (void)set;
  if (sig) *sig = 0;
  errno = ENOSYS;
  return -1;
}

int pthread_kill(pthread_t t, int sig) {
  (void)t;
  (void)sig;
  return 0;
}
