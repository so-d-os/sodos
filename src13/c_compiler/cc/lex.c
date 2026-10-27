/* lex.c - Texto -> lista de tokens. */
#include "cc.h"

static SrcFile* cur_file;
static int cur_line;
static int at_bol;
static int has_space;

SrcFile* new_file(char* name, char* text, int len) {
  SrcFile* f = (SrcFile*)xalloc((int)sizeof(SrcFile));
  f->name = name;
  f->text = text;
  f->len = len;
  return f;
}

static Token* new_token(int kind, char* start, char* end) {
  Token* t = (Token*)xalloc((int)sizeof(Token));
  t->kind = kind;
  t->loc = start;
  t->len = (int)(end - start);
  t->file = cur_file;
  t->line = cur_line;
  t->bol = at_bol;
  t->space = has_space;
  at_bol = 0;
  has_space = 0;
  return t;
}

Token* new_eof(Token* ref) {
  Token* t = (Token*)xalloc((int)sizeof(Token));
  t->kind = TK_EOF;
  t->loc = ref ? ref->loc : (char*)"";
  t->file = ref ? ref->file : NULL;
  t->line = ref ? ref->line : 0;
  t->bol = 1;
  return t;
}

int tok_is(Token* t, const char* s) {
  int n = (int)strlen(s);
  return t->len == n && memcmp(t->loc, s, (size_t)n) == 0 && t->kind != TK_STR && t->kind != TK_CHAR;
}

int tok_punct(Token* t, int p) { return t->kind == TK_PUNCT && t->punct == p; }

char* tok_text(Token* t) { return xstrndup(t->loc, t->len); }

static void lex_error(char* p, const char* msg) {
  Token t;
  memset(&t, 0, sizeof(t));
  t.loc = p;
  t.file = cur_file;
  t.line = cur_line;
  error_tok(&t, "%s", msg);
}

static int is_ident1(int c) { return isalpha(c) || c == '_' || c == '$'; }
static int is_ident2(int c) { return isalnum(c) || c == '_' || c == '$'; }

static int read_punct(char* p, int* code) {
  static const char* three[] = { "<<=", ">>=", "..." };
  static const int three_c[] = { P_SHL_EQ, P_SHR_EQ, P_ELLIPSIS };
  static const char* two[] = { "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                               "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", "::" };
  static const int two_c[] = { P_ARROW, P_INC, P_DEC, P_SHL, P_SHR, P_LE, P_GE, P_EQ, P_NE,
                               P_LAND, P_LOR, P_ADD_EQ, P_SUB_EQ, P_MUL_EQ, P_DIV_EQ, P_MOD_EQ,
                               P_AND_EQ, P_OR_EQ, P_XOR_EQ, P_HASHHASH, P_SCOPE };
  int i;
  for (i = 0; i < 3; i++)
    if (strncmp(p, three[i], 3) == 0) {
      *code = three_c[i];
      return 3;
    }
  for (i = 0; i < 21; i++)
    if (p[0] == two[i][0] && p[1] == two[i][1]) {
      *code = two_c[i];
      return 2;
    }
  if (ispunct((unsigned char)*p)) {
    *code = (unsigned char)*p;
    return 1;
  }
  return 0;
}

static int from_hex(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

static int read_escape(char** pp) {
  char* p = *pp;
  int c;
  if (*p >= '0' && *p <= '7') {
    c = *p++ - '0';
    if (*p >= '0' && *p <= '7') {
      c = (c << 3) + (*p++ - '0');
      if (*p >= '0' && *p <= '7') c = (c << 3) + (*p++ - '0');
    }
    *pp = p;
    return c;
  }
  if (*p == 'x') {
    p++;
    if (!isxdigit((unsigned char)*p)) lex_error(p, "escape \\x invalido");
    c = 0;
    while (isxdigit((unsigned char)*p)) c = (c << 4) + from_hex(*p++);
    *pp = p;
    return c;
  }
  *pp = p + 1;
  switch (*p) {
    case 'a': return 7;
    case 'b': return 8;
    case 't': return 9;
    case 'n': return 10;
    case 'v': return 11;
    case 'f': return 12;
    case 'r': return 13;
    case 'e': return 27;
    default: return (unsigned char)*p;
  }
}

static Token* read_string(char* start) {
  char* p = start + 1;
  Buf b;
  Token* t;
  memset(&b, 0, sizeof(b));
  for (;;) {
    if (*p == '\n' || *p == 0) lex_error(start, "string sem fim");
    if (*p == '"') break;
    if (*p == '\\') {
      p++;
      buf_byte(&b, read_escape(&p));
    } else {
      buf_byte(&b, *p++);
    }
  }
  buf_byte(&b, 0);
  t = new_token(TK_STR, start, p + 1);
  t->str = b.p;
  t->slen = b.len;
  return t;
}

static Token* read_char(char* start) {
  char* p = start + 1;
  int c;
  Token* t;
  if (*p == '\n' || *p == 0) lex_error(start, "caractere sem fim");
  if (*p == '\\') {
    p++;
    c = read_escape(&p);
  } else {
    c = (unsigned char)*p++;
  }
  while (*p && *p != '\'' && *p != '\n') p++; /* 'ab': usa so o primeiro */
  if (*p != '\'') lex_error(start, "caractere sem fim");
  t = new_token(TK_CHAR, start, p + 1);
  t->ival = (signed char)c;
  t->ty = ty_int;
  return t;
}

/* Tira "\\\n" e '\r', mantendo a contagem de linhas. */
static void clean_text(char* s) {
  int i = 0, j = 0, pending = 0;
  while (s[i]) {
    if (s[i] == '\r') {
      i++;
    } else if (s[i] == '\\' && s[i + 1] == '\n') {
      i += 2;
      pending++;
    } else if (s[i] == '\\' && s[i + 1] == '\r' && s[i + 2] == '\n') {
      i += 3;
      pending++;
    } else if (s[i] == '\n') {
      s[j++] = s[i++];
      for (; pending > 0; pending--) s[j++] = '\n';
    } else {
      s[j++] = s[i++];
    }
  }
  for (; pending > 0; pending--) s[j++] = '\n';
  s[j] = 0;
}

static Token* lex(char* p) {
  Token head;
  Token* cur = &head;
  head.next = NULL;
  at_bol = 1;
  has_space = 0;
  while (*p) {
    int code, n;
    if (p[0] == '/' && p[1] == '/') {
      p += 2;
      while (*p && *p != '\n') p++;
      has_space = 1;
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      char* q = p;
      p += 2;
      while (*p && !(p[0] == '*' && p[1] == '/')) {
        if (*p == '\n') cur_line++;
        p++;
      }
      if (!*p) lex_error(q, "comentario sem fim");
      p += 2;
      has_space = 1;
      continue;
    }
    if (*p == '\n') {
      p++;
      cur_line++;
      at_bol = 1;
      has_space = 0;
      continue;
    }
    if (isspace((unsigned char)*p)) {
      p++;
      has_space = 1;
      continue;
    }
    if (isdigit((unsigned char)*p) || (p[0] == '.' && isdigit((unsigned char)p[1]))) {
      char* q = p++;
      for (;;) {
        if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1])) p += 2;
        else if (is_ident2((unsigned char)*p) || *p == '.') p++;
        else break;
      }
      cur = cur->next = new_token(TK_NUM, q, p);
      continue;
    }
    if (*p == '"') {
      cur = cur->next = read_string(p);
      p += cur->len;
      continue;
    }
    if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == '"') {
      cur = cur->next = read_string(p + 1);
      p += cur->len + 1;
      continue;
    }
    if (*p == '\'') {
      cur = cur->next = read_char(p);
      p += cur->len;
      continue;
    }
    if (is_ident1((unsigned char)*p)) {
      char* q = p++;
      while (is_ident2((unsigned char)*p)) p++;
      cur = cur->next = new_token(TK_IDENT, q, p);
      cur->name = intern(q, (int)(p - q));
      continue;
    }
    n = read_punct(p, &code);
    if (n) {
      cur = cur->next = new_token(TK_PUNCT, p, p + n);
      cur->punct = code;
      p += n;
      continue;
    }
    lex_error(p, "caractere invalido");
  }
  cur = cur->next = new_token(TK_EOF, p, p);
  return head.next;
}

