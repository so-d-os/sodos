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
int access(const char* path, int mode);
int fsync(int fd);
int ftruncate(int fd, off_t len);
int dup(int fd);
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
unsigned sleep(unsigned s);
int usleep(unsigned us);
long sysconf(int name);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4

#endif
