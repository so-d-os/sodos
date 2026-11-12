/* sodos_sock.c - soquetes TCP/IPv4 e getaddrinfo sobre os k_sock_* do kernel.
 *
 * Descritores 4000..4063 (pipes em 2000, memoria compartilhada em 3000; a
 * posix-lite manda tudo o que e >= 2000 para sodos_pipe.c, que chega aqui).
 * O kernel faz o TCP sem esperar: connect volta EINPROGRESS e o poll() diz
 * quando conectou, como o NSPR espera. Um soquete sem O_NONBLOCK espera
 * cedendo a vez (threads cooperativas). Sem IPv6 e sem UDP: o Firefox volta
 * para IPv4 e TCP sozinho.
 */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8

#include <so.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "sodos.h"

#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef EISCONN
#define EISCONN 106
#endif
#ifndef EALREADY
#define EALREADY 114
#endif
#ifndef ENETUNREACH
#define ENETUNREACH 101
#endif
#ifndef SOCK_TYPE_MASK
#define SOCK_TYPE_MASK 0xF
#endif
#define FIONREAD_K 0x541B
#define FIONBIO_K 0x5421
#define SO_TYPE_K 3
#define SO_RCVBUF_K 8
#define SO_SNDBUF_K 7

#define SOCK_BASE 4000
#define SOCK_MAX 64

struct ssock {
  int used;
  int h;          /* handle do kernel, 0 = ainda sem connect */
  int nonblock;
  int err;        /* SO_ERROR ainda nao lido */
};

static struct ssock socks[SOCK_MAX];

extern void sodos_sleep_ms(unsigned ms);

int sodos_sock_is(int fd) {
  return fd >= SOCK_BASE && fd < SOCK_BASE + SOCK_MAX && socks[fd - SOCK_BASE].used;
}

static struct ssock* get(int fd) {
  if (!sodos_sock_is(fd)) {
    errno = EBADF;
    return NULL;
  }
  return &socks[fd - SOCK_BASE];
}

