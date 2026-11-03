#ifndef _SODOS_COMPAT_UNISTD_H
#define _SODOS_COMPAT_UNISTD_H
#include_next <unistd.h>
int unlinkat(int dirfd, const char* path, int flags);
ssize_t pread(int fd, void* buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off);
#endif
