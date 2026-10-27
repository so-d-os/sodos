/* pp.c - Pre-processador: #include, #define (objeto/funcao, #, ##, __VA_ARGS__),
 * #if/#ifdef/#elif/#else/#endif, #undef, #error, #pragma once.
 * Expansao com hidesets (algoritmo de Prosser), como os compiladores grandes. */
#include "cc.h"

struct Hideset {
  Hideset* next;
  char* name;
};

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam* next;
  char* name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg* next;
  char* name;
  Token* tok;
};

typedef Token* (*macro_fn)(Token*);

typedef struct Macro Macro;
struct Macro {
  Macro* next;
  char* name;
  int is_obj;
  MacroParam* params;
  int variadic;
  Token* body;
  macro_fn handler;
};

typedef struct Cond Cond;
struct Cond {
  Cond* next;
  int ctx; /* 0 then, 1 elif, 2 else */
  Token* tok;
  int included;
};

typedef struct Once Once;
struct Once {
  Once* next;
  char* path;
};

#define MHASH 1024
static Macro* macros[MHASH];
static Cond* conds;
static Once* onces;
static char* inc_dirs[32];
static int ninc;
char* pp_main_file;
static int include_depth;

static char* kw_defined;
static char* kw_va_args;

static Token* preprocess2(Token* tok);

/* ---------- hidesets ---------- */
static Hideset* hs_new(char* name) {
  Hideset* h = (Hideset*)xalloc((int)sizeof(Hideset));
  h->name = name;
  return h;
}

static int hs_contains(Hideset* h, char* name) {
  for (; h; h = h->next)
    if (h->name == name) return 1;
  return 0;
}

static Hideset* hs_union(Hideset* a, Hideset* b) {
  Hideset head;
  Hideset* cur = &head;
  head.next = NULL;
  for (; a; a = a->next) cur = cur->next = hs_new(a->name);
  cur->next = b;
  return head.next;
}

static Hideset* hs_intersection(Hideset* a, Hideset* b) {
  Hideset head;
  Hideset* cur = &head;
  head.next = NULL;
  for (; a; a = a->next)
    if (hs_contains(b, a->name)) cur = cur->next = hs_new(a->name);
  return head.next;
}

/* ---------- tokens ---------- */
static Token* copy_token(Token* t) {
  Token* c = (Token*)xalloc((int)sizeof(Token));
  *c = *t;
  c->next = NULL;
  return c;
}

static Token* add_hideset(Token* tok, Hideset* hs) {
  Token head;
  Token* cur = &head;
  head.next = NULL;
  for (; tok; tok = tok->next) {
    Token* t = copy_token(tok);
    t->hs = hs_union(t->hs, hs);
    cur = cur->next = t;
  }
  return head.next;
}

static Token* append(Token* a, Token* b) {
  Token head;
  Token* cur = &head;
  if (a->kind == TK_EOF) return b;
  head.next = NULL;
  for (; a->kind != TK_EOF; a = a->next) cur = cur->next = copy_token(a);
  cur->next = b;
  return head.next;
}

static int is_hash(Token* t) { return t->bol && tok_punct(t, '#'); }

static Token* skip_line(Token* tok) {
  if (tok->bol) return tok;
  warn_tok(tok, "tokens a mais na diretiva");
  while (!tok->bol) tok = tok->next;
  return tok;
}

