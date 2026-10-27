/* js.c - interpretador pequeno: var, if, while, alert, getElementById,
 * style.color, style.backgroundColor e innerHTML. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "browser.h"

#define JV_NONE 0
#define JV_NUM  1
#define JV_STR  2
#define JV_EL   3
#define JV_PROP 4
#define JV_DOC  5
#define JV_STY  6

typedef struct {
  int kind, num, el, prop;
  char str[96];
} jval;

typedef struct {
  char name[24];
  jval v;
} jvar;

typedef struct {
  const char* s;
  int n, i;
  doc_t* d;
  jvar vars[32];
  int nvars;
  int dead, err, stop;
} js_t;

static void jnone(jval* v) { memset(v, 0, sizeof *v); }

static void jstr(jval* v, const char* s) {
  jnone(v);
  v->kind = JV_STR;
  if (!s) s = "";
  strncpy(v->str, s, sizeof v->str - 1);
}

static int truth(jval v) {
  if (v.kind == JV_NUM) return v.num != 0;
  if (v.kind == JV_STR) return v.str[0] != 0;
  if (v.kind == JV_EL || v.kind == JV_DOC || v.kind == JV_STY || v.kind == JV_PROP) return 1;
  return 0;
}

static void skip(js_t* J) {
  for (;;) {
    while (J->i < J->n && isspace((unsigned char)J->s[J->i])) J->i++;
    if (J->i + 1 < J->n && J->s[J->i] == '/' && J->s[J->i + 1] == '/') {
      while (J->i < J->n && J->s[J->i] != '\n') J->i++;
      continue;
    }
    if (J->i + 1 < J->n && J->s[J->i] == '/' && J->s[J->i + 1] == '*') {
      J->i += 2;
      while (J->i + 1 < J->n && !(J->s[J->i] == '*' && J->s[J->i + 1] == '/')) J->i++;
      if (J->i + 1 < J->n) J->i += 2;
      continue;
    }
    break;
  }
}

static int eat_kw(js_t* J, const char* kw) {
  int n = (int)strlen(kw);
  char after;
  skip(J);
  if (J->i + n > J->n || strncmp(J->s + J->i, kw, (size_t)n) != 0) return 0;
  after = (J->i + n < J->n) ? J->s[J->i + n] : 0;
  if (isalnum((unsigned char)after) || after == '_' || after == '$') return 0;
  J->i += n;
  return 1;
}

static int take(js_t* J, const char* op) {
  int n = (int)strlen(op);
  skip(J);
  if (J->i + n > J->n || strncmp(J->s + J->i, op, (size_t)n) != 0) return 0;
  if (n == 1 && (op[0] == '=' || op[0] == '!' || op[0] == '<' || op[0] == '>')) {
    if (J->i + 1 < J->n && J->s[J->i + 1] == '=') return 0;
  }
  J->i += n;
  return 1;
}

static int read_ident(js_t* J, char* dst, int max) {
  int n = 0;
  skip(J);
  if (J->i >= J->n) return 0;
  if (!isalpha((unsigned char)J->s[J->i]) && J->s[J->i] != '_' && J->s[J->i] != '$') return 0;
  while (J->i < J->n && (isalnum((unsigned char)J->s[J->i]) || J->s[J->i] == '_' || J->s[J->i] == '$')) {
    if (n < max - 1) dst[n++] = J->s[J->i];
    J->i++;
  }
  dst[n] = 0;
  return 1;
}

static jval parse_expr(js_t* J);

static int find_el(doc_t* d, const char* id) {
  int i;
  for (i = 0; i < d->nels; i++)
    if (strcmp(d->els[i].id, id) == 0) return i;
  return -1;
}

static jvar* var_slot(js_t* J, const char* name) {
  int i;
  for (i = 0; i < J->nvars; i++)
    if (strcmp(J->vars[i].name, name) == 0) return &J->vars[i];
  if (J->nvars >= 32) return 0;
  memset(&J->vars[J->nvars], 0, sizeof J->vars[0]);
  strncpy(J->vars[J->nvars].name, name, sizeof J->vars[0].name - 1);
  return &J->vars[J->nvars++];
}

static unsigned js_color(const char* s) {
  int n, i;
  char buf[32];
  unsigned out = 0;
  if (!s) return 0;
  while (*s == ' ' || *s == '"' || *s == '\'') s++;
  n = (int)strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == ';')) n--;
  if (n >= 7 && s[0] == '#') {
    int h[6], k, ok = 1;
    for (k = 0; k < 6; k++) {
      int c = s[k + 1];
      if (c >= '0' && c <= '9') h[k] = c - '0';
      else if (c >= 'a' && c <= 'f') h[k] = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') h[k] = c - 'A' + 10;
      else ok = 0;
    }
    if (ok) return ((unsigned)((h[0] << 4) | h[1]) << 16) |
                    ((unsigned)((h[2] << 4) | h[3]) << 8) |
                    (unsigned)((h[4] << 4) | h[5]);
  }
  if (n == 4 && s[0] == '#') {
    int r = 0, g = 0, b = 0, ok = 1, k;
    int* p[3];
    p[0] = &r; p[1] = &g; p[2] = &b;
    for (k = 0; k < 3; k++) {
      int c = s[k + 1];
      if (c >= '0' && c <= '9') *p[k] = c - '0';
      else if (c >= 'a' && c <= 'f') *p[k] = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') *p[k] = c - 'A' + 10;
      else ok = 0;
    }
    if (ok) return ((unsigned)(r * 17) << 16) | ((unsigned)(g * 17) << 8) | (unsigned)(b * 17);
  }
  if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  for (i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
  buf[n] = 0;
  if (!strcmp(buf, "black")) out = 0x000000;
  else if (!strcmp(buf, "white")) out = 0xFFFFFF;
  else if (!strcmp(buf, "red")) out = 0xFF0000;
  else if (!strcmp(buf, "green")) out = 0x008000;
  else if (!strcmp(buf, "lime")) out = 0x00FF00;
  else if (!strcmp(buf, "blue")) out = 0x0000FF;
  else if (!strcmp(buf, "yellow")) out = 0xFFFF00;
  else if (!strcmp(buf, "gray") || !strcmp(buf, "grey")) out = 0x808080;
  else if (!strcmp(buf, "orange")) out = 0xFF8000;
  else if (!strcmp(buf, "purple")) out = 0x800080;
  else if (!strcmp(buf, "navy")) out = 0x000080;
  else if (!strcmp(buf, "maroon")) out = 0x800000;
  else if (!strcmp(buf, "silver")) out = 0xC0C0C0;
  else return 0;
  return out;
}

static void paint(doc_t* d, int el, int prop, unsigned color) {
  int a, b, i;
  if (el < 0 || el >= d->nels) return;
  a = d->els[el].run0;
  b = d->els[el].run1;
  if (a > b) return;
  for (i = a; i <= b && i < d->nruns; i++) {
    if (d->runs[i].flags & RUN_IMG) continue;
    if (prop == 1) d->runs[i].color = color;
    else {
      d->runs[i].bg = color;
      d->runs[i].flags |= RUN_BG;
    }
  }
}

static void set_html(doc_t* d, int el, const char* s) {
  char plain[180];
  int o = 0, tag = 0, a, b, i, n;
  if (el < 0 || el >= d->nels || !s) return;
  for (; *s && o < (int)sizeof plain - 1; s++) {
    if (*s == '<') { tag = 1; continue; }
    if (*s == '>') { tag = 0; continue; }
    if (!tag) plain[o++] = *s;
  }
  plain[o] = 0;
  n = o;
  a = d->els[el].run0;
  b = d->els[el].run1;
  if (a > b || a < 0 || a >= d->nruns) return;
  if (d->text_len + n + 1 > d->text_cap) {
    int nc = d->text_cap ? d->text_cap * 2 : 256;
    char* nb;
    while (nc < d->text_len + n + 1) nc *= 2;
    nb = (char*)realloc(d->text, (size_t)nc);
    if (!nb) return;
    d->text = nb;
    d->text_cap = nc;
  }
  memcpy(d->text + d->text_len, plain, (size_t)n);
  d->runs[a].off = d->text_len;
  d->runs[a].len = n;
  d->runs[a].flags &= ~RUN_IMG;
  d->text_len += n;
  d->text[d->text_len] = 0;
  for (i = a + 1; i <= b && i < d->nruns; i++) d->runs[i].len = 0;
}

static void as_text(jval v, char* dst, int max) {
  if (v.kind == JV_STR) strncpy(dst, v.str, (size_t)max - 1);
  else if (v.kind == JV_NUM) snprintf(dst, (size_t)max, "%d", v.num);
  else if (v.kind == JV_NONE) strncpy(dst, "undefined", (size_t)max - 1);
  else strncpy(dst, "[objeto]", (size_t)max - 1);
  dst[max - 1] = 0;
}

static void do_alert(js_t* J, jval v) {
  char tmp[160];
  if (J->dead || !J->d) return;
  as_text(v, tmp, (int)sizeof tmp);
  strncpy(J->d->js_alert, tmp, sizeof J->d->js_alert - 1);
  J->d->js_alert[sizeof J->d->js_alert - 1] = 0;
}

static void apply_prop(js_t* J, jval left, jval right) {
  char tmp[180];
  if (J->dead || left.kind != JV_PROP || !J->d) return;
  if (left.prop == 3) {
    as_text(right, tmp, (int)sizeof tmp);
    set_html(J->d, left.el, tmp);
  } else {
    unsigned col = 0;
    if (right.kind == JV_STR) col = js_color(right.str);
    else if (right.kind == JV_NUM) col = (unsigned)right.num & 0xFFFFFF;
    paint(J->d, left.el, left.prop, col);
  }
}

static jval call_method(js_t* J, jval base, const char* method, jval* args, int na) {
  jval r;
  jnone(&r);
  if (base.kind == JV_NONE && strcmp(method, "alert") == 0) {
    if (na > 0) do_alert(J, args[0]);
    return r;
  }
  if (base.kind == JV_DOC && strcmp(method, "getElementById") == 0 && na > 0) {
    char id[96];
    int el;
    as_text(args[0], id, (int)sizeof id);
    el = J->d ? find_el(J->d, id) : -1;
    if (el >= 0) {
      r.kind = JV_EL;
      r.el = el;
    }
    return r;
  }
  return r;
}

static jval prop_of(jval base, const char* name) {
  jval r;
  jnone(&r);
  if (base.kind == JV_EL && strcmp(name, "style") == 0) {
    r.kind = JV_STY;
    r.el = base.el;
    return r;
  }
  if (base.kind == JV_EL && (strcmp(name, "innerHTML") == 0 || strcmp(name, "innerText") == 0)) {
    r.kind = JV_PROP;
    r.el = base.el;
    r.prop = 3;
    return r;
  }
  if (base.kind == JV_STY && strcmp(name, "color") == 0) {
    r.kind = JV_PROP;
    r.el = base.el;
    r.prop = 1;
    return r;
  }
  if (base.kind == JV_STY &&
      (strcmp(name, "backgroundColor") == 0 || strcmp(name, "background") == 0)) {
    r.kind = JV_PROP;
    r.el = base.el;
    r.prop = 2;
    return r;
  }
  return r;
}

static jval parse_args_call(js_t* J, jval base, const char* method) {
  jval args[4];
  int na = 0;
  memset(args, 0, sizeof args);
  skip(J);
  if (!take(J, ")")) {
    do {
      if (na < 4) args[na++] = parse_expr(J);
      else parse_expr(J);
    } while (take(J, ","));
    take(J, ")");
  }
  if (J->dead && !(base.kind == JV_DOC && strcmp(method, "getElementById") == 0)) {
    jval z;
    jnone(&z);
    return z;
  }
  return call_method(J, base, method, args, na);
}

static jval parse_primary(js_t* J) {
  jval v;
  char name[32];
  jnone(&v);
  skip(J);
  if (J->i >= J->n) { J->err = 1; return v; }
  if (J->s[J->i] == '"' || J->s[J->i] == '\'') {
    char q = J->s[J->i++];
    char buf[96];
    int n = 0;
    while (J->i < J->n && J->s[J->i] != q) {
      char c = J->s[J->i++];
      if (c == '\\' && J->i < J->n) {
        c = J->s[J->i++];
        if (c == 'n') c = '\n';
        else if (c == 't') c = '\t';
      }
      if (n < (int)sizeof buf - 1) buf[n++] = c;
    }
    if (J->i < J->n && J->s[J->i] == q) J->i++;
    buf[n] = 0;
    jstr(&v, buf);
  } else if (isdigit((unsigned char)J->s[J->i]) ||
             (J->s[J->i] == '-' && J->i + 1 < J->n && isdigit((unsigned char)J->s[J->i + 1]) && 0)) {
    int n = 0, sign = 1;
    if (J->s[J->i] == '-') { sign = -1; J->i++; }
    while (J->i < J->n && isdigit((unsigned char)J->s[J->i])) {
      n = n * 10 + (J->s[J->i] - '0');
      J->i++;
    }
    v.kind = JV_NUM;
    v.num = sign * n;
  } else if (take(J, "(")) {
    v = parse_expr(J);
    take(J, ")");
  } else if (read_ident(J, name, (int)sizeof name)) {
    if (take(J, "(")) {
      jval base;
      jnone(&base);
      v = parse_args_call(J, base, name);
    } else if (strcmp(name, "document") == 0) v.kind = JV_DOC;
    else if (strcmp(name, "true") == 0) { v.kind = JV_NUM; v.num = 1; }
    else if (strcmp(name, "false") == 0) { v.kind = JV_NUM; v.num = 0; }
    else {
      jvar* slot = var_slot(J, name);
      if (slot) v = slot->v;
    }
    for (;;) {
      char mem[32];
      if (!take(J, ".")) break;
      if (!read_ident(J, mem, (int)sizeof mem)) { J->err = 1; break; }
      if (take(J, "(")) v = parse_args_call(J, v, mem);
      else v = prop_of(v, mem);
    }
  } else J->err = 1;
  return v;
}

static jval parse_unary(js_t* J) {
  if (take(J, "!")) {
    jval v = parse_unary(J);
    jval r;
    jnone(&r);
    r.kind = JV_NUM;
    r.num = !truth(v);
    return r;
  }
  if (take(J, "-")) {
    jval v = parse_unary(J);
    if (v.kind == JV_NUM) v.num = -v.num;
    return v;
  }
  take(J, "+");
  return parse_primary(J);
}

static jval parse_term(js_t* J) {
  jval a = parse_unary(J);
  for (;;) {
    int div = 0;
    jval b;
    if (take(J, "*")) div = 0;
    else if (take(J, "/")) div = 1;
    else break;
    b = parse_unary(J);
    if (a.kind != JV_NUM) { a.kind = JV_NUM; a.num = 0; }
    if (b.kind != JV_NUM) b.num = 0;
    if (div) a.num = b.num ? a.num / b.num : 0;
    else a.num = a.num * b.num;
  }
  return a;
}

static jval parse_sum(js_t* J) {
  jval a = parse_term(J);
  for (;;) {
    int sub = 0;
    jval b;
    char buf[96];
    if (take(J, "+")) sub = 0;
    else if (take(J, "-")) sub = 1;
    else break;
    b = parse_term(J);
    if (!sub && (a.kind == JV_STR || b.kind == JV_STR)) {
      char ba[96], bb[96];
      as_text(a, ba, 48);
      as_text(b, bb, 48);
      snprintf(buf, sizeof buf, "%s%s", ba, bb);
      jstr(&a, buf);
    } else {
      int na = (a.kind == JV_NUM) ? a.num : 0;
      int nb = (b.kind == JV_NUM) ? b.num : 0;
      jnone(&a);
      a.kind = JV_NUM;
      a.num = sub ? na - nb : na + nb;
    }
  }
  return a;
}

static jval parse_cmp(js_t* J) {
  jval a = parse_sum(J);
  int op = 0;
  skip(J);
  if (J->i + 1 < J->n && J->s[J->i] == '=' && J->s[J->i + 1] == '=') { op = 1; J->i += 2; }
  else if (J->i + 1 < J->n && J->s[J->i] == '!' && J->s[J->i + 1] == '=') { op = 2; J->i += 2; }
  else if (J->i + 1 < J->n && J->s[J->i] == '<' && J->s[J->i + 1] == '=') { op = 5; J->i += 2; }
  else if (J->i + 1 < J->n && J->s[J->i] == '>' && J->s[J->i + 1] == '=') { op = 6; J->i += 2; }
  else if (J->i < J->n && J->s[J->i] == '<') { op = 3; J->i++; }
  else if (J->i < J->n && J->s[J->i] == '>') { op = 4; J->i++; }
  if (!op) return a;
  {
    jval b = parse_sum(J);
    int na = a.kind == JV_NUM ? a.num : 0;
    int nb = b.kind == JV_NUM ? b.num : 0;
    int yes = 0;
    jval r;
    if ((op == 1 || op == 2) && (a.kind == JV_STR || b.kind == JV_STR)) {
      char ba[96], bb[96];
      as_text(a, ba, (int)sizeof ba);
      as_text(b, bb, (int)sizeof bb);
      yes = strcmp(ba, bb) == 0;
      if (op == 2) yes = !yes;
    } else if (op == 1) yes = na == nb;
    else if (op == 2) yes = na != nb;
    else if (op == 3) yes = na < nb;
    else if (op == 4) yes = na > nb;
    else if (op == 5) yes = na <= nb;
    else yes = na >= nb;
    jnone(&r);
    r.kind = JV_NUM;
    r.num = yes;
    return r;
  }
}

static jval parse_expr(js_t* J) {
  jval a = parse_cmp(J);
  for (;;) {
    jval b;
    int orv;
    if (take(J, "&&")) orv = 0;
    else if (take(J, "||")) orv = 1;
    else break;
    b = parse_cmp(J);
    {
      int ta = truth(a), tb = truth(b);
      jnone(&a);
      a.kind = JV_NUM;
      a.num = orv ? (ta || tb) : (ta && tb);
    }
  }
  return a;
}

static void parse_stmt(js_t* J);

static void end_stmt(js_t* J) {
  skip(J);
  if (J->i < J->n && J->s[J->i] == ';') J->i++;
}

static void parse_stmt(js_t* J) {
  char name[32];
  int mark;
  if (J->err || J->stop) return;
  skip(J);
  if (J->i >= J->n) return;
  if (take(J, ";")) return;
  if (take(J, "{")) {
    while (!J->err && !J->stop) {
      skip(J);
      if (J->i >= J->n) break;
      if (take(J, "}")) return;
      parse_stmt(J);
    }
    return;
  }
  if (eat_kw(J, "var") || eat_kw(J, "let") || eat_kw(J, "const")) {
    if (!read_ident(J, name, (int)sizeof name)) { J->err = 1; return; }
    if (take(J, "=")) {
      jval v = parse_expr(J);
      jvar* slot = var_slot(J, name);
      if (slot && !J->dead) slot->v = v;
    } else {
      var_slot(J, name);
    }
    end_stmt(J);
    return;
  }
  if (eat_kw(J, "if")) {
    jval cond;
    int save = J->dead;
    int yes;
    if (!take(J, "(")) { J->err = 1; return; }
    cond = parse_expr(J);
    take(J, ")");
    yes = truth(cond);
    if (!yes) J->dead = 1;
    parse_stmt(J);
    J->dead = save;
    if (eat_kw(J, "else")) {
      if (yes && !save) J->dead = 1;
      parse_stmt(J);
      J->dead = save;
    }
    return;
  }
  if (eat_kw(J, "while")) {
    int cond_i, body_i = 0, end_i = 0, guard = 0;
    int save = J->dead;
    jval cond;
    if (!take(J, "(")) { J->err = 1; return; }
    cond_i = J->i;
    if (J->dead) {
      parse_expr(J);
      take(J, ")");
      parse_stmt(J);
      return;
    }
    for (;;) {
      J->i = cond_i;
      cond = parse_expr(J);
      if (!take(J, ")")) { J->err = 1; return; }
      body_i = J->i;
      if (!truth(cond)) {
        J->dead = 1;
        parse_stmt(J);
        J->dead = save;
        break;
      }
      J->i = body_i;
      parse_stmt(J);
      if (guard == 0) end_i = J->i;
      if (++guard > 200) {
        J->i = end_i;
        break;
      }
    }
    return;
  }
  if (eat_kw(J, "function")) {
    int save = J->dead;
    read_ident(J, name, (int)sizeof name);
    if (take(J, "(")) {
      int d = 1;
      while (J->i < J->n && d) {
        if (J->s[J->i] == '(') d++;
        else if (J->s[J->i] == ')') d--;
        if (d) J->i++;
      }
      if (J->i < J->n && J->s[J->i] == ')') J->i++;
    }
    J->dead = 1;
    parse_stmt(J);
    J->dead = save;
    return;
  }
  if (eat_kw(J, "return")) {
    skip(J);
    if (J->i < J->n && J->s[J->i] != ';' && J->s[J->i] != '}') parse_expr(J);
    end_stmt(J);
    J->stop = 1;
    return;
  }
  mark = J->i;
  if (read_ident(J, name, (int)sizeof name) && take(J, "=")) {
    jval v = parse_expr(J);
    jvar* slot = var_slot(J, name);
    if (slot && !J->dead) slot->v = v;
    end_stmt(J);
    return;
  }
  J->i = mark;
  {
    jval left = parse_expr(J);
    if (take(J, "=")) {
      jval right = parse_expr(J);
      apply_prop(J, left, right);
    }
    end_stmt(J);
  }
}

void js_run(doc_t* d) {
  int s;
  if (!d) return;
  d->js_alert[0] = 0;
  for (s = 0; s < d->nscripts; s++) {
    js_t J;
    if (!d->scripts[s].src || d->scripts[s].len <= 0) continue;
    memset(&J, 0, sizeof J);
    J.s = d->scripts[s].src;
    J.n = d->scripts[s].len;
    J.d = d;
    while (!J.err && !J.stop && J.i < J.n) {
      int before;
      skip(&J);
      if (J.i >= J.n) break;
      before = J.i;
      parse_stmt(&J);
      if (J.i <= before) J.i = before + 1;
    }
  }
}
