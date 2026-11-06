/* sys/uio.h - iovec para readv/writev. */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
  void* iov_base;
  size_t iov_len;
};

ssize_t readv(int fd, const struct iovec* iov, int cnt);
ssize_t writev(int fd, const struct iovec* iov, int cnt);

#ifdef __cplusplus
}
#endif

#endif
