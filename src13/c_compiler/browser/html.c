/* html.c - tokenizador HTML tolerante e layout em fluxo numa grade de
 * caracteres (a fonte e monoespacada): paragrafos, titulos, listas, links,
 * pre, quebra de linha. CSS: elemento, .classe, #id e style="". */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "browser.h"

#define COL_TEXT 0x000000
#define COL_LINK 0x0000CC
#define COL_HEAD 0x000060
#define COL_CODE 0x700000
#define COL_EM   0x304060

#define CSS_RULES 256
#define CSS_DEPTH 24
#define RAW_MAX (64 * 1024) /* corpo de <style>, <script> e <textarea> */

typedef struct {
  char sel[40];
  unsigned color, bg;
  int color_set, bg_set, bold, uline, hide;
  int width, height;
} css_rule_t;

typedef struct {
  css_rule_t rules[CSS_RULES];
  int n;
} css_t;

typedef struct {
  char tag[16];
  int el;
  unsigned color;
  int color_set, bold, uline, hide;
  unsigned bg;
  int bg_set, width, height;
} frm_t;

typedef struct lay lay_t;
struct lay {
  doc_t* d;
  int cols;
  int line, col, indent;
  int line_empty, blank, space_pending;
  int bold, pre, heading, link, field, form, uline, em, code;
  int skip, in_title;
  int list_depth;
  int list_ol[16];
  int list_n[16];
  int dl_depth;
  int dl_base[16];
  int in_cell, cell_runs;
  int band_top, band_line, band_runs; /* figuras lado a lado: topo, linha e runs */
  int cp437, latin1;
  const char* base;
  char word[256];
  int wlen;
  css_t* css;
  frm_t frame[CSS_DEPTH];
  int nframe;
  int hide;
  unsigned css_color, css_bg;
  int css_color_set, css_bg_set, css_bold, css_uline;
};

/* ---------- caracteres ---------- */
static const unsigned char cp437_hi[96] = {
  ' ', 0xAD, 0x9B, 0x9C, '?', 0x9D, '|', 0x15, '"', 'c', 0xA6, 0xAE, 0xAA, 0, 'R', '-',
  0xF8, 0xF1, 0xFD, '3', '\'', 0xE6, 0x14, 0xFA, ',', '1', 0xA7, 0xAF, 0xAC, 0xAB, '?', 0xA8,
  'A', 'A', 'A', 'A', 0x8E, 0x8F, 0x92, 0x80, 'E', 0x90, 'E', 'E', 'I', 'I', 'I', 'I',
  'D', 0xA5, 'O', 'O', 'O', 'O', 0x99, 'x', 'O', 'U', 'U', 'U', 0x9A, 'Y', 'P', 0xE1,
  0x85, 0xA0, 0x83, 'a', 0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
  'd', 0xA4, 0x95, 0xA2, 0x93, 'o', 0x94, 0xF6, 'o', 0x97, 0xA3, 0x96, 0x81, 'y', 'p', 0x98
};
static const char ascii_hi[] =
  " !cL?Y|S\"ca<\0R-" "o+23'uP.,1o>????" "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYPs"
  "aaaaaaaceeeeiiii" "dnooooo/ouuuuypy";

/* code point -> ate 3 bytes da fonte; devolve quantos */
static int map_cp(unsigned cp, char* out, int cp437) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp >= 0xA0 && cp <= 0xFF) {
    unsigned char c = cp437 ? cp437_hi[cp - 0xA0] : (unsigned char)ascii_hi[cp - 0xA0];
    if (!c) return 0;
    out[0] = (char)c;
    return 1;
  }
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: out[0] = '\''; return 1;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: out[0] = '"'; return 1;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
      out[0] = '-';
      return 1;
    case 0x2026: out[0] = '.'; out[1] = '.'; out[2] = '.'; return 3;
    case 0x2022: case 0x25CF: case 0x2023: out[0] = cp437 ? (char)7 : '*'; return 1;
    case 0x20AC: out[0] = 'E'; return 1;
    case 0x2122: out[0] = 'T'; out[1] = 'M'; return 2;
    case 0x2039: out[0] = '<'; return 1;
    case 0x203A: out[0] = '>'; return 1;
    case 0x2190: out[0] = cp437 ? (char)0x1B : '<'; return 1;
    case 0x2192: out[0] = cp437 ? (char)0x1A : '>'; return 1;
    case 0x2191: out[0] = cp437 ? (char)0x18 : '^'; return 1;
    case 0x2193: out[0] = cp437 ? (char)0x19 : 'v'; return 1;
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0xFEFF: return 0;
  }
  if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) {
    out[0] = ' ';
    return 1;
  }
  out[0] = '?';
  return 1;
}

/* ---------- documento ---------- */
static void* grow(void* p, int* cap, int need, int elem) {
  if (need <= *cap) return p;
  {
    int nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    p = realloc(p, (size_t)nc * (size_t)elem);
    *cap = nc;
    return p;
  }
}

static void emit(lay_t* L, const char* s, int n, int extra_flags) {
  doc_t* d = L->d;
  run_t* r;
  int flags = extra_flags;
  unsigned color = COL_TEXT;
  unsigned bg = 0;
  if (n <= 0 || L->hide) return;
  if (L->bold || L->heading) flags |= RUN_BOLD;
  if (L->link >= 0 || L->uline) flags |= RUN_ULINE;
  if (L->css_bold > 0) flags |= RUN_BOLD;
  if (L->css_bold < 0) flags &= ~RUN_BOLD;
  if (L->css_uline > 0) flags |= RUN_ULINE;
  if (L->css_uline < 0) flags &= ~RUN_ULINE;
  if (L->link >= 0) color = COL_LINK;
  else if (L->heading) color = COL_HEAD;
  else if (L->code) color = COL_CODE;
  else if (L->em) color = COL_EM;
  if (L->css_color_set) color = L->css_color;
  if (L->css_bg_set) {
    flags |= RUN_BG;
    bg = L->css_bg;
  }
  d->text = (char*)grow(d->text, &d->text_cap, d->text_len + n + 1, 1);
  if (!d->text) return;
  memcpy(d->text + d->text_len, s, (size_t)n);
  r = d->nruns ? &d->runs[d->nruns - 1] : 0;
  if (r && !(flags & RUN_IMG) && r->img < 0 && r->line == L->line &&
      r->col + r->len == L->col && r->flags == flags && r->bg == bg &&
      r->color == color && r->link == L->link && r->field == L->field &&
      r->off + r->len == d->text_len) {
    r->len += n;
  } else {
    d->runs = (run_t*)grow(d->runs, &d->runs_cap, d->nruns + 1, (int)sizeof(run_t));
    if (!d->runs) return;
    r = &d->runs[d->nruns++];
    r->line = L->line;
    r->col = L->col;
    r->len = n;
    r->off = d->text_len;
    r->flags = flags;
    r->color = color;
    r->link = L->link;
    r->field = L->field;
    r->img = -1;
    r->img_row = 0;
    r->bg = bg;
  }
  d->text_len += n;
  L->col += n;
  L->line_empty = 0;
  L->blank = 0;
}

