/* sys/stat.h - stat do SODOS + fstatat e lstat */
#ifndef _SODOS_COMPAT_SYS_STAT_H
#define _SODOS_COMPAT_SYS_STAT_H
#include_next <sys/stat.h>
#include <fcntl.h>
int fstatat(int dirfd, const char* path, struct stat* st, int flags);
int lstat(const char* path, struct stat* st);
int chmod(const char* path, mode_t mode);
mode_t umask(mode_t mask);
#endif
