/* util.c - memoria, strings internadas, erros e leitura de arquivo. */
#include "cc.h"

int opt_warn = 1;

void* xalloc(int n) {
  void* p = calloc(1, (size_t)n);
  if (!p) {
    fprintf(stderr, "cc: sem memoria\n");
    exit(1);
  }
  return p;
}

char* xstrndup(const char* s, int n) {
  char* p = (char*)xalloc(n + 1);
  memcpy(p, s, (size_t)n);
  p[n] = 0;
  return p;
}

char* xstrdup(const char* s) { return xstrndup(s, (int)strlen(s)); }

int align_to(int n, int a) { return (n + a - 1) / a * a; }

/* ---------- strings internadas: mesmo texto, mesmo ponteiro ---------- */
typedef struct Str Str;
struct Str {
  Str* next;
  int len;
  char* s;
};

#define NHASH 4096
static Str* strs[NHASH];

char* intern(const char* s, int n) {
  unsigned h = 2166136261u;
  int i;
  Str* e;
  for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  h &= NHASH - 1;
  for (e = strs[h]; e; e = e->next)
    if (e->len == n && memcmp(e->s, s, (size_t)n) == 0) return e->s;
  e = (Str*)xalloc((int)sizeof(Str));
  e->len = n;
  e->s = xstrndup(s, n);
  e->next = strs[h];
  strs[h] = e;
  return e->s;
}

char* format(const char* fmt, ...) {
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  return xstrdup(tmp);
}

void error(const char* fmt, ...) {
  va_list ap;
  fprintf(stderr, "cc: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

static void show_line(Token* t, const char* kind, const char* fmt, va_list ap) {
  char* p;
  char* line_start;
  char* line_end;
  int col;
  Token* where = t;
  while (where && where->origin) where = where->origin;
  if (!where || !where->file) {
    fprintf(stderr, "%s: ", kind);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return;
  }
  fprintf(stderr, "%s:%d: %s: ", where->file->name, where->line, kind);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  p = where->loc;
  line_start = p;
  if (p >= where->file->text && p <= where->file->text + where->file->len) {
    while (line_start > where->file->text && line_start[-1] != '\n') line_start--;
    line_end = p;
    while (*line_end && *line_end != '\n') line_end++;
    col = (int)(p - line_start);
    fprintf(stderr, "  %.*s\n  %*s^\n", (int)(line_end - line_start), line_start, col, "");
  }
}

void error_tok(Token* t, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  show_line(t, "erro", fmt, ap);
  va_end(ap);
  exit(1);
}

void warn_tok(Token* t, const char* fmt, ...) {
  va_list ap;
  if (!opt_warn) return;
  va_start(ap, fmt);
  show_line(t, "aviso", fmt, ap);
  va_end(ap);
}

char* read_file(const char* path, int* len) {
  FILE* f = fopen(path, "rb");
  Buf b;
  char tmp[4096];
  int n;
  if (!f) return NULL;
  memset(&b, 0, sizeof(b));
  while ((n = (int)fread(tmp, 1, sizeof(tmp), f)) > 0) buf_add(&b, tmp, n);
  fclose(f);
  /* garante '\n' final e NUL */
  if (b.len == 0 || b.p[b.len - 1] != '\n') buf_byte(&b, '\n');
  buf_byte(&b, 0);
  *len = b.len - 1;
  return b.p;
}

void buf_add(Buf* b, const void* p, int n) {
  if (b->len + n > b->cap) {
    int nc = b->cap ? b->cap * 2 : 256;
    char* np;
    while (nc < b->len + n) nc *= 2;
    np = (char*)realloc(b->p, (size_t)nc);
    if (!np) error("sem memoria");
    b->p = np;
    b->cap = nc;
  }
  memcpy(b->p + b->len, p, (size_t)n);
  b->len += n;
}

void buf_byte(Buf* b, int c) {
  char ch = (char)c;
  buf_add(b, &ch, 1);
}

void buf_u32(Buf* b, unsigned v) {
  unsigned char t[4];
  t[0] = (unsigned char)v;
  t[1] = (unsigned char)(v >> 8);
  t[2] = (unsigned char)(v >> 16);
  t[3] = (unsigned char)(v >> 24);
  buf_add(b, t, 4);
}
