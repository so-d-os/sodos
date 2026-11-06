#ifndef _INTTYPES_H
#define _INTTYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>


#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#if __SIZEOF_POINTER__ == 8
#define PRId64 "ld"
#else
#define PRId64 "lld"
#endif
#define PRIi32 "i"
#if __SIZEOF_POINTER__ == 8
#define PRIi64 "li"
#else
#define PRIi64 "lli"
#endif
#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#if __SIZEOF_POINTER__ == 8
#define PRIu64 "lu"
#else
#define PRIu64 "llu"
#endif
#define PRIx32 "x"
#if __SIZEOF_POINTER__ == 8
#define PRIx64 "lx"
#else
#define PRIx64 "llx"
#endif
#define PRIX32 "X"
#if __SIZEOF_POINTER__ == 8
#define PRIX64 "lX"
#else
#define PRIX64 "llX"
#endif
#define PRIo32 "o"
#if __SIZEOF_POINTER__ == 8
#define PRIo64 "lo"
#else
#define PRIo64 "llo"
#endif
#if __SIZEOF_POINTER__ == 8
#define PRIdMAX "ld"
#else
#define PRIdMAX "lld"
#endif
#if __SIZEOF_POINTER__ == 8
#define PRIuMAX "lu"
#else
#define PRIuMAX "llu"
#endif
#if __SIZEOF_POINTER__ == 8
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#else
#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"
#endif
#define SCNd32 "d"
#define SCNd64 "lld"
#define SCNu32 "u"
#define SCNu64 "llu"
#define SCNx32 "x"
#define SCNx64 "llx"

typedef struct { intmax_t quot, rem; } imaxdiv_t;
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);
intmax_t wcstoimax(const wchar_t* s, wchar_t** end, int base);
uintmax_t wcstoumax(const wchar_t* s, wchar_t** end, int base);
intmax_t strtoimax(const char* s, char** end, int base);
uintmax_t strtoumax(const char* s, char** end, int base);
intmax_t imaxabs(intmax_t v);


/* lote6 */
#ifndef PRIXPTR
#define PRIXPTR "lX"
#define PRIxPTR "lx"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#endif

#ifdef __cplusplus
}
#endif
/* lote2 */
#define PRIi8 "i"
#define PRIi16 "i"
#define PRIx8 "x"
#define PRIx16 "x"
#define PRIX8 "X"
#define PRIX16 "X"
#define PRIo8 "o"
#define PRIo16 "o"

#endif
