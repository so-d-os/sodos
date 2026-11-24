/* html.c - tokenizador HTML tolerante e layout numa grade de caracteres
 * (a fonte e monoespacada).
 *  - fluxo: paragrafos, titulos, listas, links, pre, quebra de linha,
 *    text-align (e <center>), display inline/block/flex/none;
 *  - tabelas de verdade: as colunas sao medidas (largura minima e maxima de
 *    cada celula) e repartidas como num navegador; celulas lado a lado,
 *    fundo, bordas e alinhamento vertical;
 *  - caixas (doc_rect_t): fundo e borda de blocos e celulas;
 *  - CSS: seletores com descendente e filho (div.a > p #x), especificidade,
 *    var(--x), @media pela largura da janela, cores rgb/hsl/nomes. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "browser.h"

#define COL_TEXT 0x000000
#define COL_LINK 0x0000EE
#define COL_CODE 0x7A1F1F
#define COL_EM   0x303050
#define COL_BTN  0xE4E4E4

/* O GitHub traz ~2,6 MB de CSS: com 3072 regras as do cabecalho (que escondem
 * os menus suspensos) ficavam de fora. Cada regra tem ~1 KB. */
#define CSS_RULES 24576
#define CSS_VARS 1024
#define CSS_HASH 4096
#define CSS_DEPTH 96
#define SEL_PARTS 5
#define RAW_MAX (64 * 1024) /* corpo de <style>, <script> e <textarea> */
#define TABLE_DEPTH 4       /* tabelas dentro de tabelas; mais fundo vira fluxo */
#define TABLE_COLS 64
#define MEASURE_COLS 2000   /* largura "infinita" para medir celulas */
#define RECTS_MAX 8192

/* display */
#define D_UNSET  0
#define D_NONE   1
#define D_INLINE 2
#define D_BLOCK  3
#define D_FLEX   4

#define A_LEFT   0
#define A_CENTER 1
#define A_RIGHT  2

#define V_TOP    1
#define V_MIDDLE 2
#define V_BOTTOM 3

/* como a tag entra no fluxo */
#define M_NORMAL 0 /* o que a tag ja e */
#define M_INLINE 1 /* bloco mostrado em linha (display:inline, item de flex) */
#define M_BLOCK  2 /* tag de linha mostrada como bloco */

/* propriedades que uma regra define */
#define P_COLOR   0x1
#define P_BG      0x2
#define P_BOLD    0x4
#define P_ULINE   0x8
#define P_DISPLAY 0x10
#define P_ALIGN   0x20
#define P_UPPER   0x40
#define P_WS      0x80
#define P_LIST    0x100
#define P_BCOLOR  0x200
#define P_WIDTH   0x400
#define P_HEIGHT  0x800
#define P_VIS     0x1000
#define P_FLEXDIR 0x2000
#define P_MAUTO   0x4000
#define P_VALIGN  0x8000
#define P_NOPAD   0x10000
#define P_OFF     0x20000
#define P_SPL     0x40000
#define P_SPR     0x80000
#define P_FLOAT   0x100000

typedef struct {
  int set;
  unsigned color, bg, bcolor;
  int bold, uline, display, align, upper, ws, list_none;
  int bset, bval; /* lados da borda que a regra diz, e quais ficam ligados */
  int width, height; /* px; < 0 = -porcento */
  int hidden, flexcol, mauto, valign, nopad, off;
  int spl, spr; /* margem ou recuo dos lados: vira espaco em volta (1) ou nao (-1) */
  int flt;      /* float: 1 left, 2 right, 0 none */
} css_props_t;

typedef struct {
  char tag[12];  /* "" = qualquer */
  char id[40];
  char cls[3][40];
  int ncls;
  int comb; /* relacao com a parte da esquerda: 1 descendente, 2 filho */
} sel_part_t;

typedef struct {
  sel_part_t part[SEL_PARTS]; /* part[0] e o proprio elemento */
  int nparts, spec, next;
  css_props_t p;
} css_rule_t;

typedef struct {
  char name[40];
  char val[96];
} css_var_t;

typedef struct {
  css_rule_t* rules;
  int n, cap;
  int head[CSS_HASH];
  int any; /* regras sem tag, classe nem id (*) */
  css_var_t vars[CSS_VARS];
  int nvars;
  int vw; /* largura da janela em px, para @media */
} css_t;

/* Estilo calculado de um elemento aberto. */
typedef struct {
  char tag[16];
  char id[40];
  char cls[160];
  int el, rect, mode, hide;
  /* herdados */
  unsigned color;
  int color_set, bold, uline, align, upper, ws, list_none;
  unsigned ebg;      /* fundo por baixo do texto (contraste) */
  unsigned rbg;      /* fundo dos runs (elementos de linha) */
  int rbg_set;
  /* do proprio elemento */
  unsigned bg, bcolor;
  int bg_set, display, border, width, height, flexcol, mauto, valign, nopad;
  int legacy_center, closed_details, blocky;
  int spl, spr, flt;
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
  int list_ind[16];
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
  int nframe, frame_floor;
  int hide;
  /* estilo do elemento do topo (style_sync) */
  unsigned css_color, css_bg, ebg;
  int css_color_set, css_bg_set, css_bold, css_uline;
  int align, ws_pre, upper, nomark;
  int blk;        /* handle_tag: 1 = efeitos de bloco (quebra, margem, recuo) */
  int line_run0;  /* primeiro run da linha atual (alinhamento) */
  int run_floor;  /* figuras nao entram antes deste run (celula atual) */
  int last_emit;  /* ultima linha com conteudo */
  int measure;    /* 1 = so medindo uma celula */
  int minw;       /* maior pedaco que nao quebra (medida) */
  int table_depth;
  unsigned link_color;
  int button;     /* campo do <button> aberto, -1 */
  int glue_run, glue_col; /* palavra colada (<a>x</a>) que comecou neste run e coluna */
  /* float:right (tabela): o texto corre a esquerda dela, com cols menor, ate
   * passar da ultima linha da tabela; os runs saem de ordem e sao ordenados no fim */
  int fl_end, fl_cols, fl_used;
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
/* U+00A0..U+00FF sem a fonte da BIOS: 16 por linha (o \0 do U+00AD some) */
static const char ascii_hi[] =
  " !cL?Y|S\"ca<-\0R-" "o+23'uP.,1o>????" "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYPs"
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
    case 0x2022: case 0x25CF: case 0x2023: case 0x2219: out[0] = cp437 ? (char)7 : '*'; return 1;
    case 0x20AC: out[0] = 'E'; return 1;
    case 0x2122: out[0] = 'T'; out[1] = 'M'; return 2;
    case 0x2039: out[0] = '<'; return 1;
    case 0x203A: out[0] = '>'; return 1;
    case 0x2190: out[0] = cp437 ? (char)0x1B : '<'; return 1;
    case 0x2192: out[0] = cp437 ? (char)0x1A : '>'; return 1;
    case 0x2191: case 0x25B2: out[0] = cp437 ? (char)0x18 : '^'; return 1;
    case 0x2193: case 0x25BC: out[0] = cp437 ? (char)0x19 : 'v'; return 1;
    case 0x25B6: case 0x25BA: out[0] = cp437 ? (char)0x10 : '>'; return 1;
    case 0x25C0: case 0x25C4: out[0] = cp437 ? (char)0x11 : '<'; return 1;
    case 0x2713: case 0x2714: out[0] = cp437 ? (char)0xFB : 'v'; return 1;
    case 0x2715: case 0x2716: case 0x2717: case 0x2718: out[0] = 'x'; return 1;
    case 0x2605: case 0x2606: out[0] = '*'; return 1;
    case 0x00B7: out[0] = cp437 ? (char)0xFA : '.'; return 1;
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0xFEFF: return 0;
  }
  if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) {
    out[0] = ' ';
    return 1;
  }
  /* emoji e simbolos: some, em vez de encher a linha de '?' */
  if (cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0xE000 && cp <= 0xF8FF) ||
      (cp >= 0xFE00 && cp <= 0xFE0F))
    return 0;
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

static int lum(unsigned c) {
  return (int)((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000);
}

static int is(const char* a, const char* b) { return strcmp(a, b) == 0; }

static void emit(lay_t* L, const char* s, int n, int extra_flags) {
  doc_t* d = L->d;
  run_t* r;
  int flags = extra_flags;
  unsigned color = COL_TEXT;
  unsigned bg = 0;
  if (n <= 0 || L->hide) return;
  if (L->fl_end >= 0 && L->line > L->fl_end) { /* float_check, antes da definicao */
    L->cols = L->fl_cols;
    L->fl_end = -1;
  }
  if (L->bold || L->heading) flags |= RUN_BOLD;
  if (L->link >= 0 || L->uline) flags |= RUN_ULINE;
  if (L->css_bold > 0) flags |= RUN_BOLD;
  if (L->css_bold < 0 && !L->heading && !L->bold) flags &= ~RUN_BOLD;
  if (L->css_uline > 0) flags |= RUN_ULINE;
  if (L->css_uline < 0) flags &= ~RUN_ULINE;
  if (L->css_color_set) color = L->css_color;
  else if (L->link >= 0) color = L->link_color;
  else if (L->code) color = COL_CODE;
  else if (L->em) color = COL_EM;
  if (L->css_bg_set) {
    flags |= RUN_BG;
    bg = L->css_bg;
  }
  {
    /* texto da cor do fundo (fundo vinha de uma figura que nao temos) */
    int a = lum(color), b = lum((flags & RUN_BG) ? bg : L->ebg);
    if (a - b < 45 && b - a < 45) color = b > 128 ? 0x000000 : 0xFFFFFF;
  }
  d->text = (char*)grow(d->text, &d->text_cap, d->text_len + n + 1, 1);
  if (!d->text) return;
  memcpy(d->text + d->text_len, s, (size_t)n);
  r = d->nruns ? &d->runs[d->nruns - 1] : 0;
  if (r && !(flags & RUN_IMG) && r->img < 0 && r->line == L->line &&
      r->col + r->len == L->col && r->flags == flags && r->bg == bg &&
      r->color == color && r->link == L->link && r->field == L->field &&
      r->off + r->len == d->text_len && d->nruns > L->line_run0 && d->nruns - 1 >= L->glue_run) {
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
  if (L->line > L->last_emit) L->last_emit = L->line;
}

/* text-align: empurra os runs da linha que acabou. */
static void align_line(lay_t* L) {
  doc_t* d = L->d;
  int i, right = L->indent, shift;
  if (L->measure || L->align == A_LEFT || L->pre || L->ws_pre) return;
  for (i = L->line_run0; i < d->nruns; i++) {
    run_t* r = &d->runs[i];
    if (r->col + r->len > right) right = r->col + r->len;
  }
  shift = L->cols - right;
  if (L->align == A_CENTER) shift /= 2;
  if (shift <= 0) return;
  for (i = L->line_run0; i < d->nruns; i++) {
    run_t* r = &d->runs[i];
    r->col += shift;
    if (r->field >= 0 && r->field < d->nfields && d->fields[r->field].line == r->line &&
        d->fields[r->field].col == r->col - shift)
      d->fields[r->field].col += shift;
  }
}

/* passou da ultima linha do float: a largura volta ao normal */
static void float_check(lay_t* L) {
  if (L->fl_end >= 0 && L->line > L->fl_end) {
    L->cols = L->fl_cols;
    L->fl_end = -1;
  }
}

static void flush_line(lay_t* L) {
  if (!L->line_empty) {
    align_line(L);
    L->line++;
    L->line_empty = 1;
    L->blank = 0;
  }
  float_check(L);
  L->col = L->indent;
  L->space_pending = 0;
  L->line_run0 = L->d->nruns;
  L->glue_run = -1;
}

/* Pedacos colados de uma palavra ("(site.com)" em tags diferentes) quebram
 * juntos: o que ja esta na linha desce para a proxima. */
static int glue_wrap(lay_t* L) {
  doc_t* d = L->d;
  int k, n0 = L->glue_run, n1 = d->nruns, dx, oldcol = L->col, gcol = L->glue_col;
  if (n0 < 0 || n0 >= n1 || gcol <= L->indent || L->measure) return 0;
  for (k = n0; k < n1; k++)
    if ((d->runs[k].flags & RUN_IMG) || d->runs[k].line != L->line) return 0;
  if (oldcol - gcol >= L->cols - L->indent) return 0; /* nao cabe nem sozinho */
  d->nruns = n0;
  flush_line(L);
  d->nruns = n1;
  dx = L->indent - gcol;
  for (k = n0; k < n1; k++) {
    run_t* r = &d->runs[k];
    if (r->field >= 0 && r->field < d->nfields && d->fields[r->field].line == r->line) {
      d->fields[r->field].line = L->line;
      d->fields[r->field].col += dx;
    }
    r->line = L->line;
    r->col += dx;
  }
  L->col = oldcol + dx;
  L->line_empty = 0;
  if (L->line > L->last_emit) L->last_emit = L->line;
  L->glue_run = n0;
  L->glue_col = L->indent;
  return 1;
}

static void margin(lay_t* L, int n) {
  flush_line(L);
  if (L->d->nruns == 0) return;
  /* no topo de uma celula ou item ainda vazio a margem some */
  if (L->in_cell && L->d->nruns == L->cell_runs) return;
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
  if (n > L->minw) L->minw = n;
  sp = L->space_pending && !L->line_empty;
  if (!L->line_empty && L->col + sp + n > L->cols) {
    if (sp || !glue_wrap(L)) flush_line(L);
    sp = 0;
  }
  if (sp) {
    int keep = L->link, keepf = L->field;
    /* o espaco entre duas palavras nao herda o sublinhado do link anterior,
     * nem e parte de um campo */
    if (L->d->nruns && L->d->runs[L->d->nruns - 1].link != L->link) L->link = -1;
    L->field = -1;
    emit(L, " ", 1, 0);
    L->link = keep;
    L->field = keepf;
  }
  if (sp || L->line_empty || L->glue_run < 0) {
    L->glue_run = L->d->nruns;
    L->glue_col = L->col;
  } else if (L->col - L->glue_col + n > L->minw)
    L->minw = L->col - L->glue_col + n;
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
    L->line_run0 = L->d->nruns;
    L->glue_run = -1;
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
    L->line_run0 = L->d->nruns;
  }
  emit(L, &c, 1, 0);
  if (L->col - L->indent > L->minw) L->minw = L->col - L->indent;
}

static void text_cp(lay_t* L, unsigned cp) {
  char out[4];
  int n, i;
  if (L->skip || (L->hide && !L->in_title)) return;
  if (cp == '\r') return;
  if (L->in_title) {
    int tl = (int)strlen(L->d->title);
    int al = (int)strlen(L->d->title_ascii);
    if (cp == '\n' || cp == '\t') cp = ' ';
    if (cp == ' ' && (tl == 0 || L->d->title[tl - 1] == ' ')) return;
    n = map_cp(cp, out, L->cp437);
    for (i = 0; i < n && tl < (int)sizeof(L->d->title) - 1; i++) L->d->title[tl++] = out[i];
    L->d->title[tl] = 0;
    /* a barra de titulo do WIN so desenha ASCII: copia sem acentos */
    n = map_cp(cp, out, 0);
    for (i = 0; i < n && al < (int)sizeof(L->d->title_ascii) - 1; i++) L->d->title_ascii[al++] = out[i];
    L->d->title_ascii[al] = 0;
    return;
  }
  if (L->upper == 1) {
    if (cp >= 'a' && cp <= 'z') cp -= 32;
    else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 32;
  } else if (L->upper == 2) {
    if (cp >= 'A' && cp <= 'Z') cp += 32;
    else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 32;
  }
  if (L->pre || L->ws_pre) {
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
  { "Iacute", 0xCD }, { "Oacute", 0xD3 }, { "Uacute", 0xDA }, { "Atilde", 0xC3 },
  { "Otilde", 0xD5 }, { "Ecirc", 0xCA }, { "Acirc", 0xC2 }, { "Ocirc", 0xD4 }, { "Agrave", 0xC0 },
  { "ntilde", 0xF1 }, { "euro", 0x20AC }, { "trade", 0x2122 }, { "larr", 0x2190 },
  { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "sect", 0xA7 },
  { "para", 0xB6 }, { "shy", 0xAD }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "ouml", 0xF6 },
  { "auml", 0xE4 }, { "szlig", 0xDF }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
  { "thinsp", 0x2009 }, { "zwj", 0x200D }, { "zwnj", 0x200C }, { "lrm", 0x200E },
  { "rlm", 0x200F }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "plusmn", 0xB1 },
  { "frac12", 0xBD }, { "frac14", 0xBC }, { "sup2", 0xB2 }, { "ordf", 0xAA }, { "ordm", 0xBA },
  { 0, 0 }
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
  char id[40];
  char cls[160];
  char style[400];
  int width, height; /* px; < 0 = -porcento */
  char bgcolor[32];
  char color[32];
  char text[32];
  char linkc[32];
  char align[12];
  char valign[12];
  char placeholder[48];
  int span, border, start;
  int checked, selected, hidden, open, nowrap;
} tag_t;

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

