/* sys/time.h (SODOS) - timeval + fd_set, como o glibc entrega ao NSPR. */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>

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

#endif
