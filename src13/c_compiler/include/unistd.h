#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
off_t lseek(int fd, off_t off, int whence);
int close(int fd);
int unlink(const char* path);
int rmdir(const char* path);
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int isatty(int fd);

#endif