static const char* skip_tags = "template iframe object video audio canvas math svg map";

static int tag_is_block(const char* n) {
  return in_list(n, "p h1 h2 h3 h4 h5 h6 pre listing xmp blockquote ul ol menu dir li dl dt dd hr "
                    "div section article header footer nav main aside fieldset figure figcaption "
                    "center address details summary caption form table tr td th tbody thead tfoot "
                    "legend noscript body html hgroup search dialog");
}

static int is_void_tag(const char* n) {
  return in_list(n, "img br hr input meta link base source wbr area col embed param track");
}

static int is_table_part(const char* n) {
  return in_list(n, "table tr td th tbody thead tfoot caption");
}

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

static int add_rect(lay_t* L, int line0, int col0, int col1, int flags, unsigned bg, unsigned bc) {
  doc_t* d = L->d;
  doc_rect_t* r;
  if (L->measure || d->nrects >= RECTS_MAX) return -1;
  d->rects = (doc_rect_t*)grow(d->rects, &d->rects_cap, d->nrects + 1, (int)sizeof(doc_rect_t));
  if (!d->rects) {
    d->nrects = 0;
    d->rects_cap = 0;
    return -1;
  }
  r = &d->rects[d->nrects];
  r->line0 = line0;
  r->line1 = line0 - 1;
  r->col0 = col0;
  r->col1 = col1;
  r->flags = flags;
  r->bg = bg;
  r->border = bc;
  return d->nrects++;
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
    /* figura sem descricao (icone, espacador): nao poe [imagem] no texto */
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
static run_t* img_run(lay_t* L, int line) {
  doc_t* d = L->d;
  int pos;
  d->runs = (run_t*)grow(d->runs, &d->runs_cap, d->nruns + 1, (int)sizeof(run_t));
  if (!d->runs) return 0;
  pos = d->nruns;
  while (pos > L->run_floor && d->runs[pos - 1].line > line) pos--;
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
  int idx, cols, lines, row, start, top, gap, beside, avail;
  if (L->hide) return;
  if (!t->src[0] || d->nimgs >= DOC_IMGS || !L->cp437) {
    alt_word(L, t->alt);
    return;
  }
  /* espacadores de 1 px (layout antigo e contadores de visita) */
  if ((t->width > 0 && t->width <= 2) || (t->height > 0 && t->height <= 2)) return;
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
  avail = L->cols - L->indent;
  if (avail < 1) avail = 1;
  {
    /* figura ja vista (main.c): tamanho real; se falhou (SVG...), so o alt */
    int hw = 0, hh = 0, known = img_hint(abs, &hw, &hh);
    if (known && (hw <= 0 || hh <= 0)) {
      alt_word(L, t->alt);
      return;
    }
    idx = d->nimgs++;
    im = &d->imgs[idx];
    memset(im, 0, sizeof(*im));
    strncpy(im->url, abs, URL_MAX - 1);
    strncpy(im->alt, t->alt, sizeof(im->alt) - 1);
    if (known) {
      /* a proporcao vem da figura; o HTML/CSS so fixa um dos lados */
      int pw;
      if (t->width < 0) pw = (L->measure ? 16 : avail * -t->width / 100) * 8;
      else if (t->width > 0) pw = t->width;
      else if (t->height > 0) pw = t->height * hw / hh;
      else pw = hw;
      if (pw > avail * 8) pw = avail * 8;
      if (t->height > 0 && t->width > 0) lines = (t->height + 15) / 16;
      else lines = (pw * hh / hw + 15) / 16;
      if (lines > 45) { /* figura muito alta: reduz mantendo a proporcao */
        pw = pw * 45 / lines;
        lines = 45;
      }
      cols = (pw + 7) / 8;
      im->fit = 0;
    } else {
      if (t->width < 0) cols = L->measure ? 16 : avail * -t->width / 100;
      else if (t->width > 0) cols = (t->width + 7) / 8;
      else cols = 16;
      if (t->height > 0) lines = (t->height + 15) / 16;
      else if (t->width != 0) lines = cols / 2;
      else lines = 8;
      im->fit = !(t->width > 0 && t->height > 0);
    }
  }
  if (cols < 1) cols = 1;
  if (cols > avail) cols = avail;
  if (lines < 1) lines = 1;
  if (lines > 45) lines = 45;
  im->cols = cols;
  im->lines = lines;
  if (cols > L->minw) L->minw = cols;
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
    run_t* r = img_run(L, top + row);
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
  if (L->line > L->last_emit) L->last_emit = L->line;
  L->col = start + cols;
  L->line_empty = 0;
  L->blank = 0;
  L->glue_run = -1;
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

/* Fundo cinza de botao enquanto escreve b. */
static void button_word(lay_t* L, const char* b) {
  int keep_set = L->css_bg_set;
  unsigned keep = L->css_bg;
  if (!keep_set) {
    L->css_bg_set = 1;
    L->css_bg = COL_BTN;
  }
  inline_word(L, b);
  L->css_bg_set = keep_set;
  L->css_bg = keep;
}

/* Guarda o campo e, se for visivel, desenha [____] ou [Enviar].
 * check: 1 checkbox, 2 radio. */
static void place_field(lay_t* L, tag_t* t, int submit, int hidden, int check) {
  doc_t* d = L->d;
  doc_field_t* f;
  int slot, box;
  if (L->measure) {
    /* so a largura importa */
    char b[44];
    if (hidden || L->hide) return;
    box = submit ? (int)strlen(t->value[0] ? t->value : "Enviar") + 2
                 : check ? 3 : (t->isize > 0 ? t->isize : 20) + 2;
    if (box > 42) box = 42;
    memset(b, '_', (size_t)box);
    b[box] = 0;
    inline_word(L, b);
    return;
  }
  if (L->hide) hidden = 1;
  if (hidden && d->nfields >= DOC_FIELDS - 8) return;
  if (d->nfields >= DOC_FIELDS) return;
  slot = d->nfields++;
  f = &d->fields[slot];
  memset(f, 0, sizeof(*f));
  f->submit = submit;
  f->hidden = hidden;
  f->form = L->form;
  f->line = -1;
  f->check = check;
  f->checked = t->checked;
  copy_z(f->name, (int)sizeof(f->name), t->fname);
  copy_z(f->value, (int)sizeof(f->value), check && !t->value[0] ? "on" : t->value);
  copy_z(f->placeholder, (int)sizeof(f->placeholder), t->placeholder);
  if (hidden) return;
  if (submit || check) {
    char b[140];
    int before = d->nruns;
    if (check) copy_z(b, (int)sizeof(b), check == 1 ? "[ ]" : "( )");
    else {
      int n = 1;
      const char* v = t->value[0] ? t->value : "Enviar";
      b[0] = '[';
      while (*v && n < (int)sizeof(b) - 2) b[n++] = *v++;
      b[n++] = ']';
      b[n] = 0;
    }
    L->field = slot;
    if (check) inline_word(L, b);
    else button_word(L, b);
    L->field = -1;
    if (d->nruns > before) {
      f->line = d->runs[d->nruns - 1].line;
      f->col = d->runs[d->nruns - 1].col;
      f->len = d->runs[d->nruns - 1].len;
    }
    return;
  }
  box = t->isize > 0 ? t->isize : 20;
  if (t->width > 0) box = t->width / 8 - 2;
  if (box > 40) box = 40;
  if (box > L->cols - L->indent - 2) box = L->cols - L->indent - 2;
  if (box < 4) box = 4;
  if (L->col > L->indent && L->col + box + 2 > L->cols) flush_line(L);
  f->box = box;
  {
    char b[48];
    int i, before = d->nruns;
    b[0] = '[';
    for (i = 0; i < box; i++) b[1 + i] = '_';
    b[box + 1] = ']';
    b[box + 2] = 0;
    L->field = slot;
    inline_word(L, b);
    L->field = -1;
    if (d->nruns > before) {
      f->line = d->runs[d->nruns - 1].line;
      f->col = d->runs[d->nruns - 1].col;
      f->len = box + 2;
    }
  }
}

static void form_tag(lay_t* L, tag_t* t) {
  if (t->end) {
    L->form = -1;
  } else if (L->d->nforms < DOC_FORMS && !L->measure) {
    int slot = L->d->nforms;
    char* dst = L->d->form_action[slot];
    if (!t->action[0] || url_resolve(L->base ? L->base : "", t->action, dst, URL_MAX) != 0)
      copy_z(dst, URL_MAX, L->base ? L->base : "");
    L->form = slot;
    L->d->nforms++;
  }
}

static void bflush(lay_t* L) {
  if (L->blk) flush_line(L);
}

static void bmargin(lay_t* L, int n) {
  if (L->blk) margin(L, n);
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
    if (L->hide) return;
    if (L->line_empty) {
      L->line++;
      L->blank++;
      L->line_run0 = L->d->nruns;
    } else {
      flush_line(L);
    }
    L->col = L->indent;
    return;
  }
  if (is(n, "p")) {
    bmargin(L, 1);
    return;
  }
  if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]) {
    bmargin(L, 1);
    L->heading = t->end ? 0 : n[1] - '0';
    return;
  }
  if (is(n, "pre") || is(n, "listing") || is(n, "xmp")) {
    bmargin(L, 1);
    L->pre += t->end ? -1 : 1;
    if (L->pre < 0) L->pre = 0;
    return;
  }
  if (is(n, "blockquote")) {
    if (!L->blk) return;
    margin(L, 1);
    L->indent += t->end ? -4 : 4;
    if (L->indent < 0) L->indent = 0;
    L->col = L->indent;
    return;
  }
  if (is(n, "ul") || is(n, "ol") || is(n, "menu") || is(n, "dir")) {
    if (!L->blk) return;
    if (!t->end) {
      int ind = 4;
      if (L->nframe > 0 && L->frame[L->nframe - 1].nopad == 1) ind = 0;
      margin(L, L->list_depth == 0 ? 1 : 0);
      if (L->list_depth < 15) {
        L->list_depth++;
        L->list_ol[L->list_depth] = is(n, "ol");
        L->list_n[L->list_depth] = t->start > 0 ? t->start - 1 : 0;
        L->list_ind[L->list_depth] = ind;
      }
      L->indent += ind;
    } else {
      int ind = L->list_depth > 0 ? L->list_ind[L->list_depth] : 4;
      if (L->list_depth > 0) L->list_depth--;
      L->indent -= ind;
      if (L->indent < 0) L->indent = 0;
      margin(L, L->list_depth == 0 ? 1 : 0);
    }
    L->col = L->indent;
    return;
  }
  if (is(n, "li")) {
    if (!L->blk) return;
    flush_line(L);
    if (!t->end) {
      char mark[8];
      int k = L->list_depth;
      int save = L->link;
      mark[0] = 0;
      if (k > 0 && L->list_ol[k]) {
        int v = ++L->list_n[k];
        int m = 0;
        if (v >= 100) mark[m++] = (char)('0' + v / 100 % 10);
        if (v >= 10) mark[m++] = (char)('0' + v / 10 % 10);
        mark[m++] = (char)('0' + v % 10);
        mark[m++] = '.';
        mark[m] = 0;
      } else {
        /* disco, circulo, quadrado (o 9 da fonte nao desenha) */
        mark[0] = k >= 3 ? (L->cp437 ? (char)0xFE : '-') : k == 2 ? 'o' : (L->cp437 ? (char)7 : '*');
        mark[1] = 0;
      }
      if (L->nomark) mark[0] = 0;
      if (mark[0] && L->indent > 0) {
        int keep_css = L->css_bg_set;
        L->col = L->indent - (int)strlen(mark) - 1;
        if (L->col < 0) L->col = 0;
        L->link = -1;
        L->css_bg_set = 0;
        emit(L, mark, (int)strlen(mark), 0);
        L->css_bg_set = keep_css;
        L->link = save;
        L->col = L->indent;
      }
      L->in_cell = 1;
      L->cell_runs = L->d->nruns;
    }
    return;
  }
  if (is(n, "dl")) {
    if (!L->blk) return;
    flush_line(L);
    if (!t->end && L->dl_depth < 15) L->dl_base[++L->dl_depth] = L->indent;
    else if (t->end && L->dl_depth > 0) L->indent = L->dl_base[L->dl_depth--];
    L->col = L->indent;
    return;
  }
  /* dt/dd quase nunca sao fechados: o recuo vem do <dl>, nao da pilha */
  if (is(n, "dd") || is(n, "dt")) {
    int base = L->dl_depth > 0 ? L->dl_base[L->dl_depth] : 0;
    if (!L->blk) return;
    flush_line(L);
    if (!t->end) L->indent = base + (is(n, "dd") ? 4 : 0);
    L->col = L->indent;
    return;
  }
  if (is(n, "hr")) {
    if (L->hide) return;
    flush_line(L);
    {
      int k = L->cols - L->col;
      char sp[256];
      int i;
      if (k > (int)sizeof(sp)) k = (int)sizeof(sp);
      if (L->measure) k = 1;
      for (i = 0; i < k; i++) sp[i] = ' ';
      emit(L, sp, k, RUN_HR);
    }
    flush_line(L);
    return;
  }
  /* tabela achatada (funda demais): celulas em fila */
  if (is(n, "tr")) {
    bflush(L);
    L->in_cell = 0;
    return;
  }
  if (is(n, "td") || is(n, "th")) {
    if (!t->end && !L->line_empty) emit(L, "  ", 2, 0);
    if (!t->end) {
      L->in_cell = 1;
      L->cell_runs = L->d->nruns;
    }
    return;
  }
  if (is(n, "table")) {
    bflush(L);
    L->in_cell = 0;
    return;
  }
  if (is(n, "form")) {
    form_tag(L, t);
    if (!(L->in_cell && L->d->nruns == L->cell_runs)) bflush(L);
    return;
  }
  if (is(n, "summary")) {
    bflush(L);
    if (!t->end && !L->hide) {
      char m[3];
      int det_open = 1, k;
      for (k = L->nframe - 2; k >= 0; k--)
        if (is(L->frame[k].tag, "details")) {
          det_open = !L->frame[k].closed_details;
          break;
        }
      m[0] = L->cp437 ? (char)(det_open ? 0x1F : 0x10) : (det_open ? 'v' : '>');
      m[1] = 0;
      inline_word(L, m);
      L->space_pending = 1;
    }
    return;
  }
  if (tag_is_block(n)) {
    /* dentro de uma celula ainda vazia, um bloco nao quebra a linha */
    if (L->in_cell && L->d->nruns == L->cell_runs) return;
    bflush(L);
    return;
  }
  if (is(n, "a")) {
    if (t->end) L->link = -1;
    else if (t->href[0] && !L->measure) L->link = add_link(L, t->href);
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
  if (is(n, "button")) {
    /* o conteudo do botao vira o campo: clicar envia o formulario */
    if (t->end) {
      if (L->button >= 0 && L->button < L->d->nfields) {
        int k;
        doc_field_t* f = &L->d->fields[L->button];
        for (k = 0; k < L->d->nruns; k++)
          if (L->d->runs[k].field == L->button) {
            f->line = L->d->runs[k].line;
            f->col = L->d->runs[k].col;
            f->len = L->d->runs[k].len;
            break;
          }
      }
      L->field = -1;
      L->button = -1;
    } else if (!L->measure && L->form >= 0 && !is(t->type, "button") && !is(t->type, "reset") &&
               L->d->nfields < DOC_FIELDS && !L->hide) {
      doc_field_t* f = &L->d->fields[L->d->nfields];
      memset(f, 0, sizeof(*f));
      f->submit = 1;
      f->form = L->form;
      f->line = -1;
      copy_z(f->name, (int)sizeof(f->name), t->fname);
      copy_z(f->value, (int)sizeof(f->value), t->value);
      L->button = L->d->nfields++;
      L->field = L->button;
    }
    return;
  }
  if (is(n, "img") && !t->end) {
    place_image(L, t);
    return;
  }
  if (is(n, "input") && !t->end) {
    int submit = is(t->type, "submit") || is(t->type, "button") || is(t->type, "image") ||
                 is(t->type, "reset");
    int hidden = is(t->type, "hidden");
    int check = is(t->type, "checkbox") ? 1 : is(t->type, "radio") ? 2 : 0;
    if (is(t->type, "file")) return;
    if (is(t->type, "image") && !t->value[0]) copy_z(t->value, (int)sizeof(t->value), t->alt[0] ? t->alt : "Enviar");
    place_field(L, t, submit, hidden, check);
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

/* width="85%" -> -85 */
static int attr_len(const char* v, int n) {
  int x = attr_num(v, n);
  if (memchr(v, '%', (size_t)n)) return x > 100 ? -100 : -x;
  return x;
}

/* le uma tag a partir de s[0] == '<'; devolve bytes consumidos */
static int parse_tag(const char* s, int n, tag_t* t, lay_t* L) {
  int i = 1, k = 0;
  memset(t, 0, sizeof(*t));
  t->border = -1;
  t->span = 1;
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
    /* atributos booleanos: valem com ou sem valor */
    if (is(an, "checked")) t->checked = 1;
    else if (is(an, "selected")) t->selected = 1;
    else if (is(an, "hidden")) t->hidden = 1;
    else if (is(an, "open")) t->open = 1;
    else if (is(an, "nowrap")) t->nowrap = 1;
    if (!v) continue;
    if (is(an, "href")) attr_copy(t->href, (int)sizeof(t->href), v, vl, L);
    else if (is(an, "src")) attr_copy(t->src, (int)sizeof(t->src), v, vl, L);
    else if (is(an, "alt")) attr_copy(t->alt, (int)sizeof(t->alt), v, vl, L);
    else if (is(an, "width")) t->width = attr_len(v, vl);
    else if (is(an, "height")) t->height = attr_num(v, vl);
    else if (is(an, "name")) attr_copy(t->fname, (int)sizeof(t->fname), v, vl, L);
    else if (is(an, "action")) attr_copy(t->action, (int)sizeof(t->action), v, vl, L);
    else if (is(an, "size")) t->isize = attr_num(v, vl);
    else if (is(an, "type")) {
      attr_copy(t->type, (int)sizeof(t->type), v, vl, L);
      for (k = 0; t->type[k]; k++) t->type[k] = (char)tolower((unsigned char)t->type[k]);
    } else if (is(an, "value")) attr_copy(t->value, (int)sizeof(t->value), v, vl, L);
    else if (is(an, "id")) attr_copy(t->id, (int)sizeof(t->id), v, vl, L);
    else if (is(an, "class")) attr_copy(t->cls, (int)sizeof(t->cls), v, vl, L);
    else if (is(an, "style")) attr_copy(t->style, (int)sizeof(t->style), v, vl, L);
    else if (is(an, "bgcolor")) attr_copy(t->bgcolor, (int)sizeof(t->bgcolor), v, vl, L);
    else if (is(an, "color")) attr_copy(t->color, (int)sizeof(t->color), v, vl, L);
    else if (is(an, "text")) attr_copy(t->text, (int)sizeof(t->text), v, vl, L);
    else if (is(an, "link")) attr_copy(t->linkc, (int)sizeof(t->linkc), v, vl, L);
    else if (is(an, "placeholder")) attr_copy(t->placeholder, (int)sizeof(t->placeholder), v, vl, L);
    else if (is(an, "align") || is(an, "valign")) {
      char* dst = is(an, "align") ? t->align : t->valign;
      attr_copy(dst, 12, v, vl, L);
      for (k = 0; dst[k]; k++) dst[k] = (char)tolower((unsigned char)dst[k]);
    } else if (is(an, "colspan")) {
      t->span = attr_num(v, vl);
      if (t->span < 1) t->span = 1;
      if (t->span > TABLE_COLS) t->span = TABLE_COLS;
    } else if (is(an, "border")) t->border = vl ? attr_num(v, vl) : 1;
    else if (is(an, "start")) t->start = attr_num(v, vl);
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

/* ---------- valores CSS ---------- */
static int hexdig(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

typedef struct {
  const char* name;
  unsigned rgb;
} named_color_t;

static const named_color_t named_colors[] = {
  { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
  { "lime", 0x00FF00 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "gray", 0x808080 },
  { "grey", 0x808080 }, { "orange", 0xFFA500 }, { "purple", 0x800080 }, { "navy", 0x000080 },
  { "maroon", 0x800000 }, { "teal", 0x008080 }, { "silver", 0xC0C0C0 }, { "aqua", 0x00FFFF },
  { "cyan", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF }, { "olive", 0x808000 },
  { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 },
  { "lightgrey", 0xD3D3D3 }, { "gainsboro", 0xDCDCDC }, { "whitesmoke", 0xF5F5F5 },
  { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "darkblue", 0x00008B },
  { "darkred", 0x8B0000 }, { "darkgreen", 0x006400 }, { "crimson", 0xDC143C },
  { "coral", 0xFF7F50 }, { "gold", 0xFFD700 }, { "pink", 0xFFC0CB }, { "brown", 0xA52A2A },
  { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C }, { "indigo", 0x4B0082 },
  { "violet", 0xEE82EE }, { "tomato", 0xFF6347 }, { "salmon", 0xFA8072 },
  { "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 }, { "slategray", 0x708090 },
  { "slategrey", 0x708090 }, { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 },
  { "lightyellow", 0xFFFFE0 }, { "orangered", 0xFF4500 }, { "royalblue", 0x4169E1 },
  { "seagreen", 0x2E8B57 }, { "tan", 0xD2B48C }, { "wheat", 0xF5DEB3 },
  { "aliceblue", 0xF0F8FF }, { "ghostwhite", 0xF8F8FF }, { "snow", 0xFFFAFA },
  { "linen", 0xFAF0E6 }, { "honeydew", 0xF0FFF0 }, { "azure", 0xF0FFFF },
  { "mintcream", 0xF5FFFA }, { "lavender", 0xE6E6FA }, { "darkslategray", 0x2F4F4F },
  { "darkslategrey", 0x2F4F4F }, { "midnightblue", 0x191970 }, { "dodgerblue", 0x1E90FF },
  { "firebrick", 0xB22222 }, { "forestgreen", 0x228B22 }, { "goldenrod", 0xDAA520 },
  { "darkorange", 0xFF8C00 }, { "chocolate", 0xD2691E }, { "sienna", 0xA0522D },
  { "peru", 0xCD853F }, { "olivedrab", 0x6B8E23 }, { "cadetblue", 0x5F9EA0 },
  { "turquoise", 0x40E0D0 }, { "cornflowerblue", 0x6495ED }, { "lightcyan", 0xE0FFFF },
  { "lightpink", 0xFFB6C1 }, { "lightsalmon", 0xFFA07A }, { "palegreen", 0x98FB98 },
  { "papayawhip", 0xFFEFD5 }, { "oldlace", 0xFDF5E6 }, { "seashell", 0xFFF5EE },
  { "floralwhite", 0xFFFAF0 }, { "lemonchiffon", 0xFFFACD }, { "cornsilk", 0xFFF8DC },
  { "darkcyan", 0x008B8B }, { "darkmagenta", 0x8B008B }, { "darkviolet", 0x9400D3 },
  { "deeppink", 0xFF1493 }, { "deepskyblue", 0x00BFFF }, { "hotpink", 0xFF69B4 },
  { "lightslategray", 0x778899 }, { "lightsteelblue", 0xB0C4DE }, { "mediumblue", 0x0000CD },
  { "rebeccapurple", 0x663399 }, { "darkkhaki", 0xBDB76B }, { "darkolivegreen", 0x556B2F },
  { 0, 0 }
};

/* numero CSS em centesimos ("1.5" -> 150); devolve bytes lidos, 0 se nao e numero */
static int css_num(const char* s, int n, int* out) {
  int i = 0, neg = 0, v = 0, frac = 0, fd = 0, any = 0;
  if (i < n && (s[i] == '-' || s[i] == '+')) {
    neg = s[i] == '-';
    i++;
  }
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    if (v < 1000000) v = v * 10 + (s[i] - '0');
    i++;
    any = 1;
  }
  if (i < n && s[i] == '.') {
    i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (fd < 2) {
        frac = frac * 10 + (s[i] - '0');
        fd++;
      }
      i++;
      any = 1;
    }
  }
  if (!any) return 0;
  while (fd < 2) {
    frac *= 10;
    fd++;
  }
  v = v * 100 + frac;
  *out = neg ? -v : v;
  return i;
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* mistura com branco: alfa em centesimos */
static unsigned blend_white(int r, int g, int b, int a) {
  if (a >= 100) return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
  if (a < 0) a = 0;
  r = (r * a + 255 * (100 - a)) / 100;
  g = (g * a + 255 * (100 - a)) / 100;
  b = (b * a + 255 * (100 - a)) / 100;
  return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static int hue_part(int p, int q, int t) {
  /* t em graus*100, p e q em 0..25500 */
  while (t < 0) t += 36000;
  while (t >= 36000) t -= 36000;
  if (t < 6000) return p + (q - p) * t / 6000;
  if (t < 18000) return q;
  if (t < 24000) return p + (q - p) * (24000 - t) / 6000;
  return p;
}

/* rgb()/rgba()/hsl()/hsla(), com virgulas ou espacos e "/ alfa" */
static int css_func_color(const char* s, int n, unsigned* out) {
  int hsl = tolower((unsigned char)s[0]) == 'h';
  int v[4], pct[4], k = 0, i = 0;
  while (i < n && s[i] != '(') i++;
  if (i >= n) return 0;
  i++;
  v[3] = 100;
  pct[3] = 0;
  while (i < n && k < 4) {
    int x, m;
    while (i < n && (isspace((unsigned char)s[i]) || s[i] == ',' || s[i] == '/')) i++;
    if (i >= n || s[i] == ')') break;
    m = css_num(s + i, n - i, &x);
    if (!m) return 0;
    i += m;
    pct[k] = 0;
    if (i < n && s[i] == '%') {
      pct[k] = 1;
      i++;
    }
    while (i < n && isalpha((unsigned char)s[i])) i++; /* deg */
    v[k++] = x;
  }
  if (k < 3) return 0;
  if (k == 4) v[3] = pct[3] ? v[3] / 100 : v[3]; /* centesimos */
  else v[3] = 100;
  if (!hsl) {
    int c[3], j;
    for (j = 0; j < 3; j++) c[j] = clamp255(pct[j] ? v[j] * 255 / 10000 : v[j] / 100);
    if (v[3] <= 5) return 0; /* quase transparente */
    *out = blend_white(c[0], c[1], c[2], v[3]);
    return 1;
  }
  {
    int h = v[0], sat = v[1] / 100, l = v[2] / 100; /* h em graus*100, s e l em 0..100 */
    int q, p, r, g, b;
    if (sat < 0) sat = 0;
    if (sat > 100) sat = 100;
    if (l < 0) l = 0;
    if (l > 100) l = 100;
    q = l < 50 ? l * (100 + sat) : l * 100 + sat * 100 - l * sat; /* 0..10000 */
    p = 2 * l * 100 - q;
    q = q * 255 / 100;
    p = p * 255 / 100; /* 0..25500 */
    r = hue_part(p, q, h + 12000) / 100;
    g = hue_part(p, q, h) / 100;
    b = hue_part(p, q, h - 12000) / 100;
    if (v[3] <= 5) return 0;
    *out = blend_white(clamp255(r), clamp255(g), clamp255(b), v[3]);
    return 1;
  }
}

static int css_color(const char* s, int n, unsigned* out) {
  char buf[32];
  int i;
  while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
  while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
  if (n >= 4 && s[0] == '#') {
    int r, g, b;
    for (i = 1; i < n && hexdig(s[i]) >= 0; i++) {
    }
    if (i == 7 || i == 9) {
      r = (hexdig(s[1]) << 4) | hexdig(s[2]);
      g = (hexdig(s[3]) << 4) | hexdig(s[4]);
      b = (hexdig(s[5]) << 4) | hexdig(s[6]);
      if (i == 9) {
        int a = ((hexdig(s[7]) << 4) | hexdig(s[8])) * 100 / 255;
        if (a <= 5) return 0;
        *out = blend_white(r, g, b, a);
      } else
        *out = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
      return 1;
    }
    if (i == 4 || i == 5) {
      r = hexdig(s[1]);
      g = hexdig(s[2]);
      b = hexdig(s[3]);
      *out = ((unsigned)(r * 17) << 16) | ((unsigned)(g * 17) << 8) | (unsigned)(b * 17);
      return 1;
    }
    return 0;
  }
  if (n > 4 && (strncasecmp(s, "rgb", 3) == 0 || strncasecmp(s, "hsl", 3) == 0))
    return css_func_color(s, n, out);
  if (n <= 0 || n >= (int)sizeof buf) return 0;
  for (i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
  buf[n] = 0;
  for (i = 0; named_colors[i].name; i++)
    if (!strcmp(buf, named_colors[i].name)) {
      *out = named_colors[i].rgb;
      return 1;
    }
  return 0;
}

/* Primeira cor no valor: "url(x.png) #fff no-repeat", "linear-gradient(#a, #b)". */
static int css_find_color(const char* s, int n, unsigned* out) {
  int i = 0;
  while (i < n) {
    int j;
    while (i < n && (isspace((unsigned char)s[i]) || s[i] == ',' || s[i] == '(' || s[i] == ')')) i++;
    if (i >= n) break;
    if (i + 4 <= n && strncasecmp(s + i, "url(", 4) == 0) {
      while (i < n && s[i] != ')') i++;
      continue;
    }
    if (i + 3 <= n && (strncasecmp(s + i, "rgb", 3) == 0 || strncasecmp(s + i, "hsl", 3) == 0)) {
      j = i;
      while (j < n && s[j] != ')') j++;
      if (j < n) j++;
      if (css_color(s + i, j - i, out)) return 1;
      i = j;
      continue;
    }
    j = i;
    while (j < n && !isspace((unsigned char)s[j]) && s[j] != ',' && s[j] != '(' && s[j] != ')') j++;
    if (css_color(s + i, j - i, out)) return 1;
    i = j;
  }
  return 0;
}

/* comprimento -> px (ou -porcento). 0 se auto ou desconhecido. */
static int css_len(const char* s, int n, int* out) {
  int v, m;
  while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
  m = css_num(s, n, &v);
  if (!m) return 0;
  s += m;
  n -= m;
  if (n > 0 && s[0] == '%') v = -(v / 100);
  else if (n >= 2 && (strncasecmp(s, "em", 2) == 0 || strncasecmp(s, "re", 2) == 0)) v = v * 16 / 100;
  else if (n >= 2 && strncasecmp(s, "ch", 2) == 0) v = v * 8 / 100;
  else if (n >= 2 && strncasecmp(s, "pt", 2) == 0) v = v * 4 / 300;
  else if (n >= 2 && strncasecmp(s, "vw", 2) == 0) v = -(v / 100);
  else v = v / 100;
  *out = v;
  return 1;
}

static int word_eq(const char* s, int n, const char* w) {
  int k = (int)strlen(w);
  return n == k && strncasecmp(s, w, (size_t)k) == 0;
}

/* 1 se a palavra w aparece em s (separada por espacos, virgulas ou parenteses) */
static int has_word(const char* s, int n, const char* w) {
  int k = (int)strlen(w), i;
  for (i = 0; i + k <= n; i++) {
    if (strncasecmp(s + i, w, (size_t)k) != 0) continue;
    if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '-')) continue;
    if (i + k < n && (isalnum((unsigned char)s[i + k]) || s[i + k] == '-')) continue;
    return 1;
  }
  return 0;
}

/* palavra k (0..) do valor */
static int css_token(const char* s, int n, int k, const char** ts) {
  int i = 0;
  for (;;) {
    int j;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i >= n) return 0;
    j = i;
    while (j < n && !isspace((unsigned char)s[j])) j++;
    if (k-- == 0) {
      *ts = s + i;
      return j - i;
    }
    i = j;
  }
}

static int ntokens(const char* s, int n) {
  const char* t;
  int k = 0;
  while (css_token(s, n, k, &t)) k++;
  return k;
}

static int is_zero(const char* s, int n) {
  int v;
  if (!n) return 0;
  return css_num(s, n, &v) > 0 && v == 0;
}

/* border: 1px solid #ccc -> ligado e cor */
static void css_border(css_props_t* p, const char* v, int n, int sides) {
  unsigned col;
  int on;
  if (has_word(v, n, "none") || has_word(v, n, "hidden") || word_eq(v, n, "0") || is_zero(v, n) ||
      has_word(v, n, "transparent"))
    on = 0;
  else
    on = has_word(v, n, "solid") || has_word(v, n, "dashed") || has_word(v, n, "dotted") ||
         has_word(v, n, "double") || has_word(v, n, "groove") || has_word(v, n, "ridge") ||
         has_word(v, n, "inset") || has_word(v, n, "outset");
  p->bset |= sides;
  if (on) p->bval |= sides;
  else p->bval &= ~sides;
  if (on && css_find_color(v, n, &col)) {
    p->bcolor = col;
    p->set |= P_BCOLOR;
  }
}

static int css_var_find(css_t* c, const char* name, int n) {
  int i;
  for (i = c->nvars - 1; i >= 0; i--)
    if ((int)strlen(c->vars[i].name) == n && strncmp(c->vars[i].name, name, (size_t)n) == 0) return i;
  return -1;
}

/* copia v trocando var(--x, reserva) pelo valor */
static int css_resolve(css_t* c, const char* v, int n, char* out, int max, int depth) {
  int i = 0, o = 0;
  while (i < n && o < max - 1) {
    if (c && depth < 4 && i + 4 <= n && strncasecmp(v + i, "var(", 4) == 0) {
      int j = i + 4, d = 1, k, comma = -1, ns, ne, idx;
      while (j < n && d) {
        if (v[j] == '(') d++;
        else if (v[j] == ')') d--;
        else if (v[j] == ',' && d == 1 && comma < 0) comma = j;
        if (d) j++;
      }
      ns = i + 4;
      ne = comma >= 0 ? comma : j;
      while (ns < ne && isspace((unsigned char)v[ns])) ns++;
      while (ne > ns && isspace((unsigned char)v[ne - 1])) ne--;
      idx = css_var_find(c, v + ns, ne - ns);
      if (idx >= 0) {
        const char* val = c->vars[idx].val;
        o += css_resolve(c, val, (int)strlen(val), out + o, max - o, depth + 1);
      } else if (comma >= 0) {
        k = comma + 1;
        o += css_resolve(c, v + k, j - k, out + o, max - o, depth + 1);
      }
      i = j < n ? j + 1 : n;
      continue;
    }
    out[o++] = v[i++];
  }
  out[o] = 0;
  return o;
}

static void css_decl(css_t* c, css_props_t* p, const char* prop, const char* v, int n) {
  unsigned col;
  int x;
  (void)c;
  if (is(prop, "color")) {
    if (css_color(v, n, &col)) {
      p->color = col;
      p->set |= P_COLOR;
    }
  } else if (is(prop, "background-color") || is(prop, "background")) {
    if (css_find_color(v, n, &col)) {
      p->bg = col;
      p->set |= P_BG;
    } else if (word_eq(v, n, "none") || word_eq(v, n, "transparent")) {
      p->set &= ~P_BG;
    }
  } else if (is(prop, "font-weight")) {
    if (has_word(v, n, "bold") || has_word(v, n, "bolder") || (n == 3 && v[0] >= '6' && v[0] <= '9')) {
      p->bold = 1;
      p->set |= P_BOLD;
    } else if (has_word(v, n, "normal") || has_word(v, n, "lighter") || (n == 3 && v[0] >= '1' && v[0] <= '5')) {
      p->bold = -1;
      p->set |= P_BOLD;
    }
  } else if (is(prop, "font")) {
    if (has_word(v, n, "bold") || has_word(v, n, "600") || has_word(v, n, "700") || has_word(v, n, "800") ||
        has_word(v, n, "900")) {
      p->bold = 1;
      p->set |= P_BOLD;
    }
  } else if (is(prop, "text-decoration") || is(prop, "text-decoration-line")) {
    if (has_word(v, n, "underline")) {
      p->uline = 1;
      p->set |= P_ULINE;
    } else if (has_word(v, n, "none")) {
      p->uline = -1;
      p->set |= P_ULINE;
    }
  } else if (is(prop, "display")) {
    int d = -1;
    if (word_eq(v, n, "none")) d = D_NONE;
    else if (word_eq(v, n, "inline") || word_eq(v, n, "inline-block") || word_eq(v, n, "inline-flex") ||
             word_eq(v, n, "inline-grid"))
      d = D_INLINE;
    else if (word_eq(v, n, "block") || word_eq(v, n, "list-item") || word_eq(v, n, "flow-root") ||
             word_eq(v, n, "grid"))
      d = D_BLOCK;
    else if (word_eq(v, n, "flex")) d = D_FLEX;
    else if (n >= 5 && strncasecmp(v, "table", 5) == 0) d = D_UNSET;
    else if (word_eq(v, n, "contents")) d = D_UNSET;
    if (d >= 0) {
      p->display = d;
      p->set |= P_DISPLAY;
    }
  } else if (is(prop, "visibility")) {
    p->hidden = (has_word(v, n, "hidden") || has_word(v, n, "collapse")) ? 1 : -1;
    p->set |= P_VIS;
  } else if (is(prop, "opacity")) {
    if (is_zero(v, n)) {
      p->hidden = 1;
      p->set |= P_VIS;
    }
  } else if (is(prop, "text-align")) {
    if (has_word(v, n, "center") || has_word(v, n, "-webkit-center")) p->align = A_CENTER;
    else if (has_word(v, n, "right") || has_word(v, n, "end")) p->align = A_RIGHT;
    else p->align = A_LEFT;
    p->set |= P_ALIGN;
  } else if (is(prop, "text-transform")) {
    p->upper = has_word(v, n, "uppercase") ? 1 : has_word(v, n, "lowercase") ? 2 : -1;
    p->set |= P_UPPER;
  } else if (is(prop, "white-space")) {
    if (has_word(v, n, "pre") || has_word(v, n, "pre-wrap") || has_word(v, n, "break-spaces")) p->ws = 1;
    else if (has_word(v, n, "nowrap")) p->ws = 2;
    else p->ws = 0;
    p->set |= P_WS;
  } else if (is(prop, "list-style") || is(prop, "list-style-type")) {
    p->list_none = has_word(v, n, "none") ? 1 : -1;
    p->set |= P_LIST;
  } else if (is(prop, "border")) {
    css_border(p, v, n, RECT_SIDES);
  } else if (is(prop, "border-top")) {
    css_border(p, v, n, RECT_TOP);
  } else if (is(prop, "border-bottom")) {
    css_border(p, v, n, RECT_BOTTOM);
  } else if (is(prop, "border-left") || is(prop, "border-inline-start")) {
    css_border(p, v, n, RECT_LEFT);
  } else if (is(prop, "border-right") || is(prop, "border-inline-end")) {
    css_border(p, v, n, RECT_RIGHT);
  } else if (is(prop, "border-style")) {
    if (has_word(v, n, "none") || has_word(v, n, "hidden")) {
      p->bset |= RECT_SIDES;
      p->bval &= ~RECT_SIDES;
    } else {
      p->bset |= RECT_SIDES;
      p->bval |= RECT_SIDES;
    }
  } else if (is(prop, "border-width")) {
    if (is_zero(v, n) && ntokens(v, n) == 1) {
      p->bset |= RECT_SIDES;
      p->bval &= ~RECT_SIDES;
    }
  } else if (is(prop, "border-color")) {
    if (css_find_color(v, n, &col)) {
      p->bcolor = col;
      p->set |= P_BCOLOR;
    }
  } else if (is(prop, "width")) {
    if (css_len(v, n, &x) && x != 0) {
      p->width = x;
      p->set |= P_WIDTH;
    }
  } else if (is(prop, "height")) {
    if (css_len(v, n, &x) && x > 0) {
      p->height = x;
      p->set |= P_HEIGHT;
    }
  } else if (is(prop, "margin") || is(prop, "padding")) {
    const char* t;
    int k = ntokens(v, n), rl, ll;
    const char* rt;
    const char* lt;
    rl = css_token(v, n, k == 1 ? 0 : 1, &rt);
    ll = css_token(v, n, k == 1 ? 0 : k == 4 ? 3 : 1, &lt);
    t = rt;
    if (is(prop, "margin")) {
      p->mauto = (rl && word_eq(t, rl, "auto")) ? 1 : -1;
      p->set |= P_MAUTO;
    } else {
      p->nopad = (ll && is_zero(lt, ll)) ? 1 : -1;
      p->set |= P_NOPAD;
    }
    if (rl && css_len(rt, rl, &x)) {
      p->spr = x >= 3 ? 1 : -1;
      p->set |= P_SPR;
    }
    if (ll && css_len(lt, ll, &x)) {
      p->spl = x >= 3 ? 1 : -1;
      p->set |= P_SPL;
    }
  } else if (is(prop, "margin-left") || is(prop, "margin-right") || is(prop, "margin-inline") ||
             is(prop, "margin-inline-start") || is(prop, "margin-inline-end") ||
             is(prop, "padding-right") || is(prop, "padding-inline-end")) {
    int left = strstr(prop, "left") || strstr(prop, "start"), both = is(prop, "margin-inline");
    if (has_word(v, n, "auto") && prop[0] == 'm') {
      p->mauto = 1;
      p->set |= P_MAUTO;
    } else if (css_len(v, n, &x)) {
      if (left || both) {
        p->spl = x >= 3 ? 1 : -1;
        p->set |= P_SPL;
      }
      if (!left || both) {
        p->spr = x >= 3 ? 1 : -1;
        p->set |= P_SPR;
      }
    }
  } else if (is(prop, "padding-left") || is(prop, "padding-inline-start")) {
    p->nopad = is_zero(v, n) ? 1 : -1;
    p->set |= P_NOPAD;
    if (css_len(v, n, &x)) {
      p->spl = x >= 3 ? 1 : -1;
      p->set |= P_SPL;
    }
  } else if (is(prop, "float")) {
    p->flt = has_word(v, n, "right") || has_word(v, n, "inline-end") ? 2
           : has_word(v, n, "left") || has_word(v, n, "inline-start") ? 1 : 0;
    p->set |= P_FLOAT;
  } else if (is(prop, "flex-direction") || is(prop, "flex-flow")) {
    p->flexcol = has_word(v, n, "column") || has_word(v, n, "column-reverse") ? 1 : -1;
    p->set |= P_FLEXDIR;
  } else if (is(prop, "vertical-align")) {
    if (has_word(v, n, "middle")) p->valign = V_MIDDLE;
    else if (has_word(v, n, "bottom") || has_word(v, n, "text-bottom")) p->valign = V_BOTTOM;
    else p->valign = V_TOP;
    p->set |= P_VALIGN;
  } else if (is(prop, "left") || is(prop, "top") || is(prop, "text-indent") || is(prop, "right") ||
             is(prop, "margin-top")) {
    /* "pule para o conteudo" e textos trocados por figura: fora da tela */
    if (css_len(v, n, &x) && x <= -999) {
      p->off = 1;
      p->set |= P_OFF;
    }
  } else if (is(prop, "clip")) {
    if (n >= 5 && strncasecmp(v, "rect(", 5) == 0) {
      p->off = 1;
      p->set |= P_OFF;
    }
  } else if (is(prop, "clip-path")) {
    if (has_word(v, n, "inset") && (strstr(v, "50%") || strstr(v, "100%"))) {
      p->off = 1;
      p->set |= P_OFF;
    }
  } else if (is(prop, "font-size")) {
    if (is_zero(v, n)) {
      p->off = 1;
      p->set |= P_OFF;
    }
  }
}

/* ate o proximo ';' de fora de aspas e parenteses */
static int decl_end(const char* s, int i, int n) {
  int d = 0;
  char q = 0;
  while (i < n) {
    char c = s[i];
    if (q) {
      if (c == '\\') i++;
      else if (c == q) q = 0;
    } else if (c == '"' || c == '\'') q = c;
    else if (c == '(') d++;
    else if (c == ')') {
      if (d) d--;
    } else if (c == ';' && !d) break;
    i++;
  }
  return i;
}

/* vars = 1: so guarda --propriedades */
static void css_decls(css_t* c, css_props_t* r, const char* s, int n, int vars) {
  int i = 0;
  while (i < n) {
    char prop[40];
    char val[400];
    int ps, pe, vs, ve, k, pl, vl;
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
    i = decl_end(s, i, n);
    ve = i;
    while (ps < pe && isspace((unsigned char)s[ps])) ps++;
    while (pe > ps && isspace((unsigned char)s[pe - 1])) pe--;
    while (vs < ve && isspace((unsigned char)s[vs])) vs++;
    while (ve > vs && isspace((unsigned char)s[ve - 1])) ve--;
    if (ve - vs >= 10 && strncasecmp(s + ve - 10, "!important", 10) == 0) {
      ve -= 10;
      while (ve > vs && isspace((unsigned char)s[ve - 1])) ve--;
    }
    pl = pe - ps;
    if (pl <= 0 || pl >= (int)sizeof prop) continue;
    if (pl > 2 && s[ps] == '-' && s[ps + 1] == '-') {
      int idx;
      if (!vars || !c) continue;
      idx = css_var_find(c, s + ps, pl);
      if (idx < 0) {
        if (c->nvars >= CSS_VARS) continue;
        idx = c->nvars++;
        memcpy(c->vars[idx].name, s + ps, (size_t)pl);
        c->vars[idx].name[pl] = 0;
      }
      vl = ve - vs;
      if (vl > (int)sizeof c->vars[0].val - 1) vl = (int)sizeof c->vars[0].val - 1;
      memcpy(c->vars[idx].val, s + vs, (size_t)vl);
      c->vars[idx].val[vl] = 0;
      continue;
    }
    if (vars) continue;
    for (k = 0; k < pl; k++) prop[k] = (char)tolower((unsigned char)s[ps + k]);
    prop[pl] = 0;
    vl = css_resolve(c, s + vs, ve - vs, val, (int)sizeof val, 0);
    while (vl > 0 && isspace((unsigned char)val[vl - 1])) vl--;
    k = 0;
    while (k < vl && isspace((unsigned char)val[k])) k++;
    css_decl(c, r, prop, val + k, vl - k);
  }
  /* .sr-only: 1x1 px escondido */
  if ((r->set & P_WIDTH) && (r->set & P_HEIGHT) && r->width == 1 && r->height == 1) {
    r->off = 1;
    r->set |= P_OFF;
  }
}

/* ---------- seletores ---------- */
static unsigned sel_hash(char kind, const char* s) {
  unsigned h = 2166136261u ^ (unsigned)(unsigned char)kind;
  for (; *s; s++) h = (h ^ (unsigned)(unsigned char)*s) * 16777619u;
  return h % CSS_HASH;
}

static int ident_char(int c) {
  return isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\';
}

/* nome longo demais: dst fica vazio e a regra cai (cortar faria casar errado) */
static int copy_ident(const char* s, int i, int n, char* dst, int max) {
  int k = 0, over = 0;
  while (i < n && ident_char((unsigned char)s[i])) {
    if (s[i] == '\\' && i + 1 < n) i++;
    if (k < max - 1) dst[k++] = s[i];
    else over = 1;
    i++;
  }
  dst[over ? 0 : k] = 0;
  return i;
}

/* Le um seletor (sem virgulas). 0 = ok; -1 = nao da para avaliar (a regra cai). */
static int parse_selector(const char* s, int n, css_rule_t* r) {
  sel_part_t tmp[SEL_PARTS];
  int np = 0, i = 0, comb = 0, spec = 0, k;
  while (i < n) {
    sel_part_t* p;
    int ws = 0;
    while (i < n && isspace((unsigned char)s[i])) {
      i++;
      ws = 1;
    }
    if (i >= n) break;
    if (s[i] == '>') {
      comb = 2;
      i++;
      continue;
    }
    if (s[i] == '+' || s[i] == '~' || s[i] == '[' || s[i] == '&' || s[i] == '|') return -1;
    if (ws && np > 0 && comb == 0) comb = 1;
    if (np >= SEL_PARTS) return -1;
    p = &tmp[np];
    memset(p, 0, sizeof *p);
    p->comb = np > 0 ? (comb ? comb : 1) : 0;
    comb = 0;
    if (s[i] == '*') {
      i++;
    } else if (ident_char((unsigned char)s[i])) {
      i = copy_ident(s, i, n, p->tag, (int)sizeof p->tag);
      if (!p->tag[0]) return -1;
      for (k = 0; p->tag[k]; k++) p->tag[k] = (char)tolower((unsigned char)p->tag[k]);
      spec += 1;
    }
    while (i < n && !isspace((unsigned char)s[i]) && s[i] != '>' && s[i] != '+' && s[i] != '~') {
      if (s[i] == '.') {
        if (p->ncls >= 3) return -1;
        i = copy_ident(s, i + 1, n, p->cls[p->ncls], (int)sizeof p->cls[0]);
        if (!p->cls[p->ncls][0]) return -1;
        p->ncls++;
        spec += 10;
      } else if (s[i] == '#') {
        i = copy_ident(s, i + 1, n, p->id, (int)sizeof p->id);
        if (!p->id[0]) return -1;
        spec += 100;
      } else if (s[i] == ':') {
        char ps[24];
        int el = (i + 1 < n && s[i + 1] == ':');
        i = copy_ident(s, i + 1 + el, n, ps, (int)sizeof ps);
        for (k = 0; ps[k]; k++) ps[k] = (char)tolower((unsigned char)ps[k]);
        if (i < n && s[i] == '(') {
          int d = 1;
          i++;
          while (i < n && d) {
            if (s[i] == '(') d++;
            else if (s[i] == ')') d--;
            i++;
          }
        }
        if (el) return -1; /* ::before, ::after, ::placeholder */
        if (is(ps, "link") || is(ps, "visited") || is(ps, "any-link")) spec += 10;
        else if (is(ps, "root")) {
          strcpy(p->tag, "html");
          spec += 10;
        } else if (is(ps, "not") || is(ps, "lang") || is(ps, "dir")) {
          /* quase sempre verdadeiro aqui */
        } else
          return -1; /* hover, focus, first-child, nth-child... */
      } else if (s[i] == '[') {
        return -1;
      } else {
        return -1;
      }
    }
    np++;
  }
  if (np == 0) return -1;
  /* da direita para a esquerda; comb vai para a parte da direita */
  for (k = 0; k < np; k++) {
    r->part[k] = tmp[np - 1 - k];
    r->part[k].comb = k + 1 < np ? tmp[np - 1 - k].comb : 0;
  }
  r->nparts = np;
  r->spec = spec;
  return 0;
}

static int has_class(const char* cls, const char* want) {
  int n = (int)strlen(want);
  const char* p = cls;
  if (n <= 0) return 0;
  while (*p) {
    int k = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) break;
    while (p[k] && p[k] != ' ' && p[k] != '\t' && p[k] != '\n') k++;
    if (k == n && strncmp(p, want, (size_t)n) == 0) return 1;
    p += k;
  }
  return 0;
}

static int part_match(const sel_part_t* p, const char* tag, const char* id, const char* cls) {
  int k;
  if (p->tag[0] && strcmp(p->tag, tag) != 0) return 0;
  if (p->id[0] && strcmp(p->id, id) != 0) return 0;
  for (k = 0; k < p->ncls; k++)
    if (!has_class(cls, p->cls[k])) return 0;
  return 1;
}

static int rule_match(const lay_t* L, const css_rule_t* r, const tag_t* t) {
  int k, fi;
  if (!part_match(&r->part[0], t->name, t->id, t->cls)) return 0;
  fi = L->nframe - 1;
  for (k = 1; k < r->nparts; k++) {
    const sel_part_t* p = &r->part[k];
    if (r->part[k - 1].comb == 2) {
      if (fi < 0 || !part_match(p, L->frame[fi].tag, L->frame[fi].id, L->frame[fi].cls)) return 0;
      fi--;
    } else {
      while (fi >= 0 && !part_match(p, L->frame[fi].tag, L->frame[fi].id, L->frame[fi].cls)) fi--;
      if (fi < 0) return 0;
      fi--;
    }
  }
  return 1;
}

static void css_add(css_t* c, const char* sel, int n, const css_props_t* props) {
  css_rule_t* r;
  const sel_part_t* p;
  unsigned h;
  while (n > 0 && isspace((unsigned char)*sel)) { sel++; n--; }
  while (n > 0 && isspace((unsigned char)sel[n - 1])) n--;
  if (n < 1 || n > 300 || c->n >= CSS_RULES) return;
  if (c->n >= c->cap) {
    int nc = c->cap ? c->cap * 2 : 128;
    css_rule_t* nr;
    if (nc > CSS_RULES) nc = CSS_RULES;
    nr = (css_rule_t*)realloc(c->rules, (size_t)nc * sizeof(css_rule_t));
    if (!nr) return;
    c->rules = nr;
    c->cap = nc;
  }
  r = &c->rules[c->n];
  memset(r, 0, sizeof *r);
  if (parse_selector(sel, n, r) != 0) return;
  r->p = *props;
  p = &r->part[0];
  if (p->id[0]) h = sel_hash('#', p->id);
  else if (p->ncls) h = sel_hash('.', p->cls[0]);
  else if (p->tag[0]) h = sel_hash('t', p->tag);
  else {
    r->next = c->any;
    c->any = c->n++;
    return;
  }
  r->next = c->head[h];
  c->head[h] = c->n++;
}

/* @media: so a largura da janela, tela e cor clara */
static int media_ok(css_t* c, const char* q, int n) {
  char b[200];
  int i, k;
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  for (i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)q[i]);
  b[n] = 0;
  if (strstr(b, "print") && !strstr(b, "screen") && !strstr(b, "all") && !strstr(b, "not print")) return 0;
  if (strstr(b, "speech")) return 0;
  if (strstr(b, "prefers-color-scheme") && strstr(b, "dark")) return 0;
  if (strstr(b, "portrait")) return 0;
  if (strstr(b, "hover") && strstr(b, "none")) return 0;
  if (strstr(b, "pointer") && strstr(b, "coarse")) return 0;
  for (i = 0; b[i]; i++) {
    int mx;
    if (strncmp(b + i, "max-width", 9) == 0) mx = 1;
    else if (strncmp(b + i, "min-width", 9) == 0) mx = 0;
    else continue;
    k = i + 9;
    while (b[k] == ' ' || b[k] == ':') k++;
    {
      int px;
      if (css_len(b + k, (int)strlen(b + k), &px) && px > 0) {
        if (mx && c->vw > px) return 0;
        if (!mx && c->vw < px) return 0;
      }
    }
  }
  return 1;
}

/* fim do bloco { ... } que comeca em s[i] == '{' */
static int block_end(const char* s, int i, int n) {
  int d = 0;
  char q = 0;
  while (i < n) {
    char ch = s[i];
    if (q) {
      if (ch == '\\') i++;
      else if (ch == q) q = 0;
    } else if (ch == '"' || ch == '\'') q = ch;
    else if (ch == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
      i++;
    } else if (ch == '{') d++;
    else if (ch == '}') {
      d--;
      if (d == 0) return i;
    }
    i++;
  }
  return n;
}

/* pass 0 junta as variaveis; pass 1 as regras */
static void css_parse_pass(css_t* c, const char* s, int n, int pass, int depth) {
  int i = 0;
  if (!c || !s || n <= 0 || depth > 4) return;
  while (i < n) {
    while (i < n && (isspace((unsigned char)s[i]) || s[i] == '}' || s[i] == ';')) i++;
    if (i >= n) break;
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
      if (i + 1 < n) i += 2;
      continue;
    }
    if (s[i] == '<' || (s[i] == '-' && i + 3 < n && s[i + 1] == '-' && s[i + 2] == '>')) {
      /* <!-- --> em volta do CSS antigo */
      while (i < n && !isspace((unsigned char)s[i])) i++;
      continue;
    }
    if (s[i] == '@') {
      char kw[16];
      int k = 0, ps, e;
      i++;
      while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-') && k < 15) kw[k++] = (char)tolower((unsigned char)s[i++]);
      kw[k] = 0;
      ps = i;
      while (i < n && s[i] != '{' && s[i] != ';') i++;
      if (i >= n) break;
      if (s[i] == ';') {
        i++;
        continue;
      }
      e = block_end(s, i, n);
      if ((is(kw, "media") && media_ok(c, s + ps, i - ps)) || is(kw, "supports") ||
          is(kw, "layer") || is(kw, "document") || is(kw, "container") || is(kw, "scope"))
        css_parse_pass(c, s + i + 1, e - i - 1, pass, depth + 1);
      i = e + 1;
      continue;
    }
    {
      int ss = i, se, bs, be;
      css_props_t props;
      while (i < n && s[i] != '{' && s[i] != '}') i++;
      if (i >= n) break;
      if (s[i] == '}') {
        i++;
        continue;
      }
      se = i;
      be = block_end(s, i, n);
      bs = i + 1;
      i = be + 1;
      memset(&props, 0, sizeof props);
      if (pass == 0) {
        /* variaveis so de :root, html, body e *: as de .tema-escuro nao valem
         * para a pagina toda */
        int k = ss, ok = 0;
        while (k < se && !ok) {
          int e = k, a, b;
          while (e < se && s[e] != ',') e++;
          a = k;
          b = e;
          while (a < b && isspace((unsigned char)s[a])) a++;
          while (b > a && isspace((unsigned char)s[b - 1])) b--;
          if (word_eq(s + a, b - a, ":root") || word_eq(s + a, b - a, "html") ||
              word_eq(s + a, b - a, "body") || word_eq(s + a, b - a, "*") ||
              word_eq(s + a, b - a, ":host"))
            ok = 1;
          k = e + 1;
        }
        if (ok) css_decls(c, &props, s + bs, be - bs, 1);
        continue;
      }
      /* CSS aninhado (a { b { } }): so as declaracoes de fora valem */
      {
        int k;
        for (k = bs; k < be; k++)
          if (s[k] == '{') break;
        if (k < be) {
          while (k > bs && s[k] != ';') k--;
          be = k;
        }
      }
      css_decls(c, &props, s + bs, be - bs, 0);
      if (!props.set && !props.bset) continue;
      while (ss < se) {
        int e = ss, d = 0;
        while (e < se && (s[e] != ',' || d)) {
          if (s[e] == '(') d++;
          else if (s[e] == ')' && d) d--;
          e++;
        }
        css_add(c, s + ss, e - ss, &props);
        ss = e + 1;
      }
    }
  }
}

static void css_parse(css_t* c, const char* s, int n) {
  css_parse_pass(c, s, n, 0, 0);
  css_parse_pass(c, s, n, 1, 0);
}

static void props_onto(const css_props_t* r, frm_t* f) {
  if (r->set & P_COLOR) {
    f->color = r->color;
    f->color_set = 1;
  }
  if (r->set & P_BG) {
    f->bg = r->bg;
    f->bg_set = 1;
  }
  if (r->set & P_BOLD) f->bold = r->bold;
  if (r->set & P_ULINE) f->uline = r->uline;
  if (r->set & P_DISPLAY) f->display = r->display;
  if (r->set & P_ALIGN) {
    f->align = r->align;
    f->legacy_center = 0;
  }
  if (r->set & P_UPPER) f->upper = r->upper > 0 ? r->upper : 0;
  if (r->set & P_WS) f->ws = r->ws;
  if (r->set & P_LIST) f->list_none = r->list_none;
  if (r->bset) f->border = (f->border & ~r->bset) | (r->bval & r->bset);
  if (r->set & P_BCOLOR) f->bcolor = r->bcolor;
  if (r->set & P_WIDTH) f->width = r->width;
  if (r->set & P_HEIGHT) f->height = r->height;
  if (r->set & P_VIS) f->hide = r->hidden > 0 ? (f->hide | 2) : (f->hide & ~2);
  if (r->set & P_OFF) f->hide |= 4;
  if (r->set & P_FLEXDIR) f->flexcol = r->flexcol > 0;
  if (r->set & P_MAUTO) f->mauto = r->mauto > 0;
  if (r->set & P_VALIGN) f->valign = r->valign;
  if (r->set & P_NOPAD) f->nopad = r->nopad;
  if (r->set & P_SPL) f->spl = r->spl;
  if (r->set & P_SPR) f->spr = r->spr;
  if (r->set & P_FLOAT) f->flt = r->flt;
}

#define MATCH_MAX 256

static void css_bucket(const lay_t* L, int head, const tag_t* t, int* got, int* ng) {
  const css_t* c = L->css;
  int r, k;
  for (r = head; r >= 0 && r < c->n; r = c->rules[r].next) {
    if (*ng >= MATCH_MAX) return;
    if (!rule_match(L, &c->rules[r], t)) continue;
    for (k = 0; k < *ng; k++)
      if (got[k] == r) break;
    if (k == *ng) got[(*ng)++] = r;
  }
}

/* Aplica as regras que casam com t, da menor para a maior especificidade. */
static void css_apply(const lay_t* L, const tag_t* t, frm_t* f) {
  const css_t* c = L->css;
  int got[MATCH_MAX];
  int ng = 0, i, j;
  if (!c || c->n == 0) return;
  if (t->id[0]) css_bucket(L, c->head[sel_hash('#', t->id)], t, got, &ng);
  {
    const char* p = t->cls;
    while (*p) {
      char one[48];
      int k = 0;
      while (*p == ' ' || *p == '\t' || *p == '\n') p++;
      if (!*p) break;
      while (p[k] && p[k] != ' ' && p[k] != '\t' && p[k] != '\n') k++;
      if (k < (int)sizeof one && k < 40) {
        memcpy(one, p, (size_t)k);
        one[k] = 0;
        css_bucket(L, c->head[sel_hash('.', one)], t, got, &ng);
      }
      p += k;
    }
  }
  css_bucket(L, c->head[sel_hash('t', t->name)], t, got, &ng);
  css_bucket(L, c->any, t, got, &ng);
  /* insercao: especificidade, depois ordem no arquivo */
  for (i = 1; i < ng; i++) {
    int v = got[i];
    j = i - 1;
    while (j >= 0 && (c->rules[got[j]].spec > c->rules[v].spec ||
                      (c->rules[got[j]].spec == c->rules[v].spec && got[j] > v))) {
      got[j + 1] = got[j];
      j--;
    }
    got[j + 1] = v;
  }
  for (i = 0; i < ng; i++) props_onto(&c->rules[got[i]].p, f);
}

/* ---------- estilo dos elementos ---------- */
static int attr_color(const char* s, unsigned* out) {
  if (!s[0]) return 0;
  if (css_color(s, (int)strlen(s), out)) return 1;
  /* bgcolor="ff6600" sem # */
  if (strlen(s) == 6) {
    char b[8];
    b[0] = '#';
    memcpy(b + 1, s, 7);
    return css_color(b, 7, out);
  }
  return 0;
}

static void frame_compute(lay_t* L, tag_t* t, frm_t* f) {
  frm_t* p = L->nframe > 0 ? &L->frame[L->nframe - 1] : 0;
  const char* n = t->name;
  unsigned col;
  memset(f, 0, sizeof *f);
  f->el = -1;
  f->rect = -1;
  copy_z(f->tag, (int)sizeof f->tag, n);
  copy_z(f->id, (int)sizeof f->id, t->id);
  copy_z(f->cls, (int)sizeof f->cls, t->cls);
  if (p) {
    f->color = p->color;
    f->color_set = p->color_set;
    f->bold = p->bold;
    f->uline = p->uline;
    f->align = p->align;
    f->upper = p->upper;
    f->ws = p->ws;
    f->list_none = p->list_none;
    f->ebg = p->ebg;
    f->rbg = p->rbg;
    f->rbg_set = p->rbg_set;
    f->legacy_center = p->legacy_center;
    if (p->closed_details && !is(n, "summary")) f->hide |= 1;
    if (is(p->tag, "tr") && (is(n, "td") || is(n, "th"))) f->valign = p->valign;
  } else {
    f->ebg = L->d->page_bg;
  }
  /* estilo do navegador */
  if (is(n, "a") && t->href[0]) {
    f->color_set = 0;
    f->uline = 0;
  }
  if (is(n, "center")) {
    f->align = A_CENTER;
    f->legacy_center = 1;
  }
  if (is(n, "th") || is(n, "caption")) f->align = A_CENTER;
  if (is(n, "th")) f->bold = 1;
  if (is(n, "mark")) {
    f->rbg = 0xFFFF00;
    f->rbg_set = 1;
  }
  if (is(n, "button")) {
    f->rbg = COL_BTN;
    f->rbg_set = 1;
  }
  if (is(n, "fieldset")) {
    f->border = RECT_SIDES;
    f->bcolor = 0xC0C0C0;
  }
  if (is(n, "table")) {
    /* como no modo quirks: a tabela nao herda o alinhamento de fora */
    f->align = A_LEFT;
    f->legacy_center = 0;
  }
  if ((is(n, "dialog") && !t->open) || t->hidden || is(n, "datalist")) f->hide |= 1;
  if (is(n, "details") && !t->open) f->closed_details = 1;
  /* atributos de apresentacao (perdem para o CSS) */
  if (attr_color(t->bgcolor, &col)) {
    f->bg = col;
    f->bg_set = 1;
  }
  if (attr_color(t->color, &col) && is(n, "font")) {
    f->color = col;
    f->color_set = 1;
  }
  if (is(n, "body")) {
    if (attr_color(t->text, &col)) {
      f->color = col;
      f->color_set = 1;
    }
    if (attr_color(t->linkc, &col)) L->link_color = col;
  }
  if (t->align[0] && !is(n, "img") && !is(n, "table") && !is(n, "input")) {
    if (is(t->align, "center") || is(t->align, "middle")) {
      f->align = A_CENTER;
      if (!is(n, "tr")) f->legacy_center = 1;
    } else if (is(t->align, "right")) f->align = A_RIGHT;
    else if (is(t->align, "left") || is(t->align, "justify")) {
      f->align = A_LEFT;
      f->legacy_center = 0;
    }
  }
  if (t->valign[0])
    f->valign = is(t->valign, "top") || is(t->valign, "baseline") ? V_TOP
              : is(t->valign, "bottom") ? V_BOTTOM : V_MIDDLE;
  if (t->nowrap) f->ws = 2;
  if (t->width) f->width = t->width;
  if (t->height) f->height = t->height;
  if (t->border > 0 && is(n, "table")) {
    f->border = RECT_SIDES;
    f->bcolor = 0x808080;
  }
  css_apply(L, t, f);
  if (t->style[0]) {
    css_props_t inl;
    memset(&inl, 0, sizeof inl);
    css_decls(L->css, &inl, t->style, (int)strlen(t->style), 0);
    props_onto(&inl, f);
  }
  if (f->display == D_NONE) f->hide |= 1;
  if (!f->bcolor) f->bcolor = 0xA0A0A0;
  f->blocky = (tag_is_block(n) && f->display != D_INLINE) || f->display == D_BLOCK || f->display == D_FLEX;
  if (f->bg_set) {
    f->ebg = f->bg;
    if (is(n, "body") || is(n, "html")) {
      L->d->page_bg = f->bg;
      f->rbg_set = 0;
    } else if (f->blocky) {
      f->rbg_set = 0;
    } else {
      f->rbg = f->bg;
      f->rbg_set = 1;
    }
  }
}

static void style_sync(lay_t* L) {
  frm_t* f;
  L->css_color_set = L->css_bg_set = 0;
  L->css_bold = L->css_uline = 0;
  L->align = A_LEFT;
  L->ws_pre = 0;
  L->upper = 0;
  L->nomark = 0;
  L->ebg = L->d->page_bg;
  if (L->nframe <= 0) return;
  f = &L->frame[L->nframe - 1];
  L->css_color_set = f->color_set;
  L->css_color = f->color;
  L->css_bg_set = f->rbg_set;
  L->css_bg = f->rbg;
  L->css_bold = f->bold;
  L->css_uline = f->uline;
  L->align = f->align;
  L->ws_pre = f->ws == 1;
  L->upper = f->upper;
  L->nomark = f->list_none == 1;
  L->ebg = f->ebg;
}

/* fecha os elementos de i para cima */
static void style_pop_to(lay_t* L, int i) {
  if (i < 0) i = 0;
  while (L->nframe > i) {
    frm_t* f = &L->frame[--L->nframe];
    if (f->el >= 0 && f->el < L->d->nels) L->d->els[f->el].run1 = L->d->nruns - 1;
    if (f->rect >= 0 && f->rect < L->d->nrects) {
      doc_rect_t* r = &L->d->rects[f->rect];
      if (L->last_emit >= r->line0) r->line1 = L->last_emit;
      else r->flags = 0;
    }
    if (f->hide && L->hide > 0) L->hide--;
  }
  style_sync(L);
}

static int frame_find(lay_t* L, const char* tag) {
  int i;
  for (i = L->nframe - 1; i >= L->frame_floor; i--)
    if (is(L->frame[i].tag, tag)) return i;
  return -1;
}

static void style_close_all(lay_t* L) { style_pop_to(L, 0); }

static int frame_push(lay_t* L, const frm_t* src, const tag_t* t) {
  frm_t* f;
  if (L->nframe >= CSS_DEPTH) return -1;
  f = &L->frame[L->nframe];
  *f = *src;
  if (t->id[0] && L->d->nels < DOC_ELS && !L->measure) {
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
  return L->nframe - 1;
}

/* <li> fecha o <li> aberto, <td> o <td>, <p> antes de um bloco... mas so
 * ate a fronteira certa: o <li> de uma lista de fora fica aberto. */
static void auto_close(lay_t* L, const char* n) {
  int i;
  const char* want;
  const char* stop;
  if (is(n, "li")) {
    want = "li";
    stop = "ul ol menu dir table td th body";
  } else if (is(n, "dt") || is(n, "dd")) {
    want = "dt dd";
    stop = "dl table td body";
  } else if (is(n, "td") || is(n, "th")) {
    want = "td th";
    stop = "tr table";
  } else if (is(n, "tr")) {
    want = "tr td th";
    stop = "table tbody thead tfoot";
  } else if (is(n, "tbody") || is(n, "thead") || is(n, "tfoot")) {
    want = "tbody thead tfoot tr td th";
    stop = "table";
  } else if (is(n, "option")) {
    want = "option";
    stop = "select";
  } else if (tag_is_block(n)) {
    want = "p";
    stop = "div section article header footer nav main aside blockquote li td th table ul ol "
           "dl form fieldset body figure details dialog";
  } else
    return;
  for (i = L->nframe - 1; i >= L->frame_floor; i--) {
    if (in_list(L->frame[i].tag, want)) {
      style_pop_to(L, i);
      return;
    }
    if (in_list(L->frame[i].tag, stop)) return;
  }
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
  if (body > RAW_MAX && !is(name, "style")) body = RAW_MAX;
  if (body > 0 && out) {
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
  if (L->measure || !is_js_type(t->type) || L->d->nscripts >= DOC_SCRIPTS) {
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
  L->button = -1;
  L->cp437 = cp437;
  L->last_emit = -1;
  L->band_runs = -1;
  L->glue_run = -1;
  L->fl_end = -1;
  L->link_color = COL_LINK;
  d->page_bg = 0xFFFFFF;
  L->ebg = 0xFFFFFF;
}

static void lay_html(lay_t* L, const char* html, int i, int len);

/* texto puro de html[s..e) (sem tags), na fonte do documento */
static void plain_text(lay_t* L, const char* html, int s, int e, char* out, int max) {
  int o = 0, i = s, sp = 0;
  while (i < e && o < max - 4) {
    unsigned char c = (unsigned char)html[i];
    unsigned cp;
    if (c == '<') {
      while (i < e && html[i] != '>') i++;
      i++;
      continue;
    }
    if (c == '&') {
      int k = entity(html + i, e - i, &cp);
      if (k > 0) i += k;
      else {
        cp = '&';
        i++;
      }
    } else if (c < 0x80 || L->latin1) {
      cp = c;
      i++;
    } else {
      int need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0, k;
      cp = need == 1 ? (c & 0x1Fu) : need == 2 ? (c & 0x0Fu) : (c & 0x07u);
      for (k = 1; k <= need && i + k < e && ((unsigned char)html[i + k] & 0xC0) == 0x80; k++)
        cp = (cp << 6) | ((unsigned char)html[i + k] & 0x3F);
      if (!need || k <= need) {
        cp = c;
        i++;
      } else
        i += need + 1;
    }
    if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == 0xA0) {
      sp = o > 0;
      continue;
    }
    if (sp) out[o++] = ' ';
    sp = 0;
    o += map_cp(cp, out + o, L->cp437);
  }
  out[o] = 0;
}

static int find_close(const char* html, int i, int len, const char* name) {
  int nl = (int)strlen(name);
  for (; i + nl + 2 <= len; i++)
    if (html[i] == '<' && html[i + 1] == '/' && strncasecmp(html + i + 2, name, (size_t)nl) == 0) return i;
  return len;
}

/* <select>: mostra a opcao escolhida e manda o valor no formulario */
static int do_select(lay_t* L, tag_t* t, const char* html, int i, int len) {
  int e = find_close(html, i, len, "select"), after = e, k;
  char text[64], value[128];
  tag_t* o = (tag_t*)malloc(sizeof(tag_t));
  frm_t tmp;
  int got = 0;
  text[0] = value[0] = 0;
  while (after < len && html[after] != '>') after++;
  if (after < len) after++;
  if (!o) return after;
  for (k = i; k < e; k++) {
    int te, ts;
    if (html[k] != '<' || strncasecmp(html + k + 1, "option", 6) != 0) continue;
    te = k + parse_tag(html + k, e - k, o, L);
    ts = te;
    while (te < e && html[te] != '<') te++;
    if (!got || o->selected) {
      plain_text(L, html, ts, te, text, (int)sizeof text);
      copy_z(value, (int)sizeof value, o->value[0] ? o->value : text);
      got = 1;
      if (o->selected) break;
    }
  }
  free(o);
  frame_compute(L, t, &tmp);
  copy_z(t->value, (int)sizeof t->value, value);
  place_field(L, t, 0, 1, 0);
  if (L->hide || tmp.hide) return after;
  {
    char b[80];
    int n = 0, m = 0;
    b[n++] = '[';
    while (text[m] && n < 70) b[n++] = text[m++];
    b[n++] = ' ';
    b[n++] = L->cp437 ? (char)0x1F : 'v';
    b[n++] = ']';
    b[n] = 0;
    button_word(L, b);
  }
  return after;
}

/* ---------- tabelas ---------- */
typedef struct {
  int tag_pos;  /* '<' do td/th */
  int s, e;     /* conteudo */
  int row, col, span;
  int minw, maxw, fixw;
  int strays;   /* tags soltas antes desta celula: [strays, proxima) em stray[] */
} tcell_t;

typedef struct {
  int tag_pos; /* '<' do tr; -1 implicito */
  int cell0;   /* primeira celula */
} trow_t;

typedef struct {
  tcell_t* cell;
  int ncell, ccap;
  trow_t* row;
  int nrow, rcap;
  int* stray;
  int nstray, scap;
  int cap_pos, cap_s, cap_e;
  int end; /* depois de </table> */
} tscan_t;

static void scan_close(tscan_t* S, int* open, int at) {
  if (*open >= 0) S->cell[*open].e = at;
  *open = -1;
}

/* Acha linhas e celulas da tabela, pulando tabelas de dentro. */
static void table_scan(lay_t* L, const char* html, int i, int len, tscan_t* S, tag_t* t) {
  int depth = 0, open = -1, cap_open = 0;
  memset(S, 0, sizeof *S);
  S->cap_pos = -1;
  S->end = len;
  while (i < len) {
    int ts, te;
    if (html[i] != '<' || i + 1 >= len) {
      i++;
      continue;
    }
    if (html[i + 1] == '!' && i + 3 < len && html[i + 2] == '-' && html[i + 3] == '-') {
      int k = i + 4;
      while (k + 2 < len && !(html[k] == '-' && html[k + 1] == '-' && html[k + 2] == '>')) k++;
      i = k + 3;
      continue;
    }
    if (html[i + 1] != '/' && !isalpha((unsigned char)html[i + 1])) {
      i++;
      continue;
    }
    ts = i;
    te = i + parse_tag(html + i, len - i, t, L);
    i = te;
    if (!t->end && !t->self_close && (is(t->name, "script") || is(t->name, "style") ||
                                      is(t->name, "textarea") || is(t->name, "title"))) {
      i = find_close(html, i, len, t->name);
      while (i < len && html[i] != '>') i++;
      i++;
      continue;
    }
    if (is(t->name, "table")) {
      if (!t->end) depth++;
      else if (depth > 0) depth--;
      else {
        scan_close(S, &open, ts);
        if (cap_open) S->cap_e = ts;
        S->end = te;
        return;
      }
      continue;
    }
    if (depth > 0) continue;
    if (is(t->name, "tr") && !t->end) {
      scan_close(S, &open, ts);
      S->row = (trow_t*)grow(S->row, &S->rcap, S->nrow + 1, (int)sizeof(trow_t));
      if (!S->row) return;
      S->row[S->nrow].tag_pos = ts;
      S->row[S->nrow].cell0 = S->ncell;
      S->nrow++;
    } else if ((is(t->name, "td") || is(t->name, "th")) && !t->end) {
      tcell_t* c;
      scan_close(S, &open, ts);
      if (cap_open) {
        S->cap_e = ts;
        cap_open = 0;
      }
      if (S->nrow == 0) {
        S->row = (trow_t*)grow(S->row, &S->rcap, 1, (int)sizeof(trow_t));
        if (!S->row) return;
        S->row[0].tag_pos = -1;
        S->row[0].cell0 = 0;
        S->nrow = 1;
      }
      S->cell = (tcell_t*)grow(S->cell, &S->ccap, S->ncell + 1, (int)sizeof(tcell_t));
      if (!S->cell) return;
      c = &S->cell[S->ncell];
      memset(c, 0, sizeof *c);
      c->tag_pos = ts;
      c->s = te;
      c->e = te;
      c->row = S->nrow - 1;
      c->span = t->span;
      c->strays = S->nstray;
      open = S->ncell++;
    } else if (is(t->name, "td") || is(t->name, "th") || is(t->name, "tr") || is(t->name, "tbody") ||
               is(t->name, "thead") || is(t->name, "tfoot")) {
      scan_close(S, &open, ts);
    } else if (is(t->name, "caption")) {
      scan_close(S, &open, ts);
      if (!t->end) {
        S->cap_pos = ts;
        S->cap_s = te;
        S->cap_e = te;
        cap_open = 1;
      } else if (cap_open) {
        S->cap_e = ts;
        cap_open = 0;
      }
    } else if (open < 0 && !cap_open && (is(t->name, "form") || is(t->name, "input"))) {
      /* fora das celulas: so o que muda os formularios */
      S->stray = (int*)grow(S->stray, &S->scap, S->nstray + 1, (int)sizeof(int));
      if (!S->stray) return;
      S->stray[S->nstray++] = ts;
    }
  }
  scan_close(S, &open, len);
  if (cap_open) S->cap_e = len;
}

static void do_strays(lay_t* L, const char* html, int len, tscan_t* S, int from, int to, tag_t* t) {
  int k;
  for (k = from; k < to && k < S->nstray; k++) {
    parse_tag(html + S->stray[k], len - S->stray[k], t, L);
    if (is(t->name, "form")) form_tag(L, t);
    else if (is(t->name, "input") && !L->measure) {
      int submit = is(t->type, "submit") || is(t->type, "image");
      place_field(L, t, submit, 1, 0);
    }
  }
}

/* estado que a celula muda e a tabela devolve */
typedef struct {
  int indent, cols, line, col, line_empty, blank, space_pending;
  int bold, pre, heading, link, uline, em, code, list_depth, dl_depth;
  int in_cell, cell_runs, band_top, band_line, band_runs, line_run0, run_floor, frame_floor;
  int field, button;
} lsave_t;

static void lsave(const lay_t* L, lsave_t* s) {
  s->indent = L->indent;
  s->cols = L->cols;
  s->line = L->line;
  s->col = L->col;
  s->line_empty = L->line_empty;
  s->blank = L->blank;
  s->space_pending = L->space_pending;
  s->bold = L->bold;
  s->pre = L->pre;
  s->heading = L->heading;
  s->link = L->link;
  s->uline = L->uline;
  s->em = L->em;
  s->code = L->code;
  s->list_depth = L->list_depth;
  s->dl_depth = L->dl_depth;
  s->in_cell = L->in_cell;
  s->cell_runs = L->cell_runs;
  s->band_top = L->band_top;
  s->band_line = L->band_line;
  s->band_runs = L->band_runs;
  s->line_run0 = L->line_run0;
  s->run_floor = L->run_floor;
  s->frame_floor = L->frame_floor;
  s->field = L->field;
  s->button = L->button;
}

static void lrestore(lay_t* L, const lsave_t* s) {
  L->indent = s->indent;
  L->cols = s->cols;
  L->line = s->line;
  L->col = s->col;
  L->line_empty = s->line_empty;
  L->blank = s->blank;
  L->space_pending = s->space_pending;
  L->bold = s->bold;
  L->pre = s->pre;
  L->heading = s->heading;
  L->link = s->link;
  L->uline = s->uline;
  L->em = s->em;
  L->code = s->code;
  L->list_depth = s->list_depth;
  L->dl_depth = s->dl_depth;
  L->in_cell = s->in_cell;
  L->cell_runs = s->cell_runs;
  L->band_top = s->band_top;
  L->band_line = s->band_line;
  L->band_runs = s->band_runs;
  L->line_run0 = s->line_run0;
  L->run_floor = s->run_floor;
  L->frame_floor = s->frame_floor;
  L->field = s->field;
  L->button = s->button;
  L->wlen = 0;
}

/* abre o td/th (ou caption) em tag_pos como elemento; devolve o indice do quadro */
static int cell_open(lay_t* L, const char* html, int len, int tag_pos, tag_t* t) {
  frm_t f;
  if (tag_pos < 0) return -1;
  parse_tag(html + tag_pos, len - tag_pos, t, L);
  frame_compute(L, t, &f);
  f.mode = M_NORMAL;
  return frame_push(L, &f, t);
}

/* Mede a celula: largura sem quebrar (maxw) e o maior pedaco (minw). */
static void cell_measure(lay_t* L, const char* html, int len, tcell_t* c, tag_t* t) {
  lay_t* S = (lay_t*)malloc(sizeof(lay_t));
  doc_t* D = (doc_t*)malloc(sizeof(doc_t));
  int k, maxw = 0, fi;
  c->minw = c->maxw = c->fixw = 0;
  if (!S || !D) {
    free(S);
    free(D);
    c->maxw = 20;
    return;
  }
  lay_init(S, D, MEASURE_COLS, L->cp437);
  S->cols = MEASURE_COLS;
  S->base = L->base;
  S->latin1 = L->latin1;
  S->css = L->css;
  S->link_color = L->link_color;
  S->measure = 1;
  S->table_depth = L->table_depth;
  D->page_bg = L->d->page_bg;
  memcpy(S->frame, L->frame, (size_t)L->nframe * sizeof(frm_t));
  S->nframe = L->nframe;
  for (k = 0; k < S->nframe; k++) {
    S->frame[k].el = -1;
    S->frame[k].rect = -1;
    S->frame[k].hide = 0;
  }
  style_sync(S);
  fi = cell_open(S, html, len, c->tag_pos, t);
  S->frame_floor = S->nframe;
  S->in_cell = 1;
  if (fi >= 0) {
    frm_t* f = &S->frame[fi];
    if (f->width > 0) c->fixw = (f->width + 7) / 8;
    else if (f->width < 0) c->fixw = f->width; /* porcento */
  }
  if (c->e - c->s > 256 * 1024) {
    /* celula enorme: nao vale medir */
    c->minw = 20;
    c->maxw = MEASURE_COLS;
  } else {
    lay_html(S, html, c->s, c->e);
    flush_word(S);
    for (k = 0; k < D->nruns; k++) {
      int r = D->runs[k].col + D->runs[k].len;
      if (r > maxw) maxw = r;
    }
    c->maxw = maxw;
    c->minw = S->minw > maxw ? maxw : S->minw;
    if (fi >= 0 && S->frame[fi].ws == 2) c->minw = maxw;
    if (fi >= 0 && S->frame[fi].hide) c->minw = c->maxw = 0;
  }
  doc_free(D);
  free(D);
  free(S);
}

/* runs [a, b) em ordem de linha, sem trocar a ordem dos da mesma linha.
 * Os elementos com id (JS) passam a cobrir as novas posicoes. */
static void sort_runs(doc_t* d, int a, int b) {
  int n = b - a, w, i;
  int* idx;
  int* tmp;
  int* pos;
  run_t* copy;
  if (n < 2) return;
  idx = (int*)malloc((size_t)n * sizeof(int));
  tmp = (int*)malloc((size_t)n * sizeof(int));
  copy = (run_t*)malloc((size_t)n * sizeof(run_t));
  pos = (int*)malloc((size_t)n * sizeof(int));
  if (!idx || !tmp || !copy || !pos) {
    free(idx);
    free(tmp);
    free(copy);
    free(pos);
    return;
  }
  for (i = 0; i < n; i++) idx[i] = a + i;
  for (w = 1; w < n; w *= 2) {
    int lo;
    for (lo = 0; lo < n; lo += 2 * w) {
      int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
      int p = lo, q = mid, o = lo;
      while (p < mid && q < hi)
        tmp[o++] = d->runs[idx[q]].line < d->runs[idx[p]].line ? idx[q++] : idx[p++];
      while (p < mid) tmp[o++] = idx[p++];
      while (q < hi) tmp[o++] = idx[q++];
    }
    memcpy(idx, tmp, (size_t)n * sizeof(int));
  }
  for (i = 0; i < n; i++) {
    copy[i] = d->runs[idx[i]];
    pos[idx[i] - a] = a + i;
  }
  memcpy(d->runs + a, copy, (size_t)n * sizeof(run_t));
  for (i = 0; i < d->nels; i++) {
    doc_el_t* e = &d->els[i];
    int k, lo = -1, hi = -1;
    if (e->run0 < a || e->run1 < e->run0) continue;
    for (k = e->run0; k <= e->run1 && k < b; k++) {
      int np = pos[k - a];
      if (lo < 0 || np < lo) lo = np;
      if (np > hi) hi = np;
    }
    if (lo >= 0) {
      e->run0 = lo;
      e->run1 = hi;
    }
  }
  free(idx);
  free(tmp);
  free(copy);
  free(pos);
}

typedef struct {
  int run0, run1, rect, rect1, f0, f1, bottom, valign, fi;
  int bg_set, border;
  unsigned bg, bcolor;
} tdone_t;

/* Poe a celula na caixa [x, x+w) a partir de top. */
static void cell_layout(lay_t* L, const char* html, int len, int tag_pos, int s, int e, int x, int w,
                        int top, int deflt_valign, tdone_t* out, tag_t* t) {
  lsave_t sv;
  int nf = L->nframe, fi;
  lsave(L, &sv);
  out->rect = add_rect(L, top, x, x + w, 0, 0, 0);
  L->indent = x;
  L->cols = x + w;
  L->line = top;
  L->col = x;
  L->line_empty = 1;
  L->blank = 0;
  L->space_pending = 0;
  L->in_cell = 1;
  L->cell_runs = L->d->nruns;
  L->run_floor = L->d->nruns;
  L->line_run0 = L->d->nruns;
  L->band_runs = -1;
  L->list_depth = 0;
  L->dl_depth = 0;
  L->field = -1;
  L->button = -1;
  L->last_emit = top - 1;
  out->run0 = L->d->nruns;
  out->f0 = L->d->nfields;
  fi = cell_open(L, html, len, tag_pos, t);
  out->valign = deflt_valign;
  out->bg_set = 0;
  out->border = 0;
  if (fi >= 0) {
    frm_t* f = &L->frame[fi];
    if (f->valign) out->valign = f->valign;
    out->bg_set = f->bg_set && !f->hide;
    out->bg = f->bg;
    out->border = f->hide ? 0 : f->border;
    out->bcolor = f->bcolor;
  }
  L->frame_floor = L->nframe;
  lay_html(L, html, s, e);
  flush_word(L);
  flush_line(L);
  out->bottom = L->last_emit + 1;
  if (out->bottom < top) out->bottom = top;
  style_pop_to(L, nf);
  out->run1 = L->d->nruns;
  out->rect1 = L->d->nrects;
  out->f1 = L->d->nfields;
  lrestore(L, &sv);
}

static void cell_shift(lay_t* L, const tdone_t* c, int dy) {
  doc_t* d = L->d;
  int k;
  if (dy <= 0) return;
  for (k = c->run0; k < c->run1 && k < d->nruns; k++) d->runs[k].line += dy;
  for (k = c->rect + 1; k < c->rect1 && k < d->nrects; k++) {
    if (k < 0) continue;
    d->rects[k].line0 += dy;
    d->rects[k].line1 += dy;
  }
  for (k = c->f0; k < c->f1 && k < d->nfields; k++)
    if (d->fields[k].line >= 0) d->fields[k].line += dy;
}

/* Tabela inteira a partir de i (depois de <table ...>); devolve o fim. */
static int lay_table(lay_t* L, tag_t* ttag, int tfi, const char* html, int i, int len) {
  tscan_t S;
  tag_t* t = (tag_t*)malloc(sizeof(tag_t));
  tdone_t* done = 0;
  int cmin[TABLE_COLS], cmax[TABLE_COLS], cfix[TABLE_COLS], w[TABLE_COLS], cx[TABLE_COLS + 1];
  int ncols = 0, r, k, c, avail, gaps, smin = 0, smax = 0, target, inner, total, x0, cur, top;
  int table_run0, table_rect, tw, border_all, sdone = 0, last0 = L->last_emit;
  frm_t* tf = tfi >= 0 ? &L->frame[tfi] : 0;
  if (!t) return len;
  table_scan(L, html, i, len, &S, t);
  L->table_depth++;
  /* colunas */
  for (r = 0; r < S.nrow; r++) {
    int e = r + 1 < S.nrow ? S.row[r + 1].cell0 : S.ncell;
    c = 0;
    for (k = S.row[r].cell0; k < e; k++) {
      if (c >= TABLE_COLS) {
        S.cell[k].span = 0;
        continue;
      }
      S.cell[k].col = c;
      if (c + S.cell[k].span > TABLE_COLS) S.cell[k].span = TABLE_COLS - c;
      c += S.cell[k].span;
    }
    if (c > ncols) ncols = c;
  }
  avail = L->cols - L->indent;
  if (avail < 4) avail = 4;
  if (ncols < 1) ncols = 1;
  gaps = ncols - 1;
  for (c = 0; c < ncols; c++) cmin[c] = cmax[c] = cfix[c] = 0;
  for (k = 0; k < S.ncell; k++) {
    tcell_t* cl = &S.cell[k];
    if (cl->span < 1) continue;
    cell_measure(L, html, len, cl, t);
    if (cl->fixw < 0) cl->fixw = L->measure ? 0 : avail * -cl->fixw / 100;
    if (cl->span == 1) {
      c = cl->col;
      if (cl->minw > cmin[c]) cmin[c] = cl->minw;
      if (cl->maxw > cmax[c]) cmax[c] = cl->maxw;
      if (cl->fixw > cfix[c]) cfix[c] = cl->fixw;
    }
  }
  for (c = 0; c < ncols; c++) {
    if (cfix[c] > 0) cmax[c] = cfix[c] > cmin[c] ? cfix[c] : cmin[c];
    if (cmax[c] < cmin[c]) cmax[c] = cmin[c];
  }
  /* celulas com colspan: o que falta vai repartido */
  for (k = 0; k < S.ncell; k++) {
    tcell_t* cl = &S.cell[k];
    int sm = 0, sx = 0, need;
    if (cl->span < 2) continue;
    for (c = cl->col; c < cl->col + cl->span; c++) {
      sm += cmin[c];
      sx += cmax[c];
    }
    sm += cl->span - 1;
    sx += cl->span - 1;
    need = cl->minw - sm;
    for (c = cl->col; need > 0 && c < cl->col + cl->span; c++) {
      int add = (need + (cl->col + cl->span - c) - 1) / (cl->col + cl->span - c);
      cmin[c] += add;
      if (cmax[c] < cmin[c]) cmax[c] = cmin[c];
      need -= add;
    }
    need = cl->maxw - sx;
    for (c = cl->col; need > 0 && c < cl->col + cl->span; c++) {
      int add = (need + (cl->col + cl->span - c) - 1) / (cl->col + cl->span - c);
      cmax[c] += add;
      need -= add;
    }
  }
  for (c = 0; c < ncols; c++) {
    smin += cmin[c];
    smax += cmax[c];
  }
  /* largura da tabela */
  tw = tf && tf->width ? tf->width : ttag->width;
  if (tw < 0) tw = avail * -tw / 100;
  else if (tw > 0) tw = (tw + 7) / 8;
  if (tw > avail) tw = avail;
  if (L->measure && tw == avail) tw = 0; /* 100% de "infinito" */
  target = tw > 0 ? tw : (smax + gaps < avail ? smax + gaps : avail);
  if (target < smin + gaps && smin + gaps <= avail) target = smin + gaps;
  inner = target - gaps;
  if (inner < 0) inner = 0;
  if (inner <= smin) {
    if (smin <= avail - gaps || smin == 0) {
      for (c = 0; c < ncols; c++) w[c] = cmin[c];
    } else {
      int room = avail - gaps;
      if (room < ncols) room = ncols;
      for (c = 0; c < ncols; c++) {
        w[c] = cmin[c] * room / smin;
        if (w[c] < 1 && cmin[c] > 0) w[c] = 1;
      }
    }
  } else if (inner >= smax) {
    int extra = inner - smax, flex = 0;
    for (c = 0; c < ncols; c++) {
      w[c] = cmax[c];
      if (!cfix[c]) flex += cmax[c];
    }
    if (tw > 0 && extra > 0) {
      int given = 0;
      for (c = 0; c < ncols; c++) {
        int add = flex > 0 ? (cfix[c] ? 0 : extra * cmax[c] / flex) : extra / ncols;
        w[c] += add;
        given += add;
      }
      /* o que sobrou do arredondamento vai para a coluna mais larga */
      if (given < extra) {
        int big = 0;
        for (c = 1; c < ncols; c++)
          if (w[c] > w[big]) big = c;
        w[big] += extra - given;
      }
    }
  } else {
    int given = 0;
    for (c = 0; c < ncols; c++) {
      w[c] = cmin[c] + (cmax[c] - cmin[c]) * (inner - smin) / (smax - smin);
      given += w[c];
    }
    if (given < inner) {
      int big = 0;
      for (c = 1; c < ncols; c++)
        if (cmax[c] - w[c] > cmax[big] - w[big]) big = c;
      w[big] += inner - given;
    }
  }
  total = gaps;
  for (c = 0; c < ncols; c++) total += w[c];
  /* tabela dentro de uma celula medida: o minimo dela */
  if (L->measure && smin + gaps > L->minw) L->minw = smin + gaps;
  x0 = L->indent;
  if (total < avail) {
    int center = (tf && tf->mauto) || is(ttag->align, "center") ||
                 (tf && tfi > 0 && L->frame[tfi - 1].legacy_center);
    if (center) x0 += (avail - total) / 2;
    else if (is(ttag->align, "right") || (tf && tf->flt == 2)) x0 += avail - total;
  }
  cx[0] = x0;
  for (c = 0; c < ncols; c++) cx[c + 1] = cx[c] + w[c] + 1;
  border_all = ttag->border > 0;
  /* layout */
  flush_word(L);
  flush_line(L);
  top = L->line;
  cur = top;
  table_run0 = L->d->nruns;
  table_rect = -1;
  if (tf && !L->measure && (tf->bg_set || tf->border)) {
    table_rect = add_rect(L, top, x0, x0 + total, (tf->bg_set ? RECT_FILL : 0) | tf->border, tf->bg,
                          tf->bcolor);
  }
  if (S.cap_pos >= 0) {
    tdone_t cd;
    cell_layout(L, html, len, S.cap_pos, S.cap_s, S.cap_e, x0, total, cur, V_TOP, &cd, t);
    if (cd.rect >= 0) L->d->rects[cd.rect].flags = 0;
    cur = cd.bottom;
  }
  if (S.ncell > 0) done = (tdone_t*)malloc((size_t)S.ncell * sizeof(tdone_t));
  for (r = 0; r < S.nrow && done; r++) {
    int e = r + 1 < S.nrow ? S.row[r + 1].cell0 : S.ncell;
    int nf = L->nframe, rfi = -1, row_rect = -1, bottom = cur;
    unsigned rbg = 0;
    int rbg_set = 0, rvalign = V_MIDDLE;
    if (S.row[r].tag_pos >= 0) {
      frm_t f;
      parse_tag(html + S.row[r].tag_pos, len - S.row[r].tag_pos, t, L);
      frame_compute(L, t, &f);
      rfi = frame_push(L, &f, t);
      if (rfi >= 0) {
        rbg_set = L->frame[rfi].bg_set && !L->frame[rfi].hide;
        rbg = L->frame[rfi].bg;
        if (L->frame[rfi].valign) rvalign = L->frame[rfi].valign;
        if (L->frame[rfi].hide) {
          /* linha escondida: so os formularios contam */
          for (k = S.row[r].cell0; k < e; k++) {
            do_strays(L, html, len, &S, sdone, S.cell[k].strays, t);
            sdone = S.cell[k].strays;
            done[k].run0 = done[k].run1 = -1;
          }
          style_pop_to(L, nf);
          continue;
        }
      }
    }
    if (rbg_set) row_rect = add_rect(L, cur, x0, x0 + total, RECT_FILL, rbg, 0);
    for (k = S.row[r].cell0; k < e; k++) {
      tcell_t* cl = &S.cell[k];
      done[k].run0 = done[k].run1 = -1;
      do_strays(L, html, len, &S, sdone, cl->strays, t);
      sdone = cl->strays;
      if (cl->span < 1) continue;
      {
        int x = cx[cl->col];
        int cw = cx[cl->col + cl->span] - 1 - x;
        if (cw < 1) cw = 1;
        cell_layout(L, html, len, cl->tag_pos, cl->s, cl->e, x, cw, cur, rvalign, &done[k], t);
        if (done[k].bottom > bottom) bottom = done[k].bottom;
      }
    }
    for (k = S.row[r].cell0; k < e; k++) {
      tcell_t* cl = &S.cell[k];
      int h, dy = 0;
      if (cl->span < 1 || done[k].run0 < 0) continue;
      h = done[k].bottom - cur;
      if (done[k].valign == V_MIDDLE) dy = (bottom - cur - h) / 2;
      else if (done[k].valign == V_BOTTOM) dy = bottom - cur - h;
      cell_shift(L, &done[k], dy);
      if (done[k].rect >= 0) {
        doc_rect_t* q = &L->d->rects[done[k].rect];
        int fl = (done[k].bg_set ? RECT_FILL : 0) | done[k].border;
        if (border_all) {
          fl |= RECT_SIDES;
          if (!done[k].border) q->border = 0xA0A0A0;
          else q->border = done[k].bcolor;
        } else
          q->border = done[k].bcolor;
        q->flags = bottom > cur ? fl : 0;
        q->bg = done[k].bg;
        q->line0 = cur;
        q->line1 = bottom - 1;
      }
    }
    if (row_rect >= 0) L->d->rects[row_rect].line1 = bottom - 1;
    style_pop_to(L, nf);
    cur = bottom;
  }
  do_strays(L, html, len, &S, sdone, S.nstray, t);
  if (table_rect >= 0) {
    if (cur > top) L->d->rects[table_rect].line1 = cur - 1;
    else L->d->rects[table_rect].flags = 0;
  }
  L->line = cur;
  L->line_empty = 1;
  L->blank = 0;
  L->col = L->indent;
  L->space_pending = 0;
  L->line_run0 = L->d->nruns;
  L->last_emit = last0 > cur - 1 ? last0 : cur - 1;
  L->band_runs = -1;
  L->table_depth--;
  if (L->table_depth == 0 && !L->measure) sort_runs(L->d, table_run0, L->d->nruns);
  free(done);
  free(S.cell);
  free(S.row);
  free(S.stray);
  free(t);
  return S.end;
}

/* ---------- laco principal ---------- */
static void gap(lay_t* L, int wide) {
  if (L->line_empty) return;
  L->space_pending = 1;
  if (wide && L->col + 2 < L->cols) {
    int keep = L->link, keep_bg = L->css_bg_set;
    L->link = -1;
    L->css_bg_set = 0;
    emit(L, " ", 1, 0);
    L->link = keep;
    L->css_bg_set = keep_bg;
  }
}

static void end_tag(lay_t* L, tag_t* t) {
  int fi = frame_find(L, t->name);
  int mode = fi >= 0 ? L->frame[fi].mode : M_NORMAL;
  int hidden = L->hide > 0;
  flush_word(L);
  L->blk = !hidden && mode != M_INLINE;
  if (mode == M_INLINE && !hidden) L->space_pending = 1;
  if (mode == M_BLOCK && !hidden) flush_line(L);
  if (fi >= 0 && L->frame[fi].spr > 0 && !hidden && !L->line_empty) L->space_pending = 1;
  if (fi < 0 && (is(t->name, "li") || is(t->name, "dd") || is(t->name, "dt"))) {
    /* fecha de algo que nao esta aberto: so a quebra */
    L->blk = 0;
  }
  handle_tag(L, t);
  if (fi >= 0) style_pop_to(L, fi);
  L->blk = 1;
}

static void void_tag(lay_t* L, tag_t* t) {
  frm_t f;
  int hid, block;
  const char* n = t->name;
  if (is(n, "meta") || is(n, "link") || is(n, "base") || is(n, "source") || is(n, "wbr") ||
      is(n, "area") || is(n, "col") || is(n, "param") || is(n, "track") || is(n, "embed"))
    return;
  frame_compute(L, t, &f);
  if (!t->width && f.width) t->width = f.width;
  if (!t->height && f.height) t->height = f.height;
  hid = L->hide > 0 || f.hide;
  if (hid) {
    if (is(n, "input")) {
      int submit = is(t->type, "submit") || is(t->type, "image");
      place_field(L, t, submit, 1, 0);
    }
    return;
  }
  block = (f.display == D_BLOCK || f.display == D_FLEX) && !is(n, "hr") && !is(n, "br");
  if (block) {
    flush_word(L);
    flush_line(L);
  }
  L->blk = 1;
  {
    int before = L->d->nruns;
    handle_tag(L, t);
    if (t->id[0] && L->d->nels < DOC_ELS && !L->measure) {
      doc_el_t* e = &L->d->els[L->d->nels];
      memset(e, 0, sizeof *e);
      strncpy(e->id, t->id, sizeof e->id - 1);
      e->run0 = before;
      e->run1 = L->d->nruns - 1;
      L->d->nels++;
    }
  }
  if (block) {
    flush_word(L);
    flush_line(L);
  }
}

static int start_tag(lay_t* L, tag_t* t, const char* html, int i, int len) {
  const char* n = t->name;
  frm_t f;
  int blocktag, mode, parent_flex, hid, fi, indent0;
  auto_close(L, n);
  frame_compute(L, t, &f);
  blocktag = tag_is_block(n);
  parent_flex = L->nframe > 0 && L->frame[L->nframe - 1].display == D_FLEX &&
                !L->frame[L->nframe - 1].flexcol;
  mode = M_NORMAL;
  if (f.display == D_INLINE && blocktag && !is_table_part(n)) mode = M_INLINE;
  else if ((f.display == D_BLOCK || f.display == D_FLEX) && !blocktag) mode = M_BLOCK;
  if (parent_flex && (blocktag || mode == M_BLOCK) && !is_table_part(n) && !is(n, "form")) mode = M_INLINE;
  /* float:left nos itens de menu e paginacao: ficam lado a lado */
  if (f.flt && (blocktag || mode == M_BLOCK) && !is_table_part(n) && !is(n, "body") && !is(n, "html"))
    mode = M_INLINE;
  /* <a style="display:block"> dentro de um item em linha (float, flex,
   * inline-block): fica dentro da caixinha do item, sem quebrar a linha */
  if (mode == M_BLOCK) {
    int k;
    for (k = L->nframe - 1; k >= L->frame_floor; k--)
      if (L->frame[k].mode == M_INLINE) {
        mode = M_NORMAL;
        break;
      }
  }
  f.mode = mode;
  hid = L->hide > 0 || f.hide;
  if (!hid) {
    if (mode == M_INLINE) {
      flush_word(L);
      gap(L, parent_flex || f.display == D_INLINE || f.flt);
    } else if (blocktag || mode == M_BLOCK) {
      flush_word(L);
      if (!(L->in_cell && L->d->nruns == L->cell_runs)) flush_line(L);
    } else if (f.spl > 0) {
      flush_word(L);
      if (!L->line_empty) L->space_pending = 1;
    }
  }
  indent0 = L->indent;
  fi = frame_push(L, &f, t);
  L->blk = !hid && mode != M_INLINE;
  if (is(n, "table") && L->blk && L->table_depth < TABLE_DEPTH && !L->skip) {
    int right = (fi >= 0 && L->frame[fi].flt == 2) || is(t->align, "right");
    int run0, line0;
    flush_word(L);
    flush_line(L);
    run0 = L->d->nruns;
    line0 = L->line;
    i = lay_table(L, t, fi, html, i, len);
    if (fi >= 0) style_pop_to(L, fi);
    L->blk = 1;
    if (right && !L->measure && L->table_depth == 0 && L->fl_end < 0 && L->d->nruns > run0) {
      /* tabela a direita (ficha da Wikipedia): o texto que vem depois sobe e
       * corre a esquerda dela */
      int k, left = L->cols, bottom = line0;
      for (k = run0; k < L->d->nruns; k++) {
        const run_t* r = &L->d->runs[k];
        if (r->col < left) left = r->col;
        if (r->line > bottom) bottom = r->line;
      }
      if (left - 2 - L->indent >= 24) {
        L->fl_cols = L->cols;
        L->fl_end = bottom;
        L->fl_used = 1;
        L->cols = left - 2;
        L->line = line0;
        L->col = L->indent;
        L->line_empty = 1;
        L->blank = 1;
        L->space_pending = 0;
        L->line_run0 = L->d->nruns;
        L->glue_run = -1;
      }
    }
    return i;
  }
  handle_tag(L, t);
  L->blk = 1;
  if (fi >= 0 && !hid && !L->measure && mode != M_INLINE && L->frame[fi].blocky &&
      (L->frame[fi].bg_set || L->frame[fi].border) && !is(n, "body") && !is(n, "html") &&
      !is_table_part(n)) {
    frm_t* q = &L->frame[fi];
    q->rect = add_rect(L, L->line, indent0, L->cols, (q->bg_set ? RECT_FILL : 0) | q->border, q->bg,
                       q->bcolor);
  }
  if (fi < 0 && t->id[0] && L->d->nels < DOC_ELS && !L->measure) {
    doc_el_t* e = &L->d->els[L->d->nels];
    memset(e, 0, sizeof *e);
    strncpy(e->id, t->id, sizeof e->id - 1);
    e->run0 = L->d->nruns;
    e->run1 = L->d->nruns - 1;
    L->d->nels++;
  }
  return i;
}

static int do_tag(lay_t* L, tag_t* t, const char* html, int i, int len) {
  const char* n = t->name;
  if (!t->end && !t->self_close && (is(n, "script") || is(n, "style") || is(n, "textarea"))) {
    char* body = 0;
    int bl = 0;
    i += take_raw(html + i, len - i, n, &body, &bl);
    if (L->skip) {
      free(body);
      return i;
    }
    if (is(n, "style")) {
      if (body && L->css && !L->measure) css_parse(L->css, body, bl);
      free(body);
    } else if (is(n, "script")) {
      keep_script(L, t, body, bl);
    } else {
      if (body && bl > 0) {
        int k = 0, j;
        for (j = 0; j < bl && k < (int)sizeof(t->value) - 1; j++) {
          if (body[j] == '\r' || body[j] == '\n') continue;
          t->value[k++] = body[j];
        }
        t->value[k] = 0;
      }
      {
        frm_t f;
        frame_compute(L, t, &f);
        place_field(L, t, 0, f.hide ? 1 : 0, 0);
      }
      free(body);
    }
    return i;
  }
  if (L->skip || in_list(n, skip_tags) || is(n, "title")) {
    handle_tag(L, t);
    return i;
  }
  if (t->end) {
    end_tag(L, t);
    return i;
  }
  if (is(n, "select") && !t->self_close) return do_select(L, t, html, i, len);
  if (t->self_close || is_void_tag(n)) {
    void_tag(L, t);
    return i;
  }
  return start_tag(L, t, html, i, len);
}

static void lay_html(lay_t* L, const char* html, int i, int len) {
  tag_t* t = (tag_t*)malloc(sizeof(tag_t));
  if (!t) return;
  while (i < len) {
    unsigned char c = (unsigned char)html[i];
    if (c == '<' && i + 1 < len) {
      unsigned char c1 = (unsigned char)html[i + 1];
      if (c1 == '!' && i + 3 < len && html[i + 2] == '-' && html[i + 3] == '-') {
        int k = i + 4;
        while (k + 2 < len && !(html[k] == '-' && html[k + 1] == '-' && html[k + 2] == '>')) k++;
        i = k + 3;
        continue;
      }
      if (c1 == '!' || c1 == '?') {
        while (i < len && html[i] != '>') i++;
        i++;
        continue;
      }
      if (c1 == '/' || isalpha(c1)) {
        i += parse_tag(html + i, len - i, t, L);
        i = do_tag(L, t, html, i, len);
        continue;
      }
    }
    if (c == '&') {
      unsigned cp;
      int k = entity(html + i, len - i, &cp);
      if (k > 0) {
        text_cp(L, cp);
        i += k;
        continue;
      }
    }
    if (c < 0x80 || L->latin1) {
      text_cp(L, c);
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
        text_cp(L, c);
        i++;
      } else {
        text_cp(L, cp);
        i += need + 1;
      }
    }
  }
  free(t);
}

static void lay_finish(lay_t* L) {
  flush_word(L);
  flush_line(L);
  L->d->nlines = L->line > L->last_emit + 1 ? L->line : L->last_emit + 1;
}

/* Runs em ordem de linha e coluna: o float poe o texto de volta em linhas
 * que a tabela ja ocupou, e o desenho (doc_first_run) busca por linha.
 * Merge sort estavel por indices; doc_el_t (getElementById do js.c) segue. */
static void order_runs(doc_t* d) {
  int n = d->nruns, w, i;
  int* a = (int*)malloc(sizeof(int) * (size_t)n);
  int* b = (int*)malloc(sizeof(int) * (size_t)n);
  int* inv = (int*)malloc(sizeof(int) * (size_t)n);
  run_t* nr = (run_t*)malloc(sizeof(run_t) * (size_t)n);
  if (n < 2 || !a || !b || !inv || !nr) {
    free(a);
    free(b);
    free(inv);
    free(nr);
    return;
  }
  for (i = 0; i < n; i++) a[i] = i;
  for (w = 1; w < n; w *= 2) {
    int* t;
    for (i = 0; i < n; i += 2 * w) {
      int mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n;
      int p = i, q = mid, o = i;
      while (p < mid && q < hi) {
        const run_t* x = &d->runs[a[p]];
        const run_t* y = &d->runs[a[q]];
        if (y->line < x->line || (y->line == x->line && y->col < x->col)) b[o++] = a[q++];
        else b[o++] = a[p++];
      }
      while (p < mid) b[o++] = a[p++];
      while (q < hi) b[o++] = a[q++];
    }
    t = a;
    a = b;
    b = t;
  }
  for (i = 0; i < n; i++) {
    nr[i] = d->runs[a[i]];
    inv[a[i]] = i;
  }
  memcpy(d->runs, nr, sizeof(run_t) * (size_t)n);
  for (i = 0; i < d->nels; i++) {
    doc_el_t* e = &d->els[i];
    int k, lo = n, hi = -1;
    if (e->run1 < e->run0) continue;
    for (k = e->run0; k <= e->run1 && k < n; k++) {
      if (inv[k] < lo) lo = inv[k];
      if (inv[k] > hi) hi = inv[k];
    }
    if (hi >= 0) {
      e->run0 = lo;
      e->run1 = hi;
    }
  }
  free(a);
  free(b);
  free(inv);
  free(nr);
}

void doc_layout(doc_t* d, const char* html, int len, int cols, const char* base_url,
                int latin1, int cp437, const char* extra_css, int extra_len) {
  lay_t* L = (lay_t*)malloc(sizeof(lay_t));
  css_t* css = (css_t*)calloc(1, sizeof *css);
  int k;
  if (!L) {
    memset(d, 0, sizeof *d);
    free(css);
    return;
  }
  lay_init(L, d, cols, cp437);
  L->base = base_url;
  L->latin1 = latin1;
  L->css = css;
  if (css) {
    for (k = 0; k < CSS_HASH; k++) css->head[k] = -1;
    css->any = -1;
    css->vw = L->cols * 8;
    if (extra_css && extra_len > 0) css_parse(css, extra_css, extra_len);
  }
  lay_html(L, html, 0, len);
  lay_finish(L);
  style_close_all(L);
  if (L->d->nlines < L->last_emit + 1) L->d->nlines = L->last_emit + 1;
  if (L->fl_used) order_runs(L->d);
  if (css) free(css->rules);
  free(css);
  free(L);
}

void doc_layout_text(doc_t* d, const char* text, int len, int cols, int cp437) {
  lay_t* L = (lay_t*)malloc(sizeof(lay_t));
  int i;
  if (!L) {
    memset(d, 0, sizeof *d);
    return;
  }
  lay_init(L, d, cols, cp437);
  L->pre = 1;
  for (i = 0; i < len; i++) text_cp(L, (unsigned char)text[i]);
  lay_finish(L);
  free(L);
}

void doc_free(doc_t* d) {
  int i;
  if (!d) return;
  for (i = 0; i < d->nlinks; i++) free(d->links[i]);
  for (i = 0; i < d->nscripts; i++) free(d->scripts[i].src);
  free(d->links);
  free(d->runs);
  free(d->text);
  free(d->rects);
  memset(d, 0, sizeof(*d));
  d->page_bg = 0xFFFFFF;
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