static void flush_line(lay_t* L) {
  if (!L->line_empty) {
    L->line++;
    L->line_empty = 1;
    L->blank = 0;
  }
  L->col = L->indent;
  L->space_pending = 0;
}

static void margin(lay_t* L, int n) {
  flush_line(L);
  if (L->d->nruns == 0) return;
  while (L->blank < n) {
    L->line++;
    L->blank++;
  }
}

static void flush_word(lay_t* L) {
  int n = L->wlen;
  const char* w = L->word;
  int sp;
  if (n == 0) return;
  sp = L->space_pending && !L->line_empty;
  if (!L->line_empty && L->col + sp + n > L->cols) {
    flush_line(L);
    sp = 0;
  }
  if (sp) {
    int keep = L->link;
    /* o espaco entre duas palavras nao herda o sublinhado do link anterior */
    if (L->d->nruns && L->d->runs[L->d->nruns - 1].link != L->link) L->link = -1;
    emit(L, " ", 1, 0);
    L->link = keep;
  }
  L->space_pending = 0;
  while (n > 0) {
    int room = L->cols - L->col;
    int k;
    if (room <= 0) {
      flush_line(L);
      room = L->cols - L->col;
      if (room <= 0) room = 1;
    }
    k = n < room ? n : room;
    emit(L, w, k, 0);
    w += k;
    n -= k;
  }
  L->wlen = 0;
}

static void word_char(lay_t* L, char c) {
  if (L->wlen >= (int)sizeof(L->word)) flush_word(L);
  L->word[L->wlen++] = c;
}

static void pre_char(lay_t* L, char c) {
  if (c == '\n') {
    L->line++;
    L->line_empty = 1;
    L->blank = 0;
    L->col = L->indent;
    return;
  }
  if (c == '\t') {
    do {
      emit(L, " ", 1, 0);
    } while ((L->col - L->indent) % 8 != 0 && L->col < L->cols);
    return;
  }
  if (L->col >= L->cols) {
    L->line++;
    L->col = L->indent;
  }
  emit(L, &c, 1, 0);
}

static void text_cp(lay_t* L, unsigned cp) {
  char out[4];
  int n, i;
  if (L->skip || (L->hide && !L->in_title)) return;
  if (cp == '\r') return;
  if (L->in_title) {
    int tl = (int)strlen(L->d->title);
    if (cp == '\n' || cp == '\t') cp = ' ';
    if (cp == ' ' && (tl == 0 || L->d->title[tl - 1] == ' ')) return;
    n = map_cp(cp, out, L->cp437);
    for (i = 0; i < n && tl < (int)sizeof(L->d->title) - 1; i++) L->d->title[tl++] = out[i];
    L->d->title[tl] = 0;
    return;
  }
  if (L->pre) {
    if (cp == '\n' || cp == '\t') {
      pre_char(L, (char)cp);
      return;
    }
    n = map_cp(cp, out, L->cp437);
    for (i = 0; i < n; i++) pre_char(L, out[i]);
    return;
  }
  if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\f') {
    flush_word(L);
    L->space_pending = 1;
    return;
  }
  n = map_cp(cp, out, L->cp437);
  for (i = 0; i < n; i++) word_char(L, out[i]);
}

/* ---------- entidades ---------- */
typedef struct {
  const char* name;
  unsigned cp;
} entity_t;

