#ifndef _STDIO_H
#define _STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

/* O sistema de arquivos le e grava arquivos inteiros: fopen("r") carrega
 * tudo na memoria e fclose de um arquivo "w"/"a" grava tudo de uma vez. */
typedef struct __so_file FILE;
typedef long fpos_t;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define FILENAME_MAX 64

/* Sem import de dados entre DLLs: stdin/stdout/stderr vem de uma funcao. */
FILE* __so_stdio(int which);
#define stdin (__so_stdio(0))
#define stdout (__so_stdio(1))
#define stderr (__so_stdio(2))

FILE* fopen(const char* path, const char* mode);
int vasprintf(char** out, const char* fmt, va_list ap);
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
int fileno(FILE* f);
FILE* fdopen(int fd, const char* mode);
int fseeko(FILE* f, off_t off, int whence);
off_t ftello(FILE* f);
long ftell(FILE* f);
void rewind(FILE* f);
int feof(FILE* f);
int ferror(FILE* f);
void clearerr(FILE* f);
int fflush(FILE* f);
/* Usadas pela libc++ (M2b); no SODOS sao simples ou no-op. */
int setvbuf(FILE* f, char* buf, int mode, size_t size);
void setbuf(FILE* f, char* buf);
FILE* freopen(const char* path, const char* mode, FILE* f);
int fgetpos(FILE* f, fpos_t* pos);
int fsetpos(FILE* f, const fpos_t* pos);
#define L_tmpnam 20
#define TMP_MAX 100000
#define FOPEN_MAX 32

int remove(const char* path);
int rename(const char* from, const char* to);
char* tmpnam(char* s);
FILE* tmpfile(void);
void perror(const char* s);

int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t n, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int vsprintf(char* buf, const char* fmt, va_list ap);
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

int scanf(const char* fmt, ...);
int fscanf(FILE* f, const char* fmt, ...);
int sscanf(const char* s, const char* fmt, ...);
int vscanf(const char* fmt, va_list ap);
int vfscanf(FILE* f, const char* fmt, va_list ap);
int vsscanf(const char* s, const char* fmt, va_list ap);


#ifdef __cplusplus
}
#endif
/* lote2 */
ssize_t getline(char** line, size_t* n, FILE* f);
ssize_t getdelim(char** line, size_t* n, int delim, FILE* f);

#endif