static Token* copy_line(Token** rest, Token* tok) {
  Token head;
  Token* cur = &head;
  head.next = NULL;
  for (; !tok->bol; tok = tok->next) cur = cur->next = copy_token(tok);
  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

static Token* new_num_token(int v, Token* ref) {
  char* s = format("%d\n", v);
  Token* t = tokenize_string(s, ref->file, ref->line);
  t->bol = 0;
  return t;
}

static Token* new_str_token(const char* s, Token* ref) {
  Buf b;
  int i;
  memset(&b, 0, sizeof(b));
  buf_byte(&b, '"');
  for (i = 0; s[i]; i++) {
    if (s[i] == '\\' || s[i] == '"') buf_byte(&b, '\\');
    buf_byte(&b, s[i]);
  }
  buf_add(&b, "\"\n", 3);
  {
    Token* t = tokenize_string(b.p, ref->file, ref->line);
    t->bol = 0;
    return t;
  }
}

/* ---------- macros ---------- */
static unsigned mhash(char* name) { return (unsigned)(((unsigned long)name >> 3) & (MHASH - 1)); }

static Macro* find_macro(Token* t) {
  Macro* m;
  if (t->kind != TK_IDENT) return NULL;
  for (m = macros[mhash(t->name)]; m; m = m->next)
    if (m->name == t->name) return m;
  return NULL;
}

static Macro* add_macro(char* name, int is_obj, Token* body) {
  Macro* m = (Macro*)xalloc((int)sizeof(Macro));
  unsigned h = mhash(name);
  m->name = name;
  m->is_obj = is_obj;
  m->body = body;
  m->next = macros[h];
  macros[h] = m;
  return m;
}

static void undef_macro(char* name) {
  Macro** pp = &macros[mhash(name)];
  for (; *pp; pp = &(*pp)->next) {
    if ((*pp)->name == name) {
      *pp = (*pp)->next;
      return;
    }
  }
}

static MacroParam* read_params(Token** rest, Token* tok, int* variadic) {
  MacroParam head;
  MacroParam* cur = &head;
  head.next = NULL;
  while (!tok_punct(tok, ')')) {
    MacroParam* p;
    if (cur != &head) {
      if (!tok_punct(tok, ',')) error_tok(tok, "esperava ','");
      tok = tok->next;
    }
    if (tok_punct(tok, P_ELLIPSIS)) {
      *variadic = 1;
      tok = tok->next;
      if (!tok_punct(tok, ')')) error_tok(tok, "esperava ')'");
      break;
    }
    if (tok->kind != TK_IDENT) error_tok(tok, "esperava nome de parametro");
    p = (MacroParam*)xalloc((int)sizeof(MacroParam));
    p->name = tok->name;
    cur = cur->next = p;
    tok = tok->next;
  }
  *rest = tok->next;
  return head.next;
}

static void read_define(Token** rest, Token* tok) {
  char* name;
  Macro* m;
  if (tok->kind != TK_IDENT) error_tok(tok, "nome de macro invalido");
  name = tok->name;
  tok = tok->next;
  if (!tok->space && tok_punct(tok, '(') && !tok->bol) {
    int va = 0;
    MacroParam* params = read_params(&tok, tok->next, &va);
    m = add_macro(name, 0, copy_line(rest, tok));
    m->params = params;
    m->variadic = va;
  } else {
    m = add_macro(name, 1, copy_line(rest, tok));
  }
}

static MacroArg* read_one_arg(Token** rest, Token* tok, int read_rest) {
  Token head;
  Token* cur = &head;
  int depth = 0;
  MacroArg* a;
  head.next = NULL;
  for (;;) {
    if (depth == 0 && tok_punct(tok, ')')) break;
    if (depth == 0 && !read_rest && tok_punct(tok, ',')) break;
    if (tok->kind == TK_EOF) error_tok(tok, "fim inesperado nos argumentos da macro");
    if (tok_punct(tok, '(')) depth++;
    else if (tok_punct(tok, ')')) depth--;
    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }
  cur->next = new_eof(tok);
  a = (MacroArg*)xalloc((int)sizeof(MacroArg));
  a->tok = head.next;
  *rest = tok;
  return a;
}

static MacroArg* read_args(Token** rest, Token* tok, MacroParam* params, int variadic, Token* mtok) {
  Token* start = tok;
  MacroArg head;
  MacroArg* cur = &head;
  MacroParam* pp;
  tok = tok->next->next; /* nome ( */
  head.next = NULL;
  for (pp = params; pp; pp = pp->next) {
    if (cur != &head) {
      if (!tok_punct(tok, ',')) error_tok(mtok, "argumentos de menos para a macro");
      tok = tok->next;
    }
    cur = cur->next = read_one_arg(&tok, tok, 0);
    cur->name = pp->name;
  }
  if (variadic) {
    MacroArg* a;
    if (tok_punct(tok, ')')) {
      a = (MacroArg*)xalloc((int)sizeof(MacroArg));
      a->tok = new_eof(tok);
    } else {
      if (params) {
        if (!tok_punct(tok, ',')) error_tok(mtok, "argumentos de menos para a macro");
        tok = tok->next;
      }
      a = read_one_arg(&tok, tok, 1);
    }
    a->name = kw_va_args;
    cur = cur->next = a;
  } else if (!params && !tok_punct(tok, ')')) {
    /* M() sem parametros mas com algo dentro */
    MacroArg* dummy = read_one_arg(&tok, tok, 1);
    (void)dummy;
  }
  if (!tok_punct(tok, ')')) error_tok(mtok, "argumentos demais para a macro");
  (void)start;
  *rest = tok;
  return head.next;
}

static MacroArg* find_arg(MacroArg* args, Token* t) {
  if (t->kind != TK_IDENT) return NULL;
  for (; args; args = args->next)
    if (args->name == t->name) return args;
  return NULL;
}

static char* join_tokens(Token* tok, Token* end) {
  Buf b;
  Token* t;
  memset(&b, 0, sizeof(b));
  for (t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->space) buf_byte(&b, ' ');
    buf_add(&b, t->loc, t->len);
  }
  buf_byte(&b, 0);
  return b.p;
}

