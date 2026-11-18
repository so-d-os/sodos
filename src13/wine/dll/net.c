/*
 * net.c - WS2_32/MSWSOCK sobre os soquetes do SODOS (k_sock_*): TCP cliente.
 *
 * O modelo que os programas modernos usam (o runtime do Go, por exemplo) e
 * E/S sobreposta com porta de conclusao: WSARecv/WSASend/ConnectEx devolvem
 * ERROR_IO_PENDING e o resultado chega por GetQueuedCompletionStatusEx. Os
 * soquetes do SODOS nao bloqueiam e nao avisam; entao cada soquete guarda a
 * operacao pendente e wnet_poll() (chamado pelas esperas da porta, obj.c)
 * tenta de novo e posta a conclusao. Sockets bloqueantes (connect/recv/send
 * sem OVERLAPPED) esperam em laco curto.
 *
 * OVERLAPPED.Internal recebe um NTSTATUS (0 = ok), como no Windows.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

int wiocp_post(struct wobj* port, DWORD bytes, uintptr_t key, void* ov); /* obj.c */

#define WSAEINTR 10004
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEWOULDBLOCK 10035
#define WSAENOTSOCK 10038
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAENOTCONN 10057
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAHOST_NOT_FOUND 11001
#define SOCKET_ERROR (-1)

#define ST_CANCELLED 0xC0000120u
#define ST_REFUSED 0xC0000236u
#define ST_RESET 0xC000020Du
#define ST_TIMEOUT 0xC00000B5u

struct overlapped {
  uintptr_t internal, internal_high;
  DWORD off, off_high;
  HANDLE event;
};
struct wsabuf {
  uint32_t len;
  char* buf;
};
struct sockaddr_in4 {
  uint16_t family;
  uint16_t port;      /* ordem de rede */
  uint8_t addr[4];
  uint8_t zero[8];
};

/* estado do soquete, em wobj.p2 */
struct wsock {
  int k;                    /* handle do SODOS; 0 = ainda nao conectou */
  int connected, failed;
  int nonblock;
  int skip_ok;              /* FILE_SKIP_COMPLETION_PORT_ON_SUCCESS */
  /* operacoes pendentes */
  struct overlapped* c_ov;
  struct overlapped* r_ov;
  char* r_buf;
  uint32_t r_len;
  struct overlapped* s_ov;
  const char* s_buf;
  uint32_t s_len;
  struct wsock* next;
  struct wobj* obj;
};

struct wnet {
  struct wsock* socks;
  int started;
};

static struct wnet* net_get(void) {
  struct wproc* p = wp();
  if (!p->net) p->net = wzalloc(sizeof(struct wnet));
  return (struct wnet*)p->net;
}

static struct wsock* sock_of(uintptr_t s, struct wobj** obj) {
  struct wobj* o = wh_get((HANDLE)s, OBJ_SOCKET);
  if (obj) *obj = o;
  if (!o) {
    wset_lasterr(WSAENOTSOCK);
    return 0;
  }
  return (struct wsock*)o->p2;
}

static DWORD st_to_wsa(uintptr_t st) {
  switch ((uint32_t)st) {
    case 0: return 0;
    case ST_CANCELLED: return 995;
    case ST_REFUSED: return WSAECONNREFUSED;
    case ST_RESET: return WSAECONNRESET;
    case ST_TIMEOUT: return WSAETIMEDOUT;
  }
  return WSAECONNRESET;
}

static void complete(struct wsock* s, struct overlapped* ov, uint32_t status, uint32_t bytes) {
  struct wobj* o = s->obj;
  ov->internal = status;
  ov->internal_high = bytes;
  if (o->p1) wiocp_post((struct wobj*)o->p1, bytes, (uintptr_t)o->u1, ov);
  if (ov->event) {
    struct wobj* e = wh_get(ov->event, OBJ_EVENT);
    if (e) {
      e->signaled = 1;
      wsignal();
    }
  }
}

