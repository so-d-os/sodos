/* sys/ioctl.h - ioctl minimo (FIONREAD/FIONBIO do soquete). */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <sys/types.h>

#define FIONREAD 0x541B
#define FIONBIO 0x5421

int ioctl(int fd, unsigned long request, ...);

#endif
