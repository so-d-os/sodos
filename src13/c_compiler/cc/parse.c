/* parse.c - Tokens -> AST com tipos. C89 + partes do C99 (declaracao no meio
 * do bloco, for (int i...), //, long long, _Bool), sem ponto flutuante nem
 * campos de bits. __export marca funcao/variavel para a tabela da DLL.
 * Tambem aceita um C++ enxuto: class, metodo, heranca publica, construtor,
 * destrutor, new/delete, referencia, namespace e nullptr. */
#include "cc.h"
#include "sox.h"

/* ---------- escopos ---------- */
typedef struct VarScope VarScope;
struct VarScope {
  VarScope* next;
  char* name;
  Obj* var;
  Type* type_def;
  Type* enum_ty;
  int enum_val;
};

typedef struct TagScope TagScope;
struct TagScope {
  TagScope* next;
  char* name;
  Type* ty;
};

#define SHASH 64
typedef struct Scope Scope;
struct Scope {
  Scope* next;
  VarScope* vars[SHASH];
  TagScope* tags;
};

typedef struct {
  int is_typedef;
  int is_static;
  int is_extern;
  int is_inline;
  int is_export;
} VarAttr;

typedef struct Initializer Initializer;
struct Initializer {
  Type* ty;
  Token* tok;
  int is_flexible;
  Node* expr;
  Initializer** children;
};

static Obj* locals;
static Obj* globals;
static Scope* scope;
static Obj* cur_fn;
static Node* cur_loop;   /* para continue */
static Node* cur_brk;    /* para break: laco ou switch */
static Node* cur_switch;
static int uniq;
static int scope_depth;
static int local_ctor_ok; /* declaracao local: T x(args) e construtor */
static int raw_symbol;    /* nome ja esta mangled; take_symbol nao prefixa */
static char* ns_name;     /* namespace corrente, ou NULL */
static Type* cur_class;   /* classe do metodo que esta sendo analisado */
static Type* pend_cls;    /* Class::metodo visto pelo declarador */
static int pend_kind;     /* 1 metodo, 2 construtor, 3 destrutor */

enum { MK_FN = 1, MK_CTOR = 2, MK_DTOR = 3 };
enum { AC_PUBLIC = 0, AC_PRIVATE = 1, AC_PROTECTED = 2 };

typedef struct NsName NsName;
struct NsName {
  NsName* next;
  char* name;
};
static NsName* namespaces;

typedef struct DtorUse DtorUse;
struct DtorUse {
  DtorUse* next;
  Obj* var;       /* local: o argumento e &var */
  Obj* self;      /* sem var: o argumento e this, convertido para as_ty */
  Type* as_ty;
  char* mangle;
  int depth;
};
static DtorUse* dtor_stack;

/* palavras-chave internadas */
static char *k_void, *k_bool, *k_char, *k_short, *k_int, *k_long, *k_signed, *k_unsigned,
    *k_struct, *k_union, *k_enum, *k_typedef, *k_static, *k_extern, *k_auto, *k_register,
    *k_inline, *k_const, *k_volatile, *k_export, *k_float, *k_double, *k_sizeof, *k_if,
    *k_else, *k_for, *k_while, *k_do, *k_switch, *k_case, *k_default, *k_break, *k_continue,
    *k_goto, *k_return, *k_attribute, *k_noreturn, *k_extension, *k_func, *k_alignof,
    *k_builtin_va, *k_asm, *k_class, *k_public, *k_private, *k_protected, *k_new, *k_delete,
    *k_this, *k_namespace, *k_bool2, *k_nullptr, *k_template, *k_typename, *k_try, *k_catch,
    *k_throw, *k_virtual, *k_operator, *k_friend, *k_constexpr;

static char* K(const char* s) { return intern(s, (int)strlen(s)); }

static void init_keywords(void) {
  k_void = K("void"); k_bool = K("_Bool"); k_char = K("char"); k_short = K("short");
  k_int = K("int"); k_long = K("long"); k_signed = K("signed"); k_unsigned = K("unsigned");
  k_struct = K("struct"); k_union = K("union"); k_enum = K("enum"); k_typedef = K("typedef");
  k_static = K("static"); k_extern = K("extern"); k_auto = K("auto"); k_register = K("register");
  k_inline = K("inline"); k_const = K("const"); k_volatile = K("volatile");
  k_export = K("__export"); k_float = K("float"); k_double = K("double"); k_sizeof = K("sizeof");
  k_if = K("if"); k_else = K("else"); k_for = K("for"); k_while = K("while"); k_do = K("do");
  k_switch = K("switch"); k_case = K("case"); k_default = K("default"); k_break = K("break");
  k_continue = K("continue"); k_goto = K("goto"); k_return = K("return");
  k_attribute = K("__attribute__"); k_noreturn = K("_Noreturn"); k_extension = K("__extension__");
  k_func = K("__func__"); k_alignof = K("_Alignof"); k_builtin_va = K("__builtin_va_list");
  k_asm = K("__asm__");
  k_class = K("class"); k_public = K("public"); k_private = K("private");
  k_protected = K("protected"); k_new = K("new"); k_delete = K("delete"); k_this = K("this");
  k_namespace = K("namespace"); k_bool2 = K("bool"); k_nullptr = K("nullptr");
  k_template = K("template"); k_typename = K("typename"); k_try = K("try"); k_catch = K("catch");
  k_throw = K("throw"); k_virtual = K("virtual"); k_operator = K("operator");
  k_friend = K("friend"); k_constexpr = K("constexpr");
}

static int is_kw(Token* t, char* k) { return t->kind == TK_IDENT && t->name == k; }

static int is_keyword(Token* t) {
  char* n;
  if (t->kind != TK_IDENT) return 0;
  n = t->name;
  return n == k_void || n == k_bool || n == k_bool2 || n == k_char || n == k_short || n == k_int ||
         n == k_long || n == k_signed || n == k_unsigned || n == k_struct || n == k_union ||
         n == k_enum || n == k_typedef || n == k_static || n == k_extern || n == k_auto ||
         n == k_register || n == k_inline || n == k_const || n == k_volatile || n == k_export ||
         n == k_float || n == k_double || n == k_sizeof || n == k_if || n == k_else || n == k_for ||
         n == k_while || n == k_do || n == k_switch || n == k_case || n == k_default ||
         n == k_break || n == k_continue || n == k_goto || n == k_return || n == k_attribute ||
         n == k_noreturn || n == k_extension || n == k_alignof || n == k_class || n == k_public ||
         n == k_private || n == k_protected || n == k_new || n == k_delete || n == k_this ||
         n == k_namespace || n == k_nullptr || n == k_template || n == k_typename || n == k_try ||
         n == k_catch || n == k_throw || n == k_virtual || n == k_operator || n == k_friend ||
         n == k_constexpr;
}

static Token* skip(Token* t, int p) {
  if (!tok_punct(t, p)) {
    if (p < 256) error_tok(t, "esperava '%c'", p);
    error_tok(t, "token inesperado");
  }
  return t->next;
}

static int consume(Token** rest, Token* t, int p) {
  if (tok_punct(t, p)) {
    *rest = t->next;
    return 1;
  }
  *rest = t;
  return 0;
}

static int unsupported_kw(Token* t) {
  char* n;
  if (!t || t->kind != TK_IDENT) return 0;
  n = t->name;
  return n == k_template || n == k_typename || n == k_try || n == k_catch || n == k_throw ||
         n == k_virtual || n == k_operator || n == k_friend || n == k_constexpr;
}

static char* get_ident(Token* t) {
  if (unsupported_kw(t))
    error_tok(t, "'%s' ainda nao entra neste C++", t->name);
  if (t->kind != TK_IDENT || is_keyword(t)) error_tok(t, "esperava um nome");
  return t->name;
}

static void enter_scope(void) {
  Scope* s = (Scope*)xalloc((int)sizeof(Scope));
  s->next = scope;
  scope = s;
  scope_depth++;
}

static void leave_scope(void) {
  scope = scope->next;
  scope_depth--;
}

static unsigned shash(char* name) { return (unsigned)(((unsigned long)name >> 3) & (SHASH - 1)); }

static VarScope* find_var(Token* t) {
  Scope* s;
  if (t->kind != TK_IDENT) return NULL;
  for (s = scope; s; s = s->next) {
    VarScope* v;
    for (v = s->vars[shash(t->name)]; v; v = v->next)
      if (v->name == t->name) return v;
  }
  return NULL;
}

static VarScope* find_var_here(char* name) {
  VarScope* v;
  for (v = scope->vars[shash(name)]; v; v = v->next)
    if (v->name == name) return v;
  return NULL;
}

static Type* find_tag(Token* t) {
  Scope* s;
  for (s = scope; s; s = s->next) {
    TagScope* ts;
    for (ts = s->tags; ts; ts = ts->next)
      if (ts->name == t->name) return ts->ty;
  }
  return NULL;
}

static VarScope* push_scope(char* name) {
  VarScope* v = (VarScope*)xalloc((int)sizeof(VarScope));
  unsigned h = shash(name);
  v->name = name;
  v->next = scope->vars[h];
  scope->vars[h] = v;
  return v;
}

static void push_tag(Token* t, Type* ty) {
  TagScope* ts = (TagScope*)xalloc((int)sizeof(TagScope));
  ts->name = t->name;
  ts->ty = ty;
  ts->next = scope->tags;
  scope->tags = ts;
}

static Type* find_typedef(Token* t) {
  if (t->kind == TK_IDENT && !is_keyword(t)) {
    VarScope* v = find_var(t);
    if (v) return v->type_def;
  }
  return NULL;
}

/* ---------- nos ---------- */
static Node* new_node(int kind, Token* tok) {
  Node* n = (Node*)xalloc((int)sizeof(Node));
  n->kind = kind;
  n->tok = tok;
  return n;
}

static Node* new_binary(int kind, Node* l, Node* r, Token* tok) {
  Node* n = new_node(kind, tok);
  n->lhs = l;
  n->rhs = r;
  return n;
}

static Node* new_unary(int kind, Node* e, Token* tok) {
  Node* n = new_node(kind, tok);
  n->lhs = e;
  return n;
}

static Node* new_num(long long v, Token* tok) {
  Node* n = new_node(ND_NUM, tok);
  n->val = v;
  return n;
}

static Node* new_typed_num(long long v, Type* ty, Token* tok) {
  Node* n = new_num(v, tok);
  n->ty = ty;
  return n;
}

static Node* new_var_node(Obj* v, Token* tok) {
  Node* n = new_node(ND_VAR, tok);
  n->var = v;
  v->used = 1;
  return n;
}

static Node* new_cast(Node* e, Type* ty) {
  Node* n;
  add_type(e);
  n = new_node(ND_CAST, e->tok);
  n->lhs = e;
  n->ty = copy_type(ty);
  return n;
}

/* ---------- variaveis ---------- */
static Obj* new_var(char* name, Type* ty) {
  Obj* v = (Obj*)xalloc((int)sizeof(Obj));
  v->name = name;
  v->ty = ty;
  v->sym = -1;
  push_scope(name)->var = v;
  return v;
}

static Obj* new_lvar(char* name, Type* ty) {
  Obj* v = new_var(name, ty);
  v->is_local = 1;
  v->next = locals;
  locals = v;
  return v;
}

static Obj* new_gvar(char* name, Type* ty) {
  Obj* v = new_var(name, ty);
  v->next = globals;
  globals = v;
  return v;
}

static Obj* new_anon_gvar(Type* ty) {
  char* name = format(".L%d", uniq++);
  Obj* v = (Obj*)xalloc((int)sizeof(Obj));
  v->name = name;
  v->ty = ty;
  v->sym = -1;
  v->is_static = 1;
  v->is_definition = 1;
  v->next = globals;
  globals = v;
  return v;
}

static Obj* new_string_literal(char* s, int len) {
  Obj* v = new_anon_gvar(array_of(ty_char, len));
  v->init_data = s;
  v->is_string = 1;
  return v;
}

/* Variavel global ja declarada com esse nome no escopo global? */
static Obj* find_global(char* name) {
  Scope* s = scope;
  VarScope* v;
  while (s->next) s = s->next;
  for (v = s->vars[shash(name)]; v; v = v->next)
    if (v->name == name && v->var) return v->var;
  return NULL;
}

/* ---------- declaracoes ---------- */
static Type* declspec(Token** rest, Token* tok, VarAttr* attr);
static Type* declarator(Token** rest, Token* tok, Type* ty);
static Type* abstract_declarator(Token** rest, Token* tok, Type* ty);
static Type* type_suffix(Token** rest, Token* tok, Type* ty);
static Type* struct_decl(Token** rest, Token* tok, int is_union);
static Type* class_decl(Token** rest, Token* tok);
static Type* enum_specifier(Token** rest, Token* tok);
static Obj* declare_function(Type* ty, VarAttr* attr, Token* name_tok);
static Token* parse_fn_body(Token* tok, Obj* fn);
static Member* find_special(Type* ty, int kind);
static Node* funcall(Token** rest, Token* tok, Node* fn, Node* first);
static Node* declaration(Token** rest, Token* tok, Type* base, VarAttr* attr);
static Node* compound_stmt(Token** rest, Token* tok);
static Node* stmt(Token** rest, Token* tok);
static Node* expr(Token** rest, Token* tok);
static Node* assign(Token** rest, Token* tok);
static Node* conditional(Token** rest, Token* tok);
static Node* logor(Token** rest, Token* tok);
static Node* cast(Token** rest, Token* tok);
static Node* unary(Token** rest, Token* tok);
static Node* postfix(Token** rest, Token* tok);
static Node* primary(Token** rest, Token* tok);
static Type* type_name(Token** rest, Token* tok);
static int is_typename(Token* t);
static long long eval(Node* n);
static long long eval2(Node* n, Obj** label);
static void initializer2(Token** rest, Token* tok, Initializer* init);

