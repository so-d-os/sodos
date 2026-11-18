/* sodos_epoll.c - epoll, timerfd e eventfd sobre o poll() do port.
 *
 * O event handler do dart:io no Linux (runtime/bin/eventhandler_linux.cc) e
 * um laco de epoll_wait com um timerfd (proximo timer) e um pipe de
 * interrupcao. O SODOS nao tem esses objetos no kernel; aqui eles sao fds do
 * processo na faixa 5000..5999, que o sodos_pipe.c (read/write/close/fcntl) e o
 * sodos_poll.c reconhecem pelos ganchos fracos sodos_evt_*.
 *
 * epoll_wait nao dorme dentro do poll(): olha o conjunto com poll(..., 0) e
 * dorme em fatias curtas (sodos_sleep_ms solta o lock grande). Assim EPOLLET
 * (so a borda de subida de cada evento) e EPOLLONESHOT saem certos sem girar
 * a CPU com um fd que continua pronto.
 *
 * Ligado so no port do Dart: os stubs fracos de sodos_linux_abi.c continuam
 * dando ENOSYS no Firefox (o libevent dele usa poll).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "poll.h"
#include "sodos.h"
#include "sodos_internal.h"
#include "sys/epoll.h"
#include "sys/eventfd.h"
#include "sys/timerfd.h"

#define EVT_BASE 5000
#define EVT_MAX 64

/* EVT_DIR: fd de diretorio (open(O_DIRECTORY)); o posix-lite da EISDIR para
 * pasta, e o dart:io lista com openat64(O_DIRECTORY) + fdopendir. So guarda
 * o caminho: sodos_dart_libc.c resolve os *at e o fdopendir por ele. */
enum { EVT_FREE = 0, EVT_EPOLL, EVT_TIMER, EVT_EVENT, EVT_DIR };

struct watch {
  int fd;
  uint32_t events;  /* pedido (EPOLLIN|EPOLLOUT|... + EPOLLET/ONESHOT) */
  epoll_data_t data;
  uint32_t last;    /* EPOLLET: o que ja estava pronto na ultima olhada */
  int disabled;     /* EPOLLONESHOT ja disparou; volta com EPOLL_CTL_MOD */
};

struct evt {
  int type;
  int nonblock;
  /* epoll */
  struct watch* w;
  int nw, capw;
  /* timerfd: prazos em ns no relogio do proprio timer */
  int clock;
  uint64_t deadline;  /* 0 = desarmado */
  uint64_t interval;
  /* eventfd */
  uint64_t count;
  int semaphore;
  /* diretorio */
  char* path;
};

static struct evt evts[EVT_MAX];

static struct evt* get(int fd) {
  if (fd < EVT_BASE || fd >= EVT_BASE + EVT_MAX) return NULL;
  return evts[fd - EVT_BASE].type ? &evts[fd - EVT_BASE] : NULL;
}

static int alloc(int type, int nonblock) {
  int i;
  for (i = 0; i < EVT_MAX; i++) {
    if (!evts[i].type) {
      memset(&evts[i], 0, sizeof(evts[i]));
      evts[i].type = type;
      evts[i].nonblock = nonblock;
      return EVT_BASE + i;
    }
  }
  errno = EMFILE;
  return -1;
}

static uint64_t now_ns(int clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t ts_ns(const struct timespec* t) {
  return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

static void ns_ts(uint64_t ns, struct timespec* t) {
  t->tv_sec = (time_t)(ns / 1000000000ull);
  t->tv_nsec = (long)(ns % 1000000000ull);
}

/* ---- ganchos de sodos_pipe.c / sodos_poll.c -------------------------------- */

int sodos_evt_is(int fd) { return get(fd) != NULL; }

short sodos_evt_poll(int fd, short events) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  short ev = 0;
  if (!e) return POLLNVAL;
  switch (e->type) {
    case EVT_TIMER:
      if (e->deadline && now_ns(e->clock) >= e->deadline) ev |= POLLIN;
      break;
    case EVT_EVENT:
      if (e->count > 0) ev |= POLLIN;
      if (e->count < UINT64_MAX - 1) ev |= POLLOUT;
      break;
    case EVT_EPOLL:
      /* Um epoll dentro de outro: o Dart nao faz; diz "pronto" se houver
       * algum fd pronto no conjunto seria caro e recursivo. */
      break;
  }
  return ev & (events | POLLERR | POLLHUP);
}

int sodos_evt_close(int fd) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  if (!e) {
    errno = EBADF;
    return -1;
  }
  free(e->w);
  free(e->path);
  memset(e, 0, sizeof(*e));
  return 0;
}

