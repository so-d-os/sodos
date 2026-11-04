#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
struct timeval { long tv_sec; long tv_usec; };
struct rusage {
  struct timeval ru_utime, ru_stime;
  long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt;
  long ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv;
  long ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1
int getrusage(int who, struct rusage* usage);
#ifdef __cplusplus
}
#endif
#endif