static Token* skip_attribute(Token* tok) {
  while (is_kw(tok, k_attribute) || is_kw(tok, k_asm)) {
    int depth = 0;
    tok = tok->next;
    if (!tok_punct(tok, '(')) error_tok(tok, "esperava '('");
    do {
      if (tok_punct(tok, '(')) depth++;
      else if (tok_punct(tok, ')')) depth--;
      else if (tok->kind == TK_EOF) error_tok(tok, "fim inesperado em __attribute__");
      tok = tok->next;
    } while (depth > 0);
  }
  return tok;
}

static int is_typename(Token* t) {
  char* n;
  if (t->kind != TK_IDENT) return 0;
  n = t->name;
  if (n == k_void || n == k_bool || n == k_bool2 || n == k_char || n == k_short || n == k_int ||
      n == k_long || n == k_signed || n == k_unsigned || n == k_struct || n == k_union ||
      n == k_enum || n == k_class || n == k_typedef || n == k_static || n == k_extern ||
      n == k_auto || n == k_register || n == k_inline || n == k_const || n == k_volatile ||
      n == k_export || n == k_float || n == k_double || n == k_noreturn || n == k_attribute ||
      n == k_extension || n == k_builtin_va)
    return 1;
  return find_typedef(t) != NULL;
}

enum {
  S_VOID = 1 << 0, S_BOOL = 1 << 2, S_CHAR = 1 << 4, S_SHORT = 1 << 6, S_INT = 1 << 8,
  S_LONG = 1 << 10, S_OTHER = 1 << 12, S_SIGNED = 1 << 13, S_UNSIGNED = 1 << 14
};

static Type* declspec(Token** rest, Token* tok, VarAttr* attr) {
  Type* ty = ty_int;
  int counter = 0;
  while (is_typename(tok)) {
    char* n = tok->name;
    Type* td;
    if (n == k_typedef || n == k_static || n == k_extern || n == k_inline || n == k_export ||
        n == k_auto || n == k_register || n == k_noreturn) {
      if (!attr) error_tok(tok, "classe de armazenamento nao permitida aqui");
      if (n == k_typedef) attr->is_typedef = 1;
      else if (n == k_static) attr->is_static = 1;
      else if (n == k_extern) attr->is_extern = 1;
      else if (n == k_inline) attr->is_inline = 1;
      else if (n == k_export) attr->is_export = 1;
      tok = tok->next;
      continue;
    }
    if (n == k_const || n == k_volatile || n == k_extension) {
      tok = tok->next;
      continue;
    }
    if (n == k_attribute) {
      tok = skip_attribute(tok);
      continue;
    }
    if (n == k_float || n == k_double) error_tok(tok, "ponto flutuante nao e suportado");
    td = find_typedef(tok);
    if (n == k_struct || n == k_union || n == k_enum || n == k_class || td || n == k_builtin_va) {
      if (counter) break;
      if (n == k_struct) ty = struct_decl(&tok, tok->next, 0);
      else if (n == k_union) ty = struct_decl(&tok, tok->next, 1);
      else if (n == k_class) ty = class_decl(&tok, tok->next);
      else if (n == k_enum) ty = enum_specifier(&tok, tok->next);
      else if (n == k_builtin_va) {
        ty = pointer_to(ty_char);
        tok = tok->next;
      } else {
        ty = td;
        tok = tok->next;
      }
      counter += S_OTHER;
      continue;
    }
    if (n == k_void) counter += S_VOID;
    else if (n == k_bool || n == k_bool2) counter += S_BOOL;
    else if (n == k_char) counter += S_CHAR;
    else if (n == k_short) counter += S_SHORT;
    else if (n == k_int) counter += S_INT;
    else if (n == k_long) counter += S_LONG;
    else if (n == k_signed) counter |= S_SIGNED;
    else if (n == k_unsigned) counter |= S_UNSIGNED;
    switch (counter) {
      case S_VOID: ty = ty_void; break;
      case S_BOOL: ty = ty_bool; break;
      case S_CHAR: case S_SIGNED + S_CHAR: ty = ty_char; break;
      case S_UNSIGNED + S_CHAR: ty = ty_uchar; break;
      case S_SHORT: case S_SHORT + S_INT: case S_SIGNED + S_SHORT:
      case S_SIGNED + S_SHORT + S_INT: ty = ty_short; break;
      case S_UNSIGNED + S_SHORT: case S_UNSIGNED + S_SHORT + S_INT: ty = ty_ushort; break;
      case S_INT: case S_SIGNED: case S_SIGNED + S_INT: ty = ty_int; break;
      case S_UNSIGNED: case S_UNSIGNED + S_INT: ty = ty_uint; break;
      case S_LONG: case S_LONG + S_INT: case S_SIGNED + S_LONG:
      case S_SIGNED + S_LONG + S_INT: ty = ty_long; break;
      case S_UNSIGNED + S_LONG: case S_UNSIGNED + S_LONG + S_INT: ty = ty_ulong; break;
      case S_LONG + S_LONG: case S_LONG + S_LONG + S_INT: case S_SIGNED + S_LONG + S_LONG:
      case S_SIGNED + S_LONG + S_LONG + S_INT: ty = ty_llong; break;
      case S_UNSIGNED + S_LONG + S_LONG: case S_UNSIGNED + S_LONG + S_LONG + S_INT:
        ty = ty_ullong; break;
      default: error_tok(tok, "tipo invalido");
    }
    tok = tok->next;
  }
  *rest = tok;
  return ty;
}

static Type* func_params(Token** rest, Token* tok, Type* ty) {
  Type head;
  Type* cur = &head;
  int variadic = 0;
  Type* fn;
  head.next = NULL;
  if (is_kw(tok, k_void) && tok_punct(tok->next, ')')) {
    *rest = tok->next->next;
    return func_type(ty);
  }
  if (tok_punct(tok, ')')) {
    fn = func_type(ty);
    fn->no_proto = 1;
    *rest = tok->next;
    return fn;
  }
  while (!tok_punct(tok, ')')) {
    Type* t;
    Type* base;
    Token* name;
    if (cur != &head) tok = skip(tok, ',');
    if (tok_punct(tok, P_ELLIPSIS)) {
      variadic = 1;
      tok = tok->next;
      break;
    }
    base = declspec(&tok, tok, NULL);
    t = declarator(&tok, tok, base);
    name = t->name;
    if (t->is_class && t->is_complete && (find_special(t, MK_CTOR) || find_special(t, MK_DTOR)))
      error_tok(name ? name : tok, "classe com construtor passa por ponteiro ou referencia");
    if (t->kind == TY_ARRAY) {
      t = pointer_to(t->base);
      t->name = name;
    } else if (t->kind == TY_FUNC) {
      t = pointer_to(t);
      t->name = name;
    }
    cur = cur->next = copy_type(t);
  }
  fn = func_type(ty);
  fn->params = head.next;
  fn->is_variadic = variadic;
  *rest = skip(tok, ')');
  return fn;
}

static Type* array_dimensions(Token** rest, Token* tok, Type* ty) {
  Node* n;
  int len;
  while (is_kw(tok, k_static) || is_kw(tok, k_const)) tok = tok->next;
  if (tok_punct(tok, ']')) {
    ty = type_suffix(rest, tok->next, ty);
    return array_of(ty, -1);
  }
  n = conditional(&tok, tok);
  tok = skip(tok, ']');
  len = (int)eval(n);
  if (len < 0) error_tok(n->tok, "tamanho de array negativo");
  ty = type_suffix(rest, tok, ty);
  return array_of(ty, len);
}

static Type* type_suffix(Token** rest, Token* tok, Type* ty) {
  if (tok_punct(tok, '(')) return func_params(rest, tok->next, ty);
  if (tok_punct(tok, '[')) return array_dimensions(rest, tok->next, ty);
  *rest = tok;
  return ty;
}

static Token* pointers(Token* tok, Type** ty) {
  for (;;) {
    Token* op = tok;
    if (consume(&tok, tok, '*')) {
      if ((*ty)->is_ref) error_tok(op, "ponteiro para referencia nao e suportado");
      *ty = pointer_to(*ty);
    } else if (consume(&tok, tok, '&')) {
      Type* r;
      if ((*ty)->is_ref) error_tok(op, "referencia de referencia nao e suportada");
      r = pointer_to(*ty);
      r->is_ref = 1;
      *ty = r;
    } else {
      break;
    }
    while (is_kw(tok, k_const) || is_kw(tok, k_volatile) || is_kw(tok, k_attribute)) {
      if (is_kw(tok, k_attribute)) tok = skip_attribute(tok);
      else tok = tok->next;
    }
  }
  return tok;
}

static int is_namespace(char* name) {
  NsName* n;
  for (n = namespaces; n; n = n->next)
    if (n->name == name) return 1;
  return 0;
}

static char* join_name(char* a, char* b) {
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "%s_%s", a, b);
  if (n < 0 || n >= (int)sizeof(buf)) error("nome longo demais");
  return intern(buf, n);
}

static Token* named_tok(Token* src, char* name) {
  Token* t = (Token*)xalloc((int)sizeof(Token));
  if (src) *t = *src;
  t->kind = TK_IDENT;
  t->name = name;
  t->next = NULL;
  return t;
}

static Type* class_type_of(Token* tok) {
  Type* t = find_typedef(tok);
  if (t && t->is_class) return t;
  t = find_tag(tok);
  if (t && t->is_class) return t;
  return NULL;
}

static void prepend_this(Type* fn, Type* cls) {
  Type* p = copy_type(pointer_to(cls));
  p->name = named_tok(NULL, k_this);
  p->next = fn->params;
  fn->params = p;
}

static char* mangle_method(Type* cls, int kind, char* name) {
  char buf[256];
  int n;
  if (!cls || !cls->tag_name) error("classe sem nome");
  if (kind == MK_CTOR) n = snprintf(buf, sizeof(buf), "%s__ctor", cls->tag_name);
  else if (kind == MK_DTOR) n = snprintf(buf, sizeof(buf), "%s__dtor", cls->tag_name);
  else n = snprintf(buf, sizeof(buf), "%s_%s", cls->tag_name, name);
  if (n < 0 || n >= (int)sizeof(buf)) error("nome de metodo longo demais");
  return intern(buf, n);
}

static int mem_data(Member* m) {
  return m && !m->is_method && !m->is_ctor && !m->is_dtor;
}

/* Class::nome ou ns::nome. Devolve o tipo ja com 'this' se for metodo. */
static Type* qualified_declarator(Token** rest, Token* tok, Type* ty, Type* cls, char* qns) {
  int kind = MK_FN;
  Token* name;
  char* sym;
  int qualified = 1;
  if (tok_punct(tok, '~')) {
    kind = MK_DTOR;
    tok = tok->next;
    if (!cls || tok->kind != TK_IDENT || tok->name != cls->tag_name)
      error_tok(tok, "destrutor invalido");
    name = tok;
    tok = tok->next;
    ty = ty_void;
  } else {
    name = tok;
    if (name->kind != TK_IDENT || is_keyword(name)) error_tok(name, "esperava um nome");
    tok = tok->next;
    if (cls && name->name == cls->tag_name) {
      kind = MK_CTOR;
      ty = ty_void;
    }
  }
  if (!tok_punct(tok, '(')) error_tok(tok, "membro estatico nao e suportado");
  ty = type_suffix(&tok, tok, ty);
  if (ty->kind != TY_FUNC) error_tok(name, "esperava uma funcao");
  if (cls && ty->no_proto) ty->no_proto = 0; /* C++: () e lista vazia */
  if (cls) {
    sym = mangle_method(cls, kind, name->name);
    prepend_this(ty, cls);
    pend_cls = cls;
    pend_kind = kind;
  } else {
    sym = join_name(qns, name->name);
    pend_cls = NULL;
    pend_kind = 0;
  }
  if (!is_struct(ty) && ty->kind != TY_ENUM) ty = copy_type(ty);
  ty->name = named_tok(name, sym);
  raw_symbol = qualified;
  *rest = tok;
  return ty;
}

static Type* declarator(Token** rest, Token* tok, Type* ty) {
  Token* name = NULL;
  int qualified = 0;
  tok = pointers(tok, &ty);
  tok = skip_attribute(tok);
  if (tok_punct(tok, P_SCOPE)) {
    Type* cls = ty->is_class ? ty : (ty->origin && ty->origin->is_class ? ty->origin : NULL);
    if (!cls) error_tok(tok, "'::' sem classe");
    return qualified_declarator(rest, tok->next, ty, cls, NULL);
  }
  if (tok_punct(tok, '(') && !is_typename(tok->next) && !tok_punct(tok->next, ')')) {
    Token* start = tok;
    Type dummy;
    Type* inner;
    memset(&dummy, 0, sizeof(dummy));
    declarator(&tok, start->next, &dummy);
    tok = skip(tok, ')');
    ty = type_suffix(rest, tok, ty);
    inner = declarator(&tok, start->next, ty);
    return inner;
  }
  if (tok->kind == TK_IDENT && !is_keyword(tok) && tok_punct(tok->next, P_SCOPE)) {
    Type* cls = class_type_of(tok);
    if (cls) return qualified_declarator(rest, tok->next->next, ty, cls, NULL);
    if (is_namespace(tok->name))
      return qualified_declarator(rest, tok->next->next, ty, NULL, tok->name);
    error_tok(tok, "'%s' nao e classe nem namespace", tok->name);
  }
  if (tok->kind == TK_IDENT && !is_keyword(tok)) {
    name = tok;
    tok = tok->next;
  }
  if (name && local_ctor_ok && ty->is_class && tok_punct(tok, '(')) {
    int ctor_call = !is_typename(tok->next) || tok_punct(tok->next, ')');
    /* T x() e T x(1) sao construtor; T x(int) continua funcao */
    if (ctor_call && !(is_typename(tok->next) && !tok_punct(tok->next, ')'))) {
      if (!is_struct(ty)) ty = copy_type(ty);
      ty->name = name;
      raw_symbol = 0;
      *rest = tok;
      return ty;
    }
  }
  ty = type_suffix(&tok, tok, ty);
  tok = skip_attribute(tok);
  /* struct/union/enum nao sao copiados: um typedef feito antes da definicao
   * completa ("typedef struct T T;") tem que ver os membros depois. O nome so
   * e lido logo em seguida por quem chamou. */
  if (!is_struct(ty) && ty->kind != TY_ENUM) ty = copy_type(ty);
  ty->name = name;
  raw_symbol = qualified;
  *rest = tok;
  return ty;
}

