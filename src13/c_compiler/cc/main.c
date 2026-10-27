/* main.c - cc: compila .c e .cpp em .OBJ e chama o link para gerar .EXE ou .DLL.
 *
 *   cc [-c] [-o saida] [-dll] [-I dir] [-D nome[=valor]] [-L dir] [-E] [-w] [-v]
 *      [-nostdlib] arquivos (.c .obj .lib .dll .imp)
 *
 * Sem -c: link /out:PROG.EXE CRT0.OBJ *.obj ... LIBC.DLL KERNEL.IMP
 * No SO os arquivos padrao (headers, CRT0.OBJ, LIBC.DLL, KERNEL.IMP) ficam em
 * D: ; no Linux, em ../include e ../out relativos ao executavel do cc. */
#include "cc.h"

static char* defines[64];
static int ndefines;
static char* inputs[256];
static int ninputs;
static char* out_path;
static char* lib_dir;
static int opt_c, opt_E, opt_dll, opt_v, opt_nostdlib;

static void usage(void) {
  fprintf(stderr,
          "uso: cc [-c] [-o saida] [-dll] [-I dir] [-D nome[=valor]] [-L dir] [-E] [-w] [-v]\n"
          "        [-nostdlib] arquivos.c/.cpp [.obj .lib .dll .imp]\n");
  exit(2);
}

static char* dir_of_path(const char* p) {
  int n = (int)strlen(p);
  while (n > 0 && p[n - 1] != '/' && p[n - 1] != '\\' && p[n - 1] != ':') n--;
  return xstrndup(p, n);
}

static char* ext_of(char* p) {
  char* dot = strrchr(p, '.');
  char* sep = strrchr(p, '/');
  char* sep2 = strrchr(p, '\\');
  if (sep2 > sep) sep = sep2;
  if (!dot || (sep && dot < sep)) return (char*)"";
  return dot;
}

static int ext_is(char* p, const char* e) {
  char* x = ext_of(p);
  int i;
  if (strlen(x) != strlen(e)) return 0;
  for (i = 0; x[i]; i++)
    if (tolower((unsigned char)x[i]) != tolower((unsigned char)e[i])) return 0;
  return 1;
}

static int is_source(char* p) {
  return ext_is(p, ".c") || ext_is(p, ".cc") || ext_is(p, ".cpp") || ext_is(p, ".cxx");
}

/* troca a extensao, mantendo maiusculo/minusculo do original */
static char* replace_ext(char* p, const char* lower_ext) {
  char* x = ext_of(p);
  int base = (int)(x - p);
  int upper = x[0] && x[1] && isupper((unsigned char)x[1]);
  char* r = format("%.*s%s", base, p, lower_ext);
  if (upper) {
    int i;
    for (i = base; r[i]; i++) r[i] = (char)toupper((unsigned char)r[i]);
  }
  return r;
}

static void print_tokens(Token* t) {
  int first = 1;
  for (; t->kind != TK_EOF; t = t->next) {
    if (!first && t->bol) printf("\n");
    else if (!first && t->space) printf(" ");
    printf("%.*s", t->len, t->loc);
    first = 0;
  }
  printf("\n");
}

static void setup_pp(void) {
  int i;
  pp_reset();
  pp_init();
  for (i = 0; i < ndefines; i++) pp_define(defines[i]);
}

static void compile(char* src, char* obj) {
  Token* tok;
  Obj* prog;
  setup_pp();
  tok = pp_file(src);
  if (opt_E) {
    print_tokens(tok);
    return;
  }
  prog = parse(tok);
  codegen(prog, obj);
  if (opt_v) fprintf(stderr, "cc: %s -> %s\n", src, obj);
}

