#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __GNUC__
/* gcc -mx32: lib/setjmp.c (asm) guarda rbx, rbp, r12-r15, rsp e rip */
typedef unsigned long long jmp_buf[8];
int setjmp(jmp_buf b) __attribute__((returns_twice));
void longjmp(jmp_buf b, int v) __attribute__((noreturn));
#else
/* cc: setjmp/longjmp sao gerados inline pelo compilador. Use como no C
 * padrao: if (setjmp(b)), switch (setjmp(b)), x = setjmp(b), setjmp(b) == v. */
typedef int jmp_buf[6];
int setjmp(jmp_buf b);
void longjmp(jmp_buf b, int v);
#endif

typedef jmp_buf sigjmp_buf;
#define sigsetjmp(b, save) setjmp(b)
#define siglongjmp(b, v) longjmp(b, v)
#define _setjmp(b) setjmp(b)
#define _longjmp(b, v) longjmp(b, v)

#endif