static Type* abstract_declarator(Token** rest, Token* tok, Type* ty) {
  tok = pointers(tok, &ty);
  if (tok_punct(tok, '(') && !is_typename(tok->next) && !tok_punct(tok->next, ')')) {
    Token* start = tok;
    Type dummy;
    memset(&dummy, 0, sizeof(dummy));
    abstract_declarator(&tok, start->next, &dummy);
    tok = skip(tok, ')');
    ty = type_suffix(rest, tok, ty);
    return abstract_declarator(&tok, start->next, ty);
  }
  return type_suffix(rest, tok, ty);
}

static Type* type_name(Token** rest, Token* tok) {
  Type* ty = declspec(&tok, tok, NULL);
  return abstract_declarator(rest, tok, ty);
}

static void struct_members(Token** rest, Token* tok, Type* ty) {
  Member head;
  Member* cur = &head;
  head.next = NULL;
  while (!tok_punct(tok, '}')) {
    VarAttr attr;
    Type* base;
    int first = 1;
    memset(&attr, 0, sizeof(attr));
    base = declspec(&tok, tok, &attr);
    /* struct/union anonima dentro de outra */
    if (is_struct(base) && tok_punct(tok, ';')) {
      Member* m = (Member*)xalloc((int)sizeof(Member));
      m->ty = base;
      cur = cur->next = m;
      tok = tok->next;
      continue;
    }
    while (!consume(&tok, tok, ';')) {
      Member* m;
      if (!first) tok = skip(tok, ',');
      first = 0;
      m = (Member*)xalloc((int)sizeof(Member));
      m->ty = declarator(&tok, tok, base);
      m->name = m->ty->name;
      if (tok_punct(tok, ':')) error_tok(tok, "campos de bits nao sao suportados");
      cur = cur->next = m;
    }
  }
  *rest = tok->next;
  ty->members = head.next;
}

static Type* struct_decl(Token** rest, Token* tok, int is_union) {
  Token* tag = NULL;
  Type* ty;
  Member* m;
  int offset = 0;
  tok = skip_attribute(tok);
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag && !tok_punct(tok, '{')) {
    Type* t = find_tag(tag);
    *rest = tok;
    if (t) return t;
    t = struct_type();
    t->kind = is_union ? TY_UNION : TY_STRUCT;
    t->size = -1;
    push_tag(tag, t);
    return t;
  }
  tok = skip(tok, '{');
  ty = NULL;
  if (tag) {
    TagScope* ts;
    for (ts = scope->tags; ts; ts = ts->next)
      if (ts->name == tag->name) {
        ty = ts->ty;
        break;
      }
    if (ty && ty->is_complete) ty = NULL; /* redefinicao em outro bloco: tipo novo */
  }
  if (!ty) {
    ty = struct_type();
    if (tag) push_tag(tag, ty);
  }
  ty->kind = is_union ? TY_UNION : TY_STRUCT;
  struct_members(rest, tok, ty);
  *rest = skip_attribute(*rest);
  ty->align = 1;
  ty->size = 0;
  for (m = ty->members; m; m = m->next) {
    if (!mem_data(m)) continue;
    if (m->ty->size < 0 || (m->ty->kind == TY_ARRAY && m->ty->array_len < 0 && m->next))
      error_tok(m->name ? m->name : tok, "membro com tipo incompleto");
    if (is_union) {
      m->offset = 0;
      ty->size = MAX(ty->size, m->ty->size);
    } else {
      offset = align_to(offset, m->ty->align);
      m->offset = offset;
      offset += m->ty->size;
    }
    ty->align = MAX(ty->align, m->ty->align);
  }
  if (!is_union) ty->size = offset;
  ty->size = align_to(ty->size, ty->align);
  ty->is_complete = 1;
  return ty;
}

static Type* enum_specifier(Token** rest, Token* tok) {
  Type* ty = enum_type();
  Token* tag = NULL;
  int val = 0;
  int first = 1;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag && !tok_punct(tok, '{')) {
    Type* t = find_tag(tag);
    *rest = tok;
    if (t) return t;
    push_tag(tag, ty);
    return ty;
  }
  tok = skip(tok, '{');
  while (!tok_punct(tok, '}')) {
    VarScope* v;
    char* name;
    if (!first) {
      tok = skip(tok, ',');
      if (tok_punct(tok, '}')) break;
    }
    first = 0;
    name = get_ident(tok);
    tok = tok->next;
    if (tok_punct(tok, '=')) val = (int)eval(conditional(&tok, tok->next));
    v = push_scope(name);
    v->enum_ty = ty;
    v->enum_val = val++;
  }
  *rest = tok->next;
  if (tag) push_tag(tag, ty);
  return ty;
}

static Member* find_special(Type* ty, int kind) {
  Member* m;
  if (!ty) return NULL;
  for (m = ty->members; m; m = m->next) {
    if (kind == MK_CTOR && m->is_ctor) return m;
    if (kind == MK_DTOR && m->is_dtor) return m;
  }
  return NULL;
}

static int ctor_needs_args(Member* ctor) {
  Type* p;
  if (!ctor || !ctor->ty || !ctor->ty->params) return 0;
  p = ctor->ty->params->next; /* o primeiro e this */
  return p != NULL;
}

static Token* skip_braces(Token* tok) {
  int d = 0;
  if (!tok_punct(tok, '{')) error_tok(tok, "esperava '{'");
  do {
    if (tok_punct(tok, '{')) d++;
    else if (tok_punct(tok, '}')) d--;
    else if (tok->kind == TK_EOF) error_tok(tok, "falta '}'");
    tok = tok->next;
  } while (d > 0);
  return tok;
}

static void layout_class(Type* ty, Token* tok) {
  Member* m;
  int offset = 0;
  ty->align = 1;
  ty->size = 0;
  for (m = ty->members; m; m = m->next) {
    if (!mem_data(m)) continue;
    if (m->ty->size < 0 || (m->ty->kind == TY_ARRAY && m->ty->array_len < 0 && m->next))
      error_tok(m->name ? m->name : tok, "membro com tipo incompleto");
    offset = align_to(offset, m->ty->align ? m->ty->align : 1);
    m->offset = offset;
    offset += m->ty->size;
    ty->align = MAX(ty->align, m->ty->align);
  }
  ty->size = align_to(offset, ty->align ? ty->align : 1);
  ty->is_complete = 1;
}

static Obj* start_method(Type* cls, Type* ft, Token* name, int kind, int access) {
  VarAttr attr;
  Obj* fn;
  Member* m;
  char* mang = mangle_method(cls, kind, name ? name->name : cls->tag_name);
  Token* nt = named_tok(name, mang);
  memset(&attr, 0, sizeof(attr));
  if (kind == MK_CTOR && find_special(cls, MK_CTOR))
    error_tok(name, "sobrecarga de construtor nao e suportada");
  if (kind == MK_DTOR && find_special(cls, MK_DTOR))
    error_tok(name, "destrutor definido duas vezes");
  if (kind == MK_FN) {
    for (m = cls->members; m; m = m->next)
      if (m->is_method && m->name && m->name->name == name->name)
        error_tok(name, "sobrecarga de '%s' nao e suportada", name->name);
  }
  ft->name = nt;
  raw_symbol = 1;
  fn = declare_function(ft, &attr, nt);
  fn->method_cls = cls;
  fn->method_kind = kind;
  fn->ty = ft;
  m = (Member*)xalloc((int)sizeof(Member));
  m->ty = ft;
  m->name = (kind == MK_FN) ? name : NULL;
  m->is_method = kind == MK_FN;
  m->is_ctor = kind == MK_CTOR;
  m->is_dtor = kind == MK_DTOR;
  m->access = access;
  m->mangle = mang;
  m->owner = cls;
  m->next = cls->members;
  cls->members = m;
  return fn;
}

static void remember_class(Type* ty, Token* tag) {
  ty->is_class = 1;
  ty->tag_name = tag->name;
  if (!find_typedef(tag)) push_scope(tag->name)->type_def = ty;
}

static Type* class_decl(Token** rest, Token* tok) {
  Token* tag = NULL;
  Type* ty = NULL;
  Type* parent = NULL;
  Member head;
  Member* tail;
  int access = AC_PRIVATE;
  typedef struct PendMeth PendMeth;
  struct PendMeth {
    PendMeth* next;
    Token* at;
    Obj* fn;
  };
  PendMeth* pends = NULL;
  PendMeth** ptail = &pends;
  tok = skip_attribute(tok);
  if (tok->kind == TK_IDENT && !is_keyword(tok)) {
    tag = tok;
    tok = tok->next;
  }
  if (!tag) error_tok(tok, "class precisa de nome");
  if (tok_punct(tok, ':')) {
    tok = tok->next;
    if (is_kw(tok, k_public) || is_kw(tok, k_private) || is_kw(tok, k_protected)) {
      if (!is_kw(tok, k_public)) error_tok(tok, "so heranca public e suportada");
      tok = tok->next;
    }
    parent = class_type_of(tok);
    if (!parent) error_tok(tok, "classe base desconhecida");
    if (!parent->is_complete) error_tok(tok, "classe base incompleta");
    tok = tok->next;
  }
  if (!tok_punct(tok, '{')) {
    ty = find_tag(tag);
    if (!ty) {
      ty = struct_type();
      ty->kind = TY_STRUCT;
      ty->size = -1;
      push_tag(tag, ty);
    }
    remember_class(ty, tag);
    *rest = tok;
    return ty;
  }
  {
    TagScope* ts;
    for (ts = scope->tags; ts; ts = ts->next)
      if (ts->name == tag->name) {
        ty = ts->ty;
        break;
      }
    if (ty && ty->is_complete) ty = NULL;
  }
  if (!ty) {
    ty = struct_type();
    push_tag(tag, ty);
  }
  ty->kind = TY_STRUCT;
  ty->is_class = 1;
  ty->parent = parent;
  remember_class(ty, tag);
  tok = skip(tok, '{');
  head.next = NULL;
  tail = &head;
  if (parent) {
    Member* b = (Member*)xalloc((int)sizeof(Member));
    b->ty = parent;
    tail = tail->next = b;
  }
  ty->members = NULL;
  while (!tok_punct(tok, '}')) {
    VarAttr attr;
    Type* base;
    int first;
    if (tok->kind == TK_EOF) error_tok(tok, "falta '}'");
    if (unsupported_kw(tok)) error_tok(tok, "'%s' ainda nao entra neste C++", tok->name);
    if ((is_kw(tok, k_public) || is_kw(tok, k_private) || is_kw(tok, k_protected)) &&
        tok_punct(tok->next, ':')) {
      if (is_kw(tok, k_public)) access = AC_PUBLIC;
      else if (is_kw(tok, k_private)) access = AC_PRIVATE;
      else access = AC_PROTECTED;
      tok = tok->next->next;
      continue;
    }
    if (tok_punct(tok, '~') ||
        (tok->kind == TK_IDENT && !is_keyword(tok) && tok->name == tag->name &&
         tok_punct(tok->next, '('))) {
      int kind = tok_punct(tok, '~') ? MK_DTOR : MK_CTOR;
      Type* ft;
      Obj* fn;
      Token* nm = tok;
      PendMeth* p;
      if (kind == MK_DTOR) {
        tok = tok->next;
        if (tok->kind != TK_IDENT || tok->name != tag->name) error_tok(tok, "destrutor invalido");
        nm = tok;
        tok = tok->next;
      } else {
        tok = tok->next;
      }
      if (!tok_punct(tok, '(')) error_tok(tok, "esperava '('");
      ft = func_params(&tok, tok->next, ty_void);
      if (ft->no_proto) ft->no_proto = 0;
      prepend_this(ft, ty);
      /* o membro entra em ty->members; o layout usa a lista final */
      fn = start_method(ty, ft, nm, kind, access);
      if (tok_punct(tok, ':') || tok_punct(tok, '{')) {
        p = (PendMeth*)xalloc((int)sizeof(PendMeth));
        p->at = tok;
        p->fn = fn;
        *ptail = p;
        ptail = &p->next;
        if (tok_punct(tok, ':')) {
          while (!tok_punct(tok, '{')) {
            if (tok->kind == TK_EOF) error_tok(tok, "falta '{'");
            tok = tok->next;
          }
        }
        tok = skip_braces(tok);
      } else {
        tok = skip(tok, ';');
      }
      continue;
    }
    memset(&attr, 0, sizeof(attr));
    base = declspec(&tok, tok, &attr);
    if (attr.is_static) error_tok(tok, "membro static nao e suportado");
    first = 1;
    while (!consume(&tok, tok, ';')) {
      Type* mt;
      if (!first) tok = skip(tok, ',');
      first = 0;
      mt = declarator(&tok, tok, base);
      if (mt->kind == TY_FUNC) {
        Obj* fn;
        PendMeth* p;
        Token* nm = mt->name;
        if (!nm) error_tok(tok, "metodo sem nome");
        if (mt->no_proto) mt->no_proto = 0;
        prepend_this(mt, ty);
        fn = start_method(ty, mt, nm, MK_FN, access);
        if (tok_punct(tok, '{')) {
          p = (PendMeth*)xalloc((int)sizeof(PendMeth));
          p->at = tok;
          p->fn = fn;
          *ptail = p;
          ptail = &p->next;
          tok = skip_braces(tok);
          break;
        }
        continue;
      }
      if (!mt->name) error_tok(tok, "membro sem nome");
      if (mt->is_ref) error_tok(mt->name, "membro referencia nao e suportado");
      if (mt->is_class && (find_special(mt, MK_CTOR) || find_special(mt, MK_DTOR)))
        error_tok(mt->name, "membro objeto nao e suportado; use um ponteiro");
      {
        Member* field = (Member*)xalloc((int)sizeof(Member));
        field->ty = mt;
        field->name = mt->name;
        field->access = access;
        field->owner = ty;
        tail = tail->next = field;
      }
    }
  }
  /* metodos foram empilhados na frente; campos estao em head. Junta campos e depois metodos. */
  {
    Member* methods = ty->members;
    Member* lastf = head.next;
    ty->members = head.next;
    if (lastf) {
      while (lastf->next) lastf = lastf->next;
      lastf->next = methods;
    } else {
      ty->members = methods;
    }
  }
  *rest = tok->next;
  layout_class(ty, tag);
  while (pends) {
    parse_fn_body(pends->at, pends->fn);
    pends = pends->next;
  }
  return ty;
}

