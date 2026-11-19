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
  /* WSAEventSelect */
  HANDLE ev;                /* 0 = sem selecao */
  long mask;                /* FD_* pedidos */
  long fired;               /* FD_* ocorridos e ainda nao lidos (WSAEnumNetworkEvents) */
  int err[10];              /* iErrorCode por bit */
  int rd_armed, wr_armed, conn_seen, close_seen;
  uint64_t rx, tx;          /* bytes (log) */
  int rx_logged;
};

struct wnet {
  struct wsock* socks;
  int started;
  int watcher;  /* thread vigia ja criada */
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

static void count_io(struct wsock* s, int rx, int n) {
  if (n <= 0) return;
  if (rx) {
    s->rx += (uint64_t)n;
    if (wine_debug && !s->rx_logged) {
      s->rx_logged = 1;
      wlogx("rede: primeiros bytes recebidos, soquete ", (uint64_t)s->k);
    }
  } else {
    if (wine_debug && !s->tx) wlogx("rede: primeiros bytes enviados, soquete ", (uint64_t)s->k);
    s->tx += (uint64_t)n;
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
      if (wine_debug) wlogx("rede: conectado, soquete ", (uint64_t)s->k);
      complete(s, ov, 0, 0);
    } else if (st & 4) {
      struct overlapped* ov = s->c_ov;
      s->c_ov = 0;
      s->failed = 1;
      if (wine_debug) wlogx("rede: conexao recusada, soquete ", (uint64_t)s->k);
      complete(s, ov, ST_REFUSED, 0);
    }
  } else if (!s->connected && (st & 8)) {
    s->connected = 1;
  }
  if (s->r_ov && (st & (1 | 4))) {
    int n = k_sock_recv(s->k, s->r_buf, (int)s->r_len, 0);
    count_io(s, 1, n);
    if (n == -2 && wine_debug) wlogx("rede: conexao fechada pelo outro lado, soquete ", (uint64_t)s->k);
    if (n != -1) {
      struct overlapped* ov = s->r_ov;
      s->r_ov = 0;
      complete(s, ov, n < 0 ? ST_RESET : 0, n < 0 ? 0 : (uint32_t)n);
    }
  }
  if (s->s_ov && (st & (2 | 4))) {
    int n = k_sock_send(s->k, s->s_buf, (int)s->s_len);
    count_io(s, 0, n);
    if (n != -1) {
      struct overlapped* ov = s->s_ov;
      s->s_ov = 0;
      complete(s, ov, n < 0 ? ST_RESET : 0, n < 0 ? 0 : (uint32_t)n);
    }
  }
}

