/* sodos.h - o que o pdpmake precisa e o SODOS nao tem (ou tem diferente).
 * Entra pelo make.h quando o compilador e o CC do SODOS (__SO__). */
#ifndef MAKE_SODOS_H
#define MAKE_SODOS_H

#include <so.h>

/* O CC nao tem __attribute__. */
#undef NORETURN
#define NORETURN

/* getopt proprio: aceita o '+' na frente das opcoes e volta ao inicio
 * com mk_optind = 0 (o make le MAKEFLAGS e depois a linha de comando). */
extern char *mk_optarg;
extern int mk_optind;
int mk_getopt(int argc, char **argv, const char *optstr);
#define getopt mk_getopt
#define optarg mk_optarg
#define optind mk_optind
#undef GETOPT_RESET
#define GETOPT_RESET() (mk_optind = 0)

/* glob so no ultimo componente do caminho: * e ?, sem diferenciar caixa. */
typedef struct {
	size_t gl_pathc;
	char **gl_pathv;
} glob_t;
#define GLOB_NOSORT 0
#define GLOB_NOMATCH 3
int mk_glob(const char *pat, int flags, void *errfn, glob_t *gd);
void mk_globfree(glob_t *gd);
#define glob mk_glob
#define globfree mk_globfree

/* A receita roda como uma linha do prompt e devolve o codigo de saida.
 * Nao ha sinais: todo retorno e um "exit". */
int mk_run(const char *line);
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#define WIFEXITED(s) 1
#define WEXITSTATUS(s) (s)
#define WIFSIGNALED(s) 0
#define WTERMSIG(s) 0

/* make -t: regrava o arquivo para a FAT marcar a hora de agora. */
int mk_touch(const char *path);

/* Nomes de arquivo: FAT nao diferencia caixa e '/' vale como '\'. */
int mk_namecmp(const char *a, const char *b);
int mk_namencmp(const char *a, const char *b, size_t n);
unsigned char mk_namefold(unsigned char c);
#define NAMECMP mk_namecmp
#define NAMENCMP mk_namencmp

const char *mk_basename(const char *path);
char *mk_stpcpy(char *d, const char *s);
#define stpcpy mk_stpcpy

/* Pastas dos headers e das DLLs do sistema (como nos BUILD.BAT). */
const char *mk_sysinc(void);
const char *mk_syslib(void);

#endif
