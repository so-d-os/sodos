#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);
#define WNOHANG 1
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#ifdef __cplusplus
}
#endif
#endif