/* Anda as operacoes pendentes de um soquete. */
static void sock_step(struct wsock* s) {
  int st;
  if (!s->k) return;
  st = k_sock_poll(s->k);
  if (s->c_ov) {
    if (st & 8) {
      struct overlapped* ov = s->c_ov;
      s->c_ov = 0;
      s->connected = 1;
      complete(s, ov, 0, 0);
    } else if (st & 4) {
      struct overlapped* ov = s->c_ov;
      s->c_ov = 0;
      s->failed = 1;
      complete(s, ov, ST_REFUSED, 0);
    }
  } else if (!s->connected && (st & 8)) {
    s->connected = 1;
  }
  if (s->r_ov && (st & (1 | 4))) {
    int n = k_sock_recv(s->k, s->r_buf, (int)s->r_len, 0);
    if (n != -1) {
      struct overlapped* ov = s->r_ov;
      s->r_ov = 0;
      complete(s, ov, n < 0 ? ST_RESET : 0, n < 0 ? 0 : (uint32_t)n);
    }
  }
  if (s->s_ov && (st & (2 | 4))) {
    int n = k_sock_send(s->k, s->s_buf, (int)s->s_len);
    if (n != -1) {
      struct overlapped* ov = s->s_ov;
      s->s_ov = 0;
      complete(s, ov, n < 0 ? ST_RESET : 0, n < 0 ? 0 : (uint32_t)n);
    }
  }
}

static volatile int poll_busy;
void wnet_poll(void) {
  struct wnet* n = (struct wnet*)wp()->net;
  struct wsock* s;
  int pending = 0;
  if (!n || !n->socks) return;
  /* uma thread por vez mexe nos soquetes */
  if (__sync_lock_test_and_set(&poll_busy, 1)) return;
  /* sem operacao pendente nao ha o que conferir: nada de chamar o kernel */
  for (s = n->socks; s; s = s->next)
    if (s->c_ov || s->r_ov || s->s_ov) pending = 1;
  if (pending) {
    k_net_pump();
    for (s = n->socks; s; s = s->next) sock_step(s);
  }
  __sync_lock_release(&poll_busy);
}

static void poll_wait(int ms) {
  static volatile int w;
  wnet_poll();
  k_futex_wait(&w, 0, ms);
}

/* ---------- API ---------- */
static WINAPI int WSAStartup(uint16_t ver, uint8_t* data) {
  (void)ver;
  if (data) {
    memset(data, 0, 408);
    data[0] = 2;
    data[1] = 2;
    data[2] = 2;
    data[3] = 2;
    strcpy((char*)data + 4, "SODOS Winsock");
  }
  net_get();
  return 0;
}
static WINAPI int WSACleanup(void) { return 0; }
static WINAPI int WSAGetLastError(void) { return (int)wget_lasterr(); }
static WINAPI void WSASetLastError(int e) { wset_lasterr((DWORD)e); }

static uintptr_t new_socket(int af, int type) {
  struct wnet* n = net_get();
  struct wobj* o;
  struct wsock* s;
  HANDLE h;
  if (af != 2 /* AF_INET */ || type != 1 /* SOCK_STREAM */) {
    wset_lasterr(WSAEAFNOSUPPORT);
    return (uintptr_t)-1;
  }
  o = wobj_new(OBJ_SOCKET);
  s = (struct wsock*)wzalloc(sizeof(*s));
  if (!o || !s || !n) {
    wset_lasterr(WSAENOBUFS);
    return (uintptr_t)-1;
  }
  o->p2 = s;
  s->obj = o;
  h = wh_new(o);
  while (__sync_lock_test_and_set(&poll_busy, 1)) k_thread_yield();
  s->next = n->socks;
  n->socks = s;
  __sync_lock_release(&poll_busy);
  return (uintptr_t)h;
}
static WINAPI uintptr_t WSASocketW(int af, int type, int proto, void* info, DWORD g, DWORD fl) {
  (void)proto; (void)info; (void)g; (void)fl;
  return new_socket(af, type);
}
static WINAPI uintptr_t ms_socket(int af, int type, int proto) {
  (void)proto;
  return new_socket(af, type);
}

