/* sodos_pipe.c - pipe() e socketpair() dentro do processo.
 *
 * O Gecko e o libevent usam estes pares para acordar o laco de eventos entre
 * threads. No SODOS as threads sao do mesmo processo, entao basta um buffer
 * circular em memoria. Descritores de arquivo passados por mensagem (SCM_RIGHTS)
 * nao existem aqui: sem processos filhos, nao ha para quem passa-los.
 */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "poll.h"
#include "sodos.h"
/* O header declara int; a ABI do glibc (e o Rust) usa ssize_t no retorno. */
#define recv sodos_hdr_recv
#define send sodos_hdr_send
#include "sys/socket.h"
#undef recv
#undef send

#define PIPE_FD_BASE 2000
#define PIPE_FD_MAX 128
#define RING_SIZE (64 * 1024)

struct ring {
  char buf[RING_SIZE];
  size_t head;
  size_t count;
  int readers;
  int writers;
};

struct pipe_end {
  int used;
  int flags;
  struct ring* rx;
  struct ring* tx;
};

static struct pipe_end ends[PIPE_FD_MAX];

static struct pipe_end* get_end(int fd) {
  if (fd < PIPE_FD_BASE || fd >= PIPE_FD_BASE + PIPE_FD_MAX) return NULL;
  return ends[fd - PIPE_FD_BASE].used ? &ends[fd - PIPE_FD_BASE] : NULL;
}

int sodos_pipe_is(int fd) { return get_end(fd) != NULL; }

static int new_end(struct ring* rx, struct ring* tx, int flags) {
  int i;
  for (i = 0; i < PIPE_FD_MAX; i++) {
    if (!ends[i].used) {
      ends[i].used = 1;
      ends[i].rx = rx;
      ends[i].tx = tx;
      ends[i].flags = flags;
      if (rx) rx->readers++;
      if (tx) tx->writers++;
      return PIPE_FD_BASE + i;
    }
  }
  return -1;
}

static void put_ring(struct ring* r) {
  if (r && r->readers == 0 && r->writers == 0) free(r);
}

/* sodos_shm.c: a posix-lite manda para ca tudo o que e >= 2000. */
#define SHM_FD(fd) ((fd) >= 3000 && (fd) < 4000)
/* sodos_sock.c: soquetes TCP em 4000.. */
#define SOCK_FD(fd) ((fd) >= 4000)
int sodos_sock_close(int fd);
ssize_t sodos_sock_read(int fd, void* buf, size_t n);
ssize_t sodos_sock_write(int fd, const void* buf, size_t n);
ssize_t sodos_sock_recv(int fd, void* buf, size_t n, int flags);
ssize_t sodos_sock_send(int fd, const void* buf, size_t n, int flags);
int sodos_sock_fcntl(int fd, int cmd, long arg);
int sodos_shm_close(int fd);
int dup(int fd);
ssize_t sodos_shm_read(int fd, void* buf, size_t n);
ssize_t sodos_shm_write(int fd, const void* buf, size_t n);

int sodos_pipe_close(int fd) {
  struct pipe_end* e = get_end(fd);
  struct ring* rx;
  struct ring* tx;
  if (SOCK_FD(fd)) return sodos_sock_close(fd);
  if (SHM_FD(fd)) return sodos_shm_close(fd);
  if (!e) {
    errno = EBADF;
    return -1;
  }
  rx = e->rx;
  tx = e->tx;
  if (rx) rx->readers--;
  if (tx) tx->writers--;
  e->used = 0;
  put_ring(rx);
  if (tx != rx) put_ring(tx);
  return 0;
}

ssize_t sodos_pipe_read(int fd, void* buf, size_t n) {
  struct pipe_end* e = get_end(fd);
  struct ring* r;
  size_t i;
  if (SOCK_FD(fd)) return sodos_sock_read(fd, buf, n);
  if (SHM_FD(fd)) return sodos_shm_read(fd, buf, n);
  if (!e || !e->rx) {
    errno = EBADF;
    return -1;
  }
  r = e->rx;
  if (n == 0) return 0;
  while (r->count == 0) {
    if (r->writers == 0) return 0;
    if (e->flags & O_NONBLOCK) {
      errno = EAGAIN;
      return -1;
    }
    sodos_sleep_ms(1);
    e = get_end(fd);
    if (!e) {
      errno = EBADF;
      return -1;
    }
  }
  if (n > r->count) n = r->count;
  for (i = 0; i < n; i++) ((char*)buf)[i] = r->buf[(r->head + i) % RING_SIZE];
  r->head = (r->head + n) % RING_SIZE;
  r->count -= n;
  return (ssize_t)n;
}

ssize_t sodos_pipe_write(int fd, const void* buf, size_t n) {
  struct pipe_end* e = get_end(fd);
  struct ring* r;
  size_t done = 0;
  if (SOCK_FD(fd)) return sodos_sock_write(fd, buf, n);
  if (SHM_FD(fd)) return sodos_shm_write(fd, buf, n);
  if (!e || !e->tx) {
    errno = EBADF;
    return -1;
  }
  r = e->tx;
  while (done < n) {
    size_t i, room;
    if (r->readers == 0) {
      errno = EPIPE;
      return done ? (ssize_t)done : -1;
    }
    room = RING_SIZE - r->count;
    if (room == 0) {
      if (done) return (ssize_t)done;
      if (e->flags & O_NONBLOCK) {
        errno = EAGAIN;
        return -1;
      }
      sodos_sleep_ms(1);
      e = get_end(fd);
      if (!e) {
        errno = EBADF;
        return -1;
      }
      continue;
    }
    if (room > n - done) room = n - done;
    for (i = 0; i < room; i++) r->buf[(r->head + r->count + i) % RING_SIZE] = ((const char*)buf)[done + i];
    r->count += room;
    done += room;
  }
  return (ssize_t)done;
}

