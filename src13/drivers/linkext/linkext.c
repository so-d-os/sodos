/*
 * linkext.c - LINKEXT.DLL: qual programa abre cada extensao (kernel/linkext.h).
 *
 * So e carregado com SET LINKEXT=1. A cada arquivo aberto pelo launcher do
 * WIN le PATHSODOS\LINKEXT.CFG (sem ele, D:\SODOS\LINKEXT.CFG; texto
 * pequeno; editar pelo LINKEXT.EXE ou a
 * mao vale na hora, sem recarregar nada):
 *
 *   ; extensao(oes)=programa
 *   TXT=D:\SODOS\EDIT.EXE
 *   JPG,PNG,GIF=@IMAGEM
 *   HTM,HTML=NETSURF\NETSURF.EXE     (sem letra: relativo ao PATHSODOS)
 *
 * Devolve 0 se achou a extensao e o programa existe (o host abriu), 1 se nao
 * (o launcher padrao do WIN continua). Roda no contexto do kernel: sem libc,
 * sem SSE, sem importar KERNEL.
 */
#include <stdint.h>
#include "linkext.h"

#define CFG_MAX 8192

static char cfg[CFG_MAX];

static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void scopy(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

/* "D:\PASTA\NOME.TXT" -> "TXT" (maiusculas). 0 se nao tem extensao. */
static int ext_of(const char* file, char* out, int max) {
  const char* dot = 0;
  int i, n = 0;
  for (i = 0; file[i]; i++) {
    if (file[i] == '\\' || file[i] == '/' || file[i] == ':') dot = 0;
    else if (file[i] == '.') dot = file + i + 1;
  }
  if (!dot || !dot[0]) return 0;
  for (i = 0; dot[i] && n < max - 1; i++) out[n++] = upc(dot[i]);
  out[n] = 0;
  return n;
}

/* A lista "JPG, .PNG,gif" (ate o '=') tem ext? */
static int list_has(const char* s, const char* end, const char* ext) {
  while (s < end) {
    const char* a;
    int k;
    while (s < end && (*s == ' ' || *s == '\t' || *s == ',' || *s == ';' || *s == '.')) s++;
    a = s;
    while (s < end && *s != ',' && *s != ';' && *s != ' ' && *s != '\t') s++;
    if (s == a) continue;
    for (k = 0; a + k < s && ext[k] && upc(a[k]) == ext[k]; k++) {
    }
    if (a + k == s && !ext[k]) return 1;
  }
  return 0;
}

/* Programa da extensao em out. 1 achou. */
static int lookup(const char* ext, char* out, int max) {
  char* p = cfg;
  while (*p) {
    char* line = p;
    char* eq = 0;
    char* e;
    while (*p && *p != '\n') {
      if (!eq && *p == '=') eq = p;
      p++;
    }
    e = p;
    if (*p) p++;
    while (*line == ' ' || *line == '\t') line++;
    if (!eq || *line == ';' || *line == '#' || *line == '[') continue;
    if (!list_has(line, eq, ext)) continue;
    eq++;
    while (*eq == ' ' || *eq == '\t') eq++;
    while (e > eq && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e == eq) continue;
    {
      int n = (int)(e - eq);
      int i;
      if (n > max - 1) n = max - 1;
      for (i = 0; i < n; i++) out[i] = eq[i];
      out[n] = 0;
    }
    return 1;
  }
  return 0;
}

static int lx_open(const struct linkext_host* h, const char* file) {
  char path[LINKEXT_PATH_MAX];
  char ext[16];
  char prog[LINKEXT_PATH_MAX];
  char full[LINKEXT_PATH_MAX];
  int n;
  if (!h || h->abi < LINKEXT_ABI || !file) return 1;
  if (!ext_of(file, ext, (int)sizeof(ext))) return 1;
  h->sodos_path(LINKEXT_CFG, path, (int)sizeof(path));
  n = h->fload(path, cfg, CFG_MAX);
  /* Sem pendrive (PATHSODOS em U:): a copia do disco. */
  if (n <= 0) n = h->fload("D:\\SODOS\\" LINKEXT_CFG, cfg, CFG_MAX);
  if (n <= 0) return 1;
  cfg[n < CFG_MAX ? n : CFG_MAX - 1] = 0;
  if (!lookup(ext, prog, (int)sizeof(prog))) return 1;
  if (prog[0] == '@') {
    scopy(full, prog, (int)sizeof(full));
  } else if (prog[0] && prog[1] == ':') {
    scopy(full, prog, (int)sizeof(full));
  } else {
    h->sodos_path(prog, full, (int)sizeof(full));
  }
  if (full[0] != '@' && h->fsize(full) < 0) {
    h->log("programa da associacao nao existe:");
    h->log(full);
    return 1;
  }
  if (h->launch(full, file) != 0) return 1;
  h->log(full);
  return 0;
}

static const struct linkext_ops ops = {LINKEXT_ABI, lx_open};

const struct linkext_ops* linkext_driver(void) { return &ops; }
