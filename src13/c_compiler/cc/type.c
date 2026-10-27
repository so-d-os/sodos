/* type.c - Tipos do C (alvo i386: int/long/ponteiro = 4 bytes, long long = 8). */
#include "cc.h"

static Type* new_type(int kind, int size, int align, int is_unsigned) {
  Type* t = (Type*)xalloc((int)sizeof(Type));
  t->kind = kind;
  t->size = size;
  t->align = align;
  t->is_unsigned = is_unsigned;
  return t;
}

static Type t_void = { TY_VOID, 1, 1, 0 };
static Type t_bool = { TY_BOOL, 1, 1, 1 };
static Type t_char = { TY_CHAR, 1, 1, 0 };
static Type t_uchar = { TY_CHAR, 1, 1, 1 };
static Type t_short = { TY_SHORT, 2, 2, 0 };
static Type t_ushort = { TY_SHORT, 2, 2, 1 };
static Type t_int = { TY_INT, 4, 4, 0 };
static Type t_uint = { TY_INT, 4, 4, 1 };
static Type t_long = { TY_LONG, 4, 4, 0 };
static Type t_ulong = { TY_LONG, 4, 4, 1 };
static Type t_llong = { TY_LLONG, 8, 4, 0 };
static Type t_ullong = { TY_LLONG, 8, 4, 1 };

Type* ty_void = &t_void;
Type* ty_bool = &t_bool;
Type* ty_char = &t_char;
Type* ty_uchar = &t_uchar;
Type* ty_short = &t_short;
Type* ty_ushort = &t_ushort;
Type* ty_int = &t_int;
Type* ty_uint = &t_uint;
Type* ty_long = &t_long;
Type* ty_ulong = &t_ulong;
Type* ty_llong = &t_llong;
Type* ty_ullong = &t_ullong;

int is_integer(Type* t) {
  int k = t->kind;
  return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT || k == TY_LONG ||
         k == TY_LLONG || k == TY_ENUM;
}

int is_ptr_like(Type* t) { return t->kind == TY_PTR || t->kind == TY_ARRAY; }
int is_scalar(Type* t) { return is_integer(t) || t->kind == TY_PTR; }
int is_64(Type* t) { return t->kind == TY_LLONG; }
int is_struct(Type* t) { return t->kind == TY_STRUCT || t->kind == TY_UNION; }

Type* copy_type(Type* t) {
  Type* c = (Type*)xalloc((int)sizeof(Type));
  *c = *t;
  c->origin = t;
  c->next = NULL;
  return c;
}

Type* pointer_to(Type* base) {
  Type* t = new_type(TY_PTR, 4, 4, 1);
  t->base = base;
  return t;
}

Type* array_of(Type* base, int len) {
  Type* t = new_type(TY_ARRAY, base->size * (len < 0 ? 0 : len), base->align, 0);
  t->base = base;
  t->array_len = len;
  return t;
}

Type* func_type(Type* ret) {
  Type* t = new_type(TY_FUNC, 1, 1, 0);
  t->base = ret;
  return t;
}

Type* enum_type(void) { return new_type(TY_ENUM, 4, 4, 0); }

Type* struct_type(void) { return new_type(TY_STRUCT, 0, 1, 0); }

int type_compatible(Type* a, Type* b) {
  if (a == b) return 1;
  if (a->origin) return type_compatible(a->origin, b);
  if (b->origin) return type_compatible(a, b->origin);
  if (a->kind != b->kind) {
    /* int e long sao o mesmo aqui; enum e int tambem */
    int ai = a->kind == TY_INT || a->kind == TY_LONG || a->kind == TY_ENUM;
    int bi = b->kind == TY_INT || b->kind == TY_LONG || b->kind == TY_ENUM;
    return ai && bi && a->is_unsigned == b->is_unsigned;
  }
  switch (a->kind) {
    case TY_CHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG:
      return a->is_unsigned == b->is_unsigned;
    case TY_PTR:
      return type_compatible(a->base, b->base);
    case TY_ARRAY:
      return type_compatible(a->base, b->base);
    case TY_FUNC:
      return type_compatible(a->base, b->base);
  }
  return a->kind == TY_VOID || a->kind == TY_BOOL;
}

static Node* cast_node(Node* n, Type* ty) {
  Node* c = (Node*)xalloc((int)sizeof(Node));
  add_type(n);
  c->kind = ND_CAST;
  c->tok = n->tok;
  c->lhs = n;
  c->ty = copy_type(ty);
  return c;
}