static uint32_t ip_of(const struct sockaddr_in* a) {
  const uint8_t* b = (const uint8_t*)&a->sin_addr;
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static void put_addr(struct sockaddr* out, socklen_t* len, uint32_t ip, int port) {
  struct sockaddr_in a;
  uint8_t* b = (uint8_t*)&a.sin_addr;
  socklen_t n;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = (in_port_t)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
  b[0] = (uint8_t)(ip >> 24);
  b[1] = (uint8_t)(ip >> 16);
  b[2] = (uint8_t)(ip >> 8);
  b[3] = (uint8_t)ip;
  if (!out || !len) return;
  n = *len < (socklen_t)sizeof(a) ? *len : (socklen_t)sizeof(a);
  memcpy(out, &a, n);
  *len = sizeof(a);
}

/* errno do kernel para um erro de envio/leitura. */
static int sock_errno(struct ssock* s) {
  int e = s->h ? k_sock_err(s->h) : ENOTCONN;
  return e ? e : ECONNRESET;
}

int socket(int domain, int type, int protocol) {
  int i;
  (void)protocol;
  if (domain != AF_INET) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  if ((type & SOCK_TYPE_MASK) != SOCK_STREAM) {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  for (i = 0; i < SOCK_MAX; i++) {
    if (!socks[i].used) {
      memset(&socks[i], 0, sizeof(socks[i]));
      socks[i].used = 1;
      socks[i].nonblock = (type & SOCK_NONBLOCK) ? 1 : 0;
      return SOCK_BASE + i;
    }
  }
  errno = EMFILE;
  return -1;
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
  struct ssock* s = get(fd);
  const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
  int port, h;
  if (!s) return -1;
  if (!addr || len < (socklen_t)sizeof(struct sockaddr_in) || a->sin_family != AF_INET) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  if (s->h) {
    int m = k_sock_poll(s->h);
    errno = (m & 8) ? EISCONN : EALREADY;
    return -1;
  }
  port = ((a->sin_port & 0xFF) << 8) | ((a->sin_port >> 8) & 0xFF);
  h = k_sock_connect(ip_of(a), port);
  if (h <= 0) {
    errno = ENETUNREACH;
    return -1;
  }
  s->h = h;
  s->err = 0;
  if (s->nonblock) {
    errno = EINPROGRESS;
    return -1;
  }
  for (;;) {
    int m = k_sock_poll(h);
    if (m & 4) {
      errno = k_sock_err(h);
      return -1;
    }
    if (m & 8) return 0;
    sodos_sleep_ms(2);
  }
}

static ssize_t sock_send(struct ssock* s, const void* buf, size_t n) {
  for (;;) {
    int r;
    if (!s->h) {
      errno = ENOTCONN;
      return -1;
    }
    r = k_sock_send(s->h, buf, n > 0x40000000u ? 0x40000000 : (int)n);
    if (r >= 0) return r;
    if (r == -2) {
      errno = sock_errno(s);
      if (errno == ECONNRESET) errno = EPIPE;
      return -1;
    }
    if (s->nonblock) {
      errno = EAGAIN;
      return -1;
    }
    sodos_sleep_ms(2);
  }
}

static ssize_t sock_recv(struct ssock* s, void* buf, size_t n, int peek) {
  for (;;) {
    int r;
    if (!s->h) {
      errno = ENOTCONN;
      return -1;
    }
    r = k_sock_recv(s->h, buf, n > 0x40000000u ? 0x40000000 : (int)n, peek);
    if (r >= 0) return r;
    if (r == -2) {
      errno = sock_errno(s);
      return -1;
    }
    if (s->nonblock) {
      errno = EAGAIN;
      return -1;
    }
    sodos_sleep_ms(2);
  }
}

ssize_t sodos_sock_read(int fd, void* buf, size_t n) {
  struct ssock* s = get(fd);
  return s ? sock_recv(s, buf, n, 0) : -1;
}

ssize_t sodos_sock_write(int fd, const void* buf, size_t n) {
  struct ssock* s = get(fd);
  return s ? sock_send(s, buf, n) : -1;
}

ssize_t sodos_sock_recv(int fd, void* buf, size_t n, int flags) {
  struct ssock* s = get(fd);
  int saved;
  ssize_t r;
  if (!s) return -1;
  saved = s->nonblock;
  if (flags & MSG_DONTWAIT) s->nonblock = 1;
  r = sock_recv(s, buf, n, (flags & MSG_PEEK) != 0);
  s->nonblock = saved;
  return r;
}

ssize_t sodos_sock_send(int fd, const void* buf, size_t n, int flags) {
  struct ssock* s = get(fd);
  int saved;
  ssize_t r;
  if (!s) return -1;
  saved = s->nonblock;
  if (flags & MSG_DONTWAIT) s->nonblock = 1;
  r = sock_send(s, buf, n);
  s->nonblock = saved;
  return r;
}

int sodos_sock_close(int fd) {
  struct ssock* s = get(fd);
  if (!s) return -1;
  if (s->h) k_sock_close(s->h);
  s->used = 0;
  s->h = 0;
  return 0;
}

int sodos_sock_fcntl(int fd, int cmd, long arg) {
  struct ssock* s = get(fd);
  if (!s) return -1;
  switch (cmd) {
    case F_GETFL:
      return O_RDWR | (s->nonblock ? O_NONBLOCK : 0);
    case F_SETFL:
      s->nonblock = (arg & O_NONBLOCK) != 0;
      return 0;
    default:
      return 0; /* F_GETFD/F_SETFD (FD_CLOEXEC): nao ha exec */
  }
}

short sodos_sock_poll(int fd, short events) {
  struct ssock* s = get(fd);
  short ev = 0;
  int m;
  if (!s) return POLLNVAL;
  if (!s->h) return (short)(POLLOUT & events) | POLLHUP;
  m = k_sock_poll(s->h);
  if (m & 4) {
    s->err = 1;
    ev |= POLLERR;
    if (events & POLLIN) ev |= POLLIN;
    if (events & POLLOUT) ev |= POLLOUT;
    return ev;
  }
  if ((m & 1) && (events & POLLIN)) ev |= POLLIN;
  if ((m & 2) && (events & POLLOUT)) ev |= POLLOUT;
  return ev;
}

int getsockopt(int fd, int level, int opt, void* val, socklen_t* len) {
  struct ssock* s = get(fd);
  int v = 0;
  if (!s) return -1;
  if (level == SOL_SOCKET && opt == SO_ERROR) {
    v = 0;
    if (s->h && (k_sock_poll(s->h) & 4)) v = k_sock_err(s->h);
  } else if (level == SOL_SOCKET && opt == SO_TYPE_K) {
    v = SOCK_STREAM;
  } else if (level == SOL_SOCKET && (opt == SO_RCVBUF_K || opt == SO_SNDBUF_K)) {
    v = 65536;
  }
  if (val && len && *len >= (socklen_t)sizeof(int)) {
    memcpy(val, &v, sizeof(int));
    *len = sizeof(int);
  }
  return 0;
}

int setsockopt(int fd, int level, int opt, const void* val, socklen_t len) {
  (void)level;
  (void)opt;
  (void)val;
  (void)len;
  return get(fd) ? 0 : -1;
}

int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
  struct ssock* s = get(fd);
  unsigned lip = 0;
  int lport = 0;
  if (!s) return -1;
  if (s->h) k_sock_info(s->h, &lip, &lport, NULL, NULL);
  put_addr(addr, len, lip, lport);
  return 0;
}

int getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
  struct ssock* s = get(fd);
  unsigned rip = 0;
  int rport = 0;
  if (!s) return -1;
  if (!s->h || !(k_sock_poll(s->h) & 8)) {
    errno = ENOTCONN;
    return -1;
  }
  k_sock_info(s->h, NULL, NULL, &rip, &rport);
  put_addr(addr, len, rip, rport);
  return 0;
}

