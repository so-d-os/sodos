/* sys/select.h - fd_set e select() sobre poll() da libsodos. */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <sys/time.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif

typedef struct {
  unsigned int bits[(FD_SETSIZE + 31) / 32];
} fd_set;

#define FD_ZERO(s)                              \
  do {                                          \
    int _i;                                     \
    for (_i = 0; _i < (int)(sizeof((s)->bits) / sizeof((s)->bits[0])); _i++) \
      (s)->bits[_i] = 0;                        \
  } while (0)
#define FD_SET(fd, s) ((s)->bits[(fd) / 32] |= 1u << ((fd) % 32))
#define FD_CLR(fd, s) ((s)->bits[(fd) / 32] &= ~(1u << ((fd) % 32)))
#define FD_ISSET(fd, s) (((s)->bits[(fd) / 32] >> ((fd) % 32)) & 1u)

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout);

#endif