static const entity_t entities[] = {
  { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
  { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
  { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "lsquo", 0x2018 },
  { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 },
  { "middot", 0xB7 }, { "deg", 0xB0 }, { "times", 0xD7 }, { "divide", 0xF7 },
  { "eacute", 0xE9 }, { "aacute", 0xE1 }, { "iacute", 0xED }, { "oacute", 0xF3 },
  { "uacute", 0xFA }, { "atilde", 0xE3 }, { "otilde", 0xF5 }, { "ccedil", 0xE7 },
  { "ecirc", 0xEA }, { "acirc", 0xE2 }, { "ocirc", 0xF4 }, { "agrave", 0xE0 },
  { "Eacute", 0xC9 }, { "Aacute", 0xC1 }, { "Ccedil", 0xC7 }, { "uuml", 0xFC },
  { "ntilde", 0xF1 }, { "euro", 0x20AC }, { "trade", 0x2122 }, { "larr", 0x2190 },
  { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "sect", 0xA7 },
  { "para", 0xB6 }, { "shy", 0xAD }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "ouml", 0xF6 },
  { "auml", 0xE4 }, { "szlig", 0xDF }, { 0, 0 }
};

/* decodifica &...; em s; devolve bytes consumidos (0 = nao era entidade) */
static int entity(const char* s, int n, unsigned* cp) {
  int i = 1;
  if (n < 3) return 0;
  if (s[1] == '#') {
    unsigned v = 0;
    int hex = (s[2] == 'x' || s[2] == 'X');
    i = hex ? 3 : 2;
    while (i < n && i < 12) {
      int c = (unsigned char)s[i];
      if (hex && isxdigit(c)) v = v * 16 + (unsigned)(isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
      else if (!hex && isdigit(c)) v = v * 10 + (unsigned)(c - '0');
      else break;
      i++;
    }
    if (i == (hex ? 3 : 2)) return 0;
    if (i < n && s[i] == ';') i++;
    *cp = v ? v : '?';
    return i;
  }
  while (i < n && i < 10 && isalnum((unsigned char)s[i])) i++;
  {
    int k;
    for (k = 0; entities[k].name; k++) {
      int len = (int)strlen(entities[k].name);
      if (len == i - 1 && strncmp(s + 1, entities[k].name, (size_t)len) == 0) {
        *cp = entities[k].cp;
        if (i < n && s[i] == ';') i++;
        return i;
      }
    }
  }
  return 0;
}

/* ---------- tags ---------- */
typedef struct {
  char name[16];
  int end, self_close;
  char href[URL_MAX];
  char src[URL_MAX];
  char alt[128];
  char type[16];
  char value[128];
  char fname[32];
  char action[URL_MAX];
  int isize;
  char id[32];
  char cls[64];
  char style[180];
  int width, height;
} tag_t;

static int is(const char* a, const char* b) { return strcmp(a, b) == 0; }

static int in_list(const char* name, const char* list) {
  int n = (int)strlen(name);
  const char* p = list;
  while (*p) {
    const char* e = strchr(p, ' ');
    int len = e ? (int)(e - p) : (int)strlen(p);
    if (len == n && strncmp(p, name, (size_t)n) == 0) return 1;
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

static const char* skip_tags = "select template iframe object video audio canvas math svg map";

static int add_link(lay_t* L, const char* href) {
  char abs[URL_MAX];
  doc_t* d = L->d;
  int n;
  if (url_resolve(L->base, href, abs, URL_MAX) != 0) return -1;
  d->links = (char**)grow(d->links, &d->links_cap, d->nlinks + 1, (int)sizeof(char*));
  if (!d->links) return -1;
  n = (int)strlen(abs);
  d->links[d->nlinks] = (char*)malloc((size_t)n + 1);
  if (!d->links[d->nlinks]) return -1;
  memcpy(d->links[d->nlinks], abs, (size_t)n + 1);
  return d->nlinks++;
}

static void inline_word(lay_t* L, const char* s) {
  int i;
  flush_word(L);
  for (i = 0; s[i]; i++) word_char(L, s[i]);
  flush_word(L);
}

static void alt_word(lay_t* L, const char* alt) {
  char b[140];
  int k;
  if (!alt || !alt[0]) {
    inline_word(L, "[imagem]");
    return;
  }
  k = (int)strlen(alt);
  if (k > 120) k = 120;
  b[0] = '[';
  memcpy(b + 1, alt, (size_t)k);
  b[k + 1] = ']';
  b[k + 2] = 0;
  inline_word(L, b);
}

/* Novo run na linha 'line', mantendo os runs em ordem de linha: a figura
 * posta ao lado de outra comeca numa linha que ja tem runs depois dela. */
static run_t* img_run(doc_t* d, int line) {
  int pos;
  d->runs = (run_t*)grow(d->runs, &d->runs_cap, d->nruns + 1, (int)sizeof(run_t));
  if (!d->runs) return 0;
  pos = d->nruns;
  while (pos > 0 && d->runs[pos - 1].line > line) pos--;
  memmove(d->runs + pos + 1, d->runs + pos, (size_t)(d->nruns - pos) * sizeof(run_t));
  d->nruns++;
  return &d->runs[pos];
}

/* Reserva uma caixa na grade; main.c decodifica e desenha com gui_image.
 * Figuras seguidas, sem texto entre elas, ficam lado a lado pelo topo. */
static void place_image(lay_t* L, tag_t* t) {
  doc_t* d = L->d;
  doc_img_t* im;
  char abs[URL_MAX];
  int idx, cols, lines, row, start, top, gap, beside;
  if (L->hide || !t->src[0] || d->nimgs >= DOC_IMGS || !L->cp437) {
    alt_word(L, t->alt);
    return;
  }
  if (strncmp(t->src, "data:", 5) == 0) {
    if ((int)strlen(t->src) >= URL_MAX) {
      alt_word(L, t->alt);
      return;
    }
    strncpy(abs, t->src, URL_MAX - 1);
    abs[URL_MAX - 1] = 0;
  } else if (url_resolve(L->base ? L->base : "", t->src, abs, URL_MAX) != 0) {
    alt_word(L, t->alt);
    return;
  }
  idx = d->nimgs++;
  im = &d->imgs[idx];
  memset(im, 0, sizeof(*im));
  strncpy(im->url, abs, URL_MAX - 1);
  strncpy(im->alt, t->alt, sizeof(im->alt) - 1);
  cols = t->width > 0 ? (t->width + 7) / 8 : 16;
  lines = t->height > 0 ? (t->height + 15) / 16 : 8;
  if (cols < 1) cols = 1;
  if (cols > L->cols) cols = L->cols;
  if (lines < 1) lines = 1;
  if (lines > 30) lines = 30;
  im->cols = cols;
  im->lines = lines;
  beside = d->nruns > 0 && d->nruns == L->band_runs && L->line == L->band_line && !L->line_empty;
  gap = (L->space_pending && !L->line_empty) ? 1 : 0;
  L->space_pending = 0;
  if (L->col > L->indent && L->col + gap + cols > L->cols) {
    flush_line(L);
    beside = 0;
    gap = 0;
  }
  start = L->col + gap;
  top = beside ? L->band_top : L->line;
  for (row = 0; row < lines; row++) {
    run_t* r = img_run(d, top + row);
    if (!r) return;
    r->line = top + row;
    r->col = start;
    r->len = cols;
    r->off = d->text_len;
    r->flags = RUN_IMG;
    r->bg = 0;
    r->color = COL_TEXT;
    r->link = L->link;
    r->field = -1;
    r->img = idx;
    r->img_row = row;
  }
  if (top + lines - 1 > L->line) L->line = top + lines - 1;
  L->col = start + cols;
  L->line_empty = 0;
  L->blank = 0;
  L->band_top = top;
  L->band_line = L->line;
  L->band_runs = d->nruns;
}

static void copy_z(char* dst, int max, const char* s) {
  int i = 0;
  if (!s) s = "";
  while (s[i] && i < max - 1) {
    dst[i] = s[i];
    i++;
  }
  dst[i] = 0;
}

/* Guarda o campo e, se for visivel, desenha [____] ou [Enviar]. */
static void place_field(lay_t* L, tag_t* t, int submit, int hidden) {
  doc_t* d = L->d;
  doc_field_t* f;
  int slot, box;
  if (hidden && d->nfields >= DOC_FIELDS - 8) return;
  if (d->nfields >= DOC_FIELDS) return;
  slot = d->nfields++;
  f = &d->fields[slot];
  memset(f, 0, sizeof(*f));
  f->submit = submit;
  f->hidden = hidden;
  f->form = L->form;
  f->line = -1;
  copy_z(f->name, (int)sizeof(f->name), t->fname);
  copy_z(f->value, (int)sizeof(f->value), t->value);
  if (hidden) return;
  if (submit) {
    char b[140];
    int before = d->nruns;
    copy_z(b, (int)sizeof(b), "[");
    {
      int n = (int)strlen(b);
      const char* v = t->value[0] ? t->value : "Enviar";
      while (*v && n < (int)sizeof(b) - 2) b[n++] = *v++;
      b[n++] = ']';
      b[n] = 0;
    }
    L->field = slot;
    inline_word(L, b);
    L->field = -1;
    if (d->nruns > before) {
      f->line = d->runs[before].line;
      f->col = d->runs[before].col;
      f->len = d->runs[before].len;
    }
    return;
  }
  box = t->isize > 0 ? t->isize : 20;
  if (box > 40) box = 40;
  if (box > L->cols - L->indent - 2) box = L->cols - L->indent - 2;
  if (box < 4) box = 4;
  if (L->col > L->indent && L->col + box + 2 > L->cols) flush_line(L);
  f->box = box;
  f->line = L->line;
  f->col = L->col;
  f->len = box + 2;
  {
    char b[48];
    int i;
    b[0] = '[';
    for (i = 0; i < box; i++) b[1 + i] = '_';
    b[box + 1] = ']';
    b[box + 2] = 0;
    L->field = slot;
    inline_word(L, b);
    L->field = -1;
  }
}

static void handle_tag(lay_t* L, tag_t* t) {
  const char* n = t->name;
  if (L->skip) {
    if (in_list(n, skip_tags) && !t->self_close) L->skip += t->end ? -1 : 1;
    return;
  }
  if (in_list(n, skip_tags)) {
    if (!t->end && !t->self_close) {
      flush_word(L);
      L->skip = 1;
    }
    return;
  }
  flush_word(L);
  if (is(n, "title")) {
    L->in_title = !t->end;
    return;
  }
  if (is(n, "br")) {
    if (L->line_empty) {
      L->line++;
      L->blank++;
    } else {
      flush_line(L);
    }
    L->col = L->indent;
    return;
  }
  if (is(n, "p")) {
    margin(L, 1);
    return;
  }
  if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]) {
    margin(L, 1);
    L->heading = t->end ? 0 : n[1] - '0';
    return;
  }
  if (is(n, "pre") || is(n, "listing") || is(n, "xmp")) {
    margin(L, 1);
    L->pre += t->end ? -1 : 1;
    if (L->pre < 0) L->pre = 0;
    return;
  }
  if (is(n, "blockquote")) {
    margin(L, 1);
    L->indent += t->end ? -4 : 4;
    if (L->indent < 0) L->indent = 0;
    L->col = L->indent;
    return;
  }
  if (is(n, "ul") || is(n, "ol") || is(n, "menu") || is(n, "dir")) {
    if (!t->end) {
      margin(L, L->list_depth == 0 ? 1 : 0);
      if (L->list_depth < 15) {
        L->list_depth++;
        L->list_ol[L->list_depth] = is(n, "ol");
        L->list_n[L->list_depth] = 0;
      }
      L->indent += 4;
    } else {
      if (L->list_depth > 0) L->list_depth--;
      L->indent -= 4;
      if (L->indent < 0) L->indent = 0;
      margin(L, L->list_depth == 0 ? 1 : 0);
    }
    L->col = L->indent;
    return;
  }
  if (is(n, "li")) {
    flush_line(L);
    if (!t->end) {
      char mark[8];
      int k = L->list_depth;
      int save = L->link;
      if (k > 0 && L->list_ol[k]) {
        int v = ++L->list_n[k];
        int m = 0;
        if (v >= 100) mark[m++] = (char)('0' + v / 100 % 10);
        if (v >= 10) mark[m++] = (char)('0' + v / 10 % 10);
        mark[m++] = (char)('0' + v % 10);
        mark[m++] = '.';
        mark[m] = 0;
      } else {
        mark[0] = L->cp437 ? (char)7 : '*';
        mark[1] = 0;
      }
      L->col = L->indent - (int)strlen(mark) - 1;
      if (L->col < 0) L->col = 0;
      L->link = -1;
      emit(L, mark, (int)strlen(mark), 0);
      L->link = save;
      L->col = L->indent;
      L->in_cell = 1;
      L->cell_runs = L->d->nruns;
    }
    return;
  }
  if (is(n, "dl")) {
    flush_line(L);
    if (!t->end && L->dl_depth < 15) L->dl_base[++L->dl_depth] = L->indent;
    else if (t->end && L->dl_depth > 0) L->indent = L->dl_base[L->dl_depth--];
    L->col = L->indent;
    return;
  }
  /* dt/dd quase nunca sao fechados: o recuo vem do <dl>, nao da pilha */
  if (is(n, "dd") || is(n, "dt")) {
    int base = L->dl_depth > 0 ? L->dl_base[L->dl_depth] : 0;
    flush_line(L);
    if (!t->end) L->indent = base + (is(n, "dd") ? 4 : 0);
    L->col = L->indent;
    return;
  }
  if (is(n, "hr")) {
    flush_line(L);
    {
      int k = L->cols - L->col;
      char sp[256];
      int i;
      if (k > (int)sizeof(sp)) k = (int)sizeof(sp);
      for (i = 0; i < k; i++) sp[i] = ' ';
      emit(L, sp, k, RUN_HR);
    }
    flush_line(L);
    return;
  }
  if (is(n, "tr")) {
    flush_line(L);
    L->in_cell = 0;
    return;
  }
  if (is(n, "td") || is(n, "th")) {
    if (!t->end && !L->line_empty) {
      emit(L, "  ", 2, 0);
    }
    if (!t->end) {
      L->in_cell = 1;
      L->cell_runs = L->d->nruns;
    }
    if (is(n, "th")) L->bold += t->end ? -1 : 1;
    return;
  }
  if (is(n, "table")) {
    flush_line(L);
    L->in_cell = 0;
    return;
  }
  if (is(n, "form")) {
    if (t->end) {
      L->form = -1;
    } else if (L->d->nforms < DOC_FORMS) {
      int slot = L->d->nforms;
      char* dst = L->d->form_action[slot];
      if (!t->action[0] || url_resolve(L->base ? L->base : "", t->action, dst, URL_MAX) != 0)
        copy_z(dst, URL_MAX, L->base ? L->base : "");
      L->form = slot;
      L->d->nforms++;
    }
    if (!(L->in_cell && L->d->nruns == L->cell_runs)) flush_line(L);
    return;
  }
  if (in_list(n, "div section article header footer nav main aside fieldset figure "
                 "figcaption center address details summary caption tbody thead "
                 "tfoot legend noscript body html")) {
    /* dentro de uma celula ainda vazia, um bloco nao quebra a linha */
    if (L->in_cell && L->d->nruns == L->cell_runs) return;
    flush_line(L);
    return;
  }
  if (is(n, "a")) {
    if (t->end) L->link = -1;
    else if (t->href[0]) L->link = add_link(L, t->href);
    return;
  }
  if (is(n, "b") || is(n, "strong")) {
    L->bold += t->end ? -1 : 1;
    if (L->bold < 0) L->bold = 0;
    return;
  }
  if (is(n, "i") || is(n, "em") || is(n, "cite") || is(n, "var") || is(n, "dfn")) {
    L->em += t->end ? -1 : 1;
    if (L->em < 0) L->em = 0;
    return;
  }
  if (is(n, "u") || is(n, "ins")) {
    L->uline += t->end ? -1 : 1;
    if (L->uline < 0) L->uline = 0;
    return;
  }
  if (is(n, "code") || is(n, "tt") || is(n, "kbd") || is(n, "samp")) {
    L->code += t->end ? -1 : 1;
    if (L->code < 0) L->code = 0;
    return;
  }
  if (is(n, "img") && !t->end) {
    place_image(L, t);
    return;
  }
  if (is(n, "input") && !t->end) {
    int submit = is(t->type, "submit") || is(t->type, "button") || is(t->type, "image");
    int hidden = is(t->type, "hidden");
    if (is(t->type, "checkbox") || is(t->type, "radio") || is(t->type, "file")) return;
    place_field(L, t, submit, hidden);
    return;
  }
}

static void attr_copy(char* dst, int max, const char* v, int n, lay_t* L) {
  int i = 0, o = 0;
  while (i < n && o < max - 4) {
    if (v[i] == '&') {
      unsigned cp;
      int k = entity(v + i, n - i, &cp);
      if (k > 0) {
        if (cp < 0x80) dst[o++] = (char)cp;
        else o += map_cp(cp, dst + o, L->cp437);
        i += k;
        continue;
      }
    }
    dst[o++] = v[i++];
  }
  dst[o] = 0;
}

static int attr_num(const char* v, int n) {
  int i = 0, x = 0;
  while (i < n && (v[i] < '0' || v[i] > '9')) i++;
  while (i < n && v[i] >= '0' && v[i] <= '9') {
    x = x * 10 + (v[i] - '0');
    if (x > 4000) return 4000;
    i++;
  }
  return x;
}

/* le uma tag a partir de s[0] == '<'; devolve bytes consumidos */
static int parse_tag(const char* s, int n, tag_t* t, lay_t* L) {
  int i = 1, k = 0;
  memset(t, 0, sizeof(*t));
  if (s[i] == '/') {
    t->end = 1;
    i++;
  }
  while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == ':')) {
    if (k < (int)sizeof(t->name) - 1) t->name[k++] = (char)tolower((unsigned char)s[i]);
    i++;
  }
  t->name[k] = 0;
  while (i < n && s[i] != '>') {
    char an[16];
    int al = 0;
    const char* v = 0;
    int vl = 0;
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') {
      t->self_close = 1;
      i++;
      continue;
    }
    if (isspace((unsigned char)s[i]) || s[i] == '/') {
      i++;
      continue;
    }
    while (i < n && !isspace((unsigned char)s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') {
      if (al < (int)sizeof(an) - 1) an[al++] = (char)tolower((unsigned char)s[i]);
      i++;
    }
    an[al] = 0;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i < n && s[i] == '=') {
      i++;
      while (i < n && isspace((unsigned char)s[i])) i++;
      if (i < n && (s[i] == '"' || s[i] == '\'')) {
        char q = s[i++];
        v = s + i;
        while (i < n && s[i] != q) i++;
        vl = (int)(s + i - v);
        if (i < n) i++;
      } else {
        v = s + i;
        while (i < n && !isspace((unsigned char)s[i]) && s[i] != '>') i++;
        vl = (int)(s + i - v);
      }
    }
    if (!v) continue;
    if (is(an, "href")) attr_copy(t->href, (int)sizeof(t->href), v, vl, L);
    else if (is(an, "src")) attr_copy(t->src, (int)sizeof(t->src), v, vl, L);
    else if (is(an, "alt")) attr_copy(t->alt, (int)sizeof(t->alt), v, vl, L);
    else if (is(an, "width")) t->width = attr_num(v, vl);
    else if (is(an, "height")) t->height = attr_num(v, vl);
    else if (is(an, "name")) attr_copy(t->fname, (int)sizeof(t->fname), v, vl, L);
    else if (is(an, "action")) attr_copy(t->action, (int)sizeof(t->action), v, vl, L);
    else if (is(an, "size")) t->isize = attr_num(v, vl);
    else if (is(an, "type")) {
      attr_copy(t->type, (int)sizeof(t->type), v, vl, L);
      for (k = 0; t->type[k]; k++) t->type[k] = (char)tolower((unsigned char)t->type[k]);
    }     else if (is(an, "value")) attr_copy(t->value, (int)sizeof(t->value), v, vl, L);
    else if (is(an, "id")) attr_copy(t->id, (int)sizeof(t->id), v, vl, L);
    else if (is(an, "class")) attr_copy(t->cls, (int)sizeof(t->cls), v, vl, L);
    else if (is(an, "style")) attr_copy(t->style, (int)sizeof(t->style), v, vl, L);
    else if ((is(an, "srcset") || is(an, "data-src")) && !t->src[0]) {
      int a = 0, b;
      while (a < vl && isspace((unsigned char)v[a])) a++;
      b = a;
      while (b < vl && !isspace((unsigned char)v[b]) && v[b] != ',') b++;
      attr_copy(t->src, (int)sizeof(t->src), v + a, b - a, L);
    }
  }
  return i < n ? i + 1 : n;
}