/* ---------- inicializadores ---------- */
static Initializer* new_initializer(Type* ty, int is_flexible) {
  Initializer* init = (Initializer*)xalloc((int)sizeof(Initializer));
  init->ty = ty;
  if (ty->kind == TY_ARRAY) {
    int i;
    if (is_flexible && ty->array_len < 0) {
      init->is_flexible = 1;
      return init;
    }
    init->children = (Initializer**)xalloc((int)sizeof(Initializer*) * (ty->array_len + 1));
    for (i = 0; i < ty->array_len; i++) init->children[i] = new_initializer(ty->base, 0);
    return init;
  }
  if (is_struct(ty)) {
    int n = 0, i = 0;
    Member* m;
    for (m = ty->members; m; m = m->next)
      if (mem_data(m)) n++;
    init->children = (Initializer**)xalloc((int)sizeof(Initializer*) * (n + 1));
    for (m = ty->members; m; m = m->next)
      if (mem_data(m)) init->children[i++] = new_initializer(m->ty, 0);
  }
  return init;
}

static Token* skip_excess(Token* tok) {
  if (tok_punct(tok, '{')) {
    tok = skip_excess(tok->next);
    return skip(tok, '}');
  }
  assign(&tok, tok);
  return tok;
}

static int is_end(Token* t) {
  return tok_punct(t, '}') || (tok_punct(t, ',') && tok_punct(t->next, '}'));
}

static int consume_end(Token** rest, Token* tok) {
  if (tok_punct(tok, '}')) {
    *rest = tok->next;
    return 1;
  }
  if (tok_punct(tok, ',') && tok_punct(tok->next, '}')) {
    *rest = tok->next->next;
    return 1;
  }
  return 0;
}

static void string_initializer(Token** rest, Token* tok, Initializer* init) {
  int len, i;
  Token* t;
  Buf b;
  memset(&b, 0, sizeof(b));
  /* strings vizinhas ja juntas em primary(); aqui juntamos tambem */
  for (t = tok; t->kind == TK_STR; t = t->next) buf_add(&b, t->str, t->slen - 1);
  buf_byte(&b, 0);
  if (init->is_flexible) {
    Initializer* n = new_initializer(array_of(init->ty->base, b.len), 0);
    *init = *n;
  }
  len = MIN(init->ty->array_len, b.len);
  for (i = 0; i < len; i++) init->children[i]->expr = new_num(b.p[i], tok);
  *rest = t;
}

static int count_array_elems(Token* tok, Type* ty) {
  Initializer* dummy = new_initializer(ty->base, 0);
  int i = 0;
  int first = 1;
  for (; !consume_end(&tok, tok); i++) {
    if (!first) tok = skip(tok, ',');
    first = 0;
    initializer2(&tok, tok, dummy);
  }
  return i;
}

static void array_initializer1(Token** rest, Token* tok, Initializer* init) {
  int i;
  int first = 1;
  tok = skip(tok, '{');
  if (init->is_flexible) {
    int len = count_array_elems(tok, init->ty);
    Initializer* n = new_initializer(array_of(init->ty->base, len), 0);
    *init = *n;
  }
  for (i = 0; !consume_end(rest, tok); i++) {
    if (!first) tok = skip(tok, ',');
    first = 0;
    if (i < init->ty->array_len) initializer2(&tok, tok, init->children[i]);
    else tok = skip_excess(tok);
  }
}

static void array_initializer2(Token** rest, Token* tok, Initializer* init) {
  int i;
  if (init->is_flexible) {
    int len = count_array_elems(tok, init->ty);
    Initializer* n = new_initializer(array_of(init->ty->base, len), 0);
    *init = *n;
  }
  for (i = 0; i < init->ty->array_len && !is_end(tok); i++) {
    if (i > 0) tok = skip(tok, ',');
    initializer2(&tok, tok, init->children[i]);
  }
  *rest = tok;
}

static Member* next_data(Member* m) {
  while (m && !mem_data(m)) m = m->next;
  return m;
}

static void struct_initializer1(Token** rest, Token* tok, Initializer* init) {
  Member* m = next_data(init->ty->members);
  int i = 0;
  int first = 1;
  tok = skip(tok, '{');
  while (!consume_end(rest, tok)) {
    if (!first) tok = skip(tok, ',');
    first = 0;
    if (tok_punct(tok, '.')) error_tok(tok, "inicializador com .campo nao e suportado");
    if (m) {
      initializer2(&tok, tok, init->children[i++]);
      m = next_data(m->next);
    } else {
      tok = skip_excess(tok);
    }
  }
}

static void struct_initializer2(Token** rest, Token* tok, Initializer* init) {
  Member* m;
  int i = 0;
  for (m = next_data(init->ty->members); m && !is_end(tok); m = next_data(m->next)) {
    if (i > 0) tok = skip(tok, ',');
    initializer2(&tok, tok, init->children[i++]);
  }
  *rest = tok;
}

static void union_initializer(Token** rest, Token* tok, Initializer* init) {
  if (tok_punct(tok, '{')) {
    initializer2(&tok, tok->next, init->children[0]);
    consume(&tok, tok, ',');
    *rest = skip(tok, '}');
  } else {
    initializer2(rest, tok, init->children[0]);
  }
}

static void initializer2(Token** rest, Token* tok, Initializer* init) {
  if (init->ty->kind == TY_ARRAY && tok->kind == TK_STR) {
    string_initializer(rest, tok, init);
    return;
  }
  if (init->ty->kind == TY_ARRAY) {
    if (tok_punct(tok, '{') && tok->next->kind == TK_STR && init->ty->base->size == 1 &&
        (tok_punct(tok->next->next, '}') ||
         (tok_punct(tok->next->next, ',') && tok_punct(tok->next->next->next, '}')))) {
      string_initializer(&tok, tok->next, init);
      consume(&tok, tok, ',');
      *rest = skip(tok, '}');
      return;
    }
    if (tok_punct(tok, '{')) array_initializer1(rest, tok, init);
    else array_initializer2(rest, tok, init);
    return;
  }
  if (init->ty->kind == TY_STRUCT) {
    if (tok_punct(tok, '{')) {
      struct_initializer1(rest, tok, init);
      return;
    }
    {
      Token* t2;
      Node* e = assign(&t2, tok);
      add_type(e);
      if (is_struct(e->ty)) {
        init->expr = e;
        *rest = t2;
        return;
      }
    }
    struct_initializer2(rest, tok, init);
    return;
  }
  if (init->ty->kind == TY_UNION) {
    union_initializer(rest, tok, init);
    return;
  }
  if (tok_punct(tok, '{')) {
    initializer2(&tok, tok->next, init);
    consume(&tok, tok, ',');
    *rest = skip(tok, '}');
    return;
  }
  init->expr = assign(rest, tok);
}

static Initializer* initializer(Token** rest, Token* tok, Type* ty, Type** new_ty) {
  Initializer* init = new_initializer(ty, 1);
  initializer2(rest, tok, init);
  *new_ty = init->ty;
  return init;
}

/* local: zera tudo e atribui campo a campo */
typedef struct Desig Desig;
struct Desig {
  Desig* next;
  int idx;
  Member* member;
  Obj* var;
};

static Node* new_add(Node* l, Node* r, Token* tok);

static Node* init_desg_expr(Desig* d, Token* tok) {
  Node* n;
  if (d->var) return new_var_node(d->var, tok);
  if (d->member) {
    n = new_unary(ND_MEMBER, init_desg_expr(d->next, tok), tok);
    n->member = d->member;
    return n;
  }
  n = init_desg_expr(d->next, tok);
  n = new_add(n, new_num(d->idx, tok), tok);
  return new_unary(ND_DEREF, n, tok);
}

static Node* create_lvar_init(Initializer* init, Type* ty, Desig* d, Token* tok) {
  Node* n;
  if (ty->kind == TY_ARRAY) {
    int i;
    n = new_node(ND_NULL_EXPR, tok);
    for (i = 0; i < ty->array_len; i++) {
      Desig d2;
      d2.next = d;
      d2.idx = i;
      d2.member = NULL;
      d2.var = NULL;
      n = new_binary(ND_COMMA, n, create_lvar_init(init->children[i], ty->base, &d2, tok), tok);
    }
    return n;
  }
  if (is_struct(ty) && !init->expr) {
    Member* m;
    int i = 0;
    n = new_node(ND_NULL_EXPR, tok);
    for (m = ty->members; m; m = m->next) {
      Desig d2;
      if (!mem_data(m)) continue;
      d2.next = d;
      d2.idx = 0;
      d2.member = m;
      d2.var = NULL;
      n = new_binary(ND_COMMA, n, create_lvar_init(init->children[i], m->ty, &d2, tok), tok);
      i++;
      if (ty->kind == TY_UNION) break;
    }
    return n;
  }
  if (!init->expr) return new_node(ND_NULL_EXPR, tok);
  return new_binary(ND_ASSIGN, init_desg_expr(d, tok), init->expr, tok);
}

static Node* lvar_initializer(Token** rest, Token* tok, Obj* var) {
  Initializer* init = initializer(rest, tok, var->ty, &var->ty);
  Desig d;
  Node* lhs;
  Node* rhs;
  d.next = NULL;
  d.idx = 0;
  d.member = NULL;
  d.var = var;
  lhs = new_node(ND_MEMZERO, tok);
  lhs->var = var;
  rhs = create_lvar_init(init, var->ty, &d, tok);
  return new_binary(ND_COMMA, lhs, rhs, tok);
}

/* global: bytes + relocacoes */
static void write_buf(char* buf, long long v, int size) {
  int i;
  for (i = 0; i < size; i++) buf[i] = (char)(v >> (i * 8));
}

static Reloc* write_gvar_data(Reloc* cur, Initializer* init, Type* ty, char* buf, int offset) {
  if (ty->kind == TY_ARRAY) {
    int i;
    for (i = 0; i < ty->array_len; i++)
      cur = write_gvar_data(cur, init->children[i], ty->base, buf, offset + ty->base->size * i);
    return cur;
  }
  if (is_struct(ty)) {
    Member* m;
    int i = 0;
    if (init->expr) error_tok(init->expr->tok, "inicializador de struct global precisa de { }");
    for (m = ty->members; m; m = m->next) {
      if (!mem_data(m)) continue;
      cur = write_gvar_data(cur, init->children[i], m->ty, buf, offset + m->offset);
      i++;
      if (ty->kind == TY_UNION) break;
    }
    return cur;
  }
  if (!init->expr) return cur;
  {
    Obj* label = NULL;
    long long v = eval2(init->expr, &label);
    if (!label) {
      write_buf(buf + offset, v, ty->size);
      return cur;
    }
    if (ty->size != 4) error_tok(init->expr->tok, "endereco precisa de 4 bytes");
    {
      Reloc* r = (Reloc*)xalloc((int)sizeof(Reloc));
      r->offset = offset;
      r->target = label;
      r->addend = v;
      label->used = 1;
      cur->next = r;
      return r;
    }
  }
}

static void gvar_initializer(Token** rest, Token* tok, Obj* var) {
  Initializer* init = initializer(rest, tok, var->ty, &var->ty);
  Reloc head;
  char* buf;
  head.next = NULL;
  buf = (char*)xalloc(var->ty->size > 0 ? var->ty->size : 1);
  write_gvar_data(&head, init, var->ty, buf, 0);
  var->init_data = buf;
  var->rel = head.next;
}

/* ---------- avaliacao de constantes ---------- */
static long long eval_rval(Node* n, Obj** label);

static long long trunc_to(long long v, Type* ty) {
  if (ty->kind == TY_BOOL) return v != 0;
  if (ty->size == 1) return ty->is_unsigned ? (long long)(unsigned char)v : (long long)(signed char)v;
  if (ty->size == 2) return ty->is_unsigned ? (long long)(unsigned short)v : (long long)(short)v;
  if (ty->size == 4) return ty->is_unsigned ? (long long)(unsigned)v : (long long)(int)v;
  return v;
}

