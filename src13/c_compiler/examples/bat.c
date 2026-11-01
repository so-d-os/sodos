/* bat.c - lote no estilo COMMAND.COM, com um pouco de bash.
 *   CC BAT.C
 *   BAT SCRIPT.BAT [args...]
 *   SCRIPT.BAT              (o prompt acha o .BAT e chama este EXE)
 * Linha de ate 240 caracteres. Script de ate 32 KB e 256 linhas.
 * Comandos do prompt rodam por k_shell. O codigo deles vira ERRORLEVEL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define BAT_MAX (32 * 1024)
#define BAT_LINES 256
#define LINE_MAX 240
#define VAR_N 24
#define VAR_NAME 16
#define VAR_VAL 96
#define ARG_N 10
#define ARG_L 80

static int echo_on = 1;
static int errorlevel = 0;

static char vname[VAR_N][VAR_NAME];
static char vval[VAR_N][VAR_VAL];
static int nvars = 0;

struct bat_args {
  int n;
  char a[ARG_N][ARG_L];
};
static struct bat_args args;

struct flow {
  char** lines;
  int nlines;
  int jump;
  int stop;
  int code;
  int depth;
};

static void copy_z(char* dst, int max, const char* src) {
  int i = 0;
  if (!src) src = "";
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

static int ci_eq(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
    if (ca != cb) return 0;
  }
  return *a == *b;
}

static char* trim(char* s) {
  char* e;
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  e = s;
  while (*e) e++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
    e--;
    *e = 0;
  }
  return s;
}

static const char* var_get(const char* name) {
  int i;
  for (i = 0; i < nvars; i++)
    if (ci_eq(vname[i], name)) return vval[i];
  return 0;
}

static void var_set(const char* name, const char* val) {
  int i;
  char key[VAR_NAME];
  copy_z(key, VAR_NAME, name);
  for (i = 0; key[i]; i++)
    if (key[i] >= 'a' && key[i] <= 'z') key[i] = (char)(key[i] - 32);
  if (!key[0]) return;
  for (i = 0; i < nvars; i++) {
    if (ci_eq(vname[i], key)) {
      copy_z(vval[i], VAR_VAL, val);
      errorlevel = 0;
      return;
    }
  }
  if (nvars >= VAR_N) {
    printf("set: variaveis demais\n");
    errorlevel = 1;
    return;
  }
  copy_z(vname[nvars], VAR_NAME, key);
  copy_z(vval[nvars], VAR_VAL, val);
  nvars++;
  errorlevel = 0;
}

static void expand(const char* in, char* out, int max) {
  int o = 0;
  while (*in && o < max - 1) {
    const char* v;
    int a;
    if (*in != '%') {
      out[o++] = *in++;
      continue;
    }
    if (in[1] == '%') {
      out[o++] = '%';
      in += 2;
      continue;
    }
    if (in[1] >= '0' && in[1] <= '9') {
      int idx = in[1] - '0';
      v = (idx <= args.n) ? args.a[idx] : "";
      in += 2;
      while (*v && o < max - 1) out[o++] = *v++;
      continue;
    }
    if ((in[1] == '*') && (in[2] == 0 || in[2] == ' ' || in[2] == '\t' || in[2] == '%')) {
      in += 2;
      for (a = 1; a <= args.n && o < max - 1; a++) {
        v = args.a[a];
        if (a > 1 && o < max - 1) out[o++] = ' ';
        while (*v && o < max - 1) out[o++] = *v++;
      }
      continue;
    }
    {
      char name[VAR_NAME];
      int k = 0;
      const char* p = in + 1;
      while (*p && *p != '%' && k < VAR_NAME - 1) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
          break;
        name[k++] = c;
        p++;
      }
      name[k] = 0;
      if (k > 0 && *p == '%') {
        v = var_get(name);
        if (!v) v = "";
        in = p + 1;
        while (*v && o < max - 1) out[o++] = *v++;
        continue;
      }
    }
    out[o++] = *in++;
  }
  out[o] = 0;
}

static char* first_word(char* s, char** rest) {
  char* e;
  s = trim(s);
  e = s;
  while (*e && *e != ' ' && *e != '\t') e++;
  if (*e) {
    *e = 0;
    *rest = trim(e + 1);
  } else {
    *rest = e;
  }
  return s;
}

static int is_comment(const char* s) {
  if (s[0] == '#' || (s[0] == ':' && s[1] == ':')) return 1;
  if (ci_eq(s, "rem")) return 1;
  if ((s[0] == 'R' || s[0] == 'r') && (s[1] == 'E' || s[1] == 'e') &&
      (s[2] == 'M' || s[2] == 'm') && (s[3] == ' ' || s[3] == '\t'))
    return 1;
  return 0;
}

static int leaf_has_dot(const char* s) {
  const char* b = s;
  for (; *s; s++)
    if (*s == '\\' || *s == '/' || *s == ':') b = s + 1;
  for (; *b; b++)
    if (*b == '.') return 1;
  return 0;
}

static int label_at(const char* line, const char* name) {
  char buf[64];
  int i = 0;
  while (*line == ' ' || *line == '\t') line++;
  if (*line != ':' || line[1] == ':') return 0;
  line++;
  while (*line && *line != ' ' && *line != '\t' && i < 63) buf[i++] = *line++;
  buf[i] = 0;
  return ci_eq(buf, name);
}

static int find_label(struct flow* f, const char* name) {
  int i;
  for (i = 0; i < f->nlines; i++)
    if (label_at(f->lines[i], name)) return i;
  return -1;
}

static int run_file(const char* path, int nuser, char** user, int depth);
static void run_one(const char* src, struct flow* f, int expand_it);

static void args_set(const char* script, int nuser, char** user) {
  int i;
  memset(&args, 0, sizeof(args));
  copy_z(args.a[0], ARG_L, script);
  if (nuser > ARG_N - 1) nuser = ARG_N - 1;
  if (nuser < 0) nuser = 0;
  args.n = nuser;
  for (i = 0; i < nuser; i++) copy_z(args.a[i + 1], ARG_L, user[i] ? user[i] : "");
}

static void do_set(char* rest) {
  char* eq;
  int i;
  rest = trim(rest);
  if (!rest[0]) {
    for (i = 0; i < nvars; i++) printf("%s=%s\n", vname[i], vval[i]);
    errorlevel = 0;
    return;
  }
  eq = strchr(rest, '=');
  if (!eq) {
    printf("set: use SET NOME=valor\n");
    errorlevel = 1;
    return;
  }
  *eq = 0;
  rest = trim(rest);
  if (!rest[0]) {
    printf("set: falta o nome\n");
    errorlevel = 1;
    return;
  }
  var_set(rest, trim(eq + 1));
}

static void do_if(char* rest, struct flow* f) {
  int neg = 0;
  int ok = 0;
  char* word;
  char* nxt;
  rest = trim(rest);
  word = first_word(rest, &nxt);
  if (ci_eq(word, "not")) {
    neg = 1;
    rest = nxt;
    word = first_word(rest, &nxt);
  }
  if (ci_eq(word, "errorlevel")) {
    char* num = first_word(nxt, &nxt);
    if (!num[0]) {
      printf("if: falta o nivel\n");
      errorlevel = 1;
      return;
    }
    ok = errorlevel >= atoi(num);
    rest = nxt;
  } else if (ci_eq(word, "exist")) {
    char* name = first_word(nxt, &nxt);
    if (!name[0]) {
      printf("if: falta o arquivo\n");
      errorlevel = 1;
      return;
    }
    ok = k_fsize(name) >= 0;
    rest = nxt;
  } else {
    char* eq = strstr(word, "==");
    char* right;
    if (!eq) {
      printf("if: use EXIST, ERRORLEVEL ou a==b\n");
      errorlevel = 1;
      return;
    }
    *eq = 0;
    right = eq + 2;
    ok = strcmp(word, right) == 0;
    rest = nxt;
  }
  if (neg) ok = !ok;
  rest = trim(rest);
  if (!ok) return;
  if (!rest[0]) {
    printf("if: falta o comando\n");
    errorlevel = 1;
    return;
  }
  /* o comando ja foi expandido na linha inteira */
  run_one(rest, f, 0);
}

