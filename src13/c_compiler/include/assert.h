#ifndef _ASSERT_H
#define _ASSERT_H

void __so_assert(const char* expr, const char* file, int line);

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __so_assert(#e, __FILE__, __LINE__))
#endif

/* C11: em C++ static_assert ja e palavra-chave. */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

#endif