static long long eval2(Node* n, Obj** label) {
  add_type(n);
  switch (n->kind) {
    case ND_ADD: return eval2(n->lhs, label) + eval(n->rhs);
    case ND_SUB: return eval2(n->lhs, label) - eval(n->rhs);
    case ND_MUL: return eval(n->lhs) * eval(n->rhs);
    case ND_DIV: {
      long long b = eval(n->rhs);
      if (!b) error_tok(n->tok, "divisao por zero");
      if (n->ty->is_unsigned) return (long long)((unsigned long long)eval(n->lhs) / (unsigned long long)b);
      return eval(n->lhs) / b;
    }
    case ND_MOD: {
      long long b = eval(n->rhs);
      if (!b) error_tok(n->tok, "divisao por zero");
      if (n->ty->is_unsigned) return (long long)((unsigned long long)eval(n->lhs) % (unsigned long long)b);
      return eval(n->lhs) % b;
    }
    case ND_NEG: return -eval(n->lhs);
    case ND_BAND: return eval(n->lhs) & eval(n->rhs);
    case ND_BOR: return eval(n->lhs) | eval(n->rhs);
    case ND_BXOR: return eval(n->lhs) ^ eval(n->rhs);
    case ND_SHL: return trunc_to(eval(n->lhs) << eval(n->rhs), n->ty);
    case ND_SHR:
      if (n->ty->is_unsigned && n->ty->size == 4)
        return (long long)((unsigned)eval(n->lhs) >> eval(n->rhs));
      if (n->ty->is_unsigned) return (long long)((unsigned long long)eval(n->lhs) >> eval(n->rhs));
      return eval(n->lhs) >> eval(n->rhs);
    case ND_EQ: return eval(n->lhs) == eval(n->rhs);
    case ND_NE: return eval(n->lhs) != eval(n->rhs);
    case ND_LT:
      if (n->lhs->ty->is_unsigned)
        return (unsigned long long)eval(n->lhs) < (unsigned long long)eval(n->rhs);
      return eval(n->lhs) < eval(n->rhs);
    case ND_LE:
      if (n->lhs->ty->is_unsigned)
        return (unsigned long long)eval(n->lhs) <= (unsigned long long)eval(n->rhs);
      return eval(n->lhs) <= eval(n->rhs);
    case ND_COND: return eval(n->cond) ? eval2(n->then, label) : eval2(n->els, label);
    case ND_COMMA: return eval2(n->rhs, label);
    case ND_NOT: return !eval(n->lhs);
    case ND_BNOT: return trunc_to(~eval(n->lhs), n->ty);
    case ND_LOGAND: return eval(n->lhs) && eval(n->rhs);
    case ND_LOGOR: return eval(n->lhs) || eval(n->rhs);
    case ND_CAST: {
      long long v = eval2(n->lhs, label);
      if (is_integer(n->ty) && !(label && *label)) return trunc_to(v, n->ty);
      return v;
    }
    case ND_ADDR: return eval_rval(n->lhs, label);
    case ND_MEMBER:
      if (!label) error_tok(n->tok, "nao e uma constante");
      if (n->ty->kind != TY_ARRAY) error_tok(n->tok, "inicializador invalido");
      return eval_rval(n->lhs, label) + n->member->offset;
    case ND_VAR:
      if (!label) error_tok(n->tok, "nao e uma constante");
      if (n->var->ty->kind != TY_ARRAY && n->var->ty->kind != TY_FUNC)
        error_tok(n->tok, "inicializador invalido");
      if (n->var->is_local) error_tok(n->tok, "endereco de variavel local nao e constante");
      *label = n->var;
      return 0;
    case ND_NUM: return n->val;
  }
  error_tok(n->tok, "nao e uma constante");
  return 0;
}

static long long eval_rval(Node* n, Obj** label) {
  switch (n->kind) {
    case ND_VAR:
      if (n->var->is_local) error_tok(n->tok, "endereco de variavel local nao e constante");
      *label = n->var;
      return 0;
    case ND_DEREF: return eval2(n->lhs, label);
    case ND_MEMBER: return eval_rval(n->lhs, label) + n->member->offset;
  }
  error_tok(n->tok, "inicializador invalido");
  return 0;
}

static long long eval(Node* n) { return eval2(n, NULL); }

long long const_expr_value(Node* n) { return eval(n); }

static int is_const_expr(Node* n) {
  add_type(n);
  switch (n->kind) {
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_BAND: case ND_BOR:
    case ND_BXOR: case ND_SHL: case ND_SHR: case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
    case ND_LOGAND: case ND_LOGOR:
      return is_const_expr(n->lhs) && is_const_expr(n->rhs);
    case ND_COND:
      if (!is_const_expr(n->cond)) return 0;
      return is_const_expr(eval(n->cond) ? n->then : n->els);
    case ND_COMMA: return is_const_expr(n->rhs);
    case ND_NEG: case ND_NOT: case ND_BNOT: case ND_CAST: return is_const_expr(n->lhs);
    case ND_NUM: return 1;
  }
  return 0;
}

/* ---------- C++: chamadas, new/delete, destruidores ---------- */
static char* take_symbol(char* name) {
  if (raw_symbol) {
    raw_symbol = 0;
    return name;
  }
  if (ns_name) return join_name(ns_name, name);
  return name;
}

static void add_ns(char* name) {
  NsName* n;
  if (is_namespace(name)) return;
  n = (NsName*)xalloc((int)sizeof(NsName));
  n->name = name;
  n->next = namespaces;
  namespaces = n;
}

static Node* addr_of_var(Obj* v, Token* tok) {
  return new_unary(ND_ADDR, new_var_node(v, tok), tok);
}

static Node* call_mangle(char* mang, Node* arg, Token* tok) {
  Obj* fn;
  mang = intern(mang, (int)strlen(mang));
  fn = find_global(mang);
  Node* n;
  if (!fn) error_tok(tok, "nao achei '%s'", mang);
  add_type(arg);
  arg->next = NULL;
  n = new_unary(ND_FUNCALL, new_var_node(fn, tok), tok);
  n->func_ty = fn->ty;
  n->ty = fn->ty->base;
  n->args = arg;
  if (is_struct(n->ty)) n->ret_buf = new_lvar("", n->ty);
  return n;
}

static Obj* ensure_fn(char* name, Type* ret) {
  Obj* g;
  name = intern(name, (int)strlen(name));
  g = find_global(name);
  Scope* save;
  Type* ft;
  if (g) return g;
  save = scope;
  while (scope->next) scope = scope->next;
  ft = func_type(ret);
  ft->no_proto = 1;
  g = new_gvar(name, ft);
  g->is_function = 1;
  scope = save;
  return g;
}

static void push_dtor(Obj* var, char* mangle) {
  DtorUse* d = (DtorUse*)xalloc((int)sizeof(DtorUse));
  d->var = var;
  d->mangle = mangle;
  d->depth = scope_depth;
  d->next = dtor_stack;
  dtor_stack = d;
}

static Node* dtor_call(DtorUse* d, Token* tok) {
  Node* arg;
  if (d->var) arg = addr_of_var(d->var, tok);
  else arg = new_cast(new_var_node(d->self, tok), pointer_to(d->as_ty));
  return call_mangle(d->mangle, arg, tok);
}

static Node* take_dtor_stmts(int depth, Token* tok) {
  Node head;
  Node* cur = &head;
  head.next = NULL;
  while (dtor_stack && dtor_stack->depth == depth) {
    DtorUse* d = dtor_stack;
    dtor_stack = d->next;
    cur = cur->next = new_unary(ND_EXPR_STMT, dtor_call(d, tok), tok);
  }
  return head.next;
}

static Node* bind_ref(Node* arg, Type* param) {
  Node* p = arg;
  add_type(arg);
  if (arg->kind == ND_DEREF) p = arg->lhs;
  else if (arg->kind == ND_VAR || arg->kind == ND_MEMBER)
    p = new_unary(ND_ADDR, arg, arg->tok);
  else error_tok(arg->tok, "referencia precisa de uma variavel");
  return new_cast(p, param);
}

static Node* return_with_dtors(Node* ret, Token* tok) {
  Node* blk;
  Node head;
  Node* cur;
  DtorUse* d;
  if (!dtor_stack) return ret;
  blk = new_node(ND_BLOCK, tok);
  cur = &head;
  head.next = NULL;
  if (ret->lhs && ret->lhs->ty && ret->lhs->ty->kind != TY_VOID) {
    Obj* tmp = new_lvar("", ret->lhs->ty);
    Node* asg = new_binary(ND_ASSIGN, new_var_node(tmp, tok), ret->lhs, tok);
    cur = cur->next = new_unary(ND_EXPR_STMT, asg, tok);
    ret->lhs = new_var_node(tmp, tok);
  }
  for (d = dtor_stack; d; d = d->next)
    cur = cur->next = new_unary(ND_EXPR_STMT, dtor_call(d, tok), tok);
  cur->next = ret;
  blk->body = head.next;
  return blk;
}

static int can_access(Member* m) {
  Type* c;
  if (!m || m->access == AC_PUBLIC) return 1;
  if (!cur_class || !m->owner) return 0;
  if (m->access == AC_PRIVATE) return cur_class == m->owner;
  for (c = cur_class; c; c = c->parent)
    if (c == m->owner) return 1;
  return 0;
}

/* ---------- declaracoes locais ---------- */
static Node* declaration(Token** rest, Token* tok, Type* base, VarAttr* attr) {
  Node head;
  Node* cur = &head;
  int i = 0;
  head.next = NULL;
  while (!tok_punct(tok, ';')) {
    Type* ty;
    char* name;
    Obj* var;
    if (i++ > 0) tok = skip(tok, ',');
    local_ctor_ok = 1;
    ty = declarator(&tok, tok, base);
    local_ctor_ok = 0;
    if (ty->kind == TY_VOID) error_tok(tok, "variavel do tipo void");
    if (!ty->name) error_tok(tok, "falta o nome da variavel");
    name = ty->name->name;
    if (attr && attr->is_static) {
      /* static local: global com nome unico */
      Obj* g = new_anon_gvar(ty);
      g->name = format("%s.%s.%d", cur_fn ? cur_fn->name : "", name, uniq++);
      push_scope(name)->var = g;
      g->tok = ty->name;
      if (tok_punct(tok, '=')) gvar_initializer(&tok, tok->next, g);
      else g->is_tentative = 1;
      continue;
    }
    if (attr && attr->is_extern) {
      Obj* g = find_global(name);
      if (!g) {
        Scope* save = scope;
        while (scope->next) scope = scope->next;
        g = new_gvar(name, ty);
        g->is_extern = 1;
        g->is_function = ty->kind == TY_FUNC;
        g->tok = ty->name;
        scope = save;
      }
      push_scope(name)->var = g;
      continue;
    }
    if (ty->kind == TY_FUNC) {
      Obj* g = find_global(name);
      if (!g) {
        Scope* save = scope;
        while (scope->next) scope = scope->next;
        g = new_gvar(name, ty);
        g->is_function = 1;
        g->tok = ty->name;
        scope = save;
      }
      push_scope(name)->var = g;
      continue;
    }
    var = new_lvar(name, ty);
    var->tok = ty->name;
    if (ty->is_ref) {
      Node* rhs;
      Node* slot;
      if (!tok_punct(tok, '=')) error_tok(ty->name, "referencia sem valor inicial");
      rhs = bind_ref(assign(&tok, tok->next), ty);
      slot = new_var_node(var, ty->name);
      cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, slot, rhs, ty->name), ty->name);
    } else if (ty->is_class && tok_punct(tok, '(')) {
      Member* ctor = find_special(ty, MK_CTOR);
      Obj* fn;
      Node* call;
      if (!ctor) error_tok(ty->name, "classe sem construtor");
      fn = find_global(ctor->mangle);
      if (!fn) error_tok(ty->name, "construtor nao declarado");
      call = funcall(&tok, tok->next, new_var_node(fn, ty->name), addr_of_var(var, ty->name));
      cur = cur->next = new_unary(ND_EXPR_STMT, call, ty->name);
    } else if (tok_punct(tok, '=')) {
      Node* e;
      if (ty->is_class && find_special(ty, MK_CTOR))
        error_tok(tok, "objeto com construtor nao usa '='");
      e = lvar_initializer(&tok, tok->next, var);
      cur = cur->next = new_unary(ND_EXPR_STMT, e, tok);
    } else if (ty->is_class && find_special(ty, MK_CTOR)) {
      Member* ctor = find_special(ty, MK_CTOR);
      if (ctor_needs_args(ctor)) error_tok(ty->name, "construtor precisa de argumentos");
      cur = cur->next = new_unary(ND_EXPR_STMT, call_mangle(ctor->mangle, addr_of_var(var, ty->name), ty->name), ty->name);
    }
    if (ty->is_class) {
      Member* d = find_special(ty, MK_DTOR);
      if (d) push_dtor(var, d->mangle);
    }
    if (var->ty->size < 0 || (var->ty->kind == TY_ARRAY && var->ty->array_len < 0))
      error_tok(ty->name, "variavel com tipo incompleto");
    if (var->ty->kind == TY_VOID) error_tok(ty->name, "variavel void");
  }
  {
    Node* n = new_node(ND_BLOCK, tok);
    n->body = head.next;
    *rest = tok->next;
    return n;
  }
}

/* ---------- comandos ---------- */
static int is_label(Token* tok) {
  return tok->kind == TK_IDENT && !is_keyword(tok) && tok_punct(tok->next, ':');
}

static Node* compound_stmt(Token** rest, Token* tok) {
  Node* n = new_node(ND_BLOCK, tok);
  Node head;
  Node* cur = &head;
  int depth;
  head.next = NULL;
  enter_scope();
  depth = scope_depth;
  while (!tok_punct(tok, '}')) {
    if (tok->kind == TK_EOF) error_tok(tok, "falta '}'");
    if (is_typename(tok) && !is_label(tok)) {
      VarAttr attr;
      Type* base;
      memset(&attr, 0, sizeof(attr));
      base = declspec(&tok, tok, &attr);
      if (attr.is_typedef) {
        int first = 1;
        while (!consume(&tok, tok, ';')) {
          Type* ty;
          if (!first) tok = skip(tok, ',');
          first = 0;
          ty = declarator(&tok, tok, base);
          if (!ty->name) error_tok(tok, "typedef sem nome");
          push_scope(ty->name->name)->type_def = ty;
        }
        continue;
      }
      if (tok_punct(tok, ';') && (is_struct(base) || base->kind == TY_ENUM)) {
        tok = tok->next;
        continue;
      }
      cur = cur->next = declaration(&tok, tok, base, &attr);
    } else {
      cur = cur->next = stmt(&tok, tok);
    }
    add_type(cur);
  }
  {
    Node* c = take_dtor_stmts(depth, tok);
    while (c) {
      Node* nx = c->next;
      c->next = NULL;
      cur = cur->next = c;
      c = nx;
    }
  }
  leave_scope();
  n->body = head.next;
  *rest = tok->next;
  return n;
}

