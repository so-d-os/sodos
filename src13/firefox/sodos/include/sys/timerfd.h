/* sys/timerfd.h (SODOS) - constantes do glibc.
 * Implementacao: dart/port/sodos_epoll.c (no Firefox nao existe). */
#ifndef _SYS_TIMERFD_H
#define _SYS_TIMERFD_H
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TFD_CLOEXEC 02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2

#ifndef _SODOS_ITIMERSPEC
#define _SODOS_ITIMERSPEC
struct itimerspec {
  struct timespec it_interval;
  struct timespec it_value;
};
#endif

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec* new_value,
                    struct itimerspec* old_value);
int timerfd_gettime(int fd, struct itimerspec* curr_value);

#ifdef __cplusplus
}
#endif
#endif