void wsock_close(struct wobj* o) {
  struct wnet* n = (struct wnet*)wp()->net;
  struct wsock* s = (struct wsock*)o->p2;
  struct wsock** pp;
  if (!s) return;
  while (__sync_lock_test_and_set(&poll_busy, 1)) k_thread_yield();
  if (n)
    for (pp = &n->socks; *pp; pp = &(*pp)->next)
      if (*pp == s) {
        *pp = s->next;
        break;
      }
  __sync_lock_release(&poll_busy);
  /* operacoes pendentes terminam canceladas */
  if (s->c_ov) complete(s, s->c_ov, ST_CANCELLED, 0);
  if (s->r_ov) complete(s, s->r_ov, ST_CANCELLED, 0);
  if (s->s_ov) complete(s, s->s_ov, ST_CANCELLED, 0);
  if (s->k) k_sock_close(s->k);
  k_free(s);
  o->p2 = 0;
}
static WINAPI int closesocket(uintptr_t s) {
  if (!wh_get((HANDLE)s, OBJ_SOCKET)) {
    wset_lasterr(WSAENOTSOCK);
    return SOCKET_ERROR;
  }
  wh_close((HANDLE)s);
  return 0;
}
static WINAPI int ms_shutdown(uintptr_t s, int how) {
  (void)s;
  (void)how;
  return 0;
}
static WINAPI int ms_bind(uintptr_t s, const void* addr, int len) {
  (void)addr;
  (void)len;
  return sock_of(s, 0) ? 0 : SOCKET_ERROR;
}
static WINAPI int ms_setsockopt(uintptr_t s, int lvl, int opt, const char* v, int len) {
  (void)lvl; (void)opt; (void)v; (void)len;
  return sock_of(s, 0) ? 0 : SOCKET_ERROR;
}
static WINAPI int ms_getsockopt(uintptr_t s, int lvl, int opt, char* v, int* len) {
  struct wsock* k = sock_of(s, 0);
  (void)lvl;
  if (!k) return SOCKET_ERROR;
  if (v && len && *len >= 4) {
    *(int*)v = 0;
    if (opt == 0x1007 /* SO_ERROR */ && k->failed) *(int*)v = WSAECONNREFUSED;
    if (opt == 0x1008 /* SO_TYPE */) *(int*)v = 1;
    *len = 4;
  }
  return 0;
}
static WINAPI int ioctlsocket(uintptr_t s, long cmd, DWORD* arg) {
  struct wsock* k = sock_of(s, 0);
  if (!k) return SOCKET_ERROR;
  if ((DWORD)cmd == 0x8004667Eu) k->nonblock = arg && *arg; /* FIONBIO */
  if ((DWORD)cmd == 0x4004667Fu && arg) *arg = 0;            /* FIONREAD */
  return 0;
}

static int start_connect(struct wsock* k, const struct sockaddr_in4* a) {
  unsigned ip = ((unsigned)a->addr[0] << 24) | ((unsigned)a->addr[1] << 16) | ((unsigned)a->addr[2] << 8) |
                a->addr[3];
  int port = (a->port >> 8) | ((a->port & 0xFF) << 8);
  int h;
  if (a->family != 2) {
    wset_lasterr(WSAEAFNOSUPPORT);
    return 0;
  }
  h = k_sock_connect(ip, port);
  if (h <= 0) {
    wset_lasterr(WSAENOBUFS);
    return 0;
  }
  k->k = h;
  return 1;
}