Token* tokenize(SrcFile* f) {
  cur_file = f;
  cur_line = 1;
  clean_text(f->text);
  f->len = (int)strlen(f->text);
  return lex(f->text);
}

Token* tokenize_string(char* text, SrcFile* f, int line) {
  SrcFile* save = cur_file;
  int save_line = cur_line;
  Token* t;
  cur_file = f;
  cur_line = line;
  t = lex(text);
  cur_file = save;
  cur_line = save_line;
  return t;
}

/* pp-number -> valor e tipo */
void convert_number(Token* t) {
  char* p = t->loc;
  char* end = t->loc + t->len;
  unsigned long long v = 0;
  int base = 10;
  int u = 0, l = 0;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && end - p > 2) {
    p += 2;
    base = 16;
  } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && end - p > 2) {
    p += 2;
    base = 2;
  } else if (p[0] == '0') {
    base = 8;
  }
  for (; p < end; p++) {
    int d;
    if (isdigit((unsigned char)*p)) d = *p - '0';
    else if (base == 16 && isxdigit((unsigned char)*p)) d = from_hex(*p);
    else break;
    if (d >= base) error_tok(t, "digito invalido no numero");
    v = v * (unsigned)base + (unsigned)d;
  }
  for (; p < end; p++) {
    if (*p == 'u' || *p == 'U') u = 1;
    else if (*p == 'l' || *p == 'L') l++;
    else if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'f' || *p == 'F')
      error_tok(t, "ponto flutuante nao e suportado");
    else error_tok(t, "sufixo invalido no numero");
  }
  t->kind = TK_NUM;
  t->ival = (long long)v;
  if (l >= 2) {
    t->ty = u ? ty_ullong : ty_llong;
  } else if (base == 10 && !u) {
    t->ty = v > 2147483647ULL ? ty_llong : (l ? ty_long : ty_int);
  } else {
    if (v > 4294967295ULL) t->ty = u ? ty_ullong : ty_llong;
    else if (v > 2147483647ULL || u) t->ty = l ? ty_ulong : ty_uint;
    else t->ty = l ? ty_long : ty_int;
  }
}
