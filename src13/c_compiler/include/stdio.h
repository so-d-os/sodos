#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* O sistema de arquivos le e grava arquivos inteiros: fopen("r") carrega
 * tudo na memoria e fclose de um arquivo "w"/"a" grava tudo de uma vez. */
typedef struct __so_file FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024
#define FILENAME_MAX 64

/* Sem import de dados entre DLLs: stdin/stdout/stderr vem de uma funcao. */
FILE* __so_stdio(int which);
#define stdin (__so_stdio(0))
#define stdout (__so_stdio(1))
#define stderr (__so_stdio(2))

FILE* fopen(const char* path, const char* mode);
/* data foi obtido com malloc. fclose libera. */
FILE* fopen_mem(char* data, int size);
int fclose(FILE* f);
size_t fread(void* buf, size_t sz, size_t n, FILE* f);
size_t fwrite(const void* buf, size_t sz, size_t n, FILE* f);
int fgetc(FILE* f);
int getc(FILE* f);
int ungetc(int c, FILE* f);
char* fgets(char* s, int n, FILE* f);
int fputc(int c, FILE* f);
int putc(int c, FILE* f);
int fputs(const char* s, FILE* f);
int puts(const char* s);
int putchar(int c);
int getchar(void);
int fseek(FILE* f, long off, int whence);
long ftell(FILE* f);
void rewind(FILE* f);
int feof(FILE* f);
int ferror(FILE* f);
void clearerr(FILE* f);
int fflush(FILE* f);
int remove(const char* path);
void perror(const char* s);

int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t n, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int vsprintf(char* buf, const char* fmt, va_list ap);
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

#endif
