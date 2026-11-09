/* utime.h - utime() sobre utimes() (firefox/sodos/port/sodos_linux_abi.c). */
#ifndef _UTIME_H
#define _UTIME_H

#include <sys/time.h>
#include <sys/types.h>

struct utimbuf {
  time_t actime;
  time_t modtime;
};

static inline int utime(const char* path, const struct utimbuf* t)
{
  struct timeval tv[2];
  if (!t) return utimes(path, 0);
  tv[0].tv_sec = t->actime;
  tv[0].tv_usec = 0;
  tv[1].tv_sec = t->modtime;
  tv[1].tv_usec = 0;
  return utimes(path, tv);
}

#endif