/* ---- fds de diretorio ---------------------------------------------------- */

int sodos_dirfd_new(const char* path) {
  SODOS_BIG_GUARD;
  size_t n = strlen(path) + 1;
  char* copy = (char*)malloc(n);
  int fd;
  if (!copy) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(copy, path, n);
  fd = alloc(EVT_DIR, 0);
  if (fd < 0) {
    free(copy);
    return -1;
  }
  get(fd)->path = copy;
  return fd;
}

/* Caminho do fd de diretorio, ou NULL se fd nao e um. */
const char* sodos_dirfd_path(int fd) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  return e && e->type == EVT_DIR ? e->path : NULL;
}

static ssize_t timer_read(struct evt* e, uint64_t* out) {
  for (;;) {
    uint64_t now;
    if (!e->deadline) {
      /* Desarmado: o Linux bloqueia para sempre; nao bloqueante da EAGAIN. */
      if (e->nonblock) {
        errno = EAGAIN;
        return -1;
      }
    } else {
      now = now_ns(e->clock);
      if (now >= e->deadline) {
        uint64_t n = 1;
        if (e->interval) {
          n += (now - e->deadline) / e->interval;
          e->deadline += n * e->interval;
        } else {
          e->deadline = 0;
        }
        *out = n;
        return 8;
      }
      if (e->nonblock) {
        errno = EAGAIN;
        return -1;
      }
    }
    sodos_sleep_ms(1);
  }
}

ssize_t sodos_evt_read(int fd, void* buf, size_t n) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  uint64_t v;
  ssize_t r;
  if (!e || e->type == EVT_EPOLL || e->type == EVT_DIR) {
    errno = !e ? EBADF : e->type == EVT_DIR ? EISDIR : EINVAL;
    return -1;
  }
  if (n < 8) {
    errno = EINVAL;
    return -1;
  }
  if (e->type == EVT_TIMER) {
    r = timer_read(e, &v);
    if (r < 0) return r;
  } else {
    while (e->count == 0) {
      if (e->nonblock) {
        errno = EAGAIN;
        return -1;
      }
      sodos_sleep_ms(1);
      if (!get(fd)) {
        errno = EBADF;
        return -1;
      }
    }
    if (e->semaphore) {
      v = 1;
      e->count--;
    } else {
      v = e->count;
      e->count = 0;
    }
  }
  memcpy(buf, &v, 8);
  return 8;
}

ssize_t sodos_evt_write(int fd, const void* buf, size_t n) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  uint64_t v;
  if (!e || e->type != EVT_EVENT) {
    errno = e ? EINVAL : EBADF;
    return -1;
  }
  if (n < 8) {
    errno = EINVAL;
    return -1;
  }
  memcpy(&v, buf, 8);
  if (v == UINT64_MAX) {
    errno = EINVAL;
    return -1;
  }
  while (UINT64_MAX - 1 - e->count < v) {
    if (e->nonblock) {
      errno = EAGAIN;
      return -1;
    }
    sodos_sleep_ms(1);
    if (!get(fd)) {
      errno = EBADF;
      return -1;
    }
  }
  e->count += v;
  return 8;
}

int sodos_evt_fcntl(int fd, int cmd, long arg) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  if (!e) {
    errno = EBADF;
    return -1;
  }
  switch (cmd) {
    case F_GETFD:
    case F_SETFD:
      return 0;
    case F_GETFL:
      return O_RDWR | (e->nonblock ? O_NONBLOCK : 0);
    case F_SETFL:
      e->nonblock = (arg & O_NONBLOCK) != 0;
      return 0;
    default:
      errno = EINVAL;
      return -1;
  }
}