static WINAPI BOOL ConnectEx(uintptr_t s, const struct sockaddr_in4* addr, int len, void* sbuf, DWORD slen,
                             DWORD* sent, struct overlapped* ov) {
  struct wsock* k = sock_of(s, 0);
  (void)len; (void)sbuf; (void)slen;
  if (!k) return 0;
  if (sent) *sent = 0;
  if (!start_connect(k, addr)) return 0;
  ov->internal = 0x103; /* STATUS_PENDING */
  k->c_ov = ov;
  wsignal(); /* quem espera na porta passa a vigiar a rede */
  wset_lasterr(ERROR_IO_PENDING);
  return 0;
}
static WINAPI int ms_connect(uintptr_t s, const struct sockaddr_in4* addr, int len) {
  struct wsock* k = sock_of(s, 0);
  uint64_t t0 = wnow_ms();
  (void)len;
  if (!k) return SOCKET_ERROR;
  if (!start_connect(k, addr)) return SOCKET_ERROR;
  if (k->nonblock) {
    wset_lasterr(WSAEWOULDBLOCK);
    return SOCKET_ERROR;
  }
  for (;;) {
    int st;
    poll_wait(2);
    st = k_sock_poll(k->k);
    if (st & 8) {
      k->connected = 1;
      return 0;
    }
    if ((st & 4) || wnow_ms() - t0 > 30000) {
      k->failed = 1;
      wset_lasterr((st & 4) ? WSAECONNREFUSED : WSAETIMEDOUT);
      return SOCKET_ERROR;
    }
  }
}

static WINAPI int WSARecv(uintptr_t s, struct wsabuf* bufs, DWORD nb, DWORD* got, DWORD* flags,
                          struct overlapped* ov, void* fn) {
  struct wsock* k = sock_of(s, 0);
  int n;
  (void)fn;
  if (!k) return SOCKET_ERROR;
  if (got) *got = 0;
  if (flags) *flags = 0;
  if (!nb || !k->k) {
    wset_lasterr(WSAENOTCONN);
    return SOCKET_ERROR;
  }
  for (;;) {
    n = bufs[0].len ? k_sock_recv(k->k, bufs[0].buf, (int)bufs[0].len, 0) : 0;
    if (n >= 0) {
      if (got) *got = (DWORD)n;
      if (ov) {
        ov->internal = 0;
        ov->internal_high = (uintptr_t)n;
        if (!k->skip_ok) complete(k, ov, 0, (uint32_t)n);
      }
      return 0;
    }
    if (n == -2) {
      wset_lasterr(WSAECONNRESET);
      return SOCKET_ERROR;
    }
    if (ov) {
      ov->internal = 0x103;
      k->r_buf = bufs[0].buf;
      k->r_len = bufs[0].len;
      k->r_ov = ov;
      wsignal();
      wset_lasterr(ERROR_IO_PENDING);
      return SOCKET_ERROR;
    }
    if (k->nonblock) {
      wset_lasterr(WSAEWOULDBLOCK);
      return SOCKET_ERROR;
    }
    poll_wait(2);
  }
}
static WINAPI int WSASend(uintptr_t s, struct wsabuf* bufs, DWORD nb, DWORD* sent, DWORD flags,
                          struct overlapped* ov, void* fn) {
  struct wsock* k = sock_of(s, 0);
  int n;
  (void)fn;
  (void)flags;
  if (!k) return SOCKET_ERROR;
  if (sent) *sent = 0;
  if (!nb || !k->k) {
    wset_lasterr(WSAENOTCONN);
    return SOCKET_ERROR;
  }
  for (;;) {
    n = bufs[0].len ? k_sock_send(k->k, bufs[0].buf, (int)bufs[0].len) : 0;
    if (n >= 0) {
      if (sent) *sent = (DWORD)n;
      if (ov) {
        ov->internal = 0;
        ov->internal_high = (uintptr_t)n;
        if (!k->skip_ok) complete(k, ov, 0, (uint32_t)n);
      }
      return 0;
    }
    if (n == -2) {
      wset_lasterr(WSAECONNRESET);
      return SOCKET_ERROR;
    }
    if (ov) {
      ov->internal = 0x103;
      k->s_buf = bufs[0].buf;
      k->s_len = bufs[0].len;
      k->s_ov = ov;
      wsignal();
      wset_lasterr(ERROR_IO_PENDING);
      return SOCKET_ERROR;
    }
    if (k->nonblock) {
      wset_lasterr(WSAEWOULDBLOCK);
      return SOCKET_ERROR;
    }
    poll_wait(2);
  }
}
static WINAPI int ms_recv(uintptr_t s, char* buf, int len, int flags) {
  struct wsabuf b;
  DWORD got = 0, fl = 0;
  (void)flags;
  b.len = (uint32_t)len;
  b.buf = buf;
  return WSARecv(s, &b, 1, &got, &fl, 0, 0) == 0 ? (int)got : SOCKET_ERROR;
}
static WINAPI int ms_send(uintptr_t s, const char* buf, int len, int flags) {
  struct wsabuf b;
  DWORD sent = 0;
  b.len = (uint32_t)len;
  b.buf = (char*)buf;
  return WSASend(s, &b, 1, &sent, (DWORD)flags, 0, 0) == 0 ? (int)sent : SOCKET_ERROR;
}

