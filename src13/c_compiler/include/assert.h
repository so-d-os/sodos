#ifndef _ASSERT_H
#define _ASSERT_H

void __so_assert(const char* expr, const char* file, int line);

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __so_assert(#e, __FILE__, __LINE__))
#endif

#endif