static Node* expr_stmt(Token** rest, Token* tok) {
  Node* n;
  if (tok_punct(tok, ';')) {
    *rest = tok->next;
    return new_node(ND_BLOCK, tok);
  }
  n = new_node(ND_EXPR_STMT, tok);
  n->lhs = expr(&tok, tok);
  *rest = skip(tok, ';');
  return n;
}

static Node* stmt(Token** rest, Token* tok) {
  Node* n;
  if (is_kw(tok, k_return)) {
    n = new_node(ND_RETURN, tok);
    if (consume(rest, tok->next, ';')) {
      if (cur_fn->ty->base->kind != TY_VOID) warn_tok(tok, "return sem valor");
      return return_with_dtors(n, tok);
    }
    {
      Node* e = expr(&tok, tok->next);
      *rest = skip(tok, ';');
      add_type(e);
      if (cur_fn->ty->base->kind == TY_VOID) {
        if (e->ty->kind != TY_VOID) warn_tok(e->tok, "return com valor em funcao void");
        n->lhs = e;
      } else if (!is_struct(cur_fn->ty->base)) {
        n->lhs = new_cast(e, cur_fn->ty->base);
      } else {
        n->lhs = e;
      }
    }
    return return_with_dtors(n, tok);
  }
  if (is_kw(tok, k_if)) {
    n = new_node(ND_IF, tok);
    tok = skip(tok->next, '(');
    n->cond = expr(&tok, tok);
    tok = skip(tok, ')');
    n->then = stmt(&tok, tok);
    if (is_kw(tok, k_else)) n->els = stmt(&tok, tok->next);
    *rest = tok;
    return n;
  }
  if (is_kw(tok, k_switch)) {
    Node* save_sw = cur_switch;
    Node* save_brk = cur_brk;
    n = new_node(ND_SWITCH, tok);
    tok = skip(tok->next, '(');
    n->cond = expr(&tok, tok);
    tok = skip(tok, ')');
    add_type(n->cond);
    if (!is_integer(n->cond->ty)) error_tok(n->cond->tok, "switch precisa de inteiro");
    if (!is_64(n->cond->ty)) n->cond = new_cast(n->cond, ty_int);
    cur_switch = n;
    cur_brk = n;
    n->then = stmt(rest, tok);
    cur_switch = save_sw;
    cur_brk = save_brk;
    return n;
  }
  if (is_kw(tok, k_case)) {
    Node* e;
    if (!cur_switch) error_tok(tok, "case fora de switch");
    n = new_node(ND_CASE, tok);
    e = conditional(&tok, tok->next);
    n->val = eval(e);
    tok = skip(tok, ':');
    n->lhs = stmt(rest, tok);
    n->case_next = cur_switch->case_next;
    cur_switch->case_next = n;
    return n;
  }
  if (is_kw(tok, k_default)) {
    if (!cur_switch) error_tok(tok, "default fora de switch");
    n = new_node(ND_CASE, tok);
    tok = skip(tok->next, ':');
    n->lhs = stmt(rest, tok);
    cur_switch->default_case = n;
    return n;
  }
  if (is_kw(tok, k_for)) {
    Node* save_loop = cur_loop;
    Node* save_brk = cur_brk;
    Node* clean;
    int depth;
    n = new_node(ND_FOR, tok);
    tok = skip(tok->next, '(');
    enter_scope();
    depth = scope_depth;
    if (is_typename(tok)) {
      VarAttr attr;
      Type* base;
      memset(&attr, 0, sizeof(attr));
      base = declspec(&tok, tok, &attr);
      n->init = declaration(&tok, tok, base, &attr);
    } else {
      n->init = expr_stmt(&tok, tok);
    }
    if (!tok_punct(tok, ';')) n->cond = expr(&tok, tok);
    tok = skip(tok, ';');
    if (!tok_punct(tok, ')')) n->inc = expr(&tok, tok);
    tok = skip(tok, ')');
    cur_loop = n;
    cur_brk = n;
    n->then = stmt(rest, tok);
    cur_loop = save_loop;
    cur_brk = save_brk;
    clean = take_dtor_stmts(depth, n->tok);
    leave_scope();
    if (clean) {
      Node* blk = new_node(ND_BLOCK, n->tok);
      n->next = clean;
      blk->body = n;
      return blk;
    }
    return n;
  }
  if (is_kw(tok, k_while)) {
    Node* save_loop = cur_loop;
    Node* save_brk = cur_brk;
    n = new_node(ND_FOR, tok);
    tok = skip(tok->next, '(');
    n->cond = expr(&tok, tok);
    tok = skip(tok, ')');
    cur_loop = n;
    cur_brk = n;
    n->then = stmt(rest, tok);
    cur_loop = save_loop;
    cur_brk = save_brk;
    return n;
  }
  if (is_kw(tok, k_do)) {
    Node* save_loop = cur_loop;
    Node* save_brk = cur_brk;
    n = new_node(ND_DO, tok);
    cur_loop = n;
    cur_brk = n;
    n->then = stmt(&tok, tok->next);
    cur_loop = save_loop;
    cur_brk = save_brk;
    if (!is_kw(tok, k_while)) error_tok(tok, "esperava 'while'");
    tok = skip(tok->next, '(');
    n->cond = expr(&tok, tok);
    tok = skip(tok, ')');
    *rest = skip(tok, ';');
    return n;
  }
  if (is_kw(tok, k_goto)) {
    n = new_node(ND_GOTO, tok);
    n->label = get_ident(tok->next);
    n->goto_next = cur_fn->gotos;
    cur_fn->gotos = n;
    *rest = skip(tok->next->next, ';');
    return n;
  }
  if (is_kw(tok, k_break)) {
    if (!cur_brk) error_tok(tok, "break fora de laco/switch");
    n = new_node(ND_BREAK, tok);
    n->target = cur_brk;
    *rest = skip(tok->next, ';');
    return n;
  }
  if (is_kw(tok, k_continue)) {
    if (!cur_loop) error_tok(tok, "continue fora de laco");
    n = new_node(ND_CONTINUE, tok);
    n->target = cur_loop;
    *rest = skip(tok->next, ';');
    return n;
  }
  if (is_label(tok)) {
    n = new_node(ND_LABEL, tok);
    n->label = tok->name;
    n->lhs = stmt(rest, tok->next->next);
    n->label_next = cur_fn->labels;
    cur_fn->labels = n;
    return n;
  }
  if (tok_punct(tok, '{')) return compound_stmt(rest, tok->next);
  return expr_stmt(rest, tok);
}

/* ---------- expressoes ---------- */
static Node* expr(Token** rest, Token* tok) {
  Node* n = assign(&tok, tok);
  if (tok_punct(tok, ',')) return new_binary(ND_COMMA, n, expr(rest, tok->next), tok);
  *rest = tok;
  return n;
}

/* A op= B  ->  tmp = &A, *tmp = *tmp op B */
static Node* to_assign(Node* binary) {
  Token* tok = binary->tok;
  Obj* var;
  Node* e1;
  Node* e2;
  Node* e3;
  add_type(binary->lhs);
  add_type(binary->rhs);
  var = new_lvar("", pointer_to(binary->lhs->ty));
  e1 = new_binary(ND_ASSIGN, new_var_node(var, tok), new_unary(ND_ADDR, binary->lhs, tok), tok);
  e2 = new_unary(ND_DEREF, new_var_node(var, tok), tok);
  e3 = new_unary(ND_DEREF, new_var_node(var, tok), tok);
  binary->lhs = e3;
  return new_binary(ND_COMMA, e1, new_binary(ND_ASSIGN, e2, binary, tok), tok);
}

static Node* new_add(Node* l, Node* r, Token* tok) {
  add_type(l);
  add_type(r);
  if (is_integer(l->ty) && is_integer(r->ty)) return new_binary(ND_ADD, l, r, tok);
  if (l->ty->base && r->ty->base) error_tok(tok, "soma de dois ponteiros");
  if (!l->ty->base && r->ty->base) {
    Node* t = l;
    l = r;
    r = t;
  }
  if (!l->ty->base) error_tok(tok, "operandos invalidos para '+'");
  /* void* e ponteiro de funcao contam em bytes (sizeof == 1), como no gcc */
  if (is_64(r->ty)) r = new_cast(r, ty_int);
  r = new_binary(ND_MUL, r, new_typed_num(l->ty->base->size, ty_int, tok), tok);
  return new_binary(ND_ADD, l, r, tok);
}

static Node* new_sub(Node* l, Node* r, Token* tok) {
  add_type(l);
  add_type(r);
  if (is_integer(l->ty) && is_integer(r->ty)) return new_binary(ND_SUB, l, r, tok);
  if (l->ty->base && is_integer(r->ty)) {
    if (is_64(r->ty)) r = new_cast(r, ty_int);
    r = new_binary(ND_MUL, r, new_typed_num(l->ty->base->size, ty_int, tok), tok);
    return new_binary(ND_SUB, l, r, tok);
  }
  if (l->ty->base && r->ty->base) {
    int sz = l->ty->base->size;
    Node* n = new_binary(ND_SUB, new_cast(l, ty_int), new_cast(r, ty_int), tok);
    n->ty = ty_int;
    if (sz <= 1) return n;
    return new_binary(ND_DIV, n, new_typed_num(sz, ty_int, tok), tok);
  }
  error_tok(tok, "operandos invalidos para '-'");
  return NULL;
}

static Node* assign(Token** rest, Token* tok) {
  Node* n = conditional(&tok, tok);
  if (tok_punct(tok, '=')) return new_binary(ND_ASSIGN, n, assign(rest, tok->next), tok);
  if (tok_punct(tok, P_ADD_EQ)) return to_assign(new_add(n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_SUB_EQ)) return to_assign(new_sub(n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_MUL_EQ)) return to_assign(new_binary(ND_MUL, n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_DIV_EQ)) return to_assign(new_binary(ND_DIV, n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_MOD_EQ)) return to_assign(new_binary(ND_MOD, n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_AND_EQ)) return to_assign(new_binary(ND_BAND, n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_OR_EQ)) return to_assign(new_binary(ND_BOR, n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_XOR_EQ)) return to_assign(new_binary(ND_BXOR, n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_SHL_EQ)) return to_assign(new_binary(ND_SHL, n, assign(rest, tok->next), tok));
  if (tok_punct(tok, P_SHR_EQ)) return to_assign(new_binary(ND_SHR, n, assign(rest, tok->next), tok));
  *rest = tok;
  return n;
}

static Node* conditional(Token** rest, Token* tok) {
  Node* c = logor(&tok, tok);
  Node* n;
  if (!tok_punct(tok, '?')) {
    *rest = tok;
    return c;
  }
  n = new_node(ND_COND, tok);
  n->cond = c;
  n->then = expr(&tok, tok->next);
  tok = skip(tok, ':');
  n->els = conditional(rest, tok);
  return n;
}

static Node* binop_level(Token** rest, Token* tok, int level);

/* niveis: 0 || , 1 &&, 2 |, 3 ^, 4 &, 5 == !=, 6 < > <= >=, 7 << >>, 8 + -, 9 * / % */
static Node* binop_level(Token** rest, Token* tok, int level) {
  Node* n;
  if (level == 10) return cast(rest, tok);
  n = binop_level(&tok, tok, level + 1);
  for (;;) {
    Token* op = tok;
    if (tok->kind != TK_PUNCT) break;
    switch (level) {
      case 0:
        if (op->punct != P_LOR) goto done;
        n = new_binary(ND_LOGOR, n, binop_level(&tok, tok->next, 1), op);
        continue;
      case 1:
        if (op->punct != P_LAND) goto done;
        n = new_binary(ND_LOGAND, n, binop_level(&tok, tok->next, 2), op);
        continue;
      case 2:
        if (op->punct != '|') goto done;
        n = new_binary(ND_BOR, n, binop_level(&tok, tok->next, 3), op);
        continue;
      case 3:
        if (op->punct != '^') goto done;
        n = new_binary(ND_BXOR, n, binop_level(&tok, tok->next, 4), op);
        continue;
      case 4:
        if (op->punct != '&') goto done;
        n = new_binary(ND_BAND, n, binop_level(&tok, tok->next, 5), op);
        continue;
      case 5:
        if (op->punct == P_EQ) n = new_binary(ND_EQ, n, binop_level(&tok, tok->next, 6), op);
        else if (op->punct == P_NE) n = new_binary(ND_NE, n, binop_level(&tok, tok->next, 6), op);
        else goto done;
        continue;
      case 6:
        if (op->punct == '<') n = new_binary(ND_LT, n, binop_level(&tok, tok->next, 7), op);
        else if (op->punct == P_LE) n = new_binary(ND_LE, n, binop_level(&tok, tok->next, 7), op);
        else if (op->punct == '>') n = new_binary(ND_LT, binop_level(&tok, tok->next, 7), n, op);
        else if (op->punct == P_GE) n = new_binary(ND_LE, binop_level(&tok, tok->next, 7), n, op);
        else goto done;
        continue;
      case 7:
        if (op->punct == P_SHL) n = new_binary(ND_SHL, n, binop_level(&tok, tok->next, 8), op);
        else if (op->punct == P_SHR) n = new_binary(ND_SHR, n, binop_level(&tok, tok->next, 8), op);
        else goto done;
        continue;
      case 8:
        if (op->punct == '+') n = new_add(n, binop_level(&tok, tok->next, 9), op);
        else if (op->punct == '-') n = new_sub(n, binop_level(&tok, tok->next, 9), op);
        else goto done;
        continue;
      case 9:
        if (op->punct == '*') n = new_binary(ND_MUL, n, cast(&tok, tok->next), op);
        else if (op->punct == '/') n = new_binary(ND_DIV, n, cast(&tok, tok->next), op);
        else if (op->punct == '%') n = new_binary(ND_MOD, n, cast(&tok, tok->next), op);
        else goto done;
        continue;
    }
    break;
  }
done:
  *rest = tok;
  return n;
}