static Type* promote(Type* t) {
  if (t->kind == TY_LLONG) return t->is_unsigned ? ty_ullong : ty_llong;
  if (t->kind == TY_INT || t->kind == TY_LONG) return t->is_unsigned ? ty_uint : ty_int;
  return ty_int; /* bool, char, short, enum */
}

static Type* common_type(Type* a, Type* b) {
  if (a->base && (a->kind == TY_PTR || a->kind == TY_ARRAY)) return pointer_to(a->base);
  if (a->kind == TY_FUNC) return pointer_to(a);
  if (b->kind == TY_FUNC) return pointer_to(b);
  a = promote(a);
  b = promote(b);
  if (a->kind == TY_LLONG || b->kind == TY_LLONG) {
    if ((a->kind == TY_LLONG && a->is_unsigned) || (b->kind == TY_LLONG && b->is_unsigned))
      return ty_ullong;
    return ty_llong;
  }
  if (a->is_unsigned || b->is_unsigned) return ty_uint;
  return ty_int;
}

static void usual_conv(Node* n) {
  Type* t = common_type(n->lhs->ty, n->rhs->ty);
  n->lhs = cast_node(n->lhs, t);
  n->rhs = cast_node(n->rhs, t);
}

void add_type(Node* n) {
  Node* x;
  if (!n || n->ty) return;
  add_type(n->lhs);
  add_type(n->rhs);
  add_type(n->cond);
  add_type(n->then);
  add_type(n->els);
  add_type(n->init);
  add_type(n->inc);
  for (x = n->body; x; x = x->next) add_type(x);
  for (x = n->args; x; x = x->next) add_type(x);

  switch (n->kind) {
    case ND_NUM:
      n->ty = ty_int;
      return;
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BAND: case ND_BOR: case ND_BXOR:
      usual_conv(n);
      n->ty = n->lhs->ty;
      return;
    case ND_NEG: {
      Type* t = promote(n->lhs->ty);
      n->lhs = cast_node(n->lhs, t);
      n->ty = t;
      return;
    }
    case ND_ASSIGN:
      if (n->lhs->ty->kind == TY_ARRAY) error_tok(n->tok, "nao da para atribuir a um array");
      if (!is_struct(n->lhs->ty)) n->rhs = cast_node(n->rhs, n->lhs->ty);
      n->ty = n->lhs->ty;
      return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
      usual_conv(n);
      n->ty = ty_int;
      return;
    case ND_FUNCALL:
      n->ty = n->func_ty->base;
      return;
    case ND_NOT: case ND_LOGAND: case ND_LOGOR:
      n->ty = ty_int;
      return;
    case ND_BNOT: case ND_SHL: case ND_SHR: {
      Type* t = promote(n->lhs->ty);
      n->lhs = cast_node(n->lhs, t);
      if (n->rhs) n->rhs = cast_node(n->rhs, ty_int);
      n->ty = t;
      return;
    }
    case ND_VAR:
      n->ty = n->var->ty;
      return;
    case ND_COND:
      if (n->then->ty->kind == TY_VOID || n->els->ty->kind == TY_VOID) {
        n->ty = ty_void;
      } else if (is_integer(n->then->ty) && is_integer(n->els->ty)) {
        Type* t = common_type(n->then->ty, n->els->ty);
        n->then = cast_node(n->then, t);
        n->els = cast_node(n->els, t);
        n->ty = t;
      } else if (n->then->ty->kind == TY_PTR || n->then->ty->kind == TY_ARRAY) {
        n->ty = n->then->ty->kind == TY_ARRAY ? pointer_to(n->then->ty->base) : n->then->ty;
      } else if (n->els->ty->kind == TY_PTR || n->els->ty->kind == TY_ARRAY) {
        n->ty = n->els->ty->kind == TY_ARRAY ? pointer_to(n->els->ty->base) : n->els->ty;
      } else {
        n->ty = n->then->ty;
      }
      return;
    case ND_COMMA:
      n->ty = n->rhs->ty;
      return;
    case ND_MEMBER:
      n->ty = n->member->ty;
      return;
    case ND_ADDR:
      if (n->lhs->ty->kind == TY_ARRAY) n->ty = pointer_to(n->lhs->ty->base);
      else n->ty = pointer_to(n->lhs->ty);
      return;
    case ND_DEREF:
      if (n->lhs->ty->kind == TY_FUNC) {
        n->ty = n->lhs->ty;
        return;
      }
      if (!n->lhs->ty->base) error_tok(n->tok, "'*' em algo que nao e ponteiro");
      if (n->lhs->ty->base->kind == TY_VOID) error_tok(n->tok, "'*' em ponteiro void");
      n->ty = n->lhs->ty->base;
      return;
  }
}