/* ReadFile/WriteFile num soquete (file.c) */
int wsock_read(struct wobj* o, void* buf, DWORD n, DWORD* done, void* ov) {
  struct wsabuf b;
  DWORD fl = 0;
  uintptr_t h = 0;
  int i;
  for (i = 1; i < WHANDLE_MAX; i++)
    if (wp()->handles[i] == o) h = 0x200u + (unsigned)i * 4u;
  b.len = n;
  b.buf = (char*)buf;
  return WSARecv(h, &b, 1, done, &fl, (struct overlapped*)ov, 0) == 0;
}
int wsock_write(struct wobj* o, const void* buf, DWORD n, DWORD* done, void* ov) {
  struct wsabuf b;
  uintptr_t h = 0;
  int i;
  for (i = 1; i < WHANDLE_MAX; i++)
    if (wp()->handles[i] == o) h = 0x200u + (unsigned)i * 4u;
  b.len = n;
  b.buf = (char*)buf;
  return WSASend(h, &b, 1, done, 0, (struct overlapped*)ov, 0) == 0;
}

static WINAPI BOOL WSAGetOverlappedResult(uintptr_t s, struct overlapped* ov, DWORD* bytes, BOOL wait, DWORD* flags) {
  (void)s;
  (void)wait;
  if (flags) *flags = 0;
  if (ov->internal == 0x103) {
    wset_lasterr(996); /* WSA_IO_INCOMPLETE */
    return 0;
  }
  if (bytes) *bytes = (DWORD)ov->internal_high;
  if (ov->internal) {
    wset_lasterr(st_to_wsa(ov->internal));
    return 0;
  }
  return 1;
}
static WINAPI BOOL GetOverlappedResult(HANDLE h, struct overlapped* ov, DWORD* bytes, BOOL wait) {
  return WSAGetOverlappedResult((uintptr_t)h, ov, bytes, wait, 0);
}
static WINAPI BOOL CancelIoEx(HANDLE h, struct overlapped* ov) {
  struct wobj* o = wh_get(h, OBJ_SOCKET);
  struct wsock* k = o ? (struct wsock*)o->p2 : 0;
  int n = 0;
  if (!k) {
    wset_lasterr(1168); /* ERROR_NOT_FOUND */
    return 0;
  }
  while (__sync_lock_test_and_set(&poll_busy, 1)) k_thread_yield();
  if (k->c_ov && (!ov || k->c_ov == ov)) {
    struct overlapped* v = k->c_ov;
    k->c_ov = 0;
    complete(k, v, ST_CANCELLED, 0);
    n++;
  }
  if (k->r_ov && (!ov || k->r_ov == ov)) {
    struct overlapped* v = k->r_ov;
    k->r_ov = 0;
    complete(k, v, ST_CANCELLED, 0);
    n++;
  }
  if (k->s_ov && (!ov || k->s_ov == ov)) {
    struct overlapped* v = k->s_ov;
    k->s_ov = 0;
    complete(k, v, ST_CANCELLED, 0);
    n++;
  }
  __sync_lock_release(&poll_busy);
  if (!n) {
    wset_lasterr(1168);
    return 0;
  }
  return 1;
}
static WINAPI BOOL CancelIo(HANDLE h) { return CancelIoEx(h, 0); }
static WINAPI BOOL SetFileCompletionNotificationModes(HANDLE h, uint8_t flags) {
  struct wobj* o = wh_get(h, OBJ_SOCKET);
  if (o && o->p2) ((struct wsock*)o->p2)->skip_ok = flags & 1;
  return 1;
}

