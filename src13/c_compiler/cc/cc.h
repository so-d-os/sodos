/* cc.h - Compilador C do SO: pre-processador, parser e gerador i386 (COBJ).
 * Escrito em C simples para compilar no Linux (gcc) e em si mesmo. */
#ifndef CC_H
#define CC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

typedef struct Token Token;
typedef struct Hideset Hideset;
typedef struct Type Type;
typedef struct Member Member;
typedef struct Obj Obj;
typedef struct Node Node;
typedef struct Reloc Reloc;

/* ---------- util.c ---------- */
void* xalloc(int n);
char* xstrndup(const char* s, int n);
char* xstrdup(const char* s);
char* intern(const char* s, int n);
char* format(const char* fmt, ...);
void error(const char* fmt, ...);
void error_tok(Token* t, const char* fmt, ...);
void warn_tok(Token* t, const char* fmt, ...);
char* read_file(const char* path, int* len);

typedef struct {
  char* p;
  int len;
  int cap;
} Buf;
void buf_add(Buf* b, const void* p, int n);
void buf_byte(Buf* b, int c);
void buf_u32(Buf* b, unsigned v);

/* ---------- lex.c ---------- */
enum {
  TK_IDENT, TK_PUNCT, TK_NUM, TK_CHAR, TK_STR, TK_EOF
};

/* pontuacao de mais de um caractere (os simples usam o proprio char) */
enum {
  P_ARROW = 256, P_INC, P_DEC, P_SHL, P_SHR, P_LE, P_GE, P_EQ, P_NE, P_LAND, P_LOR,
  P_ADD_EQ, P_SUB_EQ, P_MUL_EQ, P_DIV_EQ, P_MOD_EQ, P_AND_EQ, P_OR_EQ, P_XOR_EQ,
  P_SHL_EQ, P_SHR_EQ, P_ELLIPSIS, P_HASHHASH, P_SCOPE
};

typedef struct {
  char* name;
  char* text;
  int len;
} SrcFile;

struct Token {
  int kind;
  int punct;
  Token* next;
  char* loc;   /* texto original */
  int len;
  char* name;  /* TK_IDENT: nome internado */
  SrcFile* file;
  int line;
  int bol;     /* primeiro do linha */
  int space;   /* espaco antes */
  Hideset* hs;
  Token* origin; /* token da macro que gerou este (para mensagens) */
  /* literais */
  long long ival;
  Type* ty;    /* tipo do numero/char/string */
  char* str;   /* string decodificada (com NUL) */
  int slen;    /* bytes incluindo o NUL */
};

Token* tokenize(SrcFile* f);
Token* tokenize_string(char* text, SrcFile* f, int line);
SrcFile* new_file(char* name, char* text, int len);
Token* new_eof(Token* ref);
int tok_is(Token* t, const char* s);
int tok_punct(Token* t, int p);
char* tok_text(Token* t);
void convert_number(Token* t);

/* ---------- pp.c ---------- */
void pp_add_include(char* dir);
void pp_define(char* def);
void pp_init(void);
void pp_reset(void);
Token* preprocess(Token* tok);
Token* pp_file(char* path);
extern char* pp_main_file;

/* ---------- type.c ---------- */
enum {
  TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_LLONG, TY_ENUM,
  TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT, TY_UNION
};

struct Type {
  int kind;
  int size;
  int align;
  int is_unsigned;
  Type* base;        /* ponteiro/array: elemento; funcao: retorno */
  int array_len;     /* -1: sem tamanho */
  Token* name;       /* declarador */
  /* struct/union */
  Member* members;
  int is_complete;
  /* funcao */
  Type* params;      /* lista ligada por next */
  Type* next;
  int is_variadic;
  int no_proto;      /* int f() */
  Type* origin;      /* copia de onde veio */
  int is_class;      /* struct declarado com class */
  int is_ref;        /* ponteiro que e uma referencia T& */
  Type* parent;      /* classe base; subobjeto no offset 0 */
  char* tag_name;    /* nome internado da classe */
};