int main(int argc, char** argv) {
  char* self_dir = dir_of_path(argv[0]);
  char* objs[256];
  int nobjs = 0;
  int i;
  int extra_inc = 0;
  char* inc_dirs[32];
  for (i = 1; i < argc; i++) {
    char* a = argv[i];
    if (strcmp(a, "-c") == 0) opt_c = 1;
    else if (strcmp(a, "-E") == 0) opt_E = 1;
    else if (strcmp(a, "-v") == 0) opt_v = 1;
    else if (strcmp(a, "-w") == 0) opt_warn = 0;
    else if (strcmp(a, "-dll") == 0 || strcmp(a, "-shared") == 0) opt_dll = 1;
    else if (strcmp(a, "-nostdlib") == 0) opt_nostdlib = 1;
    else if (strcmp(a, "-o") == 0) {
      if (++i >= argc) usage();
      out_path = argv[i];
    } else if (strncmp(a, "-I", 2) == 0) {
      char* d = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
      if (!d) usage();
      if (extra_inc >= 32) error("-I demais");
      inc_dirs[extra_inc++] = d;
    } else if (strncmp(a, "-D", 2) == 0) {
      char* d = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
      if (!d) usage();
      if (ndefines >= 64) error("-D demais");
      defines[ndefines++] = d;
    } else if (strncmp(a, "-L", 2) == 0) {
      char* d = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
      if (!d) usage();
      lib_dir = d;
    } else if (strcmp(a, "-O") == 0 || strcmp(a, "-O0") == 0 || strcmp(a, "-O1") == 0 ||
               strcmp(a, "-O2") == 0 || strcmp(a, "-g") == 0) {
      /* sem otimizador; aceita para scripts */
    } else if (a[0] == '-' && a[1]) {
      fprintf(stderr, "cc: opcao desconhecida %s\n", a);
      usage();
    } else {
      if (ninputs >= 256) error("arquivos demais");
      inputs[ninputs++] = a;
    }
  }
  if (ninputs == 0) usage();

  for (i = 0; i < extra_inc; i++) pp_add_include(inc_dirs[i]);
#ifdef __SO__
  pp_add_include("D:");
  if (!lib_dir) lib_dir = "D:";
#else
  pp_add_include(format("%s../include/", self_dir));
  if (!lib_dir) lib_dir = format("%s../out/", self_dir);
#endif
  {
    int n = (int)strlen(lib_dir);
    if (n > 0 && lib_dir[n - 1] != '/' && lib_dir[n - 1] != '\\' && lib_dir[n - 1] != ':')
      lib_dir = format("%s/", lib_dir);
  }

  for (i = 0; i < ninputs; i++) {
    char* in = inputs[i];
    if (is_source(in)) {
      char* obj;
      if (opt_E) {
        compile(in, NULL);
        continue;
      }
      if (opt_c && out_path && ninputs == 1) obj = out_path;
      else obj = replace_ext(in, ".obj");
      compile(in, obj);
      objs[nobjs++] = obj;
    } else {
      objs[nobjs++] = in;
    }
  }
  if (opt_E || opt_c) return 0;

  {
    Buf cmd;
    char* exe = out_path ? out_path : replace_ext(inputs[0], opt_dll ? ".dll" : ".exe");
    char* tmp;
    int rc;
    memset(&cmd, 0, sizeof(cmd));
#ifdef __SO__
    tmp = format("LINK /out:%s", exe);
#else
    tmp = format("%slink /out:%s", self_dir, exe);
#endif
    buf_add(&cmd, tmp, (int)strlen(tmp));
    if (opt_dll) buf_add(&cmd, " /dll", 5);
    if (!opt_nostdlib && !opt_dll) {
      tmp = format(" %sCRT0.OBJ", lib_dir);
      buf_add(&cmd, tmp, (int)strlen(tmp));
    }
    for (i = 0; i < nobjs; i++) {
      tmp = format(" %s", objs[i]);
      buf_add(&cmd, tmp, (int)strlen(tmp));
    }
    if (!opt_nostdlib) {
      tmp = format(" %sLIBC.DLL %sKERNEL.IMP", lib_dir, lib_dir);
      buf_add(&cmd, tmp, (int)strlen(tmp));
    }
    buf_byte(&cmd, 0);
    if (opt_v) fprintf(stderr, "cc: %s\n", cmd.p);
    rc = system(cmd.p);
#ifndef __SO__
    for (i = 0; i < ninputs; i++)
      if (is_source(inputs[i])) remove(objs[i]);
#endif
    if (rc != 0) {
      fprintf(stderr, "cc: link falhou (%d)\n", rc);
      return 1;
    }
  }
  return 0;
}