int shutdown(int fd, int how) {
  (void)how;
  return get(fd) ? 0 : -1;
}

int ioctl(int fd, unsigned long req, ...) {
  va_list ap;
  void* arg;
  struct ssock* s;
  va_start(ap, req);
  arg = va_arg(ap, void*);
  va_end(ap);
  if (!sodos_sock_is(fd)) {
    errno = ENOSYS;
    return -1;
  }
  s = get(fd);
  if (req == FIONBIO_K) {
    s->nonblock = arg && *(int*)arg;
    return 0;
  }
  if (req == FIONREAD_K) {
    static char peekbuf[16384];
    int n = 0;
    if (s->h) n = k_sock_recv(s->h, peekbuf, (int)sizeof(peekbuf), 1);
    if (arg) *(int*)arg = n > 0 ? n : 0;
    return 0;
  }
  errno = EINVAL;
  return -1;
}

/* ---- nomes --------------------------------------------------------------- */

static int parse_ipv4(const char* s, uint32_t* out) {
  uint32_t v = 0;
  int parts = 0;
  while (parts < 4) {
    int n = 0, digits = 0;
    while (*s >= '0' && *s <= '9') {
      n = n * 10 + (*s - '0');
      s++;
      if (++digits > 3 || n > 255) return 0;
    }
    if (!digits) return 0;
    v = (v << 8) | (uint32_t)n;
    parts++;
    if (parts < 4) {
      if (*s != '.') return 0;
      s++;
    }
  }
  if (*s) return 0;
  *out = v;
  return 1;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** res) {
  struct addrinfo* ai;
  struct sockaddr_in* sa;
  uint32_t ip = 0;
  int port = 0;
  int family = hints ? hints->ai_family : AF_UNSPEC;
  int socktype = hints ? hints->ai_socktype : 0;
  if (!res) return EAI_FAIL;
  *res = NULL;
  if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
  if (socktype && socktype != SOCK_STREAM) return EAI_SOCKTYPE;
  if (service && *service) {
    char* end;
    long p = strtol(service, &end, 10);
    if (*end || p < 0 || p > 65535) return EAI_SERVICE;
    port = (int)p;
  }
  if (!node || !*node) {
    ip = (hints && (hints->ai_flags & AI_PASSIVE)) ? 0 : 0x7F000001u;
  } else if (!parse_ipv4(node, &ip)) {
    unsigned got = 0;
    if (hints && (hints->ai_flags & AI_NUMERICHOST)) return EAI_NONAME;
    if (strcmp(node, "localhost") == 0) {
      ip = 0x7F000001u;
    } else {
      int rc = k_net_ready() ? k_dns(node, &got) : -99;
      if (rc != 0 || got == 0) return EAI_NONAME;
      ip = got;
    }
  }
  ai = (struct addrinfo*)calloc(1, sizeof(*ai) + sizeof(*sa));
  if (!ai) return EAI_MEMORY;
  sa = (struct sockaddr_in*)(ai + 1);
  {
    socklen_t n = sizeof(*sa);
    put_addr((struct sockaddr*)sa, &n, ip, port);
  }
  ai->ai_family = AF_INET;
  ai->ai_socktype = SOCK_STREAM;
  ai->ai_protocol = IPPROTO_TCP;
  ai->ai_addrlen = sizeof(*sa);
  ai->ai_addr = (struct sockaddr*)sa;
  if (hints && (hints->ai_flags & AI_CANONNAME) && node) ai->ai_canonname = strdup(node);
  *res = ai;
  return 0;
}

