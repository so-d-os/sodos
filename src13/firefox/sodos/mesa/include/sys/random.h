#ifndef _SODOS_SYS_RANDOM_H
#define _SODOS_SYS_RANDOM_H
#include <sys/types.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
#ifdef __cplusplus
extern "C"
#endif
ssize_t getrandom(void* buf, size_t n, unsigned int flags);
#endif
