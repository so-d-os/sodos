/* fcntl.h - open do SODOS + *at() e fcntl() */
#ifndef _SODOS_COMPAT_FCNTL_H
#define _SODOS_COMPAT_FCNTL_H
#include_next <fcntl.h>
#define O_CLOEXEC 0x80000
#define O_DIRECTORY 0x10000
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
int fcntl(int fd, int cmd, ...);
int openat(int dirfd, const char* path, int flags, ...);
#endif
