/* errno.h - codigos extras (POSIX) para o NetSurf sobre o errno do SODOS */
#ifndef _SODOS_COMPAT_ERRNO_H
#define _SODOS_COMPAT_ERRNO_H
#include_next <errno.h>
#define EPERM 1
#define ESRCH 3
#define EINTR 4
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define ECHILD 10
#define EAGAIN 11
#define EWOULDBLOCK EAGAIN
#define EFAULT 14
#define EBUSY 16
#define EXDEV 18
#define ENODEV 19
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define ENAMETOOLONG 36
#define ENOTEMPTY 39
#define ELOOP 40
#define EILSEQ 84
#define ENOTSUP 95
#define EOPNOTSUPP ENOTSUP
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ECONNRESET 104
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EINPROGRESS 115
#define EOVERFLOW 75
#endif