static int hexdig(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int css_color(const char* s, int n, unsigned* out) {
  char buf[32];
  int i;
  while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
  while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
  if (n >= 4 && s[0] == '#') {
    int r, g, b;
    if (n >= 7 && hexdig(s[1]) >= 0 && hexdig(s[2]) >= 0) {
      r = (hexdig(s[1]) << 4) | hexdig(s[2]);
      g = (hexdig(s[3]) << 4) | hexdig(s[4]);
      b = (hexdig(s[5]) << 4) | hexdig(s[6]);
      if (r >= 0 && g >= 0 && b >= 0) { *out = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b; return 1; }
    } else if (hexdig(s[1]) >= 0 && hexdig(s[2]) >= 0 && hexdig(s[3]) >= 0) {
      r = hexdig(s[1]); g = hexdig(s[2]); b = hexdig(s[3]);
      *out = ((unsigned)(r * 17) << 16) | ((unsigned)(g * 17) << 8) | (unsigned)(b * 17);
      return 1;
    }
  }
  if (n > 4 && tolower((unsigned char)s[0]) == 'r' && tolower((unsigned char)s[1]) == 'g') {
    const char* p = s;
    const char* end = s + n;
    int rgb[3], k, any;
    while (p < end && *p != '(') p++;
    if (p < end) p++;
    for (k = 0; k < 3; k++) {
      rgb[k] = 0;
      any = 0;
      while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
      while (p < end && *p >= '0' && *p <= '9') {
        rgb[k] = rgb[k] * 10 + (*p - '0');
        any = 1;
        p++;
      }
      if (p < end && *p == '%') p++;
      if (!any) return 0;
      if (rgb[k] > 255) rgb[k] = 255;
    }
    *out = ((unsigned)rgb[0] << 16) | ((unsigned)rgb[1] << 8) | (unsigned)rgb[2];
    return 1;
  }
  if (n <= 0 || n >= (int)sizeof buf) return 0;
  for (i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
  buf[n] = 0;
  if (!strcmp(buf, "black")) *out = 0x000000;
  else if (!strcmp(buf, "white")) *out = 0xFFFFFF;
  else if (!strcmp(buf, "red")) *out = 0xFF0000;
  else if (!strcmp(buf, "green")) *out = 0x008000;
  else if (!strcmp(buf, "lime")) *out = 0x00FF00;
  else if (!strcmp(buf, "blue")) *out = 0x0000FF;
  else if (!strcmp(buf, "yellow")) *out = 0xFFFF00;
  else if (!strcmp(buf, "gray") || !strcmp(buf, "grey")) *out = 0x808080;
  else if (!strcmp(buf, "orange")) *out = 0xFF8000;
  else if (!strcmp(buf, "purple")) *out = 0x800080;
  else if (!strcmp(buf, "navy")) *out = 0x000080;
  else if (!strcmp(buf, "maroon")) *out = 0x800000;
  else if (!strcmp(buf, "teal")) *out = 0x008080;
  else if (!strcmp(buf, "silver")) *out = 0xC0C0C0;
  else if (!strcmp(buf, "aqua")) *out = 0x00FFFF;
  else if (!strcmp(buf, "fuchsia")) *out = 0xFF00FF;
  else if (!strcmp(buf, "olive")) *out = 0x808000;
  else return 0;
  return 1;
}

static int css_px(const char* s, int n) {
  int i = 0, v = 0;
  while (i < n && isspace((unsigned char)s[i])) i++;
  if (i >= n || s[i] < '0' || s[i] > '9') return 0;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i] - '0');
    if (v > 4000) return 4000;
    i++;
  }
  return v;
}