short sodos_pipe_poll(int fd, short events) {
  struct pipe_end* e = get_end(fd);
  short ev = 0;
  if (!e) return POLLNVAL;
  if (e->rx && (events & POLLIN)) {
    if (e->rx->count > 0) ev |= POLLIN;
    else if (e->rx->writers == 0) ev |= POLLHUP;
  }
  if (e->tx && (events & POLLOUT)) {
    if (e->tx->readers == 0) ev |= POLLERR;
    else if (e->tx->count < RING_SIZE) ev |= POLLOUT;
  }
  return ev;
}

static struct ring* new_ring(void) { return (struct ring*)calloc(1, sizeof(struct ring)); }

int pipe2(int fds[2], int flags) {
  struct ring* r = new_ring();
  int a, b;
  if (!r) {
    errno = ENOMEM;
    return -1;
  }
  flags &= O_NONBLOCK;
  a = new_end(r, NULL, flags);
  b = a < 0 ? -1 : new_end(NULL, r, flags);
  if (b < 0) {
    if (a >= 0) sodos_pipe_close(a);
    else free(r);
    errno = EMFILE;
    return -1;
  }
  fds[0] = a;
  fds[1] = b;
  return 0;
}

int pipe(int fds[2]) { return pipe2(fds, 0); }

int socketpair(int domain, int type, int protocol, int sv[2]) {
  struct ring* r1;
  struct ring* r2;
  int flags = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
  int a, b;
  (void)protocol;
  if (domain != AF_UNIX) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  r1 = new_ring();
  r2 = new_ring();
  if (!r1 || !r2) {
    free(r1);
    free(r2);
    errno = ENOMEM;
    return -1;
  }
  a = new_end(r1, r2, flags);
  b = a < 0 ? -1 : new_end(r2, r1, flags);
  if (b < 0) {
    if (a >= 0) sodos_pipe_close(a);
    errno = EMFILE;
    return -1;
  }
  sv[0] = a;
  sv[1] = b;
  return 0;
}

int fcntl(int fd, int cmd, ...) {
  struct pipe_end* e = get_end(fd);
  va_list ap;
  long arg;
  va_start(ap, cmd);
  arg = va_arg(ap, long);
  va_end(ap);
  if (SOCK_FD(fd)) return sodos_sock_fcntl(fd, cmd, arg);
  switch (cmd) {
    case F_GETFD:
    case F_SETFD:
      return 0;
    case F_GETFL:
      return e ? (e->flags | O_RDWR) : O_RDWR;
    case F_SETFL:
      if (e) e->flags = (int)arg & O_NONBLOCK;
      return 0;
    case 0:    /* F_DUPFD */
    case 1030: /* F_DUPFD_CLOEXEC */
      return dup(fd);
    case 5:  /* F_GETLK: so existe este processo, nunca ha trava de outro */
    case 36: /* F_OFD_GETLK */
      if (arg) ((struct flock*)arg)->l_type = F_UNLCK;
      return 0;
    case 6:  /* F_SETLK */
    case 7:  /* F_SETLKW */
    case 37: /* F_OFD_SETLK */
    case 38: /* F_OFD_SETLKW */
      return 0;
    case 1033: /* F_ADD_SEALS: nao ha outro processo de quem proteger */
      return 0;
    case 1034: /* F_GET_SEALS */
      return 0;
    default:
      errno = EINVAL;
      return -1;
  }
}

/* ---- operacoes de soquete sobre o par ------------------------------------ */

ssize_t recv(int fd, void* buf, size_t n, int flags) {
  if (SOCK_FD(fd)) return sodos_sock_recv(fd, buf, n, flags);
  if (!get_end(fd)) {
    errno = ENOSYS;
    return -1;
  }
  return sodos_pipe_read(fd, buf, n);
}

ssize_t send(int fd, const void* buf, size_t n, int flags) {
  if (SOCK_FD(fd)) return sodos_sock_send(fd, buf, n, flags);
  if (!get_end(fd)) {
    errno = ENOSYS;
    return -1;
  }
  return sodos_pipe_write(fd, buf, n);
}

ssize_t recvmsg(int fd, struct msghdr* m, int flags) {
  ssize_t total = 0;
  size_t i;
  (void)flags;
  if (!get_end(fd)) {
    errno = ENOSYS;
    return -1;
  }
  for (i = 0; i < m->msg_iovlen; i++) {
    ssize_t r;
    if (total && !sodos_pipe_poll(fd, POLLIN)) break;
    r = sodos_pipe_read(fd, m->msg_iov[i].iov_base, m->msg_iov[i].iov_len);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < m->msg_iov[i].iov_len) break;
  }
  m->msg_controllen = 0;
  m->msg_flags = 0;
  return total;
}

ssize_t sendmsg(int fd, const struct msghdr* m, int flags) {
  ssize_t total = 0;
  size_t i;
  (void)flags;
  if (!get_end(fd)) {
    errno = ENOSYS;
    return -1;
  }
  for (i = 0; i < m->msg_iovlen; i++) {
    ssize_t r = sodos_pipe_write(fd, m->msg_iov[i].iov_base, m->msg_iov[i].iov_len);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < m->msg_iov[i].iov_len) break;
  }
  return total;
}

#endif /* LP64 */