static Node* logor(Token** rest, Token* tok) { return binop_level(rest, tok, 0); }

static Node* cast(Token** rest, Token* tok) {
  if (tok_punct(tok, '(') && is_typename(tok->next)) {
    Token* start = tok;
    Type* ty = type_name(&tok, tok->next);
    Node* n;
    tok = skip(tok, ')');
    if (tok_punct(tok, '{')) error_tok(start, "literal composto nao e suportado");
    n = new_cast(cast(rest, tok), ty);
    n->tok = start;
    return n;
  }
  return unary(rest, tok);
}

static Node* expr_new(Token** rest, Token* tok) {
  /* declspec + ponteiros. O '(' seguinte e o construtor, nao um tipo funcao. */
  Type* ty = declspec(&tok, tok, NULL);
  tok = pointers(tok, &ty);
  int is_arr = 0;
  Node* ne = NULL;
  Node* sz;
  Node* seq;
  Obj* slot;
  Node* call;
  if (ty->size < 0) error_tok(tok, "new de tipo incompleto");
  if (tok_punct(tok, '[')) {
    is_arr = 1;
    ne = expr(&tok, tok->next);
    tok = skip(tok, ']');
    if (ty->is_class && (find_special(ty, MK_CTOR) || find_special(ty, MK_DTOR)))
      error_tok(tok, "new[] nao chama construtor nem destrutor");
  }
  ensure_fn("malloc", pointer_to(ty_void));
  if (is_arr)
    sz = new_binary(ND_MUL, new_cast(ne, ty_uint), new_typed_num(ty->size, ty_uint, tok), tok);
  else sz = new_typed_num(ty->size, ty_uint, tok);
  add_type(sz);
  slot = new_lvar("", pointer_to(ty));
  call = call_mangle("malloc", sz, tok);
  seq = new_binary(ND_ASSIGN, new_var_node(slot, tok), new_cast(call, pointer_to(ty)), tok);
  if (!is_arr && tok_punct(tok, '(')) {
    Member* ctor = find_special(ty, MK_CTOR);
    Obj* cfn;
    Node* cc;
    if (!ty->is_class || !ctor) error_tok(tok, "tipo sem construtor");
    cfn = find_global(ctor->mangle);
    if (!cfn) error_tok(tok, "construtor nao declarado");
    cc = funcall(&tok, tok->next, new_var_node(cfn, tok), new_var_node(slot, tok));
    seq = new_binary(ND_COMMA, seq, cc, tok);
  } else if (!is_arr && ty->is_class && find_special(ty, MK_CTOR)) {
    Member* ctor = find_special(ty, MK_CTOR);
    if (ctor_needs_args(ctor)) error_tok(tok, "construtor precisa de argumentos");
    seq = new_binary(ND_COMMA, seq, call_mangle(ctor->mangle, new_var_node(slot, tok), tok), tok);
  }
  seq = new_binary(ND_COMMA, seq, new_var_node(slot, tok), tok);
  add_type(seq);
  *rest = tok;
  return seq;
}

static Node* expr_delete(Token** rest, Token* tok) {
  int arr = 0;
  Node* p;
  Node* seq;
  Obj* slot;
  Type* base;
  if (tok_punct(tok, '[')) {
    tok = skip(tok, '[');
    tok = skip(tok, ']');
    arr = 1;
  }
  p = cast(&tok, tok);
  add_type(p);
  *rest = tok;
  if (!p->ty || p->ty->kind != TY_PTR) error_tok(p->tok, "delete precisa de um ponteiro");
  base = p->ty->base;
  if (arr && base && base->is_class && find_special(base, MK_DTOR))
    error_tok(p->tok, "delete[] nao chama destrutor");
  ensure_fn("free", ty_void);
  slot = new_lvar("", p->ty);
  seq = new_binary(ND_ASSIGN, new_var_node(slot, p->tok), p, p->tok);
  if (!arr && base && base->is_class) {
    Member* d = find_special(base, MK_DTOR);
    if (d) seq = new_binary(ND_COMMA, seq, call_mangle(d->mangle, new_var_node(slot, p->tok), p->tok), p->tok);
  }
  seq = new_binary(ND_COMMA, seq, call_mangle("free", new_var_node(slot, p->tok), p->tok), p->tok);
  add_type(seq);
  return seq;
}

static Node* unary(Token** rest, Token* tok) {
  if (unsupported_kw(tok)) error_tok(tok, "'%s' ainda nao entra neste C++", tok->name);
  if (is_kw(tok, k_new)) return expr_new(rest, tok->next);
  if (is_kw(tok, k_delete)) return expr_delete(rest, tok->next);
  if (tok_punct(tok, '+')) return cast(rest, tok->next);
  if (tok_punct(tok, '-')) return new_unary(ND_NEG, cast(rest, tok->next), tok);
  if (tok_punct(tok, '&')) {
    Node* l = cast(rest, tok->next);
    add_type(l);
    return new_unary(ND_ADDR, l, tok);
  }
  if (tok_punct(tok, '*')) {
    Node* l = cast(rest, tok->next);
    add_type(l);
    if (l->ty->kind == TY_FUNC) return l;
    return new_unary(ND_DEREF, l, tok);
  }
  if (tok_punct(tok, '!')) return new_unary(ND_NOT, cast(rest, tok->next), tok);
  if (tok_punct(tok, '~')) return new_unary(ND_BNOT, cast(rest, tok->next), tok);
  if (tok_punct(tok, P_INC))
    return to_assign(new_add(unary(rest, tok->next), new_num(1, tok), tok));
  if (tok_punct(tok, P_DEC))
    return to_assign(new_sub(unary(rest, tok->next), new_num(1, tok), tok));
  if (is_kw(tok, k_sizeof) || is_kw(tok, k_alignof)) {
    int al = is_kw(tok, k_alignof);
    Type* ty;
    if (tok_punct(tok->next, '(') && is_typename(tok->next->next)) {
      ty = type_name(&tok, tok->next->next);
      *rest = skip(tok, ')');
    } else {
      Node* n = unary(rest, tok->next);
      add_type(n);
      ty = n->ty;
    }
    if (ty->size < 0 || (ty->kind == TY_ARRAY && ty->array_len < 0 && !al))
      error_tok(tok, "sizeof de tipo incompleto");
    return new_typed_num(al ? ty->align : ty->size, ty_uint, tok);
  }
  return postfix(rest, tok);
}

static Member* get_member(Type* ty, Token* name) {
  Member* m;
  if (!ty) return NULL;
  for (m = ty->members; m; m = m->next)
    if (m->name && m->name->name == name->name) return m;
  for (m = ty->members; m; m = m->next)
    if (!m->name && is_struct(m->ty) && get_member(m->ty, name)) return m;
  return NULL;
}

static Node* struct_ref(Node* n, Token* name) {
  add_type(n);
  if (!is_struct(n->ty)) error_tok(n->tok, "nao e struct nem union");
  for (;;) {
    Member* m = get_member(n->ty, name);
    Node* r;
    if (!m) error_tok(name, "membro '%s' nao existe", name->name);
    r = new_unary(ND_MEMBER, n, name);
    r->member = m;
    if (m->name) {
      if (!can_access(m)) error_tok(name, "'%s' nao e acessivel aqui", name->name);
      return r;
    }
    n = r;
    add_type(n);
  }
}

/* (typeof A)((A += 1) - 1) */
static Node* new_post(Node* n, int addend, Token* tok) {
  Node* a;
  add_type(n);
  a = to_assign(new_add(n, new_num(addend, tok), tok));
  return new_cast(new_add(a, new_num(-addend, tok), tok), n->ty);
}

static Node* funcall(Token** rest, Token* tok, Node* fn, Node* first) {
  Type* fty = NULL;
  Type* param;
  Node head;
  Node* cur = &head;
  Node* n;
  add_type(fn);
  if (fn->ty->kind == TY_FUNC) fty = fn->ty;
  else if (fn->ty->kind == TY_PTR && fn->ty->base->kind == TY_FUNC) fty = fn->ty->base;
  else error_tok(fn->tok, "nao e uma funcao");
  param = fty->params;
  head.next = NULL;
  {
  int seen = 0;
  if (first) {
    add_type(first);
    if (param) {
      if (param->is_ref) first = bind_ref(first, param);
      else if (!is_struct(param)) first = new_cast(first, param);
      param = param->next;
    }
    first->next = NULL;
    cur = cur->next = first;
  }
  while (!tok_punct(tok, ')')) {
    Node* arg;
    if (seen) tok = skip(tok, ',');
    seen = 1;
    arg = assign(&tok, tok);
    add_type(arg);
    if (param) {
      if (param->is_ref) arg = bind_ref(arg, param);
      else if (!is_struct(param)) arg = new_cast(arg, param);
      param = param->next;
    } else {
      if (!fty->is_variadic && !fty->no_proto) error_tok(arg->tok, "argumentos demais");
      if (arg->ty->kind == TY_BOOL || arg->ty->kind == TY_CHAR || arg->ty->kind == TY_SHORT)
        arg = new_cast(arg, ty_int);
      else if (arg->ty->kind == TY_ARRAY) arg = new_cast(arg, pointer_to(arg->ty->base));
      else if (arg->ty->kind == TY_FUNC) arg = new_cast(arg, pointer_to(arg->ty));
    }
    add_type(arg);
    if (arg->ty->kind == TY_VOID) error_tok(arg->tok, "argumento void");
    cur = cur->next = arg;
  }
  }
  if (param) error_tok(tok, "argumentos de menos");
  *rest = skip(tok, ')');
  n = new_unary(ND_FUNCALL, fn, tok);
  n->func_ty = fty;
  n->ty = fty->base;
  n->args = head.next;
  if (is_struct(n->ty)) n->ret_buf = new_lvar("", n->ty);
  return n;
}

static Node* postfix(Token** rest, Token* tok) {
  Node* n = primary(&tok, tok);
  for (;;) {
    if (tok_punct(tok, '(')) {
      if (n->kind == ND_MEMBER && n->member && n->member->is_method) {
        Obj* fn = find_global(n->member->mangle);
        if (!fn) error_tok(n->tok, "metodo '%s' nao declarado", n->member->mangle);
        n = funcall(&tok, tok->next, new_var_node(fn, n->tok), new_unary(ND_ADDR, n->lhs, n->tok));
      } else {
        n = funcall(&tok, tok->next, n, NULL);
      }
      continue;
    }
    if (tok_punct(tok, '[')) {
      Token* start = tok;
      Node* idx = expr(&tok, tok->next);
      tok = skip(tok, ']');
      n = new_unary(ND_DEREF, new_add(n, idx, start), start);
      continue;
    }
    if (tok_punct(tok, '.')) {
      n = struct_ref(n, tok->next);
      tok = tok->next->next;
      continue;
    }
    if (tok_punct(tok, P_ARROW)) {
      n = new_unary(ND_DEREF, n, tok);
      n = struct_ref(n, tok->next);
      tok = tok->next->next;
      continue;
    }
    if (tok_punct(tok, P_INC)) {
      n = new_post(n, 1, tok);
      tok = tok->next;
      continue;
    }
    if (tok_punct(tok, P_DEC)) {
      n = new_post(n, -1, tok);
      tok = tok->next;
      continue;
    }
    if (n->kind == ND_MEMBER && n->member && n->member->is_method)
      error_tok(n->tok, "metodo precisa ser chamado");
    *rest = tok;
    return n;
  }
}