static void css_decls(css_rule_t* r, const char* s, int n) {
  int i = 0;
  while (i < n) {
    char prop[32];
    int ps, pe, vs, ve, k, pl;
    unsigned col;
    while (i < n && (isspace((unsigned char)s[i]) || s[i] == ';')) i++;
    ps = i;
    while (i < n && s[i] != ':' && s[i] != ';') i++;
    pe = i;
    if (i >= n || s[i] != ':') {
      while (i < n && s[i] != ';') i++;
      continue;
    }
    i++;
    vs = i;
    while (i < n && s[i] != ';') i++;
    ve = i;
    while (ps < pe && isspace((unsigned char)s[ps])) ps++;
    while (pe > ps && isspace((unsigned char)s[pe - 1])) pe--;
    while (vs < ve && isspace((unsigned char)s[vs])) vs++;
    while (ve > vs && isspace((unsigned char)s[ve - 1])) ve--;
    if (ve - vs >= 10 && strncmp(s + ve - 10, "!important", 10) == 0) {
      ve -= 10;
      while (ve > vs && isspace((unsigned char)s[ve - 1])) ve--;
    }
    pl = pe - ps;
    if (pl <= 0 || pl >= (int)sizeof prop) continue;
    for (k = 0; k < pl; k++) prop[k] = (char)tolower((unsigned char)s[ps + k]);
    prop[pl] = 0;
    if (is(prop, "color") && css_color(s + vs, ve - vs, &col)) {
      r->color = col;
      r->color_set = 1;
    } else if ((is(prop, "background-color") || is(prop, "background")) &&
               !(is(prop, "background") && ve - vs >= 4 &&
                 (s[vs] == 'u' || s[vs] == 'U') && (s[vs + 1] == 'r' || s[vs + 1] == 'R')) &&
               css_color(s + vs, ve - vs, &col)) {
      r->bg = col;
      r->bg_set = 1;
    } else if (is(prop, "font-weight")) {
      if (ve - vs == 4 && strncmp(s + vs, "bold", 4) == 0) r->bold = 1;
      else if (ve - vs >= 3 && s[vs] >= '6' && s[vs] <= '9') r->bold = 1;
      else if (ve - vs == 6 && strncmp(s + vs, "normal", 6) == 0) r->bold = -1;
      else if (ve - vs == 3 && strncmp(s + vs, "400", 3) == 0) r->bold = -1;
    } else if (is(prop, "text-decoration")) {
      if (ve - vs >= 9 && memchr(s + vs, 'u', (size_t)(ve - vs))) r->uline = 1;
      else if (ve - vs == 4 && strncmp(s + vs, "none", 4) == 0) r->uline = -1;
    } else if (is(prop, "display") && ve - vs == 4 && strncmp(s + vs, "none", 4) == 0) {
      r->hide = 1;
    } else if (is(prop, "width")) r->width = css_px(s + vs, ve - vs);
    else if (is(prop, "height")) r->height = css_px(s + vs, ve - vs);
  }
}