static Token* stringize(Token* hash, Token* arg) {
  return new_str_token(join_tokens(arg, NULL), hash);
}

static Token* paste(Token* lhs, Token* rhs) {
  char* s = format("%.*s%.*s\n", lhs->len, lhs->loc, rhs->len, rhs->loc);
  Token* t = tokenize_string(s, lhs->file, lhs->line);
  if (t->next->kind != TK_EOF) error_tok(lhs, "## gerou '%s', que nao e um token", s);
  t->bol = lhs->bol;
  t->space = lhs->space;
  t->hs = lhs->hs;
  t->origin = lhs->origin;
  return t;
}

static Token* subst(Token* tok, MacroArg* args) {
  Token head;
  Token* cur = &head;
  head.next = NULL;
  while (tok->kind != TK_EOF) {
    MacroArg* arg;
    if (tok_punct(tok, '#')) {
      arg = find_arg(args, tok->next);
      if (!arg) error_tok(tok->next, "'#' precisa de um parametro da macro");
      cur = cur->next = stringize(tok, arg->tok);
      tok = tok->next->next;
      continue;
    }
    if (tok_punct(tok, P_HASHHASH)) {
      if (cur == &head) error_tok(tok, "'##' no inicio da macro");
      tok = tok->next;
      arg = find_arg(args, tok);
      if (arg) {
        if (arg->tok->kind != TK_EOF) {
          Token* t;
          *cur = *paste(cur, arg->tok);
          for (t = arg->tok->next; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
        }
        tok = tok->next;
        continue;
      }
      *cur = *paste(cur, tok);
      tok = tok->next;
      continue;
    }
    arg = find_arg(args, tok);
    if (arg && tok_punct(tok->next, P_HASHHASH)) {
      Token* rhs = tok->next->next;
      if (arg->tok->kind == TK_EOF) {
        MacroArg* arg2 = find_arg(args, rhs);
        if (arg2) {
          Token* t;
          for (t = arg2->tok; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
        } else {
          cur = cur->next = copy_token(rhs);
        }
        tok = rhs->next;
        continue;
      }
      {
        Token* t;
        for (t = arg->tok; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
      }
      tok = tok->next;
      continue;
    }
    if (arg) {
      Token* t = preprocess2(arg->tok);
      if (t->kind != TK_EOF) {
        t->bol = tok->bol;
        t->space = tok->space;
      }
      for (; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
      tok = tok->next;
      continue;
    }
    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }
  cur->next = tok;
  return head.next;
}

static int expand_macro(Token** rest, Token* tok) {
  Macro* m;
  Token* body;
  Token* t;
  Hideset* hs;
  MacroArg* args;
  Token* rparen;
  if (hs_contains(tok->hs, tok->name)) return 0;
  m = find_macro(tok);
  if (!m) return 0;
  if (m->handler) {
    *rest = m->handler(tok);
    (*rest)->next = tok->next;
    (*rest)->bol = tok->bol;
    (*rest)->space = tok->space;
    return 1;
  }
  if (m->is_obj) {
    hs = hs_union(tok->hs, hs_new(m->name));
    body = add_hideset(m->body, hs);
    for (t = body; t->kind != TK_EOF; t = t->next) t->origin = tok;
    *rest = append(body, tok->next);
    (*rest)->bol = tok->bol;
    (*rest)->space = tok->space;
    return 1;
  }
  if (!tok_punct(tok->next, '(')) return 0;
  args = read_args(&rparen, tok, m->params, m->variadic, tok);
  hs = hs_intersection(tok->hs, rparen->hs);
  hs = hs_union(hs, hs_new(m->name));
  body = subst(m->body, args);
  body = add_hideset(body, hs);
  for (t = body; t->kind != TK_EOF; t = t->next) t->origin = tok;
  *rest = append(body, rparen->next);
  (*rest)->bol = tok->bol;
  (*rest)->space = tok->space;
  return 1;
}

/* ---------- #if ---------- */
static long long pp_expr(Token** rest, Token* tok);

static long long pp_primary(Token** rest, Token* tok) {
  long long v;
  if (tok_punct(tok, '(')) {
    v = pp_expr(&tok, tok->next);
    if (!tok_punct(tok, ')')) error_tok(tok, "esperava ')'");
    *rest = tok->next;
    return v;
  }
  if (tok->kind == TK_NUM) {
    convert_number(tok);
    *rest = tok->next;
    return tok->ival;
  }
  if (tok->kind == TK_CHAR) {
    *rest = tok->next;
    return tok->ival;
  }
  error_tok(tok, "expressao invalida no #if");
  return 0;
}

static long long pp_unary(Token** rest, Token* tok) {
  if (tok_punct(tok, '-')) return -pp_unary(rest, tok->next);
  if (tok_punct(tok, '+')) return pp_unary(rest, tok->next);
  if (tok_punct(tok, '!')) return !pp_unary(rest, tok->next);
  if (tok_punct(tok, '~')) return ~pp_unary(rest, tok->next);
  return pp_primary(rest, tok);
}

static int prec_of(Token* t) {
  if (t->kind != TK_PUNCT) return -1;
  switch (t->punct) {
    case '*': case '/': case '%': return 10;
    case '+': case '-': return 9;
    case P_SHL: case P_SHR: return 8;
    case '<': case '>': case P_LE: case P_GE: return 7;
    case P_EQ: case P_NE: return 6;
    case '&': return 5;
    case '^': return 4;
    case '|': return 3;
    case P_LAND: return 2;
    case P_LOR: return 1;
  }
  return -1;
}

static long long pp_binary(Token** rest, Token* tok, int min) {
  long long a = pp_unary(&tok, tok);
  for (;;) {
    int p = prec_of(tok);
    int op;
    long long b;
    if (p < min) break;
    op = tok->punct;
    b = pp_binary(&tok, tok->next, p + 1);
    switch (op) {
      case '*': a = a * b; break;
      case '/': if (!b) error_tok(tok, "divisao por zero no #if"); a = a / b; break;
      case '%': if (!b) error_tok(tok, "divisao por zero no #if"); a = a % b; break;
      case '+': a = a + b; break;
      case '-': a = a - b; break;
      case P_SHL: a = a << b; break;
      case P_SHR: a = a >> b; break;
      case '<': a = a < b; break;
      case '>': a = a > b; break;
      case P_LE: a = a <= b; break;
      case P_GE: a = a >= b; break;
      case P_EQ: a = a == b; break;
      case P_NE: a = a != b; break;
      case '&': a = a & b; break;
      case '^': a = a ^ b; break;
      case '|': a = a | b; break;
      case P_LAND: a = a && b; break;
      case P_LOR: a = a || b; break;
    }
  }
  *rest = tok;
  return a;
}

static long long pp_expr(Token** rest, Token* tok) {
  long long c = pp_binary(&tok, tok, 1);
  if (tok_punct(tok, '?')) {
    long long a = pp_expr(&tok, tok->next);
    long long b;
    if (!tok_punct(tok, ':')) error_tok(tok, "esperava ':'");
    b = pp_expr(&tok, tok->next);
    *rest = tok;
    return c ? a : b;
  }
  *rest = tok;
  return c;
}

static long long eval_if(Token** rest, Token* tok) {
  Token* start = tok;
  Token* line = copy_line(rest, tok);
  Token head;
  Token* cur = &head;
  Token* t;
  long long v;
  head.next = NULL;
  /* defined X / defined(X) antes de expandir */
  for (t = line; t->kind != TK_EOF;) {
    if (t->kind == TK_IDENT && t->name == kw_defined) {
      Token* s = t;
      int paren = 0;
      int def;
      t = t->next;
      if (tok_punct(t, '(')) {
        paren = 1;
        t = t->next;
      }
      if (t->kind != TK_IDENT) error_tok(s, "defined precisa de um nome");
      def = find_macro(t) != NULL;
      t = t->next;
      if (paren) {
        if (!tok_punct(t, ')')) error_tok(t, "esperava ')'");
        t = t->next;
      }
      cur = cur->next = new_num_token(def, s);
      continue;
    }
    cur = cur->next = t;
    t = t->next;
  }
  cur->next = t;
  line = preprocess2(head.next);
  for (t = line; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT) {
      Token* z = new_num_token(0, t);
      Token* nx = t->next;
      *t = *z;
      t->next = nx;
    }
  }
  if (line->kind == TK_EOF) error_tok(start, "#if sem expressao");
  v = pp_expr(&t, line);
  if (t->kind != TK_EOF) error_tok(t, "tokens a mais no #if");
  return v;
}

static Cond* push_cond(Token* tok, int included) {
  Cond* c = (Cond*)xalloc((int)sizeof(Cond));
  c->next = conds;
  c->tok = tok;
  c->included = included;
  conds = c;
  return c;
}

static Token* skip_nested(Token* tok);

static Token* skip_cond(Token* tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (tok_is(tok->next, "if") || tok_is(tok->next, "ifdef") ||
                         tok_is(tok->next, "ifndef"))) {
      tok = skip_nested(tok->next->next);
      continue;
    }
    if (is_hash(tok) && (tok_is(tok->next, "elif") || tok_is(tok->next, "else") ||
                         tok_is(tok->next, "endif")))
      break;
    tok = tok->next;
  }
  return tok;
}

static Token* skip_nested(Token* tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (tok_is(tok->next, "if") || tok_is(tok->next, "ifdef") ||
                         tok_is(tok->next, "ifndef"))) {
      tok = skip_nested(tok->next->next);
      continue;
    }
    if (is_hash(tok) && tok_is(tok->next, "endif")) return tok->next->next;
    tok = tok->next;
  }
  return tok;
}

