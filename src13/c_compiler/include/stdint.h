#ifndef _STDINT_H
#define _STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
typedef long int64_t;
typedef unsigned long uint64_t;
#else
typedef long long int64_t;
typedef unsigned long long uint64_t;
#endif
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
typedef long intptr_t;
typedef unsigned long uintptr_t;
typedef long intmax_t;
typedef unsigned long uintmax_t;
#else
typedef int intptr_t;
typedef unsigned int uintptr_t;
typedef long long intmax_t;
typedef unsigned long long uintmax_t;
#endif

/* Tipos "least/fast" que a libc++ e o C99 exigem. No SODOS (x32) o menor
 * tipo com pelo menos N bits e o proprio intN_t, exceto int64. */
typedef int8_t int_least8_t;
typedef uint8_t uint_least8_t;
typedef int16_t int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t int_least32_t;
typedef uint32_t uint_least32_t;
typedef int64_t int_least64_t;
typedef uint64_t uint_least64_t;

typedef int8_t int_fast8_t;
typedef uint8_t uint_fast8_t;
typedef int16_t int_fast16_t;
typedef uint16_t uint_fast16_t;
typedef int32_t int_fast32_t;
typedef uint32_t uint_fast32_t;
typedef int64_t int_fast64_t;
typedef uint64_t uint_fast64_t;

#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 255
#define INT16_MIN (-32768)
#define INT16_MAX 32767
#define UINT16_MAX 65535
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295U
#define INT64_MIN (-INT64_C(9223372036854775807) - 1)
#define INT64_MAX INT64_C(9223372036854775807)
#define UINT64_MAX UINT64_C(18446744073709551615)
#define SIZE_MAX ((size_t)-1)
#define PTRDIFF_MIN (-__PTRDIFF_MAX__ - 1)
#define PTRDIFF_MAX __PTRDIFF_MAX__
#define INTPTR_MIN (-__INTPTR_MAX__ - 1)
#define INTPTR_MAX __INTPTR_MAX__
#define UINTPTR_MAX __UINTPTR_MAX__
#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define INT8_C(c) c
#define UINT8_C(c) c
#define INT16_C(c) c
#define UINT16_C(c) c
#define INT32_C(c) c
#define UINT32_C(c) c##U
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#define INT64_C(c) c##L
#define UINT64_C(c) c##UL
#define INTMAX_C(c) c##L
#define UINTMAX_C(c) c##UL
#else
#define INT64_C(c) c##LL
#define UINT64_C(c) c##ULL
#define INTMAX_C(c) c##LL
#define UINTMAX_C(c) c##ULL
#endif

#endif

#ifndef INT_FAST8_MIN
#define INT_FAST8_MIN INT8_MIN
#define INT_FAST8_MAX INT8_MAX
#define UINT_FAST8_MAX UINT8_MAX
#define INT_FAST16_MIN INT16_MIN
#define INT_FAST16_MAX INT16_MAX
#define UINT_FAST16_MAX UINT16_MAX
#define INT_FAST32_MIN INT32_MIN
#define INT_FAST32_MAX INT32_MAX
#define UINT_FAST32_MAX UINT32_MAX
#define INT_FAST64_MIN INT64_MIN
#define INT_FAST64_MAX INT64_MAX
#define UINT_FAST64_MAX UINT64_MAX
#define INT_LEAST8_MIN INT8_MIN
#define INT_LEAST8_MAX INT8_MAX
#define UINT_LEAST8_MAX UINT8_MAX
#define INT_LEAST16_MIN INT16_MIN
#define INT_LEAST16_MAX INT16_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MIN INT32_MIN
#define INT_LEAST32_MAX INT32_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MIN INT64_MIN
#define INT_LEAST64_MAX INT64_MAX
#define UINT_LEAST64_MAX UINT64_MAX
#endif