static int has_class(const char* cls, const char* want) {
  int n = (int)strlen(want);
  const char* p = cls;
  if (n <= 0) return 0;
  while (*p) {
    int k = 0;
    while (*p == ' ') p++;
    if (!*p) break;
    while (p[k] && p[k] != ' ') k++;
    if (k == n && strncmp(p, want, (size_t)n) == 0) return 1;
    p += k;
  }
  return 0;
}

static int sel_match(const char* sel, const tag_t* t) {
  const char* dot = strchr(sel, '.');
  const char* hash = strchr(sel, '#');
  if (hash && !dot) {
    if (sel[0] == '#') return strcmp(hash + 1, t->id) == 0;
    if ((int)strlen(t->name) != (int)(hash - sel) || strncmp(t->name, sel, (size_t)(hash - sel))) return 0;
    return strcmp(hash + 1, t->id) == 0;
  }
  if (dot) {
    if (sel[0] == '.') return has_class(t->cls, dot + 1);
    if ((int)strlen(t->name) != (int)(dot - sel) || strncmp(t->name, sel, (size_t)(dot - sel))) return 0;
    return has_class(t->cls, dot + 1);
  }
  return is(sel, t->name);
}

static void props_onto(const css_rule_t* r, frm_t* f) {
  if (r->color_set) { f->color = r->color; f->color_set = 1; }
  if (r->bg_set) { f->bg = r->bg; f->bg_set = 1; }
  if (r->bold) f->bold = r->bold;
  if (r->uline) f->uline = r->uline;
  if (r->hide) f->hide = 1;
  if (r->width) f->width = r->width;
  if (r->height) f->height = r->height;
}

