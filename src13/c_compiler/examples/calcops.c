/* calcops.c - avaliador de expressoes inteiras: + - * / % ( ) */
#include <ctype.h>
#include "calcops.h"

static const char* p;
static const char* error_msg;
static int ops;

static num expr(void);

static void skip_spaces(void) {
  while (*p == ' ') p++;
}

static num primary(void) {
  num v = 0;
  skip_spaces();
  if (*p == '(') {
    p++;
    v = expr();
    skip_spaces();
    if (*p != ')') error_msg = "falta ')'";
    else p++;
    return v;
  }
  if (*p == '-') {
    p++;
    ops++;
    return -primary();
  }
  if (!isdigit((unsigned char)*p)) {
    error_msg = "esperava numero";
    return 0;
  }
  while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
  return v;
}

static num term(void) {
  num v = primary();
  for (;;) {
    char op;
    num r;
    skip_spaces();
    op = *p;
    if (op != '*' && op != '/' && op != '%') return v;
    p++;
    r = primary();
    ops++;
    if ((op == '/' || op == '%') && r == 0) {
      error_msg = "divisao por zero";
      return 0;
    }
    if (op == '*') v *= r;
    else if (op == '/') v /= r;
    else v %= r;
  }
}

static num expr(void) {
  num v = term();
  for (;;) {
    char op;
    skip_spaces();
    op = *p;
    if (op != '+' && op != '-') return v;
    p++;
    ops++;
    if (op == '+') v += term();
    else v -= term();
  }
}

num calc_eval(const char* s, const char** err) {
  num v;
  p = s;
  error_msg = 0;
  v = expr();
  skip_spaces();
  if (!error_msg && *p) error_msg = "sobrou texto";
  *err = error_msg;
  return v;
}

int calc_ops_used(void) { return ops; }