static void put_addr(struct sockaddr_in4* a, unsigned ip, int port) {
  memset(a, 0, sizeof(*a));
  a->family = 2;
  a->port = (uint16_t)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
  a->addr[0] = (uint8_t)(ip >> 24);
  a->addr[1] = (uint8_t)(ip >> 16);
  a->addr[2] = (uint8_t)(ip >> 8);
  a->addr[3] = (uint8_t)ip;
}
static int name_of(uintptr_t s, struct sockaddr_in4* a, int* len, int peer) {
  struct wsock* k = sock_of(s, 0);
  unsigned lip = 0, rip = 0;
  int lport = 0, rport = 0;
  if (!k) return SOCKET_ERROR;
  if (!len || *len < 16) {
    wset_lasterr(WSAEFAULT);
    return SOCKET_ERROR;
  }
  if (k->k) k_sock_info(k->k, &lip, &lport, &rip, &rport);
  else if (peer) {
    wset_lasterr(WSAENOTCONN);
    return SOCKET_ERROR;
  }
  put_addr(a, peer ? rip : lip, peer ? rport : lport);
  *len = 16;
  return 0;
}
static WINAPI int getsockname(uintptr_t s, struct sockaddr_in4* a, int* len) { return name_of(s, a, len, 0); }
static WINAPI int getpeername(uintptr_t s, struct sockaddr_in4* a, int* len) { return name_of(s, a, len, 1); }

static WINAPI BOOL AcceptEx_stub(void) {
  wset_lasterr(WSAEOPNOTSUPP);
  return 0;
}
static WINAPI int WSAIoctl(uintptr_t s, DWORD code, const DWORD* in, DWORD inlen, void* out, DWORD outlen,
                           DWORD* ret, void* ov, void* fn) {
  (void)s; (void)inlen; (void)ov; (void)fn;
  if (code == 0xC8000006u && in && out && outlen >= 8) { /* SIO_GET_EXTENSION_FUNCTION_POINTER */
    *(void**)out = in[0] == 0x25A207B9u ? (void*)ConnectEx : (void*)AcceptEx_stub;
    if (ret) *ret = 8;
    return 0;
  }
  if (ret) *ret = 0;
  if (code == 0x98000004u) return 0; /* SIO_KEEPALIVE_VALS */
  wset_lasterr(WSAEOPNOTSUPP);
  return SOCKET_ERROR;
}
static WINAPI int WSAEnumProtocolsW(int* protos, void* buf, DWORD* len) {
  (void)protos;
  (void)buf;
  if (len) *len = 0;
  wset_lasterr(WSAENOBUFS);
  return SOCKET_ERROR;
}

/* ---------- nomes ---------- */
struct addrinfow {
  int flags, family, socktype, protocol;
  size_t addrlen;
  wchar16* canonname;
  struct sockaddr_in4* addr;
  struct addrinfow* next;
};

static int parse_ip(const char* s, unsigned* ip) {
  unsigned v = 0, part = 0;
  int dots = 0, digits = 0;
  for (; *s; s++) {
    if (*s >= '0' && *s <= '9') {
      part = part * 10 + (unsigned)(*s - '0');
      if (part > 255) return 0;
      digits++;
    } else if (*s == '.' && digits) {
      v = (v << 8) | part;
      part = 0;
      digits = 0;
      dots++;
    } else {
      return 0;
    }
  }
  if (dots != 3 || !digits) return 0;
  *ip = (v << 8) | part;
  return 1;
}

