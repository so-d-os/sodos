/* sys/ptrace.h (SODOS) - constantes; nao ha ptrace. */
#ifndef _SYS_PTRACE_H
#define _SYS_PTRACE_H
#ifdef __cplusplus
extern "C" {
#endif
#define PTRACE_TRACEME 0
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
long ptrace(int request, ...);
#ifdef __cplusplus
}
#endif
#endif