/* ---- eventfd ------------------------------------------------------------- */

int eventfd(unsigned int initval, int flags) {
  SODOS_BIG_GUARD;
  int fd = alloc(EVT_EVENT, (flags & EFD_NONBLOCK) != 0);
  if (fd < 0) return -1;
  get(fd)->count = initval;
  get(fd)->semaphore = (flags & EFD_SEMAPHORE) != 0;
  return fd;
}

int eventfd_read(int fd, eventfd_t* value) {
  return sodos_evt_read(fd, value, 8) == 8 ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value) {
  return sodos_evt_write(fd, &value, 8) == 8 ? 0 : -1;
}

/* ---- timerfd ------------------------------------------------------------- */

int timerfd_create(int clockid, int flags) {
  SODOS_BIG_GUARD;
  int fd;
  if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
    errno = EINVAL;
    return -1;
  }
  fd = alloc(EVT_TIMER, (flags & TFD_NONBLOCK) != 0);
  if (fd < 0) return -1;
  get(fd)->clock = clockid;
  return fd;
}

static void timer_get(struct evt* e, struct itimerspec* cur) {
  uint64_t now = now_ns(e->clock);
  ns_ts(e->interval, &cur->it_interval);
  ns_ts(e->deadline && e->deadline > now ? e->deadline - now : (e->deadline ? 1 : 0),
        &cur->it_value);
}

int timerfd_settime(int fd, int flags, const struct itimerspec* nv, struct itimerspec* old) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  uint64_t v;
  if (!e || e->type != EVT_TIMER) {
    errno = e ? EINVAL : EBADF;
    return -1;
  }
  if (!nv) {
    errno = EFAULT;
    return -1;
  }
  if (old) timer_get(e, old);
  v = ts_ns(&nv->it_value);
  e->interval = ts_ns(&nv->it_interval);
  if (v == 0) {
    e->deadline = 0;  /* desarma */
  } else if (flags & TFD_TIMER_ABSTIME) {
    e->deadline = v;
  } else {
    e->deadline = now_ns(e->clock) + v;
  }
  return 0;
}

int timerfd_gettime(int fd, struct itimerspec* cur) {
  SODOS_BIG_GUARD;
  struct evt* e = get(fd);
  if (!e || e->type != EVT_TIMER) {
    errno = e ? EINVAL : EBADF;
    return -1;
  }
  timer_get(e, cur);
  return 0;
}

/* ---- epoll --------------------------------------------------------------- */

int epoll_create1(int flags) {
  SODOS_BIG_GUARD;
  if (flags & ~EPOLL_CLOEXEC) {
    errno = EINVAL;
    return -1;
  }
  return alloc(EVT_EPOLL, 0);
}

int epoll_create(int size) {
  if (size <= 0) {
    errno = EINVAL;
    return -1;
  }
  return epoll_create1(0);
}

static struct watch* find(struct evt* ep, int fd) {
  int i;
  for (i = 0; i < ep->nw; i++)
    if (ep->w[i].fd == fd) return &ep->w[i];
  return NULL;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
  SODOS_BIG_GUARD;
  struct evt* ep = get(epfd);
  struct watch* w;
  if (!ep || ep->type != EVT_EPOLL) {
    errno = ep ? EINVAL : EBADF;
    return -1;
  }
  if (fd == epfd) {
    errno = EINVAL;
    return -1;
  }
  /* Arquivos comuns (fds da posix-lite: 3..999) nao entram em epoll no Linux. */
  if (fd > 2 && fd < 1000) {
    errno = EPERM;
    return -1;
  }
  w = find(ep, fd);
  switch (op) {
    case EPOLL_CTL_ADD:
      if (w) {
        errno = EEXIST;
        return -1;
      }
      if (!ev) {
        errno = EFAULT;
        return -1;
      }
      if (ep->nw == ep->capw) {
        int cap = ep->capw ? ep->capw * 2 : 16;
        struct watch* nwp = (struct watch*)realloc(ep->w, (size_t)cap * sizeof(*nwp));
        if (!nwp) {
          errno = ENOMEM;
          return -1;
        }
        ep->w = nwp;
        ep->capw = cap;
      }
      w = &ep->w[ep->nw++];
      w->fd = fd;
      w->events = ev->events;
      w->data = ev->data;
      w->last = 0;
      w->disabled = 0;
      return 0;
    case EPOLL_CTL_MOD:
      if (!w) {
        errno = ENOENT;
        return -1;
      }
      if (!ev) {
        errno = EFAULT;
        return -1;
      }
      w->events = ev->events;
      w->data = ev->data;
      w->last = 0;
      w->disabled = 0;
      return 0;
    case EPOLL_CTL_DEL:
      if (!w) {
        errno = ENOENT;
        return -1;
      }
      *w = ep->w[--ep->nw];
      return 0;
    default:
      errno = EINVAL;
      return -1;
  }
}