static WINAPI int GetAddrInfoW(const wchar16* node, const wchar16* service, const struct addrinfow* hints,
                               struct addrinfow** res) {
  char host[256];
  unsigned ip = 0;
  int port = 0, n;
  struct addrinfow* ai;
  *res = 0;
  if (hints && hints->family == 23) return 11004; /* AF_INET6: WSANO_DATA */
  n = node ? wutf16_to_8(node, -1, host, (int)sizeof(host) - 1) : 0;
  host[n] = 0;
  if (service) {
    const wchar16* q;
    for (q = service; *q >= '0' && *q <= '9'; q++) port = port * 10 + (*q - '0');
  }
  if (!n) {
    ip = (hints && (hints->flags & 1)) ? 0 : 0x7F000001u; /* AI_PASSIVE */
  } else if (wstreq(host, "localhost")) {
    ip = 0x7F000001u;
  } else if (!parse_ip(host, &ip)) {
    if (k_dns(host, &ip) != 0) {
      if (wine_debug) wlog("DNS falhou: ", host);
      return WSAHOST_NOT_FOUND;
    }
  }
  ai = (struct addrinfow*)wzalloc(sizeof(*ai) + sizeof(struct sockaddr_in4));
  if (!ai) return 8;
  ai->family = 2;
  ai->socktype = hints && hints->socktype ? hints->socktype : 1;
  ai->protocol = hints && hints->protocol ? hints->protocol : 6;
  ai->addrlen = 16;
  ai->addr = (struct sockaddr_in4*)(ai + 1);
  put_addr(ai->addr, ip, port);
  *res = ai;
  return 0;
}
static WINAPI void FreeAddrInfoW(struct addrinfow* ai) {
  while (ai) {
    struct addrinfow* n = ai->next;
    k_free(ai);
    ai = n;
  }
}
static WINAPI int ms_gethostname(char* buf, int len) {
  if (len < 6) return SOCKET_ERROR;
  strcpy(buf, "SODOS");
  return 0;
}
static WINAPI uint16_t ms_htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static WINAPI uint32_t ms_htonl(uint32_t v) { return __builtin_bswap32(v); }

/* iphlpapi: sem adaptadores para listar (o Go so usa para net.Interfaces). */
static WINAPI DWORD GetAdaptersAddresses(DWORD fam, DWORD fl, void* r, void* buf, DWORD* len) {
  (void)fam; (void)fl; (void)r; (void)buf;
  if (len) *len = 0;
  return 232; /* ERROR_NO_DATA */
}

#define S(n) {#n, (void*)n}
const struct wine_sym net_syms[] = {
    S(WSAStartup), S(WSACleanup), S(WSAGetLastError), S(WSASetLastError), S(WSASocketW),
    {"socket", (void*)ms_socket}, S(closesocket), {"shutdown", (void*)ms_shutdown},
    {"bind", (void*)ms_bind}, {"setsockopt", (void*)ms_setsockopt},
    {"getsockopt", (void*)ms_getsockopt}, S(ioctlsocket), S(ConnectEx),
    {"connect", (void*)ms_connect}, S(WSARecv), S(WSASend), {"recv", (void*)ms_recv},
    {"send", (void*)ms_send}, S(WSAGetOverlappedResult), S(GetOverlappedResult), S(CancelIoEx),
    S(CancelIo), S(SetFileCompletionNotificationModes), S(getsockname), S(getpeername),
    S(WSAIoctl), S(WSAEnumProtocolsW), S(GetAddrInfoW), S(FreeAddrInfoW),
    {"gethostname", (void*)ms_gethostname}, {"htons", (void*)ms_htons},
    {"ntohs", (void*)ms_htons}, {"htonl", (void*)ms_htonl}, {"ntohl", (void*)ms_htonl},
    S(GetAdaptersAddresses),
    {0, 0}};
