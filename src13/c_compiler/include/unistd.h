#ifndef _UNISTD_H
#define _UNISTD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
ssize_t readlink(const char* path, char* buf, size_t n);
long syscall(long number, ...);
off_t lseek(int fd, off_t off, int whence);
int close(int fd);
int unlink(const char* path);
int rmdir(const char* path);
int chdir(const char* path);
int execvp(const char* file, char* const argv[]);
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
int fork(void);
int pipe(int fds[2]);
int dup2(int oldfd, int newfd);
pid_t wait(int* status);
void _exit(int code);
int execv(const char* path, char* const argv[]);
int execve(const char* path, char* const argv[], char* const envp[]);

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
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85


#ifdef __cplusplus
}
#endif
#endif