static void css_one(css_t* c, const char* sel, int n, const css_rule_t* props) {
  char b[40];
  int i;
  css_rule_t* r;
  while (n > 0 && isspace((unsigned char)*sel)) { sel++; n--; }
  while (n > 0 && isspace((unsigned char)sel[n - 1])) n--;
  if (n < 1 || n >= (int)sizeof b) return;
  if (memchr(sel, ' ', (size_t)n) || memchr(sel, '>', (size_t)n) || memchr(sel, '+', (size_t)n) ||
      memchr(sel, '[', (size_t)n) || memchr(sel, '~', (size_t)n)) return;
  memcpy(b, sel, (size_t)n);
  b[n] = 0;
  for (i = 0; b[i]; i++) if (b[i] == ':') break;
  if (b[i] == ':') {
    if (strcmp(b + i, ":link") && strcmp(b + i, ":visited") && strcmp(b + i, ":hover") &&
        strcmp(b + i, ":active") && strcmp(b + i, ":focus")) return;
    b[i] = 0;
  }
  for (i = 0; b[i] && b[i] != '.' && b[i] != '#'; i++)
    b[i] = (char)tolower((unsigned char)b[i]);
  if (!b[0] || c->n >= CSS_RULES) return;
  r = &c->rules[c->n++];
  *r = *props;
  strncpy(r->sel, b, sizeof r->sel - 1);
  r->sel[sizeof r->sel - 1] = 0;
}

static void css_parse(css_t* c, const char* s, int n) {
  int i = 0;
  if (!c || !s || n <= 0) return;
  while (i < n) {
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i >= n) break;
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
      if (i + 1 < n) i += 2;
      continue;
    }
    if (s[i] == '@') {
      while (i < n && s[i] != '{' && s[i] != ';') i++;
      if (i < n && s[i] == ';') { i++; continue; }
      if (i < n && s[i] == '{') {
        int d = 1;
        i++;
        while (i < n && d) {
          if (s[i] == '{') d++;
          else if (s[i] == '}') d--;
          i++;
        }
      }
      continue;
    }
    {
      int ss = i, se, bs, be;
      css_rule_t props;
      while (i < n && s[i] != '{') i++;
      if (i >= n) break;
      se = i++;
      bs = i;
      while (i < n && s[i] != '}') i++;
      be = i;
      if (i < n) i++;
      memset(&props, 0, sizeof props);
      css_decls(&props, s + bs, be - bs);
      while (ss < se) {
        int e = ss;
        while (e < se && s[e] != ',') e++;
        css_one(c, s + ss, e - ss, &props);
        ss = e + 1;
      }
    }
  }
}

static void css_onto(const css_t* c, const tag_t* t, frm_t* f) {
  int i;
  if (!c) return;
  for (i = 0; i < c->n; i++)
    if (sel_match(c->rules[i].sel, t)) props_onto(&c->rules[i], f);
}

static void style_inline(const tag_t* t, frm_t* f) {
  css_rule_t inl;
  if (!t->style[0]) return;
  memset(&inl, 0, sizeof inl);
  css_decls(&inl, t->style, (int)strlen(t->style));
  props_onto(&inl, f);
}

static void style_sync(lay_t* L) {
  frm_t* f;
  L->css_color_set = L->css_bg_set = 0;
  L->css_bold = L->css_uline = 0;
  if (L->nframe <= 0) return;
  f = &L->frame[L->nframe - 1];
  L->css_color_set = f->color_set;
  L->css_color = f->color;
  L->css_bg_set = f->bg_set;
  L->css_bg = f->bg;
  L->css_bold = f->bold;
  L->css_uline = f->uline;
}

static int is_void_tag(const char* n) {
  return in_list(n, "img br hr input meta link base source wbr area col embed param track");
}

static int autoclose_tag(const char* n) {
  return in_list(n, "p li dt dd tr td th option h1 h2 h3 h4 h5 h6");
}

static void style_pop(lay_t* L, const char* tag) {
  int i;
  for (i = L->nframe - 1; i >= 0; i--) {
    if (!is(L->frame[i].tag, tag)) continue;
    while (L->nframe > i) {
      frm_t* f = &L->frame[--L->nframe];
      if (f->el >= 0 && f->el < L->d->nels) L->d->els[f->el].run1 = L->d->nruns - 1;
      if (f->hide && L->hide > 0) L->hide--;
    }
    style_sync(L);
    return;
  }
}

static void style_close_all(lay_t* L) {
  while (L->nframe > 0) {
    char tag[16];
    strncpy(tag, L->frame[L->nframe - 1].tag, sizeof tag - 1);
    tag[sizeof tag - 1] = 0;
    style_pop(L, tag);
  }
}

static void style_push(lay_t* L, tag_t* t) {
  frm_t* f;
  if (L->nframe >= CSS_DEPTH) return;
  if (autoclose_tag(t->name)) style_pop(L, t->name);
  if (L->nframe >= CSS_DEPTH) return;
  f = &L->frame[L->nframe];
  memset(f, 0, sizeof *f);
  f->el = -1;
  if (L->nframe > 0) {
    f->color = L->frame[L->nframe - 1].color;
    f->color_set = L->frame[L->nframe - 1].color_set;
    f->bold = L->frame[L->nframe - 1].bold;
  }
  strncpy(f->tag, t->name, sizeof f->tag - 1);
  css_onto(L->css, t, f);
  style_inline(t, f);
  if (!t->width && f->width) t->width = f->width;
  if (!t->height && f->height) t->height = f->height;
  if (t->id[0] && L->d->nels < DOC_ELS) {
    doc_el_t* e = &L->d->els[L->d->nels];
    memset(e, 0, sizeof *e);
    strncpy(e->id, t->id, sizeof e->id - 1);
    e->run0 = L->d->nruns;
    e->run1 = L->d->nruns - 1;
    f->el = L->d->nels++;
  }
  if (f->hide) L->hide++;
  L->nframe++;
  style_sync(L);
}

static void style_void(lay_t* L, tag_t* t) {
  frm_t f;
  memset(&f, 0, sizeof f);
  css_onto(L->css, t, &f);
  style_inline(t, &f);
  if (!t->width && f.width) t->width = f.width;
  if (!t->height && f.height) t->height = f.height;
}

static int is_js_type(const char* t) {
  if (!t || !t[0]) return 1;
  if (strcmp(t, "module") == 0) return 1;
  return strstr(t, "javascript") != 0;
}

static int take_raw(const char* s, int n, const char* name, char** out, int* outn) {
  int nl = (int)strlen(name);
  int i, found = 0, close_at = n, body;
  *out = 0;
  *outn = 0;
  for (i = 0; i + nl + 2 <= n; i++) {
    if (s[i] == '<' && s[i + 1] == '/') {
      int k;
      for (k = 0; k < nl; k++)
        if (tolower((unsigned char)s[i + 2 + k]) != name[k]) break;
      if (k == nl) { found = 1; close_at = i; break; }
    }
  }
  body = close_at;
  if (body > RAW_MAX) body = RAW_MAX;
  if (body > 0) {
    *out = (char*)malloc((size_t)body + 1);
    if (*out) {
      memcpy(*out, s, (size_t)body);
      (*out)[body] = 0;
      *outn = body;
    }
  }
  if (!found) return n;
  while (close_at < n && s[close_at] != '>') close_at++;
  return close_at < n ? close_at + 1 : n;
}

