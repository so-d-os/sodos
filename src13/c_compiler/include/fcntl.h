#ifndef _FCNTL_H
#define _FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_EXCL   0x80
#define O_NOCTTY 0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_ACCMODE 3
#define O_NOFOLLOW 0x20000
#define O_PATH 010000000
#define AT_FDCWD -100
#define AT_EACCESS 0x200
#define AT_SYMLINK_NOFOLLOW 0x100
int faccessat(int dirfd, const char* path, int mode, int flags);
#define O_DIRECTORY 0x10000
ssize_t readahead(int fd, long off, size_t n);
#define O_CLOEXEC 0x80000
#define O_BINARY 0
#define O_TEXT   0
#define O_SYNC   0x101000
#define O_FSYNC  O_SYNC

/* fcntl(2) */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7

#define FD_CLOEXEC 1

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

struct flock {
  short l_type;
  short l_whence;
  off_t l_start;
  off_t l_len;
  pid_t l_pid;
};

/* lockf(3) */
#define F_ULOCK 0
#define F_LOCK 1
#define F_TLOCK 2
#define F_TEST 3

int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);
int fcntl(int fd, int cmd, ...);
int lockf(int fd, int cmd, off_t len);
int fallocate(int fd, int mode, long off, long len); /* como em sodos_linux_abi.c */
int posix_fallocate(int fd, long off, long len); /* idem */
#define POSIX_FADV_NORMAL 0
#define POSIX_FADV_RANDOM 1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED 3
#define POSIX_FADV_DONTNEED 4
#define POSIX_FADV_NOREUSE 5
int posix_fadvise(int fd, off_t off, off_t len, int advice);


#ifdef __cplusplus
}
#endif
#endif
