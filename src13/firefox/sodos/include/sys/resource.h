/* sys/resource.h - recursos de processo (minimo para o NSPR). */
#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/time.h>

struct rusage {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long ru_maxrss;
  long ru_ixrss;
  long ru_idrss;
  long ru_isrss;
  long ru_minflt;
  long ru_majflt;
  long ru_nswap;
  long ru_inblock;
  long ru_oublock;
  long ru_msgsnd;
  long ru_msgrcv;
  long ru_nsignals;
  long ru_nvcsw;
  long ru_nivcsw;
};

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1

#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_NOFILE 7

typedef unsigned long rlim_t;
struct rlimit {
  rlim_t rlim_cur;
  rlim_t rlim_max;
};

int getrusage(int who, struct rusage* usage);
int getrlimit(int resource, struct rlimit* rlim);
int setrlimit(int resource, const struct rlimit* rlim);

#endif