static void run_one(const char* src, struct flow* f, int expand_it) {
  char exp[512];
  char line[512];
  char* s;
  char* cmd;
  char* rest;
  int quiet = 0;

  if (expand_it) expand(src, exp, (int)sizeof(exp));
  else copy_z(exp, (int)sizeof(exp), src);
  s = trim(exp);
  if (s[0] == '@') {
    quiet = 1;
    s = trim(s + 1);
  }
  if (!s[0] || s[0] == ':' || is_comment(s)) return;
  if (echo_on && !quiet && expand_it) printf("%s\n", s);
  if ((int)strlen(s) > 255) {
    printf("linha longa demais\n");
    errorlevel = 1;
    return;
  }
  copy_z(line, (int)sizeof(line), s);
  cmd = first_word(line, &rest);
  if (ci_eq(cmd, "rem") || is_comment(cmd)) return;
  if (ci_eq(cmd, "echo") && (ci_eq(rest, "on") || ci_eq(rest, "off"))) {
    echo_on = ci_eq(rest, "on");
    errorlevel = 0;
    return;
  }
  if (ci_eq(cmd, "set")) {
    do_set(rest);
    return;
  }
  if (ci_eq(cmd, "goto")) {
    char* lab = first_word(rest, &rest);
    int at;
    if (!lab[0]) {
      printf("goto: falta o rotulo\n");
      errorlevel = 1;
      return;
    }
    at = find_label(f, lab);
    if (at < 0) {
      printf("goto: rotulo %s nao existe\n", lab);
      f->stop = 1;
      f->code = 1;
      errorlevel = 1;
      return;
    }
    f->jump = at;
    errorlevel = 0;
    return;
  }
  if (ci_eq(cmd, "if")) {
    do_if(rest, f);
    return;
  }
  if (ci_eq(cmd, "exit")) {
    int code = 0;
    rest = trim(rest);
    if (ci_eq(rest, "/b")) rest = "";
    else if ((rest[0] == '/' || rest[0] == '-') && (rest[1] == 'b' || rest[1] == 'B') &&
             (rest[2] == ' ' || rest[2] == '\t'))
      rest = trim(rest + 2);
    if (rest[0]) code = atoi(rest);
    errorlevel = code;
    f->stop = 1;
    f->code = code;
    return;
  }
  if (ci_eq(cmd, "call")) {
    char* file;
    char* uarg[ARG_N];
    int n = 0;
    struct bat_args saved;
    int rc;
    file = first_word(rest, &rest);
    if (!file[0]) {
      printf("call: falta o script\n");
      errorlevel = 1;
      return;
    }
    while (rest[0] && n < ARG_N - 1) {
      uarg[n++] = first_word(rest, &rest);
    }
    memcpy(&saved, &args, sizeof(saved));
    rc = run_file(file, n, uarg, f->depth);
    memcpy(&args, &saved, sizeof(saved));
    errorlevel = rc;
    return;
  }
  errorlevel = k_shell(s);
}