/* gethostbyname*_r do glibc (devolvem int). O NSPR do Firefox usa estes:
 * sem IPv6 (o socket AF_INET6 falha) ele nem chama o getaddrinfo. */
extern int h_errno;
#define HOST_NOT_FOUND_K 1
#define NO_RECOVERY_K 3

static int host_lookup(const char* name, uint32_t* ip) {
  unsigned got = 0;
  if (!name || !*name) return -1;
  if (parse_ipv4(name, ip)) return 0;
  if (strcmp(name, "localhost") == 0) {
    *ip = 0x7F000001u;
    return 0;
  }
  if (!k_net_ready() || k_dns(name, &got) != 0 || got == 0) return -1;
  *ip = got;
  return 0;
}

/* Monta o hostent em buf: endereco, lista de enderecos, aliases vazios, nome. */
static int fill_hostent(const char* name, uint32_t ip, struct hostent* he, char* buf,
                        size_t len) {
  size_t nlen = strlen(name) + 1;
  char** list;
  char** aliases;
  uint8_t* addr;
  uintptr_t p = ((uintptr_t)buf + 7u) & ~(uintptr_t)7u;
  size_t need = (p - (uintptr_t)buf) + 3 * sizeof(char*) + 8 + nlen;
  if (need > len) return -1;
  list = (char**)p;
  aliases = list + 2;
  addr = (uint8_t*)(aliases + 1);
  addr[0] = (uint8_t)(ip >> 24);
  addr[1] = (uint8_t)(ip >> 16);
  addr[2] = (uint8_t)(ip >> 8);
  addr[3] = (uint8_t)ip;
  list[0] = (char*)addr;
  list[1] = NULL;
  aliases[0] = NULL;
  memcpy(addr + 8, name, nlen);
  he->h_name = (char*)(addr + 8);
  he->h_aliases = aliases;
  he->h_addrtype = AF_INET;
  he->h_length = 4;
  he->h_addr_list = list;
  return 0;
}

int gethostbyname2_r(const char* name, int af, struct hostent* ret, char* buf, size_t buflen,
                     struct hostent** result, int* herr) {
  uint32_t ip;
  *result = NULL;
  if (af != AF_INET) {
    if (herr) *herr = HOST_NOT_FOUND_K;
    return EAFNOSUPPORT;
  }
  if (host_lookup(name, &ip) != 0) {
    if (herr) *herr = HOST_NOT_FOUND_K;
    return ENOENT;
  }
  if (fill_hostent(name, ip, ret, buf, buflen) != 0) {
    if (herr) *herr = NO_RECOVERY_K;
    return ERANGE;
  }
  if (herr) *herr = 0;
  *result = ret;
  return 0;
}

int gethostbyname_r(const char* name, struct hostent* ret, char* buf, size_t buflen,
                     struct hostent** result, int* herr) {
  return gethostbyname2_r(name, AF_INET, ret, buf, buflen, result, herr);
}

struct hostent* gethostbyname2(const char* name, int af) {
  static struct hostent he;
  static char buf[512];
  struct hostent* r = NULL;
  int err = 0;
  gethostbyname2_r(name, af, &he, buf, sizeof(buf), &r, &err);
  h_errno = err;
  return r;
}

struct hostent* gethostbyname(const char* name) { return gethostbyname2(name, AF_INET); }

void freeaddrinfo(struct addrinfo* ai) {
  while (ai) {
    struct addrinfo* next = ai->ai_next;
    free(ai->ai_canonname);
    free(ai);
    ai = next;
  }
}

const char* gai_strerror(int e) {
  switch (e) {
    case EAI_NONAME: return "nome nao encontrado";
    case EAI_FAMILY: return "familia de endereco nao suportada (so IPv4)";
    case EAI_MEMORY: return "sem memoria";
    default: return "erro de getaddrinfo";
  }
}

#endif /* LP64 */