/* ---------- #include ---------- */
static char* dir_of(const char* path) {
  int n = (int)strlen(path);
  while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\' && path[n - 1] != ':') n--;
  return xstrndup(path, n);
}

static int file_exists(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

static char* search_include(char* name, int quoted, Token* from) {
  int i;
  if (name[0] == '/' || (name[0] && name[1] == ':')) return file_exists(name) ? name : NULL;
  if (quoted && from && from->file) {
    char* p = format("%s%s", dir_of(from->file->name), name);
    if (file_exists(p)) return p;
  }
  for (i = 0; i < ninc; i++) {
    char* p = format("%s%s", inc_dirs[i], name);
    if (file_exists(p)) return p;
  }
  return NULL;
}

static int is_once(char* path) {
  Once* o;
  for (o = onces; o; o = o->next)
    if (strcmp(o->path, path) == 0) return 1;
  return 0;
}

static Token* include_file(Token* rest, char* path, Token* from) {
  int len;
  char* text;
  Token* t;
  if (is_once(path)) return rest;
  text = read_file(path, &len);
  if (!text) error_tok(from, "nao consegui ler %s", path);
  if (include_depth > 64) error_tok(from, "#include aninhado demais");
  t = tokenize(new_file(path, text, len));
  return append(t, rest);
}

static char* read_include_name(Token** rest, Token* tok, int* quoted) {
  Token* t;
  if (tok->kind == TK_STR) {
    *quoted = 1;
    *rest = skip_line(tok->next);
    return xstrndup(tok->loc + 1, tok->len - 2);
  }
  if (tok_punct(tok, '<')) {
    Token* start = tok;
    for (t = tok->next; !tok_punct(t, '>'); t = t->next)
      if (t->bol || t->kind == TK_EOF) error_tok(start, "esperava '>'");
    *quoted = 0;
    *rest = skip_line(t->next);
    return join_tokens(tok->next, t);
  }
  if (tok->kind == TK_IDENT) {
    Token* line = preprocess2(copy_line(rest, tok));
    return read_include_name(&t, line, quoted);
  }
  error_tok(tok, "esperava \"arquivo\" ou <arquivo>");
  return NULL;
}

/* ---------- laco principal ---------- */
static Token* preprocess2(Token* tok) {
  Token head;
  Token* cur = &head;
  head.next = NULL;
  while (tok->kind != TK_EOF) {
    Token* start;
    if (tok->kind == TK_IDENT && expand_macro(&tok, tok)) continue;
    if (!is_hash(tok)) {
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }
    start = tok;
    tok = tok->next;
    if (tok->bol) continue; /* '#' sozinho */
    if (tok_is(tok, "include")) {
      int quoted;
      char* name = read_include_name(&tok, tok->next, &quoted);
      char* path = search_include(name, quoted, start);
      if (!path) error_tok(start, "nao achei o include '%s'", name);
      tok = include_file(tok, path, start);
      continue;
    }
    if (tok_is(tok, "define")) {
      read_define(&tok, tok->next);
      continue;
    }
    if (tok_is(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT) error_tok(tok, "nome de macro invalido");
      undef_macro(tok->name);
      tok = skip_line(tok->next);
      continue;
    }
    if (tok_is(tok, "if")) {
      long long v = eval_if(&tok, tok->next);
      push_cond(start, v != 0);
      if (!v) tok = skip_cond(tok);
      continue;
    }
    if (tok_is(tok, "ifdef") || tok_is(tok, "ifndef")) {
      int neg = tok_is(tok, "ifndef");
      int def;
      tok = tok->next;
      if (tok->kind != TK_IDENT) error_tok(tok, "esperava nome de macro");
      def = find_macro(tok) != NULL;
      if (neg) def = !def;
      push_cond(tok, def);
      tok = skip_line(tok->next);
      if (!def) tok = skip_cond(tok);
      continue;
    }
    if (tok_is(tok, "elif")) {
      if (!conds || conds->ctx == 2) error_tok(start, "#elif fora de lugar");
      conds->ctx = 1;
      if (!conds->included && eval_if(&tok, tok->next)) {
        conds->included = 1;
      } else {
        if (conds->included) copy_line(&tok, tok->next);
        tok = skip_cond(tok);
      }
      continue;
    }
    if (tok_is(tok, "else")) {
      if (!conds || conds->ctx == 2) error_tok(start, "#else fora de lugar");
      conds->ctx = 2;
      tok = skip_line(tok->next);
      if (conds->included) tok = skip_cond(tok);
      else conds->included = 1;
      continue;
    }
    if (tok_is(tok, "endif")) {
      if (!conds) error_tok(start, "#endif sem #if");
      conds = conds->next;
      tok = skip_line(tok->next);
      continue;
    }
    if (tok_is(tok, "error")) {
      Token* line = copy_line(&tok, tok->next);
      error_tok(start, "#error %s", join_tokens(line, NULL));
    }
    if (tok_is(tok, "warning")) {
      Token* line = copy_line(&tok, tok->next);
      warn_tok(start, "#warning %s", join_tokens(line, NULL));
      continue;
    }
    if (tok_is(tok, "pragma")) {
      if (tok_is(tok->next, "once") && start->file) {
        Once* o = (Once*)xalloc((int)sizeof(Once));
        o->path = start->file->name;
        o->next = onces;
        onces = o;
      }
      copy_line(&tok, tok->next);
      continue;
    }
    if (tok_is(tok, "line") || tok->kind == TK_NUM) {
      Token* line = preprocess2(copy_line(&tok, tok->kind == TK_NUM ? tok : tok->next));
      Token* t;
      int delta;
      if (line->kind != TK_NUM) error_tok(line, "#line precisa de um numero");
      delta = (int)strtol(line->loc, NULL, 10) - tok->line;
      for (t = tok; t->kind != TK_EOF; t = t->next)
        if (t->file == start->file) t->line += delta;
      continue;
    }
    error_tok(tok, "diretiva desconhecida");
  }
  cur->next = tok;
  return head.next;
}

/* ---------- macros embutidas ---------- */
static Token* file_macro(Token* t) {
  while (t->origin) t = t->origin;
  return new_str_token(t->file ? t->file->name : "?", t);
}

static Token* line_macro(Token* t) {
  while (t->origin) t = t->origin;
  return new_num_token(t->line, t);
}

static void builtin(const char* name, macro_fn fn) {
  Macro* m = add_macro(intern(name, (int)strlen(name)), 1, NULL);
  m->handler = fn;
}

void pp_define(char* def) {
  char* eq = strchr(def, '=');
  char* text;
  SrcFile* f;
  if (eq) text = format("%.*s %s\n", (int)(eq - def), def, eq + 1);
  else text = format("%s 1\n", def);
  f = new_file("<linha de comando>", text, (int)strlen(text));
  {
    Token* t = tokenize(f);
    Token* rest;
    read_define(&rest, t);
  }
}

void pp_add_include(char* dir) {
  int n = (int)strlen(dir);
  if (ninc >= 32) error("-I demais");
  if (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\' && dir[n - 1] != ':')
    dir = format("%s/", dir);
  inc_dirs[ninc++] = dir;
}

/* entre um arquivo e outro: macros, #if e #pragma once comecam do zero */
void pp_reset(void) {
  memset(macros, 0, sizeof(macros));
  conds = NULL;
  onces = NULL;
  include_depth = 0;
}

void pp_init(void) {
  kw_defined = intern("defined", 7);
  kw_va_args = intern("__VA_ARGS__", 11);
  builtin("__FILE__", file_macro);
  builtin("__LINE__", line_macro);
  pp_define("__SOCC__");
  pp_define("__SO__");
  pp_define("__i386__");
  pp_define("__i386");
  pp_define("i386");
  pp_define("__STDC__");
  pp_define("__STDC_VERSION__=199901L");
  pp_define("__SIZEOF_POINTER__=4");
  pp_define("__SIZEOF_INT__=4");
  pp_define("__SIZEOF_LONG__=4");
  pp_define("__SIZEOF_LONG_LONG__=8");
  pp_define("__ILP32__");
  pp_define("__DATE__=\"Jan  1 2026\"");
  pp_define("__TIME__=\"00:00:00\"");
  pp_define("__inline=inline");
  pp_define("__inline__=inline");
  pp_define("__restrict=");
  pp_define("__restrict__=");
  pp_define("restrict=");
  pp_define("__const=const");
  pp_define("__volatile__=volatile");
  pp_define("__signed__=signed");
}

Token* pp_file(char* path) {
  int len;
  char* text = read_file(path, &len);
  Token* t;
  if (!text) error("nao consegui ler %s", path);
  pp_main_file = path;
  t = tokenize(new_file(path, text, len));
  t = preprocess2(t);
  if (conds) error_tok(conds->tok, "#if sem #endif");
  return t;
}