static int open_script(const char* path, char* used, int max) {
  if (k_fsize(path) >= 0) {
    copy_z(used, max, path);
    return 1;
  }
  if (!leaf_has_dot(path) && (int)strlen(path) + 4 < max) {
    snprintf(used, (size_t)max, "%s.BAT", path);
    if (k_fsize(used) >= 0) return 1;
  }
  return 0;
}

static int run_loaded(const char* shown, char* text, int depth) {
  char* lines[BAT_LINES];
  struct flow f;
  int n = 0;
  int i;
  char* p = text;
  f.lines = lines;
  f.nlines = 0;
  f.jump = -1;
  f.stop = 0;
  f.code = 0;
  f.depth = depth;
  while (*p && n < BAT_LINES) {
    char* start = p;
    int len = 0;
    while (*p && *p != '\n') {
      p++;
      len++;
    }
    if (*p == '\n') *p++ = 0;
    if (len > 0 && start[len - 1] == '\r') start[--len] = 0;
    if (len > LINE_MAX) {
      printf("%s: linha %d longa demais\n", shown, n + 1);
      return 1;
    }
    lines[n++] = start;
  }
  if (*p) {
    printf("%s: passa de %d linhas\n", shown, BAT_LINES);
    return 1;
  }
  f.nlines = n;
  for (i = 0; i < n && !f.stop; i++) {
    f.jump = -1;
    run_one(lines[i], &f, 1);
    if (f.stop) break;
    if (f.jump >= 0 && f.jump < n) i = f.jump - 1;
  }
  if (f.stop) return f.code;
  return errorlevel;
}

static int run_file(const char* path, int nuser, char** user, int depth) {
  char used[80];
  char* text;
  int sz, n, rc;
  struct bat_args saved;
  if (depth >= 8) {
    printf("call: scripts aninhados demais\n");
    return 1;
  }
  if (!open_script(path, used, (int)sizeof(used))) {
    printf("nao achei %s\n", path);
    return 1;
  }
  sz = k_fsize(used);
  if (sz < 0) return 1;
  if (sz > BAT_MAX) {
    printf("%s: passa de %d bytes\n", used, BAT_MAX);
    return 1;
  }
  text = (char*)malloc((size_t)sz + 1);
  if (!text) {
    printf("sem memoria\n");
    return 1;
  }
  n = k_fload(used, text, sz + 1);
  if (n < 0) {
    printf("nao li %s\n", used);
    free(text);
    return 1;
  }
  text[n] = 0;
  memcpy(&saved, &args, sizeof(saved));
  args_set(used, nuser, user);
  rc = run_loaded(used, text, depth + 1);
  memcpy(&args, &saved, sizeof(saved));
  free(text);
  return rc;
}

static void usage(void) {
  printf("BAT script [argumentos]\n");
  printf("  BAT DEMO.BAT\n");
  printf("  DEMO.BAT          o prompt tambem aceita o nome do arquivo\n");
  printf("REM ou # ou :: comentam. @ no inicio nao repete a linha.\n");
  printf("ECHO ON|OFF, SET N=valor, %%N%% %%1 %%*, GOTO :rotulo\n");
  printf("IF [NOT] EXIST arq cmd, IF [NOT] ERRORLEVEL n cmd, IF [NOT] a==b cmd\n");
  printf("CALL outro.bat, EXIT [n]. O resto e comando do prompt.\n");
}

int main(int argc, char** argv) {
  int rc;
  if (argc < 2 || ci_eq(argv[1], "/?") || ci_eq(argv[1], "-h")) {
    usage();
    return argc < 2 ? 1 : 0;
  }
  rc = run_file(argv[1], argc - 2, argv + 2, 0);
  return rc;
}