#define FD_READ 1
#define FD_WRITE 2
#define FD_CONNECT 16
#define FD_CLOSE 32
static void ev_fire(struct wsock* s, long what, int bit, int err) {
  struct wobj* e;
  if (!(s->mask & what)) return;
  s->fired |= what;
  s->err[bit] = err;
  e = s->ev ? wh_get(s->ev, OBJ_EVENT) : 0;
  if (e) {
    e->signaled = 1;
    wsignal();
  }
}
/* WSAEventSelect: o estado do soquete vira FD_* no evento */
static void sock_events(struct wsock* s) {
  int st;
  if (!s->ev || !s->k) return;
  st = k_sock_poll(s->k);
  if (!s->conn_seen && (st & (8 | 4))) {
    s->conn_seen = 1;
    if (wine_debug) wlogx((st & 8) ? "rede: FD_CONNECT ok, soquete " : "rede: FD_CONNECT falhou, soquete ", (uint64_t)s->k);
    if (st & 8) s->connected = 1;
    ev_fire(s, FD_CONNECT, 4, (st & 8) ? 0 : WSAECONNREFUSED);
    if (st & 8) s->wr_armed = 1;
  }
  if (!s->connected) return;
  if (s->rd_armed && (st & 1)) {
    s->rd_armed = 0;
    ev_fire(s, FD_READ, 0, 0);
  }
  if (s->wr_armed && (st & 2)) {
    s->wr_armed = 0;
    ev_fire(s, FD_WRITE, 1, 0);
  }
  if (!s->close_seen && (st & 4)) {
    s->close_seen = 1;
    ev_fire(s, FD_CLOSE, 5, 0);
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
    if (s->c_ov || s->r_ov || s->s_ov || s->ev) pending = 1;
  if (pending) {
    k_net_pump();
    for (s = n->socks; s; s = s->next) {
      sock_step(s);
      sock_events(s);
    }
  }
  __sync_lock_release(&poll_busy);
}

static void poll_wait(int ms) {
  static volatile int w;
  wnet_poll();
  k_futex_wait(&w, 0, ms);
}

/* Vigia da rede do processo: quem espera um evento (WSAEventSelect ou o
 * evento de um OVERLAPPED) nao passa pela porta de conclusao, entao uma
 * thread do proprio processo anda os soquetes a cada 5 ms. */
static WINAPI DWORD net_watcher(void* arg) {
  static volatile int tick;
  (void)arg;
  for (;;) {
    wnet_poll();
    k_futex_wait(&tick, 0, 5);
  }
  return 0;
}
static void watcher_start(void) {
  typedef HANDLE(WINAPI * ct_fn)(void*, size_t, void*, void*, DWORD, DWORD*);
  struct wnet* n = net_get();
  ct_fn ct;
  if (!n || n->watcher) return;
  n->watcher = 1;
  ct = (ct_fn)builtin_lookup("CreateThread");
  if (ct) ct(0, 0, (void*)net_watcher, 0, 0, 0);
}

/* ---------- WSA*Event: eventos comuns do Windows ---------- */
typedef HANDLE(WINAPI* cev_fn)(void*, BOOL, BOOL, const wchar16*);
typedef BOOL(WINAPI* h_fn)(HANDLE);
typedef DWORD(WINAPI* wmo_fn)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
static WINAPI HANDLE WSACreateEvent(void) {
  cev_fn f = (cev_fn)builtin_lookup("CreateEventW");
  return f ? f(0, 1, 0, 0) : 0; /* manual, nao sinalizado */
}
static WINAPI BOOL WSACloseEvent(HANDLE h) {
  h_fn f = (h_fn)builtin_lookup("CloseHandle");
  return f ? f(h) : 0;
}
static WINAPI BOOL WSASetEvent(HANDLE h) {
  h_fn f = (h_fn)builtin_lookup("SetEvent");
  return f ? f(h) : 0;
}
static WINAPI BOOL WSAResetEvent(HANDLE h) {
  h_fn f = (h_fn)builtin_lookup("ResetEvent");
  return f ? f(h) : 0;
}
static WINAPI DWORD WSAWaitForMultipleEvents(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alert) {
  wmo_fn f = (wmo_fn)builtin_lookup("WaitForMultipleObjectsEx");
  return f ? f(n, hs, all, ms, alert) : 0xFFFFFFFFu;
}
static WINAPI int WSAEventSelect(uintptr_t s, HANDLE ev, long mask) {
  struct wsock* k = sock_of(s, 0);
  if (!k) return SOCKET_ERROR;
  if (wine_debug) wlogx("rede: WSAEventSelect (evento<<16|mascara) ", ((uint64_t)(uintptr_t)ev << 16) | (uint64_t)(mask & 0xFFFF));
  k->ev = mask ? ev : 0;
  k->mask = mask;
  k->nonblock = 1; /* como no Windows */
  k->rd_armed = 1;
  if (k->connected && !k->conn_seen) k->conn_seen = 1;
  if (k->connected) k->wr_armed = 1;
  if (mask) watcher_start();
  wsignal();
  return 0;
}
struct wsanet_events { /* WSANETWORKEVENTS: o long do Windows tem 4 bytes (44 no total) */
  int32_t events;
  int32_t err[10];
};
static WINAPI int WSAEnumNetworkEvents(uintptr_t s, HANDLE ev, struct wsanet_events* out) {
  struct wsock* k = sock_of(s, 0);
  int i;
  if (wine_debug) wlogx("rede: WSAEnumNetworkEvents chamado, soquete do programa ", (uint64_t)s);
  if (wine_debug && !k) wlog("rede: WSAEnumNetworkEvents: ", "soquete invalido");
  if (!k) return SOCKET_ERROR;
  if (!out) {
    wset_lasterr(WSAEFAULT);
    return SOCKET_ERROR;
  }
  while (__sync_lock_test_and_set(&poll_busy, 1)) k_thread_yield();
  out->events = (int32_t)k->fired;
  if (wine_debug && k->fired) wlogx("rede: WSAEnumNetworkEvents (soquete<<8|FD_*) ", ((uint64_t)k->k << 8) | (uint64_t)k->fired);
  for (i = 0; i < 10; i++) out->err[i] = k->err[i];
  k->fired = 0;
  __sync_lock_release(&poll_busy);
  if (ev) WSAResetEvent(ev);
  return 0;
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
    if (wine_debug) wlogx("rede: soquete recusado (af<<8|tipo) ", (uint64_t)((af << 8) | type));
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
  if (s->k && wine_debug) {
    wlogx("rede: fechado soquete ", (uint64_t)s->k);
    wlogx("  bytes enviados ", s->tx);
    wlogx("  bytes recebidos ", s->rx);
  }
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
  if (wine_debug) wlogx("rede: conectar (ip<<16|porta) ", ((uint64_t)ip << 16) | (unsigned)port);
  if (h <= 0) {
    if (wine_debug) wlogx("rede: k_sock_connect falhou ", (uint64_t)(int64_t)h);
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
  if (ov->event) watcher_start();
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
  k->rd_armed = 1; /* FD_READ volta a valer depois de cada recv */
  for (;;) {
    n = bufs[0].len ? k_sock_recv(k->k, bufs[0].buf, (int)bufs[0].len, 0) : 0;
    count_io(k, 1, n);
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
      if (ov->event) watcher_start();
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
    count_io(k, 0, n);
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
      if (ov->event) watcher_start();
      ov->internal = 0x103;
      k->s_buf = bufs[0].buf;
      k->s_len = bufs[0].len;
      k->s_ov = ov;
      wsignal();
      wset_lasterr(ERROR_IO_PENDING);
      return SOCKET_ERROR;
    }
    if (k->nonblock) {
      if (wine_debug >= 2) wlogx("rede: send sem espaco (WOULDBLOCK), soquete ", (uint64_t)k->k);
      k->wr_armed = 1; /* FD_WRITE de novo quando houver espaco */
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
  {
    struct wobj* po = wh_get(h, OBJ_PIPE);
    if (po) {
      if (wpipe_cancel(po, ov)) return 1;
      wset_lasterr(1168); /* ERROR_NOT_FOUND */
      return 0;
    }
  }
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
  o = wh_get(h, OBJ_PIPE);
  if (o) { /* pipe: concluido na hora nao vai para a porta (o Mojo usa) */
    o->skip_ok = flags & 1;
    if (wine_debug >= 2) wlogx("pipe: SetFileCompletionNotificationModes ", flags);
  }
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

static char* ip_text(uint32_t net, char* out) { /* endereco em ordem de rede */
  const uint8_t* b = (const uint8_t*)&net;
  int i, o = 0;
  for (i = 0; i < 4; i++) {
    unsigned v = b[i];
    if (v >= 100) out[o++] = (char)('0' + v / 100);
    if (v >= 10) out[o++] = (char)('0' + v / 10 % 10);
    out[o++] = (char)('0' + v % 10);
    if (i < 3) out[o++] = '.';
  }
  out[o] = 0;
  return out;
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
  if (n > 0 && !host[n - 1]) n--; /* o -1 conta o terminador */
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
    if (wine_debug) {
      char t[16];
      wlog("DNS ok: ", host);
      wlog("  -> ", ip_text(__builtin_bswap32(ip), t));
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


/* ---------- enderecos e nomes (ws2_32): o que o Chromium e o FETCH pedem ---------- */
static WINAPI uint32_t ms_inet_addr(const char* s) {
  unsigned ip;
  if (!s || !parse_ip(s, &ip)) return 0xFFFFFFFFu; /* INADDR_NONE */
  return __builtin_bswap32(ip);
}
static WINAPI char* ms_inet_ntoa(uint32_t net) {
  static char buf[16];
  return ip_text(net, buf);
}
static WINAPI int ms_inet_pton(int fam, const char* src, void* dst) {
  unsigned ip;
  if (fam != 2) return -1; /* so IPv4 */
  if (!src || !parse_ip(src, &ip)) return 0;
  *(uint32_t*)dst = __builtin_bswap32(ip);
  return 1;
}
static WINAPI const char* ms_inet_ntop(int fam, const void* src, char* dst, size_t n) {
  char tmp[16];
  if (fam != 2 || !src || !dst) return 0;
  ip_text(*(const uint32_t*)src, tmp);
  if (strlen(tmp) + 1 > n) return 0;
  strcpy(dst, tmp);
  return dst;
}
static WINAPI int InetPtonW(int fam, const wchar16* src, void* dst) {
  char* s = wdup16to8(src);
  int r = ms_inet_pton(fam, s, dst);
  k_free(s);
  return r;
}
static WINAPI const wchar16* InetNtopW(int fam, const void* src, wchar16* dst, size_t n) {
  char tmp[16];
  int i;
  if (!ms_inet_ntop(fam, src, tmp, sizeof(tmp)) || strlen(tmp) + 1 > n) return 0;
  for (i = 0; tmp[i]; i++) dst[i] = (wchar16)tmp[i];
  dst[i] = 0;
  return dst;
}
/* getaddrinfo (A): mesmo layout do ADDRINFOW, so o canonname e char* */
static WINAPI int ms_getaddrinfo(const char* node, const char* service, const struct addrinfow* hints,
                                 struct addrinfow** res) {
  wchar16* wn = node ? wdup8to16(node) : 0;
  wchar16* ws = service ? wdup8to16(service) : 0;
  int r = GetAddrInfoW(wn, ws, hints, res);
  k_free(wn);
  k_free(ws);
  return r;
}
/* ADDRINFOEXW: flags, family, socktype, protocol, addrlen, canonname, addr, blob, bloblen, provider, next */
struct addrinfoexw {
  int flags, family, socktype, protocol;
  size_t addrlen;
  wchar16* canonname;
  struct sockaddr_in4* addr;
  void* blob;
  size_t bloblen;
  void* provider;
  struct addrinfoexw* next;
};
static WINAPI int GetAddrInfoExW(const wchar16* name, const wchar16* service, DWORD ns, void* nsid,
                                 const struct addrinfoexw* hints, struct addrinfoexw** res, void* timeout,
                                 void* ov, void* done_cb, HANDLE* cancel) {
  struct addrinfow h, *r = 0;
  struct addrinfoexw* e;
  int rc;
  (void)ns; (void)nsid; (void)timeout; (void)done_cb;
  memset(&h, 0, sizeof(h));
  if (hints) {
    h.flags = hints->flags;
    h.family = hints->family;
    h.socktype = hints->socktype;
    h.protocol = hints->protocol;
  }
  if (cancel) *cancel = 0;
  rc = GetAddrInfoW(name, service, hints ? &h : 0, &r);
  *res = 0;
  if (rc) return rc;
  e = (struct addrinfoexw*)wzalloc(sizeof(*e) + sizeof(struct sockaddr_in4));
  if (!e) {
    FreeAddrInfoW(r);
    return 8;
  }
  e->flags = r->flags;
  e->family = r->family;
  e->socktype = r->socktype;
  e->protocol = r->protocol;
  e->addrlen = r->addrlen;
  e->addr = (struct sockaddr_in4*)(e + 1);
  memcpy(e->addr, r->addr, sizeof(struct sockaddr_in4));
  FreeAddrInfoW(r);
  *res = e;
  (void)ov; /* resolvido na hora: devolve NO_ERROR mesmo com OVERLAPPED */
  return 0;
}
static WINAPI void FreeAddrInfoExW(struct addrinfoexw* ai) {
  while (ai) {
    struct addrinfoexw* n = ai->next;
    k_free(ai);
    ai = n;
  }
}
static WINAPI int GetAddrInfoExCancel(HANDLE* h) {
  (void)h;
  return 0;
}
/* getnameinfo: so a forma numerica */
static WINAPI int ms_getnameinfo(const struct sockaddr_in4* sa, int salen, char* host, DWORD hl, char* serv,
                                 DWORD sl, int fl) {
  char tmp[16];
  (void)salen;
  (void)fl;
  if (!sa || sa->family != 2) return 10047; /* WSAEAFNOSUPPORT */
  if (host && hl) {
    ip_text(*(const uint32_t*)sa->addr, tmp);
    if (strlen(tmp) + 1 > hl) return 10014;
    strcpy(host, tmp);
  }
  if (serv && sl) {
    unsigned port = ((unsigned)(sa->port & 0xFF) << 8) | (sa->port >> 8);
    char p[8];
    int o = 0, k;
    char t[8];
    int n = 0;
    do {
      t[n++] = (char)('0' + port % 10);
      port /= 10;
    } while (port);
    for (k = n - 1; k >= 0; k--) p[o++] = t[k];
    p[o] = 0;
    if ((DWORD)o + 1 > sl) return 10014;
    strcpy(serv, p);
  }
  return 0;
}
static WINAPI int GetNameInfoW(const struct sockaddr_in4* sa, int salen, wchar16* host, DWORD hl, wchar16* serv,
                               DWORD sl, int fl) {
  char h[64], s8[16];
  int r = ms_getnameinfo(sa, salen, host ? h : 0, host ? sizeof(h) : 0, serv ? s8 : 0, serv ? sizeof(s8) : 0, fl);
  int i;
  if (r) return r;
  if (host) {
    if (strlen(h) + 1 > hl) return 10014;
    for (i = 0; h[i]; i++) host[i] = (wchar16)h[i];
    host[i] = 0;
  }
  if (serv) {
    if (strlen(s8) + 1 > sl) return 10014;
    for (i = 0; s8[i]; i++) serv[i] = (wchar16)s8[i];
    serv[i] = 0;
  }
  return 0;
}
/* gethostbyname: um endereco IPv4, numa estrutura fixa (como no Windows, por thread) */
struct hostent_ {
  char* name;
  char** aliases;
  short addrtype, length;
  char** addr_list;
};
static WINAPI struct hostent_* ms_gethostbyname(const char* name) {
  static struct hostent_ he;
  static char* none[1];
  static char* list[2];
  static uint32_t addr;
  static char nm[128];
  struct addrinfow* r = 0;
  wchar16* wn = name ? wdup8to16(name) : 0;
  int rc = GetAddrInfoW(wn, 0, 0, &r);
  k_free(wn);
  if (rc || !r) {
    wset_lasterr(11001); /* WSAHOST_NOT_FOUND */
    return 0;
  }
  memcpy(&addr, r->addr->addr, 4);
  FreeAddrInfoW(r);
  strncpy(nm, name ? name : "", sizeof(nm) - 1);
  he.name = nm;
  he.aliases = none;
  he.addrtype = 2;
  he.length = 4;
  list[0] = (char*)&addr;
  list[1] = 0;
  he.addr_list = list;
  return &he;
}

#define S(n) {#n, (void*)n}
const struct wine_sym net_syms[] = {
    S(WSACreateEvent), S(WSACloseEvent), S(WSASetEvent), S(WSAResetEvent), S(WSAWaitForMultipleEvents),
    S(WSAEventSelect), S(WSAEnumNetworkEvents),
    {"inet_addr", (void*)ms_inet_addr}, {"inet_ntoa", (void*)ms_inet_ntoa}, {"inet_pton", (void*)ms_inet_pton},
    {"inet_ntop", (void*)ms_inet_ntop}, S(InetPtonW), S(InetNtopW), {"getaddrinfo", (void*)ms_getaddrinfo},
    {"freeaddrinfo", (void*)FreeAddrInfoW}, S(GetAddrInfoExW), S(FreeAddrInfoExW), S(GetAddrInfoExCancel),
    {"getnameinfo", (void*)ms_getnameinfo}, S(GetNameInfoW), {"gethostbyname", (void*)ms_gethostbyname},
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