static Node* primary(Token** rest, Token* tok) {
  if (tok_punct(tok, '(')) {
    Node* n;
    if (tok_punct(tok->next, '{')) error_tok(tok, "expressao-comando ({...}) nao e suportada");
    n = expr(&tok, tok->next);
    *rest = skip(tok, ')');
    return n;
  }
  if (tok->kind == TK_NUM) {
    Node* n;
    if (!tok->ty) convert_number(tok);
    n = new_typed_num(tok->ival, tok->ty, tok);
    *rest = tok->next;
    return n;
  }
  if (tok->kind == TK_CHAR) {
    *rest = tok->next;
    return new_typed_num(tok->ival, ty_int, tok);
  }
  if (tok->kind == TK_STR) {
    Buf b;
    Token* t;
    Obj* v;
    memset(&b, 0, sizeof(b));
    for (t = tok; t->kind == TK_STR; t = t->next) buf_add(&b, t->str, t->slen - 1);
    buf_byte(&b, 0);
    v = new_string_literal(b.p, b.len);
    *rest = t;
    return new_var_node(v, tok);
  }
  if (is_kw(tok, k_func) && cur_fn) {
    Obj* v = new_string_literal(cur_fn->name, (int)strlen(cur_fn->name) + 1);
    *rest = tok->next;
    return new_var_node(v, tok);
  }
  if (is_kw(tok, k_nullptr)) {
    *rest = tok->next;
    return new_typed_num(0, pointer_to(ty_void), tok);
  }
  if (is_kw(tok, k_this)) {
    Obj* p;
    if (cur_fn) {
      for (p = cur_fn->params; p; p = p->next)
        if (p->name == k_this) {
          *rest = tok->next;
          return new_var_node(p, tok);
        }
    }
    error_tok(tok, "'this' fora de um metodo");
  }
  if (tok->kind == TK_IDENT && !is_keyword(tok) && tok_punct(tok->next, P_SCOPE) &&
      is_namespace(tok->name)) {
    Token* name = tok->next->next;
    char* sym;
    Obj* g;
    if (name->kind != TK_IDENT || is_keyword(name)) error_tok(name, "esperava um nome");
    sym = join_name(tok->name, name->name);
    g = find_global(sym);
    if (!g) error_tok(name, "'%s::%s' nao foi declarado", tok->name, name->name);
    *rest = name->next;
    return new_var_node(g, name);
  }
  if (tok->kind == TK_IDENT && !is_keyword(tok)) {
    VarScope* sc = find_var(tok);
    *rest = tok->next;
    if (sc) {
      if (sc->var) {
        Node* nv = new_var_node(sc->var, tok);
        add_type(nv);
        if (nv->ty && nv->ty->is_ref) {
          nv = new_unary(ND_DEREF, nv, tok);
          add_type(nv);
        }
        return nv;
      }
      if (sc->enum_ty) return new_typed_num(sc->enum_val, ty_int, tok);
      error_tok(tok, "'%s' e um tipo, nao um valor", tok->name);
    }
    if (cur_class && cur_fn && get_member(cur_class, tok)) {
      Obj* th = NULL;
      Obj* p;
      for (p = cur_fn->params; p; p = p->next)
        if (p->name == k_this) th = p;
      if (!th) error_tok(tok, "'%s' nao foi declarado", tok->name);
      *rest = tok->next;
      return struct_ref(new_unary(ND_DEREF, new_var_node(th, tok), tok), tok);
    }
    if (ns_name) {
      Obj* g = find_global(join_name(ns_name, tok->name));
      if (g) return new_var_node(g, tok);
    }
    if (tok_punct(tok->next, '(')) {
      /* C89: funcao nao declarada = int f() */
      Scope* save = scope;
      Obj* g;
      Type* ft = func_type(ty_int);
      ft->no_proto = 1;
      warn_tok(tok, "funcao '%s' sem declaracao", tok->name);
      while (scope->next) scope = scope->next;
      g = new_gvar(tok->name, ft);
      g->is_function = 1;
      g->tok = tok;
      scope = save;
      return new_var_node(g, tok);
    }
    error_tok(tok, "'%s' nao foi declarado", tok->name);
  }
  error_tok(tok, "esperava uma expressao");
  return NULL;
}

/* ---------- topo ---------- */
static Token* parse_typedef(Token* tok, Type* base) {
  int first = 1;
  while (!consume(&tok, tok, ';')) {
    Type* ty;
    if (!first) tok = skip(tok, ',');
    first = 0;
    ty = declarator(&tok, tok, base);
    if (!ty->name) error_tok(tok, "typedef sem nome");
    push_scope(ty->name->name)->type_def = ty;
  }
  return tok;
}

/* Parametros acima de ebp. Em long mode [ebp+0] e o rbp salvo (8) e
 * [ebp+8] e o endereco de volta (8), entao o 1o argumento fica em [ebp+16].
 * _start e DllMain recebem os argumentos em registradores: ficam abaixo. */
static int entry_fn(Obj* fn) {
  return fn->name && (strcmp(fn->name, "_start") == 0 || strcmp(fn->name, "DllMain") == 0);
}

static void assign_offsets(Obj* fn) {
  int off = fn->ret_struct ? 20 : 16;
  int bottom = 0;
  Obj* v;
  if (entry_fn(fn)) {
    for (v = fn->params; v; v = v->next) {
      bottom += align_to(v->ty->size, 4);
      v->offset = -bottom;
    }
  } else {
    for (v = fn->params; v; v = v->next) {
      v->offset = off;
      off += align_to(v->ty->size, 4);
    }
  }
  for (v = fn->locals; v; v = v->next) {
    int al = (v->ty->align >= 4 || v->ty->size >= 4) ? 4 : v->ty->align;
    bottom += v->ty->size > 0 ? v->ty->size : 4;
    bottom = align_to(bottom, al);
    v->offset = -bottom;
  }
  fn->stack_size = align_to(bottom, 4);
}

static Obj* declare_function(Type* ty, VarAttr* attr, Token* name_tok) {
  char* name = take_symbol(name_tok->name);
  Obj* fn = find_global(name);
  Scope* save;
  if (fn) {
    if (!fn->is_function) error_tok(name_tok, "'%s' redeclarado como funcao", name);
    if (fn->ty->no_proto && !ty->no_proto) fn->ty = ty;
    if (attr->is_static) fn->is_static = 1;
    if (attr->is_export) fn->is_export = 1;
    return fn;
  }
  save = scope;
  while (scope->next) scope = scope->next;
  fn = new_gvar(name, ty);
  scope = save;
  fn->is_function = 1;
  fn->is_static = attr->is_static;
  fn->is_export = attr->is_export;
  fn->tok = name_tok;
  return fn;
}

static Obj* this_param_of(Obj* fn) {
  Obj* p;
  for (p = fn->params; p; p = p->next)
    if (p->name == k_this) return p;
  return NULL;
}

static Node* this_as(Obj* fn, Type* as, Token* tok) {
  Obj* p = this_param_of(fn);
  Node* n;
  if (!p) error_tok(tok, "'this' fora de um metodo");
  n = new_var_node(p, tok);
  if (as) n = new_cast(n, pointer_to(as));
  return n;
}

static void append_stmts(Node* block, Node* stmts) {
  Node* c;
  if (!stmts) return;
  if (!block->body) {
    block->body = stmts;
    return;
  }
  c = block->body;
  while (c->next) c = c->next;
  c->next = stmts;
}

static Token* parse_fn_body(Token* tok, Obj* fn) {
  Type* pt;
  Obj head;
  Obj* curp = &head;
  Type* save_cls = cur_class;
  Node* base_call = NULL;
  int fn_depth;
  if (fn->body) error_tok(fn->tok ? fn->tok : tok, "funcao '%s' definida duas vezes", fn->name);
  fn->is_definition = 1;
  fn->ret_struct = is_struct(fn->ty->base);
  cur_fn = fn;
  cur_class = fn->method_cls;
  locals = NULL;
  enter_scope();
  fn_depth = scope_depth;
  head.next = NULL;
  for (pt = fn->ty->params; pt; pt = pt->next) {
    Obj* p;
    if (!pt->name) error_tok(fn->tok ? fn->tok : tok, "parametro sem nome na definicao de '%s'", fn->name);
    p = new_lvar(pt->name->name, pt);
    p->tok = pt->name;
    curp->next = (Obj*)xalloc((int)sizeof(Obj));
    curp = curp->next;
    *curp = *p;
    curp->next = NULL;
    find_var_here(p->name)->var = curp;
  }
  fn->params = head.next;
  locals = NULL;
  if (fn->method_kind == MK_DTOR && fn->method_cls && fn->method_cls->parent) {
    Member* bd = find_special(fn->method_cls->parent, MK_DTOR);
    if (bd) {
      DtorUse* u = (DtorUse*)xalloc((int)sizeof(DtorUse));
      u->self = this_param_of(fn);
      u->as_ty = fn->method_cls->parent;
      u->mangle = bd->mangle;
      u->depth = fn_depth;
      u->next = dtor_stack;
      dtor_stack = u;
    }
  }
  if (fn->method_kind == MK_CTOR && tok_punct(tok, ':')) {
    Type* parent = fn->method_cls ? fn->method_cls->parent : NULL;
    Member* ctor;
    Obj* cfn;
    if (!parent) error_tok(tok, "nao tem classe base");
    tok = tok->next;
    if (tok->kind != TK_IDENT || tok->name != parent->tag_name)
      error_tok(tok, "esperava %s", parent->tag_name);
    tok = tok->next;
    if (!tok_punct(tok, '(')) error_tok(tok, "esperava '('");
    ctor = find_special(parent, MK_CTOR);
    if (!ctor) error_tok(tok, "base sem construtor");
    cfn = find_global(ctor->mangle);
    if (!cfn) error_tok(tok, "construtor da base nao declarado");
    base_call = funcall(&tok, tok->next, new_var_node(cfn, tok), this_as(fn, parent, tok));
  } else if (fn->method_kind == MK_CTOR && fn->method_cls && fn->method_cls->parent) {
    Member* ctor = find_special(fn->method_cls->parent, MK_CTOR);
    if (ctor) {
      if (ctor_needs_args(ctor))
        error_tok(tok, "construtor da base precisa de : %s(...)", fn->method_cls->parent->tag_name);
      base_call = call_mangle(ctor->mangle, this_as(fn, fn->method_cls->parent, tok), tok);
    }
  }
  tok = skip(tok, '{');
  fn->body = compound_stmt(&tok, tok);
  if (base_call) {
    Node* st = new_unary(ND_EXPR_STMT, base_call, fn->tok ? fn->tok : tok);
    st->next = fn->body->body;
    fn->body->body = st;
  }
  append_stmts(fn->body, take_dtor_stmts(fn_depth, fn->tok ? fn->tok : tok));
  fn->locals = locals;
  leave_scope();
  assign_offsets(fn);
  cur_fn = NULL;
  cur_class = save_cls;
  return tok;
}

static Token* function(Token* tok, Type* base, VarAttr* attr) {
  Type* ty = declarator(&tok, tok, base);
  Obj* fn;
  Type* cls = pend_cls;
  int kind = pend_kind;
  pend_cls = NULL;
  pend_kind = 0;
  if (!ty->name) error_tok(tok, "funcao sem nome");
  if (ty->kind != TY_FUNC) error_tok(ty->name, "esperava uma funcao");
  fn = declare_function(ty, attr, ty->name);
  if (cls) {
    fn->method_cls = cls;
    fn->method_kind = kind;
  }
  if (consume(&tok, tok, ';')) return tok;
  if (fn->is_definition) error_tok(ty->name, "funcao '%s' definida duas vezes", fn->name);
  fn->ty = ty;
  fn->tok = ty->name;
  return parse_fn_body(tok, fn);
}

static Token* global_variable(Token* tok, Type* base, VarAttr* attr) {
  int first = 1;
  while (!consume(&tok, tok, ';')) {
    Type* ty;
    Obj* v;
    if (!first) tok = skip(tok, ',');
    first = 0;
    ty = declarator(&tok, tok, base);
    if (!ty->name) error_tok(tok, "variavel sem nome");
    if (ty->kind == TY_FUNC) {
      declare_function(ty, attr, ty->name);
      continue;
    }
    {
      char* sym = take_symbol(ty->name->name);
      if (ty->is_class && find_special(ty, MK_CTOR))
        error_tok(ty->name, "objeto global com construtor nao e suportado");
      v = find_global(sym);
      if (v) {
        if (v->is_function) error_tok(ty->name, "'%s' ja e uma funcao", v->name);
        if (ty->kind == TY_ARRAY && ty->array_len >= 0) v->ty = ty;
        if (attr->is_static) v->is_static = 1;
        if (attr->is_export) v->is_export = 1;
      } else {
        v = new_gvar(sym, ty);
        v->is_static = attr->is_static;
        v->is_export = attr->is_export;
        v->tok = ty->name;
      }
    }
    if (tok_punct(tok, '=')) {
      if (v->is_definition && v->init_data) error_tok(ty->name, "'%s' inicializado duas vezes", v->name);
      v->is_definition = 1;
      v->is_extern = 0;
      v->is_tentative = 0;
      gvar_initializer(&tok, tok->next, v);
    } else if (attr->is_extern) {
      if (!v->is_definition) v->is_extern = 1;
    } else if (!v->is_definition) {
      v->is_definition = 1;
      v->is_extern = 0;
      v->is_tentative = 1;
    }
  }
  return tok;
}

static int is_function_decl(Token* tok, Type* base) {
  Type* ty;
  if (tok_punct(tok, ';')) return 0;
  if (tok_punct(tok, P_SCOPE)) return 1;
  if (tok->kind == TK_IDENT && tok_punct(tok->next, P_SCOPE)) return 1;
  ty = declarator(&tok, tok, base);
  return ty->kind == TY_FUNC;
}

Obj* parse(Token* tok) {
  init_keywords();
  globals = NULL;
  scope = NULL;
  cur_fn = NULL;
  cur_loop = NULL;
  cur_brk = NULL;
  cur_switch = NULL;
  cur_class = NULL;
  ns_name = NULL;
  namespaces = NULL;
  dtor_stack = NULL;
  pend_cls = NULL;
  pend_kind = 0;
  raw_symbol = 0;
  scope_depth = 0;
  enter_scope();
  while (tok->kind != TK_EOF) {
    VarAttr attr;
    Type* base;
    if (tok_punct(tok, ';')) {
      tok = tok->next;
      continue;
    }
    if (ns_name && tok_punct(tok, '}')) {
      ns_name = NULL;
      tok = tok->next;
      continue;
    }
    if (unsupported_kw(tok)) error_tok(tok, "'%s' ainda nao entra neste C++", tok->name);
    if (is_kw(tok, k_namespace)) {
      char* name;
      if (ns_name) error_tok(tok, "namespace aninhado nao e suportado");
      name = get_ident(tok->next);
      tok = skip(tok->next->next, '{');
      add_ns(name);
      ns_name = name;
      continue;
    }
    memset(&attr, 0, sizeof(attr));
    base = declspec(&tok, tok, &attr);
    if (attr.is_typedef) {
      tok = parse_typedef(tok, base);
      continue;
    }
    if (is_function_decl(tok, base)) {
      tok = function(tok, base, &attr);
      continue;
    }
    tok = global_variable(tok, base, &attr);
  }
  return globals;
}
