/* alloca.h (SODOS) */
#ifndef _ALLOCA_H
#define _ALLOCA_H
#include <stddef.h>
#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif
#endif
