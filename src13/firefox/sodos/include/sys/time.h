/* sys/time.h (SODOS) - timeval + fd_set, como o glibc entrega ao NSPR. */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
  time_t tv_sec;
  suseconds_t tv_usec;
};

struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};

int gettimeofday(struct timeval* tv, void* tz);

/* O NSPR usa fd_set via <sys/time.h>; o select.h define os macros. */
#include <sys/select.h>

/* lote2 */
int utimes(const char* path, const struct timeval times[2]);
#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, CMP) \
  (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP (b)->tv_usec) : ((a)->tv_sec CMP (b)->tv_sec))
#define timeradd(a, b, r)                          \
  do {                                             \
    (r)->tv_sec = (a)->tv_sec + (b)->tv_sec;       \
    (r)->tv_usec = (a)->tv_usec + (b)->tv_usec;    \
    if ((r)->tv_usec >= 1000000) {                 \
      (r)->tv_sec++;                               \
      (r)->tv_usec -= 1000000;                     \
    }                                              \
  } while (0)
#define timersub(a, b, r)                          \
  do {                                             \
    (r)->tv_sec = (a)->tv_sec - (b)->tv_sec;       \
    (r)->tv_usec = (a)->tv_usec - (b)->tv_usec;    \
    if ((r)->tv_usec < 0) {                        \
      (r)->tv_sec--;                               \
      (r)->tv_usec += 1000000;                     \
    }                                              \
  } while (0)

/* lote3 */
struct itimerval {
  struct timeval it_interval;
  struct timeval it_value;
};
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2
int setitimer(int which, const struct itimerval* val, struct itimerval* old);
int getitimer(int which, struct itimerval* val);

#ifdef __cplusplus
}
#endif

#endif