/* Uma olhada no conjunto: preenche out[] e devolve quantos eventos. */
static int scan(int epfd, struct epoll_event* out, int max) {
  SODOS_BIG_GUARD;
  struct evt* ep = get(epfd);
  struct pollfd pf[64];
  int i, base, n = 0;
  if (!ep || ep->type != EVT_EPOLL) {
    errno = EBADF;
    return -1;
  }
  for (base = 0; base < ep->nw && n < max; base += 64) {
    int cnt = ep->nw - base < 64 ? ep->nw - base : 64;
    for (i = 0; i < cnt; i++) {
      struct watch* w = &ep->w[base + i];
      short want = 0;
      if (w->events & (EPOLLIN | EPOLLRDNORM)) want |= POLLIN;
      if (w->events & EPOLLPRI) want |= POLLPRI;
      if (w->events & (EPOLLOUT | EPOLLWRNORM)) want |= POLLOUT;
      pf[i].fd = w->disabled ? -1 : w->fd;
      pf[i].events = want;
      pf[i].revents = 0;
    }
    poll(pf, (nfds_t)cnt, 0);
    /* O poll pode ter cedido a CPU: o conjunto pode ter mudado (outra thread
     * em epoll_ctl). Se mudou de tamanho, descarta esta olhada. */
    ep = get(epfd);
    if (!ep || base + cnt > ep->nw) return 0;
    for (i = 0; i < cnt && n < max; i++) {
      struct watch* w = &ep->w[base + i];
      short r = pf[i].revents;
      uint32_t got = 0;
      if (w->disabled || pf[i].fd != w->fd) continue;
      if (r & POLLNVAL) {
        got = EPOLLERR | EPOLLHUP;  /* fechado sem EPOLL_CTL_DEL */
      } else {
        if (r & POLLIN) got |= EPOLLIN;
        if (r & POLLPRI) got |= EPOLLPRI;
        if (r & POLLOUT) got |= EPOLLOUT;
        if (r & POLLERR) got |= EPOLLERR;
        if (r & POLLHUP) got |= EPOLLHUP | (w->events & EPOLLRDHUP);
      }
      if (w->events & EPOLLET) {
        uint32_t edge = got & ~w->last;
        w->last = got;
        got = edge;
      }
      if (!got) continue;
      out[n].events = got;
      out[n].data = w->data;
      n++;
      if (w->events & EPOLLONESHOT) w->disabled = 1;
    }
  }
  return n;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
  uint64_t start;
  if (maxevents <= 0) {
    errno = EINVAL;
    return -1;
  }
  start = sodos_now_ms();
  for (;;) {
    int n = scan(epfd, events, maxevents);
    if (n != 0) return n;
    if (timeout == 0) return 0;
    {
      uint64_t el = sodos_now_ms() - start;
      unsigned slice = 2;
      if (timeout > 0) {
        if (el >= (uint64_t)timeout) return 0;
        if ((uint64_t)timeout - el < slice) slice = (unsigned)((uint64_t)timeout - el);
      }
      sodos_sleep_ms(slice ? slice : 1);
    }
  }
}

int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                const sigset_t* ss) {
  (void)ss;
  return epoll_wait(epfd, events, maxevents, timeout);
}
