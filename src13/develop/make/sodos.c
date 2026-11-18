/* sodos.c - camada do SODOS para o pdpmake: getopt, glob, execucao das
 * receitas, touch e comparacao de nomes de arquivo. */
#include "make.h"
#include <dirent.h>

/* ---------- getopt ---------- */
char *mk_optarg;
int mk_optind = 1;
static int opt_pos; /* posicao dentro de "-abc" */

int
mk_getopt(int argc, char **argv, const char *optstr)
{
	const char *arg, *o;
	int c;

	if (mk_optind == 0) {
		mk_optind = 1;
		opt_pos = 0;
	}
	if (*optstr == '+')
		optstr++;
	mk_optarg = NULL;

	if (opt_pos == 0) {
		if (mk_optind >= argc)
			return -1;
		arg = argv[mk_optind];
		if (arg[0] != '-' || arg[1] == '\0')
			return -1;
		if (arg[1] == '-' && arg[2] == '\0') {
			mk_optind++;
			return -1;
		}
		opt_pos = 1;
	}

	arg = argv[mk_optind];
	c = (unsigned char)arg[opt_pos++];
	o = c == ':' ? NULL : strchr(optstr, c);
	if (o == NULL) {
		fprintf(stderr, "%s: opcao invalida -- '%c'\n", myname, c);
		if (arg[opt_pos] == '\0') {
			mk_optind++;
			opt_pos = 0;
		}
		return '?';
	}

	if (o[1] == ':') {
		if (arg[opt_pos] != '\0') {
			mk_optarg = (char *)arg + opt_pos;
		} else if (mk_optind + 1 < argc) {
			mk_optarg = argv[++mk_optind];
		} else {
			fprintf(stderr, "%s: a opcao -%c pede um argumento\n", myname, c);
			mk_optind++;
			opt_pos = 0;
			return '?';
		}
		mk_optind++;
		opt_pos = 0;
	} else if (arg[opt_pos] == '\0') {
		mk_optind++;
		opt_pos = 0;
	}
	return c;
}

/* ---------- nomes de arquivo ---------- */
unsigned char
mk_namefold(unsigned char c)
{
	if (c >= 'a' && c <= 'z')
		return (unsigned char)(c - 'a' + 'A');
	if (c == '/')
		return '\\';
	return c;
}

int
mk_namencmp(const char *a, const char *b, size_t n)
{
	while (n--) {
		unsigned char x = mk_namefold((unsigned char)*a++);
		unsigned char y = mk_namefold((unsigned char)*b++);

		if (x != y)
			return x < y ? -1 : 1;
		if (x == '\0')
			break;
	}
	return 0;
}

int
mk_namecmp(const char *a, const char *b)
{
	return mk_namencmp(a, b, (size_t)-1);
}

static const char *
last_sep(const char *path)
{
	const char *s, *sep = NULL;

	for (s = path; *s; ++s) {
		if (*s == '\\' || *s == '/' || *s == ':')
			sep = s;
	}
	return sep;
}

const char *
mk_basename(const char *path)
{
	const char *sep = last_sep(path);

	return sep ? sep + 1 : path;
}

char *
mk_stpcpy(char *d, const char *s)
{
	while ((*d = *s++) != '\0')
		d++;
	return d;
}

/* ---------- glob ---------- */
static int
wild_match(const char *pat, const char *name)
{
	for (; *pat; ++pat, ++name) {
		if (*pat == '*') {
			while (pat[1] == '*')
				pat++;
			if (pat[1] == '\0')
				return TRUE;
			for (; *name; ++name) {
				if (wild_match(pat + 1, name))
					return TRUE;
			}
			return FALSE;
		}
		if (*name == '\0')
			return FALSE;
		if (*pat != '?' && mk_namefold((unsigned char)*pat) !=
				mk_namefold((unsigned char)*name))
			return FALSE;
	}
	return *name == '\0';
}

int
mk_glob(const char *pat, int flags, void *errfn, glob_t *gd)
{
	const char *sep = last_sep(pat);
	const char *mask = sep ? sep + 1 : pat;
	char *dir;
	DIR *d;
	struct dirent *e;

	(void)flags;
	(void)errfn;
	gd->gl_pathc = 0;
	gd->gl_pathv = NULL;

	// Pasta do padrao, com o separador no fim ("" = pasta atual).
	dir = xstrndup(pat, (size_t)(mask - pat));
	d = opendir(*dir ? dir : ".");
	if (d == NULL) {
		free(dir);
		return GLOB_NOMATCH;
	}
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' && mask[0] != '.')
			continue;
		if (!wild_match(mask, e->d_name))
			continue;
		gd->gl_pathv = xrealloc(gd->gl_pathv,
							(gd->gl_pathc + 1) * sizeof(char *));
		gd->gl_pathv[gd->gl_pathc++] = xconcat3(dir, e->d_name, "");
	}
	closedir(d);
	free(dir);
	return gd->gl_pathc ? 0 : GLOB_NOMATCH;
}

void
mk_globfree(glob_t *gd)
{
	size_t i;

	for (i = 0; i < gd->gl_pathc; ++i)
		free(gd->gl_pathv[i]);
	free(gd->gl_pathv);
	gd->gl_pathc = 0;
	gd->gl_pathv = NULL;
}

/* ---------- receitas ---------- */
/*
 * Roda uma linha de receita como o prompt faria: comando interno, .EXE
 * ou .BAT.  O __so_shell da libc guarda o estado do malloc e do stdio
 * do make enquanto o outro programa roda (a LIBC.DLL e uma so).
 * O prompt aceita ate 255 caracteres por linha.
 */
int
mk_run(const char *line)
{
	if (strlen(line) > 255) {
		warning("linha de comando com mais de 255 caracteres");
		return -1;
	}
	return __so_shell(line);
}

/* ---------- touch ---------- */
int
mk_touch(const char *path)
{
	FILE *f;
	char *buf = NULL;
	long size = 0;
	int ok;

	f = fopen(path, "rb");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0) {
			buf = xmalloc((size_t)size);
			fseek(f, 0, SEEK_SET);
			if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
				fclose(f);
				free(buf);
				return -1;
			}
		}
		fclose(f);
	}

	f = fopen(path, "wb");
	if (f == NULL) {
		free(buf);
		return -1;
	}
	ok = size <= 0 || fwrite(buf, 1, (size_t)size, f) == (size_t)size;
	if (fclose(f) != 0)
		ok = FALSE;
	free(buf);
	return ok ? 0 : -1;
}

/* ---------- pastas do sistema ---------- */
static int
file_exists(const char *path)
{
	return k_fsize(path) >= 0;
}

const char *
mk_sysinc(void)
{
	return file_exists("D:\\STDIO.H") ? "D:\\" : "U:\\INCLUDE\\";
}

const char *
mk_syslib(void)
{
	return file_exists("D:\\LIBC.DLL") ? "D:\\" : "U:\\";
}
