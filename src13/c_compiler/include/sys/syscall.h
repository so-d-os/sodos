#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H
/* Numeros x86_64 usados pelo Gecko/Rust. */
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_getpid 39
#define SYS_gettid 186
#define SYS_getrandom 318
#define SYS_membarrier 324
#define SYS_rseq 334
#define SYS_rt_sigprocmask 14
#define SYS_futex 202
#define SYS_getdents64 217
#define SYS_tgkill 234
#define SYS_ioprio_set 251
#define SYS_ioprio_get 252
#define __NR_gettid SYS_gettid
#define __NR_futex SYS_futex
#define __NR_getdents64 SYS_getdents64
#define __NR_tgkill SYS_tgkill
#define __NR_ioprio_set SYS_ioprio_set
#define __NR_ioprio_get SYS_ioprio_get
#endif