static void keep_script(lay_t* L, tag_t* t, char* body, int blen) {
  doc_script_t* sc;
  char abs[URL_MAX];
  if (!is_js_type(t->type) || L->d->nscripts >= DOC_SCRIPTS) {
    free(body);
    return;
  }
  sc = &L->d->scripts[L->d->nscripts];
  memset(sc, 0, sizeof *sc);
  if (t->src[0] && url_resolve(L->base ? L->base : "", t->src, abs, URL_MAX) == 0) {
    strncpy(sc->url, abs, URL_MAX - 1);
    free(body);
    L->d->nscripts++;
    return;
  }
  if (!body) return;
  sc->src = body;
  sc->len = blen;
  L->d->nscripts++;
}

static void lay_init(lay_t* L, doc_t* d, int cols, int cp437) {
  memset(d, 0, sizeof(*d));
  memset(L, 0, sizeof(*L));
  L->d = d;
  L->cols = cols < 20 ? 20 : cols;
  L->line_empty = 1;
  L->link = -1;
  L->field = -1;
  L->form = -1;
  L->cp437 = cp437;
}

static void lay_finish(lay_t* L) {
  flush_word(L);
  L->d->nlines = L->line + (L->line_empty ? 0 : 1);
}

void doc_layout(doc_t* d, const char* html, int len, int cols, const char* base_url,
                int latin1, int cp437, const char* extra_css, int extra_len) {
  lay_t L;
  int i = 0;
  css_t* css = (css_t*)calloc(1, sizeof *css);
  tag_t* t = (tag_t*)malloc(sizeof(tag_t));
  lay_init(&L, d, cols, cp437);
  L.base = base_url;
  L.latin1 = latin1;
  L.css = css;
  if (css && extra_css && extra_len > 0) css_parse(css, extra_css, extra_len);
  if (!t) {
    free(css);
    return;
  }
  while (i < len) {
    unsigned char c = (unsigned char)html[i];
    if (c == '<' && i + 1 < len) {
      unsigned char c1 = (unsigned char)html[i + 1];
      if (c1 == '!' && i + 3 < len && html[i + 2] == '-' && html[i + 3] == '-') {
        const char* e = strstr(html + i + 4, "-->");
        i = e ? (int)(e - html) + 3 : len;
        continue;
      }
      if (c1 == '!' || c1 == '?') {
        while (i < len && html[i] != '>') i++;
        i++;
        continue;
      }
      if (c1 == '/' || isalpha(c1)) {
        int before, nf;
        i += parse_tag(html + i, len - i, t, &L);
        if (!t->end && !t->self_close &&
            (is(t->name, "script") || is(t->name, "style") || is(t->name, "textarea"))) {
          char* body = 0;
          int bl = 0;
          i += take_raw(html + i, len - i, t->name, &body, &bl);
          if (is(t->name, "style")) {
            if (body && L.css) css_parse(L.css, body, bl);
            free(body);
          } else if (is(t->name, "script")) keep_script(&L, t, body, bl);
          else {
            if (body && bl > 0) {
              int n = bl;
              int k = 0;
              int j;
              if (n > (int)sizeof(t->value) - 1) n = (int)sizeof(t->value) - 1;
              for (j = 0; j < n && k < (int)sizeof(t->value) - 1; j++) {
                if (body[j] == '\r' || body[j] == '\n') continue;
                t->value[k++] = body[j];
              }
              t->value[k] = 0;
            }
            place_field(&L, t, 0, 0);
            free(body);
          }
          continue;
        }
        before = d->nruns;
        nf = L.nframe;
        if (!t->end && (t->self_close || is_void_tag(t->name))) style_void(&L, t);
        else if (!t->end) style_push(&L, t);
        handle_tag(&L, t);
        if (!t->end && L.nframe == nf && t->id[0] && d->nels < DOC_ELS) {
          doc_el_t* e = &d->els[d->nels];
          memset(e, 0, sizeof *e);
          strncpy(e->id, t->id, sizeof e->id - 1);
          e->run0 = before;
          e->run1 = d->nruns - 1;
          d->nels++;
        }
        if (t->end) style_pop(&L, t->name);
        continue;
      }
    }
    if (c == '&') {
      unsigned cp;
      int k = entity(html + i, len - i, &cp);
      if (k > 0) {
        text_cp(&L, cp);
        i += k;
        continue;
      }
    }
    if (c < 0x80 || L.latin1) {
      text_cp(&L, c);
      i++;
      continue;
    }
    {
      /* UTF-8; byte invalido vira Latin-1 */
      unsigned cp = 0;
      int need = 0, k;
      if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; need = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; need = 2; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; need = 3; }
      for (k = 1; k <= need; k++) {
        if (i + k >= len || ((unsigned char)html[i + k] & 0xC0) != 0x80) break;
        cp = (cp << 6) | ((unsigned char)html[i + k] & 0x3F);
      }
      if (need == 0 || k <= need) {
        text_cp(&L, c);
        i++;
      } else {
        text_cp(&L, cp);
        i += need + 1;
      }
    }
  }
  lay_finish(&L);
  style_close_all(&L);
  free(t);
  free(css);
}

void doc_layout_text(doc_t* d, const char* text, int len, int cols, int cp437) {
  lay_t L;
  int i;
  lay_init(&L, d, cols, cp437);
  L.pre = 1;
  for (i = 0; i < len; i++) text_cp(&L, (unsigned char)text[i]);
  lay_finish(&L);
}

void doc_free(doc_t* d) {
  int i;
  if (!d) return;
  for (i = 0; i < d->nlinks; i++) free(d->links[i]);
  for (i = 0; i < d->nscripts; i++) free(d->scripts[i].src);
  free(d->links);
  free(d->runs);
  free(d->text);
  memset(d, 0, sizeof(*d));
}

int doc_first_run(const doc_t* d, int line) {
  int lo = 0, hi = d->nruns;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (d->runs[mid].line < line) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

int doc_link_at(const doc_t* d, int line, int col) {
  int i;
  for (i = doc_first_run(d, line); i < d->nruns && d->runs[i].line == line; i++) {
    const run_t* r = &d->runs[i];
    if (r->link >= 0 && col >= r->col && col < r->col + r->len) return r->link;
  }
  return -1;
}

int doc_field_at(const doc_t* d, int line, int col) {
  int i;
  for (i = doc_first_run(d, line); i < d->nruns && d->runs[i].line == line; i++) {
    const run_t* r = &d->runs[i];
    if (r->field >= 0 && col >= r->col && col < r->col + r->len) return r->field;
  }
  return -1;
}
