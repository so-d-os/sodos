#ifndef _SODOS_SYS_SELECT_H
#define _SODOS_SYS_SELECT_H
#include <sys/time.h>
#define FD_SETSIZE 64
typedef struct { unsigned long bits[2]; } fd_set;
#define FD_ZERO(s) ((s)->bits[0] = (s)->bits[1] = 0)
#define FD_SET(fd, s) ((s)->bits[(fd) / 32] |= 1ul << ((fd) % 32))
#define FD_CLR(fd, s) ((s)->bits[(fd) / 32] &= ~(1ul << ((fd) % 32)))
#define FD_ISSET(fd, s) (((s)->bits[(fd) / 32] >> ((fd) % 32)) & 1)
int select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv);
#endif
