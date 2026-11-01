#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

struct stat {
  mode_t st_mode;
  off_t st_size;
  time_t st_mtime;
  dev_t st_dev;
  ino_t st_ino;
};

int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);
int mkdir(const char* path, mode_t mode);

#endif
