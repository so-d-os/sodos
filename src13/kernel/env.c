/*
 * env.c - Variaveis de ambiente do SODOS (SET NOME=valor), como no DOS:
 * uma tabela so, do sistema todo. O AUTOEXEC.BAT, o prompt (SET) e os
 * programas (k_getenv/k_setenv, BAT.EXE) usam a mesma.
 *
 * Nomes sem diferenca de caixa (guardados em maiusculas). SET NOME= apaga.
 * env_gen() muda a cada SET: quem depende de uma variavel (fsdrv.c com
 * FAT32DLL) compara a geracao em vez de reler a tabela toda hora.
 */
#include <stdint.h>
#include "env.h"
#include "shell.h"

#define ENV_N 32

static char env_name[ENV_N][ENV_NAME_MAX];
static char env_val[ENV_N][ENV_VAL_MAX];
static uint32_t gen = 1;

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_is(const char* a, const char* b) {
  while (*a && *b && up(*a) == up(*b)) {
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

static int find(const char* name) {
  int i;
  for (i = 0; i < ENV_N; i++)
    if (env_name[i][0] && name_is(env_name[i], name)) return i;
  return -1;
}

const char* env_get(const char* name) {
  int i;
  if (!name || !name[0]) return 0;
  i = find(name);
  return (i >= 0 && env_val[i][0]) ? env_val[i] : 0;
}

int env_set(const char* name, const char* val) {
  int i, k;
  if (!name || !name[0]) return -1;
  for (k = 0; name[k]; k++)
    if (name[k] == '=' || name[k] == ' ' || k >= ENV_NAME_MAX - 1) return -1;
  i = find(name);
  if (!val || !val[0]) {
    if (i >= 0) {
      env_name[i][0] = 0;
      env_val[i][0] = 0;
      gen++;
    }
    return 0;
  }
  if (i < 0) {
    for (i = 0; i < ENV_N && env_name[i][0]; i++) {
    }
    if (i >= ENV_N) return -2;
    for (k = 0; name[k]; k++) env_name[i][k] = up(name[k]);
    env_name[i][k] = 0;
  }
  for (k = 0; val[k] && k < ENV_VAL_MAX - 1; k++) env_val[i][k] = val[k];
  env_val[i][k] = 0;
  gen++;
  return 0;
}

uint32_t env_gen(void) { return gen; }

int env_at(int index, const char** name, const char** val) {
  int i, n = 0;
  for (i = 0; i < ENV_N; i++) {
    if (!env_name[i][0]) continue;
    if (n++ == index) {
      if (name) *name = env_name[i];
      if (val) *val = env_val[i];
      return 1;
    }
  }
  return 0;
}

static char* trim(char* s) {
  int n = 0;
  while (*s == ' ' || *s == '\t') s++;
  while (s[n]) n++;
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
  return s;
}

/* SET, SET NOME=valor, SET NOME= (apaga), SET PREFIXO (lista as que comecam). */
int cmd_set(int argc, char** argv) {
  char line[ENV_NAME_MAX + ENV_VAL_MAX + 8];
  int i, k = 0;
  char* s;
  char* eq;
  const char* n;
  const char* v;
  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (i > 1 && k < (int)sizeof(line) - 1) line[k++] = ' ';
    while (*a && k < (int)sizeof(line) - 1) line[k++] = *a++;
  }
  line[k] = 0;
  s = trim(line);
  eq = s;
  while (*eq && *eq != '=') eq++;
  if (!*eq) {
    int len = 0;
    while (s[len]) len++;
    for (i = 0; env_at(i, &n, &v); i++) {
      int j;
      for (j = 0; j < len && up(n[j]) == up(s[j]); j++) {
      }
      if (j < len) continue;
      puts_k(n);
      putc('=');
      puts_k(v);
      putc('\n');
    }
    return 0;
  }
  *eq = 0;
  s = trim(s);
  k = env_set(s, trim(eq + 1));
  if (k == -2) puts_k("set: variaveis demais\n");
  else if (k != 0) puts_k("set: use SET NOME=valor\n");
  return k ? 1 : 0;
}