struct Member {
  Member* next;
  Type* ty;
  Token* name;
  int offset;
  int is_method;
  int is_ctor;
  int is_dtor;
  int access;        /* 0 public, 1 private, 2 protected */
  char* mangle;      /* funcao global do metodo */
  Type* owner;
};

extern Type* ty_void;
extern Type* ty_bool;
extern Type* ty_char;
extern Type* ty_uchar;
extern Type* ty_short;
extern Type* ty_ushort;
extern Type* ty_int;
extern Type* ty_uint;
extern Type* ty_long;
extern Type* ty_ulong;
extern Type* ty_llong;
extern Type* ty_ullong;

int is_integer(Type* t);
int is_ptr_like(Type* t);   /* ponteiro ou array */
int is_scalar(Type* t);
int is_64(Type* t);
int is_struct(Type* t);
Type* copy_type(Type* t);
Type* pointer_to(Type* base);
Type* array_of(Type* base, int len);
Type* func_type(Type* ret);
Type* enum_type(void);
Type* struct_type(void);
void add_type(Node* n);
int type_compatible(Type* a, Type* b);

/* ---------- parse.c ---------- */
enum {
  ND_NULL_EXPR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_BAND, ND_BOR, ND_BXOR,
  ND_SHL, ND_SHR, ND_EQ, ND_NE, ND_LT, ND_LE, ND_ASSIGN, ND_COND, ND_COMMA,
  ND_MEMBER, ND_ADDR, ND_DEREF, ND_NOT, ND_BNOT, ND_NEG, ND_LOGAND, ND_LOGOR,
  ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_CASE, ND_BLOCK, ND_GOTO, ND_LABEL,
  ND_FUNCALL, ND_EXPR_STMT, ND_VAR, ND_NUM, ND_CAST, ND_MEMZERO, ND_BREAK, ND_CONTINUE
};

struct Node {
  int kind;
  Type* ty;
  Token* tok;
  Node* next;
  Node* lhs;
  Node* rhs;
  Node* cond;
  Node* then;
  Node* els;
  Node* init;
  Node* inc;
  Node* body;
  Member* member;
  Type* func_ty;
  Node* args;
  Obj* ret_buf;   /* struct devolvida por valor */
  Obj* var;
  long long val;
  /* rotulos */
  int brk;
  int cont;
  char* label;
  int label_id;
  Node* goto_next;
  Node* label_next;
  Node* case_next;
  Node* default_case;
  Node* target;   /* break/continue: laco ou switch (fora da arvore) */
};

typedef struct InitData InitData;
struct Reloc {
  Reloc* next;
  int offset;
  Obj* target;
  long long addend;
};

struct Obj {
  Obj* next;
  char* name;
  Type* ty;
  Token* tok;
  int is_local;
  int offset;          /* local: relativo a ebp */
  int is_function;
  int is_definition;
  int is_static;
  int is_extern;
  int is_export;
  int is_tentative;
  int is_string;
  /* global */
  char* init_data;
  Reloc* rel;
  int sym;             /* indice no .OBJ, -1 se ainda nao tem */
  int sect;            /* onde ficou: COB_SECT_* */
  int value;           /* offset na secao */
  /* funcao */
  Obj* params;
  Node* body;
  Obj* locals;
  int stack_size;
  int ret_struct;      /* devolve struct: ponteiro escondido em [ebp+16] */
  Node* gotos;
  Node* labels;
  int used;
  int method_kind;   /* 0 funcao, 1 metodo, 2 construtor, 3 destrutor */
  Type* method_cls;
};

Obj* parse(Token* tok);
long long const_expr_value(Node* n);

/* ---------- gen.c ---------- */
void codegen(Obj* globals, const char* out_path);

/* ---------- main.c ---------- */
extern int opt_warn;

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
int align_to(int n, int a);

#endif
