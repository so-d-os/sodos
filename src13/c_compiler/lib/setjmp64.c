/* setjmp64.c - setjmp/longjmp LP64 usando os builtins do GCC. */
#include <setjmp.h>

int setjmp(jmp_buf b) { return __builtin_setjmp((void**)b); }
void longjmp(jmp_buf b, int v) { (void)v; __builtin_longjmp((void**)b, 1); }
