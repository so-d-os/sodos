#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"
#define PRIi32 "i"
#define PRIi64 "lli"
#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"
#define PRIx32 "x"
#define PRIx64 "llx"
#define PRIX32 "X"
#define PRIX64 "llX"
#define PRIo32 "o"
#define PRIo64 "llo"
#define PRIdMAX "lld"
#define PRIuMAX "llu"
#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"
#define SCNd32 "d"
#define SCNd64 "lld"
#define SCNu32 "u"
#define SCNu64 "llu"
#define SCNx32 "x"
#define SCNx64 "llx"

typedef struct { intmax_t quot, rem; } imaxdiv_t;
intmax_t strtoimax(const char* s, char** end, int base);
uintmax_t strtoumax(const char* s, char** end, int base);
intmax_t imaxabs(intmax_t v);

#endif
