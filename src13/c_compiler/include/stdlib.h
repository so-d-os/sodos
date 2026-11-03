#ifndef _STDLIB_H
#define _STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

void* malloc(size_t n);
void* calloc(size_t n, size_t sz);
void* realloc(void* p, size_t n);
void* aligned_alloc(size_t align, size_t n);
void free(void* p);

void exit(int code);
void _Exit(int code);
void abort(void);
int atexit(void (*fn)(void));
void quick_exit(int code);
int at_quick_exit(void (*fn)(void));
int mblen(const char* s, size_t n);
int system(const char* cmd);
char* getenv(const char* name);

int atoi(const char* s);
long atol(const char* s);
long long atoll(const char* s);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
long long strtoll(const char* s, char** end, int base);
unsigned long long strtoull(const char* s, char** end, int base);
double atof(const char* s);
double strtod(const char* s, char** end);
float strtof(const char* s, char** end);
long double strtold(const char* s, char** end);

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int putenv(char* s);
char* realpath(const char* path, char* out);

int abs(int v);
long labs(long v);
long long llabs(long long v);

int rand(void);
void srand(unsigned seed);

void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t n, size_t sz,
              int (*cmp)(const void*, const void*));


#ifdef __cplusplus
}
#endif
#endif
