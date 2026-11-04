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
#endif
