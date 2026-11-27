/* notepad.c - Bloco de notas do SODOS (NOTEPAD.EXE), no estilo do Windows 10.
 *
 *   NOTEPAD [arquivo]
 *
 * Programa independente do WIN (antes o bloco de notas era so a janela
 * embutida no video/visual.c). O WIN abre este .EXE pelo Iniciar, pelo icone
 * do desktop, pelo duplo clique num .TXT e pelo @NOTEPAD do LINKEXT.CFG.
 *
 *   - texto em UTF-16 por dentro (como o Notepad do Windows): abre e grava
 *     UTF-8, UTF-8 com BOM, UTF-16 LE/BE e ANSI (Windows-1252); fim de linha
 *     Windows (CRLF), Unix (LF) ou Macintosh (CR), detectados ao abrir
 *   - fonte TrueType suavizada (video/wfont.c, o mesmo rasterizador do WIN):
 *     Noto Mono no texto, Noto Sans nos menus; zoom de 10% a 500%
 *   - menus Arquivo / Editar / Ferramentas, menu de contexto no texto
 *   - Abrir e Salvar como com lista de pastas, Localizar, Substituir, Ir
 *     para, desfazer/refazer em varios niveis, quebra automatica de linha,
 *     barra de status (Ln, Col, zoom, fim de linha, codificacao)
 *   - tudo e desenhado numa imagem propria e vai para a janela com um
 *     k_gui_pixels: a letra suavizada fica igual no modo retro e no avancado
 *   - cores do tema do sistema (Configuracoes > Temas): Classico = Notepad do
 *     Windows 10; Moderno claro e Escuro com as cores do TEMA.CFG, na hora
 *
 * Opcoes (fonte, zoom, quebra, barra de status) em NOTEPAD.CFG ao lado do
 * .EXE. Teclas: as do Notepad (Ctrl+N/O/S, Ctrl+Shift+S, Ctrl+Z/Y/X/C/V/A,
 * Ctrl+F/H/G, F3, Shift+F3, F5, F10 abre o menu, F1 Sobre).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>

/* Rasterizador TrueType do WIN, com Latin-1 inteiro e as letras extras do
 * Windows-1252 em 0x80..0x9F (aspas curvas, travessao, euro...). */
#define WFONT_LAST 255
#define WFONT_CP1252 1
#include "../video/wfont.c"

typedef unsigned short wch;

int k_win_launch_arg(const char* exe, const char* arg);

static char self_path[128];   /* este .EXE (Nova janela) */
static char fd_target[300];   /* Salvar como: caminho escolhido */

/* ------------------------------------------------------------ cores
 * Seguem Configuracoes > Temas (k_ui_theme): tema Classico = o Notepad do
 * Windows 10 (claro); Moderno e Escuro = as cores do TEMA.CFG (w_*), claro ou
 * escuro. Trocar o tema com o NOTEPAD aberto vale na hora (pal_sync). */
struct pal {
  unsigned ed_bg, ed_fg, sel_bg, sel_fg;          /* texto */
  unsigned bar_bg, bar_line, text, dim, dis;      /* barra de menus, textos */
  unsigned mb_hot, mb_hotb, mb_open, mb_openb;    /* item da barra */
  unsigned pop_bg, pop_b, pop_hot, pop_sep;       /* menu suspenso */
  unsigned stat_bg, stat_line;                    /* barra de status */
  unsigned sb_track, sb_thumb, sb_hot, sb_drag, sb_arrow, sb_dis;
  unsigned dlg_bg, dlg_title, dlg_body, dlg_frame, dlg_frame_in;
  unsigned btn, btn_b, btn_hot, btn_down, btn_downb;
  unsigned field_bg, field_b, focus, check;
  unsigned list_bg, list_b, list_hot, list_sel, head, ask;
  int dark;
};
static struct pal P;
static unsigned theme_gen = 0xFFFFFFFFu;

#define C_SELBG      P.sel_bg
#define C_SELFG      P.sel_fg
#define C_MB_HOT     P.mb_hot
#define C_MB_HOTB    P.mb_hotb
#define C_MB_OPEN    P.mb_open
#define C_MB_OPENB   P.mb_openb
#define C_POP_BG     P.pop_bg
#define C_POP_BORDER P.pop_b
#define C_POP_HOT    P.pop_hot
#define C_POP_SEP    P.pop_sep
#define C_DIS        P.dis
#define C_STAT_BG    P.stat_bg
#define C_STAT_LINE  P.stat_line
#define C_SB_TRACK   P.sb_track
#define C_SB_THUMB   P.sb_thumb
#define C_SB_HOT     P.sb_hot
#define C_SB_DRAG    P.sb_drag
#define C_SB_ARROW   P.sb_arrow
#define C_DLG_BG     P.dlg_bg
#define C_BTN        P.btn
#define C_BTN_B      P.btn_b
#define C_BTN_HOT    P.btn_hot
#define C_BTN_DOWN   P.btn_down
#define C_BTN_DOWNB  P.btn_downb
#define C_FIELD_B    P.field_b
#define C_FOCUS      P.focus
#define C_LIST_HOT   P.list_hot
#define C_LIST_SEL   P.list_sel
#define C_HEAD_TEXT  P.head

/* t/255 do caminho de a ate b */
static unsigned mixc(unsigned a, unsigned b, int t) {
  unsigned r = (((a >> 16) & 255) * (unsigned)(255 - t) + ((b >> 16) & 255) * (unsigned)t) / 255;
  unsigned g = (((a >> 8) & 255) * (unsigned)(255 - t) + ((b >> 8) & 255) * (unsigned)t) / 255;
  unsigned bl = ((a & 255) * (unsigned)(255 - t) + (b & 255) * (unsigned)t) / 255;
  return (r << 16) | (g << 8) | bl;
}

static int lum(unsigned c) {
  return (int)((((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10);
}

/* Classico: as cores do Notepad do Windows 10 */
static void pal_classic(void) {
  P.dark = 0;
  P.ed_bg = 0xFFFFFF;
  P.ed_fg = 0x000000;
  P.sel_bg = 0x0078D7;
  P.sel_fg = 0xFFFFFF;
  P.bar_bg = 0xFFFFFF;
  P.bar_line = 0xF0F0F0;
  P.text = 0x000000;
  P.dim = 0x5F5F5F;
  P.dis = 0x8D8D8D;
  P.mb_hot = 0xE5F3FF;
  P.mb_hotb = 0xCCE8FF;
  P.mb_open = 0xCCE8FF;
  P.mb_openb = 0x99D1FF;
  P.pop_bg = 0xF2F2F2;
  P.pop_b = 0xCCCCCC;
  P.pop_hot = 0x91C9F7;
  P.pop_sep = 0xD7D7D7;
  P.stat_bg = 0xF0F0F0;
  P.stat_line = 0xD7D7D7;
  P.sb_track = 0xF0F0F0;
  P.sb_thumb = 0xCDCDCD;
  P.sb_hot = 0xA6A6A6;
  P.sb_drag = 0x606060;
  P.sb_arrow = 0x606060;
  P.sb_dis = 0xBFBFBF;
  P.dlg_bg = 0xF0F0F0;
  P.dlg_title = 0xFFFFFF;
  P.dlg_body = 0xFFFFFF;
  P.dlg_frame = 0x1883D7;
  P.dlg_frame_in = 0xAAAAAA;
  P.btn = 0xE1E1E1;
  P.btn_b = 0xADADAD;
  P.btn_hot = 0xE5F1FB;
  P.btn_down = 0xCCE4F7;
  P.btn_downb = 0x005499;
  P.field_bg = 0xFFFFFF;
  P.field_b = 0x7A7A7A;
  P.focus = 0x0078D7;
  P.check = 0x333333;
  P.list_bg = 0xFFFFFF;
  P.list_b = 0xD9D9D9;
  P.list_hot = 0xE5F3FF;
  P.list_sel = 0xCCE8FF;
  P.head = 0x4C607A;
  P.ask = 0x003399;
}

/* Moderno / Escuro: as cores dos widgets do tema (w_* do TEMA.CFG) */
static void pal_theme(const k_ui_theme_t* t) {
  int dark = lum(t->bg) < 110;
  unsigned acc = t->accent ? t->accent : t->sel_bg;
  P.dark = dark;
  P.ed_bg = t->panel;
  P.ed_fg = t->text;
  P.sel_bg = t->sel_bg;
  P.sel_fg = t->sel_text;
  P.bar_bg = t->bg;
  P.bar_line = t->border;
  P.text = t->text;
  P.dim = t->text_dim;
  P.dis = t->text_dis;
  P.mb_hot = mixc(t->bg, acc, dark ? 45 : 30);
  P.mb_hotb = P.mb_hot;
  P.mb_open = mixc(t->bg, acc, dark ? 80 : 55);
  P.mb_openb = P.mb_open;
  P.pop_bg = t->face;
  P.pop_b = t->border;
  P.pop_hot = mixc(t->face, acc, dark ? 85 : 50);
  P.pop_sep = t->border;
  P.stat_bg = t->bg;
  P.stat_line = t->border;
  P.sb_track = t->bg;
  P.sb_thumb = mixc(t->bg, t->text, dark ? 55 : 50);
  P.sb_hot = mixc(t->bg, t->text, dark ? 95 : 90);
  P.sb_drag = mixc(t->bg, t->text, 150);
  P.sb_arrow = t->text_dim;
  P.sb_dis = t->text_dis;
  P.dlg_bg = t->bg;
  P.dlg_title = t->panel;
  P.dlg_body = t->panel;
  P.dlg_frame = acc;
  P.dlg_frame_in = t->border;
  P.btn = t->face;
  P.btn_b = t->border;
  P.btn_hot = mixc(t->face, acc, dark ? 60 : 35);
  P.btn_down = mixc(t->face, acc, dark ? 110 : 70);
  P.btn_downb = acc;
  P.field_bg = t->field_bg;
  P.field_b = mixc(t->border, t->text, dark ? 50 : 70);
  P.focus = acc;
  P.check = t->text_dim;
  P.list_bg = t->field_bg;
  P.list_b = t->border;
  P.list_hot = mixc(t->field_bg, acc, dark ? 45 : 30);
  P.list_sel = mixc(t->field_bg, acc, dark ? 90 : 60);
  P.head = t->text_dim;
  P.ask = dark ? mixc(acc, 0xFFFFFF, 90) : mixc(acc, 0x000000, 60);
}

/* 1 = o tema mudou desde a ultima olhada (redesenhar) */
static int pal_sync(void) {
  k_ui_theme_t t;
  unsigned g = (unsigned)k_ui_theme(0, 0);
  if (g == theme_gen) return 0;
  theme_gen = g;
  memset(&t, 0, sizeof(t));
  k_ui_theme(0, &t);
  if (t.modern && (t.bg || t.panel || t.text)) pal_theme(&t);
  else pal_classic();
  return 1;
}

/* ------------------------------------------------------------ utilidades */
static void scopy(char* d, const char* s, int max) {
  int i = 0;
  if (max < 1) return;
  while (s && s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static void scat(char* d, const char* s, int max) {
  int n = (int)strlen(d);
  scopy(d + n, s, max - n);
}

static int up_ascii(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int ci_eq(const char* a, const char* b) {
  while (*a && *b) {
    if (up_ascii((unsigned char)*a) != up_ascii((unsigned char)*b)) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* CP850 (o teclado e a fonte do WIN) -> Unicode, de 0x80 a 0xFF */
static const wch cp850_uni[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
  0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
  0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
  0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
  0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
  0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
  0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
  0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
  0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
  0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};

static unsigned cp850_to_uni(int c) {
  c &= 0xFF;
  return c < 0x80 ? (unsigned)c : cp850_uni[c - 0x80];
}

/* Unicode -> byte do Windows-1252; -1 sem equivalente */
static int uni_to_1252(unsigned u) {
  int k;
  if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) return (int)u;
  for (k = 0; k < 32; k++)
    if (cp1252_hi[k] == u && u != 0x3F) return 0x80 + k;
  return -1;
}

static unsigned cp1252_to_uni(int b) {
  b &= 0xFF;
  if (b >= 0x80 && b < 0xA0) return cp1252_hi[b - 0x80] == 0x3F ? 0xFFFD : cp1252_hi[b - 0x80];
  return (unsigned)b;
}

/* Proximo codigo de um texto UTF-8 (letra solta invalida vale como Latin-1) */
static unsigned u8_next(const unsigned char** pp) {
  const unsigned char* p = *pp;
  unsigned c = *p++;
  if (c >= 0xC0 && c < 0xE0 && (p[0] & 0xC0) == 0x80) {
    c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
    p++;
  } else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
    c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
    p += 2;
  } else if (c >= 0xF0 && c < 0xF8 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 &&
             (p[2] & 0xC0) == 0x80) {
    c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    p += 3;
  }
  *pp = p;
  return c;
}

/* bytes [0, n) sao UTF-8 valido? */
static int utf8_valid(const unsigned char* b, int n) {
  int i = 0;
  while (i < n) {
    unsigned c = b[i];
    int k, j;
    if (c < 0x80) {
      i++;
      continue;
    }
    if (c >= 0xC2 && c < 0xE0) k = 1;
    else if (c >= 0xE0 && c < 0xF0) k = 2;
    else if (c >= 0xF0 && c < 0xF5) k = 3;
    else return 0;
    for (j = 1; j <= k; j++)
      if (i + j >= n || (b[i + j] & 0xC0) != 0x80) return 0;
    i += k + 1;
  }
  return 1;
}

/* codigo -> UTF-8 em out (ate 4 bytes); devolve quantos */
static int u8_put(unsigned u, char* out) {
  if (u < 0x80) {
    out[0] = (char)u;
    return 1;
  }
  if (u < 0x800) {
    out[0] = (char)(0xC0 | (u >> 6));
    out[1] = (char)(0x80 | (u & 0x3F));
    return 2;
  }
  if (u < 0x10000) {
    out[0] = (char)(0xE0 | (u >> 12));
    out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    out[2] = (char)(0x80 | (u & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (u >> 18));
  out[1] = (char)(0x80 | ((u >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((u >> 6) & 0x3F));
  out[3] = (char)(0x80 | (u & 0x3F));
  return 4;
}

/* Maiuscula/minuscula do Latin-1 */
static wch w_upper(wch c) {
  if (c >= 'a' && c <= 'z') return (wch)(c - 32);
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (wch)(c - 32);
  return c;
}
static wch w_lower(wch c) {
  if (c >= 'A' && c <= 'Z') return (wch)(c + 32);
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (wch)(c + 32);
  return c;
}
static int w_isword(wch c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         c == '_' || (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c < 0x2000);
}
static int w_isspace(wch c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* ------------------------------------------------------------ imagem da janela */
static unsigned* fb;
static int fbw, fbh;
static int clx0, cly0, clx1, cly1;

static unsigned px_of(unsigned rgb) {
  return 0xFF000000u | ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);
}

static void clip_all(void) {
  clx0 = 0;
  cly0 = 0;
  clx1 = fbw;
  cly1 = fbh;
}

static void clip_to(int x, int y, int w, int h) {
  clx0 = x < 0 ? 0 : x;
  cly0 = y < 0 ? 0 : y;
  clx1 = x + w > fbw ? fbw : x + w;
  cly1 = y + h > fbh ? fbh : y + h;
}

static void fill(int x, int y, int w, int h, unsigned rgb) {
  int x1 = x + w, y1 = y + h, yy;
  unsigned p = px_of(rgb);
  if (x < clx0) x = clx0;
  if (y < cly0) y = cly0;
  if (x1 > clx1) x1 = clx1;
  if (y1 > cly1) y1 = cly1;
  for (yy = y; yy < y1; yy++) {
    unsigned* r = fb + yy * fbw;
    int xx;
    for (xx = x; xx < x1; xx++) r[xx] = p;
  }
}

static void frame(int x, int y, int w, int h, unsigned rgb) {
  fill(x, y, w, 1, rgb);
  fill(x, y + h - 1, w, 1, rgb);
  fill(x, y, 1, h, rgb);
  fill(x + w - 1, y, 1, h, rgb);
}

/* sombra suave de menu/dialogo: n faixas que clareiam */
static void shadow(int x, int y, int w, int h, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int a = 60 - i * (60 / n);
    int yy, xx;
    for (yy = y + i + 2; yy < y + h + i + 1; yy++) {
      xx = x + w + i;
      if (xx >= clx0 && xx < clx1 && yy >= cly0 && yy < cly1) {
        unsigned* p = fb + yy * fbw + xx;
        unsigned s = *p, r = s & 0xFF, g = (s >> 8) & 0xFF, b = (s >> 16) & 0xFF;
        r = r * (255 - a) / 255;
        g = g * (255 - a) / 255;
        b = b * (255 - a) / 255;
        *p = 0xFF000000u | (b << 16) | (g << 8) | r;
      }
    }
    yy = y + h + i;
    for (xx = x + i + 2; xx < x + w + i; xx++) {
      if (xx >= clx0 && xx < clx1 && yy >= cly0 && yy < cly1) {
        unsigned* p = fb + yy * fbw + xx;
        unsigned s = *p, r = s & 0xFF, g = (s >> 8) & 0xFF, b = (s >> 16) & 0xFF;
        r = r * (255 - a) / 255;
        g = g * (255 - a) / 255;
        b = b * (255 - a) / 255;
        *p = 0xFF000000u | (b << 16) | (g << 8) | r;
      }
    }
  }
}

/* ------------------------------------------------------------ fontes */
#define NG 224 /* codigos 32..255 */
struct font {
  struct wfont_glyph g[NG];
  int asc, h, px, cw;   /* cw: largura da celula (fonte de largura fixa) */
  unsigned char* mem;   /* mascaras (pool do wfont, solto da biblioteca) */
  int ok;
};
static struct font f_ui, f_tx;

static void* wf_alloc_cb(uint32_t n) { return malloc(n ? n : 1); }
static void wf_free_cb(void* p) { free(p); }

/* indice do glifo do codigo u */
static int gmap(unsigned u) {
  int b;
  if (u >= 32 && u < 127) return (int)u - 32;
  if (u >= 0xA0 && u <= 0xFF) return (int)u - 32;
  b = uni_to_1252(u);
  if (b >= 0x80 && b < 0xA0) return b - 32;
  return '?' - 32;
}

static unsigned char* load_bytes(const char* p, int* n) {
  int sz = k_fsize(p);
  unsigned char* b;
  if (sz < 0 || sz > 48 * 1024 * 1024) return 0;
  b = (unsigned char*)malloc((size_t)sz + 2);
  if (!b) return 0;
  if (sz > 0 && k_fload(p, (char*)b, sz + 1) != sz) {
    free(b);
    return 0;
  }
  b[sz] = 0;
  *n = sz;
  return b;
}

/* Arquivo de fonte: \SODOS\FONTS\ do disco do .EXE, D:, U:, C:, Z:. */
static char exe_drive = 'D';
static unsigned char* font_file(const char* name, int* n) {
  char order[8];
  int i, k = 0;
  unsigned mask = k_drive_mask();
  order[k++] = exe_drive;
  order[k++] = 'D';
  order[k++] = 'U';
  order[k++] = 'C';
  order[k++] = 'Z';
  order[k] = 0;
  for (i = 0; i < k; i++) {
    char p[64];
    unsigned char* b;
    int L = order[i];
    if (L < 'A' || L > 'Z' || !(mask & (1u << (L - 'A')))) continue;
    snprintf(p, sizeof(p), "%c:\\SODOS\\FONTS\\%s", L, name);
    b = load_bytes(p, n);
    if (b && *n > 1024) return b;
    if (b) free(b);
  }
  return 0;
}

static void font_free(struct font* f) {
  if (f->mem) free(f->mem);
  f->mem = 0;
  f->ok = 0;
}

/* Desenha os 224 glifos de data no tamanho px. 0 = ok. */
static int font_build(struct font* f, const unsigned char* data, int len, int px) {
  int c;
  unsigned char* lo = 0;
  font_free(f);
  if (wfont_build(data, len, px, wf_alloc_cb, wf_free_cb) != 0) return -1;
  for (c = 32; c <= 255; c++) f->g[c - 32] = *wfont_glyph((unsigned char)c);
  /* a memoria das mascaras passa a ser desta fonte: o proximo wfont_build
   * nao a solta (e o pool do wfont.c, incluido aqui) */
  lo = pool;
  pool = 0;
  f->mem = lo;
  f->asc = wfont_ascent();
  f->h = wfont_height();
  f->px = px;
  f->cw = (f->g['M' - 32].adv + 128) >> 8;
  if (f->cw < 1) f->cw = px / 2;
  f->ok = 1;
  return 0;
}

/* Sem TTF nenhum: a fonte 8x16 do WIN (k_font_get), sem suavizar. */
static void font_bitmap(struct font* f) {
  static unsigned char mem[NG * 8 * 16];
  int i;
  font_free(f);
  for (i = 0; i < NG; i++) {
    unsigned u = (unsigned)(i + 32);
    unsigned char gl[16];
    int c = (int)u, r, b;
    if (u >= 0x80) {
      int k;
      c = '?';
      for (k = 0; k < 128; k++)
        if (cp850_uni[k] == u) c = 0x80 + k;
    }
    memset(gl, 0, sizeof(gl));
    k_font_get(c, gl);
    for (r = 0; r < 16; r++)
      for (b = 0; b < 8; b++) mem[i * 128 + r * 8 + b] = (gl[r] & (0x80 >> b)) ? 255 : 0;
    f->g[i].left = 0;
    f->g[i].top = 12;
    f->g[i].w = 8;
    f->g[i].h = 16;
    f->g[i].adv = 8 * 256;
    f->g[i].a = mem + i * 128;
  }
  f->asc = 12;
  f->h = 16;
  f->px = 16;
  f->cw = 8;
  f->ok = 1;
}

/* glifo idx com o topo da linha em y, caneta em x (pixels) */
static void glyph_draw(const struct font* f, int idx, int x, int y, unsigned rgb) {
  const struct wfont_glyph* g = &f->g[idx];
  int gx = x + g->left, gy = y + f->asc - g->top, i, j;
  unsigned fr = (rgb >> 16) & 0xFF, fg = (rgb >> 8) & 0xFF, fbb = rgb & 0xFF;
  unsigned solid = px_of(rgb);
  if (!g->a) return;
  if (gx >= clx1 || gy >= cly1 || gx + g->w <= clx0 || gy + g->h <= cly0) return;
  for (j = 0; j < g->h; j++) {
    int yy = gy + j;
    const unsigned char* al;
    unsigned* row;
    if (yy < cly0 || yy >= cly1) continue;
    al = g->a + j * g->w;
    row = fb + yy * fbw;
    for (i = 0; i < g->w; i++) {
      int xx = gx + i;
      unsigned a = al[i];
      if (!a || xx < clx0 || xx >= clx1) continue;
      if (a >= 250) {
        row[xx] = solid;
      } else {
        unsigned s = row[xx], na = 255 - a;
        unsigned r = (fr * a + (s & 0xFF) * na) / 255;
        unsigned gg = (fg * a + ((s >> 8) & 0xFF) * na) / 255;
        unsigned b = (fbb * a + ((s >> 16) & 0xFF) * na) / 255;
        row[xx] = 0xFF000000u | (b << 16) | (gg << 8) | r;
      }
    }
  }
}

/* texto UTF-8 com a fonte f; y = topo da linha. Devolve a largura. */
static int text_draw(const struct font* f, int x, int y, const char* s, unsigned rgb) {
  const unsigned char* p = (const unsigned char*)s;
  int pen = x * 256;
  while (*p) {
    unsigned u = u8_next(&p);
    int idx = gmap(u);
    glyph_draw(f, idx, (pen + 128) >> 8, y, rgb);
    pen += f->g[idx].adv;
  }
  return ((pen + 128) >> 8) - x;
}

static int text_w(const struct font* f, const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  int pen = 0;
  while (*p) pen += f->g[gmap(u8_next(&p))].adv;
  return (pen + 128) >> 8;
}

/* texto em wch */
static int wtext_draw(const struct font* f, int x, int y, const wch* s, int n, unsigned rgb) {
  int pen = x * 256, i;
  for (i = 0; i < n; i++) {
    int idx = gmap(s[i]);
    glyph_draw(f, idx, (pen + 128) >> 8, y, rgb);
    pen += f->g[idx].adv;
  }
  return ((pen + 128) >> 8) - x;
}

static int wtext_w(const struct font* f, const wch* s, int n) {
  int pen = 0, i;
  for (i = 0; i < n; i++) pen += f->g[gmap(s[i])].adv;
  return (pen + 128) >> 8;
}

/* corta com "..." para caber em maxw */
static void text_fit(const struct font* f, int x, int y, const char* s, int maxw, unsigned rgb) {
  char b[300];
  int n;
  if (text_w(f, s) <= maxw) {
    text_draw(f, x, y, s, rgb);
    return;
  }
  scopy(b, s, (int)sizeof(b) - 4);
  n = (int)strlen(b);
  while (n > 0) {
    n--;
    while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80) n--;
    b[n] = 0;
    scat(b, "...", (int)sizeof(b));
    if (text_w(f, b) <= maxw) break;
    b[n] = 0;
  }
  text_draw(f, x, y, b, rgb);
}

/* ------------------------------------------------------------ opcoes */
struct mono { const char* name; const char* file; };
static const struct mono monos[] = {
  {"Noto Mono", "NOTOMONO.TTF"},
  {"JetBrains Mono", "JBMONO.TTF"},
  {"Fira Mono", "FIRAMON.TTF"},
};
#define NMONO 3
static int opt_font = 0;
static int opt_zoom = 100;
static int opt_wrap = 0;
static int opt_status = 1;
static int base_px = 16;   /* tamanho do texto em 100% */
static char cfg_path[128];
static unsigned char* mono_data;
static int mono_len, mono_loaded = -1;

static void cfg_load(void) {
  int n = 0;
  unsigned char* b = load_bytes(cfg_path, &n);
  char* p;
  if (!b) return;
  p = (char*)b;
  while (*p) {
    char key[32];
    int k = 0, v;
    while (*p == ' ' || *p == '\r' || *p == '\n') p++;
    while (*p && *p != '=' && *p != '\n' && k < 31) key[k++] = *p++;
    key[k] = 0;
    if (*p != '=') {
      while (*p && *p != '\n') p++;
      continue;
    }
    p++;
    v = atoi(p);
    if (ci_eq(key, "fonte")) opt_font = clampi(v, 0, NMONO - 1);
    else if (ci_eq(key, "zoom")) opt_zoom = clampi(v, 10, 500);
    else if (ci_eq(key, "quebra")) opt_wrap = v != 0;
    else if (ci_eq(key, "status")) opt_status = v != 0;
    while (*p && *p != '\n') p++;
  }
  free(b);
}

static void cfg_save(void) {
  char b[200];
  int n = snprintf(b, sizeof(b),
                   "; NOTEPAD.CFG - opcoes do Bloco de notas\r\nfonte=%d\r\nzoom=%d\r\n"
                   "quebra=%d\r\nstatus=%d\r\n",
                   opt_font, opt_zoom, opt_wrap, opt_status);
  if (cfg_path[0]) k_fsave(cfg_path, b, n);
}

/* fonte do texto no tamanho do zoom (relida so ao trocar de fonte) */
static void tx_font_apply(void) {
  int px = base_px * opt_zoom / 100;
  int f;
  if (px < 6) px = 6;
  if (px > 64) px = 64;
  for (f = 0; f < NMONO; f++) {
    int want = (opt_font + f) % NMONO;
    if (mono_loaded != want) {
      if (mono_data) free(mono_data);
      mono_data = font_file(monos[want].file, &mono_len);
      mono_loaded = mono_data ? want : -1;
    }
    if (mono_data && font_build(&f_tx, mono_data, mono_len, px) == 0) {
      opt_font = want;
      return;
    }
  }
  font_bitmap(&f_tx);
}

static void ui_font_init(int px) {
  static const char* const files[] = {"NOTOSANS.TTF", "INTER.TTF", "ROBOTO.TTF", "DEJAVU.TTF"};
  int i;
  for (i = 0; i < 4; i++) {
    int n = 0;
    unsigned char* d = font_file(files[i], &n);
    if (!d) continue;
    if (font_build(&f_ui, d, n, px) == 0) {
      free(d);
      return;
    }
    free(d);
  }
  font_bitmap(&f_ui);
}

/* ------------------------------------------------------------ o texto */
static wch* tx;
static int tn, tcap;
static int cur, anc;           /* cursor e ancora da selecao */
static int goal_x = -1;        /* coluna desejada nas setas para cima/baixo */
static int dirty;
static char doc_path[300];
#define ENC_UTF8 0
#define ENC_UTF8BOM 1
#define ENC_U16LE 2
#define ENC_U16BE 3
#define ENC_ANSI 4
#define NENC 5
static const char* const enc_names[NENC] = {"UTF-8", "UTF-8 com BOM", "UTF-16 LE", "UTF-16 BE",
                                            "ANSI"};
static int doc_enc = ENC_UTF8;
#define EOL_CRLF 0
#define EOL_LF 1
#define EOL_CR 2
static const char* const eol_names[3] = {"Windows (CRLF)", "Unix (LF)", "Macintosh (CR)"};
static int doc_eol = EOL_CRLF;

static int tx_reserve(int n) {
  if (n + 1 <= tcap) return 1;
  {
    int nc = tcap ? tcap : 4096;
    wch* nb;
    while (nc < n + 1) nc *= 2;
    nb = (wch*)realloc(tx, (size_t)nc * sizeof(wch));
    if (!nb) return 0;
    tx = nb;
    tcap = nc;
  }
  return 1;
}

static int sel_lo(void) { return cur < anc ? cur : anc; }
static int sel_hi(void) { return cur < anc ? anc : cur; }
static int has_sel(void) { return cur != anc; }

/* ------------------------------------------------------------ linhas visuais */
static int* rs;     /* inicio de cada linha visual */
static int* rln;    /* linha logica dela */
static int nrows, rcap;
static int lay_dirty = 1;
static int wrap_cols = 80;
static int max_cols;   /* linha mais larga (sem quebra): barra horizontal */

static int ch_cols(wch c, int col) { return c == '\t' ? 8 - col % 8 : 1; }

static void row_push(int start, int line) {
  if (nrows >= rcap) {
    int nc = rcap ? rcap * 2 : 1024;
    int* a = (int*)realloc(rs, (size_t)nc * sizeof(int));
    int* b;
    if (!a) return;
    rs = a;
    b = (int*)realloc(rln, (size_t)nc * sizeof(int));
    if (!b) return;
    rln = b;
    rcap = nc;
  }
  rs[nrows] = start;
  rln[nrows] = line;
  nrows++;
}

static void layout(void) {
  int i = 0, line = 0;
  nrows = 0;
  max_cols = 0;
  for (;;) {
    int ls = i, le;
    while (i < tn && tx[i] != '\n') i++;
    le = i;
    if (!opt_wrap) {
      int col = 0, p;
      row_push(ls, line);
      for (p = ls; p < le; p++) col += ch_cols(tx[p], col);
      if (col > max_cols) max_cols = col;
    } else {
      int rstart = ls, col = 0, p = ls, last_sp = -1;
      int wc = wrap_cols < 4 ? 4 : wrap_cols;
      row_push(ls, line);
      while (p < le) {
        wch c = tx[p];
        int w = ch_cols(c, col);
        if (c != ' ' && col + w > wc && p > rstart) {
          int br = last_sp >= rstart ? last_sp + 1 : p;
          if (br <= rstart) br = p;
          row_push(br, line);
          rstart = br;
          col = 0;
          last_sp = -1;
          p = br;
          continue;
        }
        if (c == ' ' || c == '\t') last_sp = p;
        col += w;
        p++;
      }
    }
    if (i >= tn) break;
    i++;
    line++;
  }
  lay_dirty = 0;
}

static void lay_ok(void) {
  if (lay_dirty) layout();
}

/* fim do conteudo da linha visual r (sem o '\n') */
static int row_end(int r) {
  int e = r + 1 < nrows ? rs[r + 1] : tn;
  if (r + 1 < nrows && rln[r + 1] != rln[r] && e > rs[r]) e--;
  return e;
}

static int row_of(int pos) {
  int lo = 0, hi = nrows - 1;
  lay_ok();
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (rs[mid] <= pos) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

/* coluna visual (celulas) de pos na sua linha visual */
static int col_of(int pos) {
  int r = row_of(pos), p, col = 0;
  for (p = rs[r]; p < pos && p < tn; p++) col += ch_cols(tx[p], col);
  return col;
}

/* posicao na linha visual r mais perto da coluna x */
static int pos_at(int r, int x) {
  int p, col = 0, e;
  lay_ok();
  if (r < 0) r = 0;
  if (r >= nrows) r = nrows - 1;
  e = row_end(r);
  for (p = rs[r]; p < e; p++) {
    int w = ch_cols(tx[p], col);
    if (col + (w + 1) / 2 > x) return p;
    col += w;
  }
  /* linha quebrada: o fim e o comeco da seguinte; fica antes do espaco */
  if (r + 1 < nrows && rln[r + 1] == rln[r] && e > rs[r]) return e - 1;
  return e;
}

static int line_start(int pos) {
  while (pos > 0 && tx[pos - 1] != '\n') pos--;
  return pos;
}
static int line_end(int pos) {
  while (pos < tn && tx[pos] != '\n') pos++;
  return pos;
}

/* ------------------------------------------------------------ desfazer */
struct urec {
  int pos, dlen, ilen;
  wch* del;
  wch* ins;
  int kind;            /* 1 digitar, 2 apagar para tras, 3 apagar, 0 outro */
  unsigned ms;
  int c0, a0;          /* cursor e ancora antes */
};
#define UMAX 3000
static struct urec* undo_st;
static int un, upos;   /* registros; os de upos em diante sao o refazer */
static int u_break;    /* 1 = o proximo digitar nao se junta ao anterior */

static wch* wdup(const wch* s, int n) {
  wch* d = (wch*)malloc((size_t)(n ? n : 1) * sizeof(wch));
  if (d && n) memcpy(d, s, (size_t)n * sizeof(wch));
  return d;
}

static void urec_free(struct urec* r) {
  if (r->del) free(r->del);
  if (r->ins) free(r->ins);
  r->del = r->ins = 0;
}

static void undo_clear(void) {
  int i;
  for (i = 0; i < un; i++) urec_free(&undo_st[i]);
  un = upos = 0;
  u_break = 1;
}

/* troca [pos, pos+dlen) por ins[0..ilen); sem registro de desfazer */
static int raw_replace(int pos, int dlen, const wch* ins, int ilen) {
  if (pos < 0) pos = 0;
  if (pos > tn) pos = tn;
  if (dlen > tn - pos) dlen = tn - pos;
  if (!tx_reserve(tn - dlen + ilen)) return 0;
  memmove(tx + pos + ilen, tx + pos + dlen, (size_t)(tn - pos - dlen) * sizeof(wch));
  if (ilen) memcpy(tx + pos, ins, (size_t)ilen * sizeof(wch));
  tn += ilen - dlen;
  lay_dirty = 1;
  return 1;
}

static void title_update(void);
static void ensure_visible(void);

/* Edicao com desfazer. kind: 1 digitando, 2 Backspace, 3 Del, 0 outro. */
static void edit(int pos, int dlen, const wch* ins, int ilen, int kind) {
  struct urec* r = 0;
  unsigned now = k_ms();
  int c0 = cur, a0 = anc;
  if (pos < 0) pos = 0;
  if (pos > tn) pos = tn;
  if (dlen > tn - pos) dlen = tn - pos;
  if (!dlen && !ilen) return;
  /* refazer some com a edicao nova */
  while (un > upos) urec_free(&undo_st[--un]);
  if (!u_break && un > 0 && kind) {
    struct urec* last = &undo_st[un - 1];
    if (kind == 1 && last->kind == 1 && !dlen && last->pos + last->ilen == pos &&
        now - last->ms < 2000 && !(ilen == 1 && (ins[0] == '\n'))) {
      wch* ni = (wch*)realloc(last->ins, (size_t)(last->ilen + ilen) * sizeof(wch));
      if (ni) {
        memcpy(ni + last->ilen, ins, (size_t)ilen * sizeof(wch));
        last->ins = ni;
        last->ilen += ilen;
        last->ms = now;
        r = last;
      }
    } else if (kind == 2 && last->kind == 2 && !ilen && pos + dlen == last->pos &&
               now - last->ms < 2000) {
      wch* nd = (wch*)malloc((size_t)(last->dlen + dlen) * sizeof(wch));
      if (nd) {
        memcpy(nd, tx + pos, (size_t)dlen * sizeof(wch));
        memcpy(nd + dlen, last->del, (size_t)last->dlen * sizeof(wch));
        free(last->del);
        last->del = nd;
        last->dlen += dlen;
        last->pos = pos;
        last->ms = now;
        r = last;
      }
    } else if (kind == 3 && last->kind == 3 && !ilen && pos == last->pos &&
               now - last->ms < 2000) {
      wch* nd = (wch*)realloc(last->del, (size_t)(last->dlen + dlen) * sizeof(wch));
      if (nd) {
        memcpy(nd + last->dlen, tx + pos, (size_t)dlen * sizeof(wch));
        last->del = nd;
        last->dlen += dlen;
        last->ms = now;
        r = last;
      }
    }
  }
  if (!r && undo_st) {
    if (un >= UMAX) {
      urec_free(&undo_st[0]);
      memmove(undo_st, undo_st + 1, (size_t)(un - 1) * sizeof(struct urec));
      un--;
    }
    r = &undo_st[un++];
    r->pos = pos;
    r->dlen = dlen;
    r->ilen = ilen;
    r->del = wdup(tx + pos, dlen);
    r->ins = wdup(ins, ilen);
    r->kind = kind;
    r->ms = now;
    r->c0 = c0;
    r->a0 = a0;
  }
  upos = un;
  u_break = kind == 0;
  raw_replace(pos, dlen, ins, ilen);
  cur = anc = pos + ilen;
  goal_x = -1;
  if (!dirty) {
    dirty = 1;
    title_update();
  }
}

static void do_undo(void) {
  struct urec* r;
  if (upos <= 0) return;
  r = &undo_st[--upos];
  raw_replace(r->pos, r->ilen, r->del, r->dlen);
  cur = r->c0;
  anc = r->a0;
  if (cur > tn) cur = tn;
  if (anc > tn) anc = tn;
  u_break = 1;
  goal_x = -1;
  dirty = 1;
  title_update();
  ensure_visible();
}

static void do_redo(void) {
  struct urec* r;
  if (upos >= un) return;
  r = &undo_st[upos++];
  raw_replace(r->pos, r->dlen, r->ins, r->ilen);
  cur = anc = r->pos + r->ilen;
  u_break = 1;
  goal_x = -1;
  dirty = 1;
  title_update();
  ensure_visible();
}

/* ------------------------------------------------------------ geometria */
static int menu_h, stat_h, sbw = 17;
static int ed_x, ed_y, ed_w, ed_h, vis_rows, lh;
static int top_row, scroll_x;
static int pad_l = 6;
static int hsb_on;

static void geometry(void) {
  menu_h = f_ui.h + 9;
  if (menu_h < 20) menu_h = 20;
  stat_h = opt_status ? f_ui.h + 9 : 0;
  hsb_on = !opt_wrap;
  lh = f_tx.h + (f_tx.h >= 14 ? 2 : 1);
  ed_x = 0;
  ed_y = menu_h + 1;
  ed_w = fbw - sbw;
  ed_h = fbh - stat_h - ed_y - (hsb_on ? sbw : 0);
  if (ed_w < 40) ed_w = 40;
  if (ed_h < lh) ed_h = lh;
  vis_rows = ed_h / lh;
  if (vis_rows < 1) vis_rows = 1;
  {
    int wc = (ed_w - pad_l - 4) / (f_tx.cw > 0 ? f_tx.cw : 8);
    if (wc < 4) wc = 4;
    if (wc != wrap_cols) {
      wrap_cols = wc;
      if (opt_wrap) lay_dirty = 1;
    }
  }
}

static int max_top(void) {
  lay_ok();
  return nrows - vis_rows > 0 ? nrows - vis_rows : 0;
}

static int content_w(void) { return pad_l + (max_cols + 2) * f_tx.cw; }

static void clamp_scroll(void) {
  int mx;
  top_row = clampi(top_row, 0, max_top());
  mx = content_w() - ed_w;
  if (opt_wrap || mx < 0) mx = 0;
  scroll_x = clampi(scroll_x, 0, mx);
}

static void ensure_visible(void) {
  int r, x;
  lay_ok();
  r = row_of(cur);
  if (r < top_row) top_row = r;
  if (r >= top_row + vis_rows) top_row = r - vis_rows + 1;
  if (!opt_wrap) {
    x = pad_l + col_of(cur) * f_tx.cw;
    if (x - scroll_x < pad_l) scroll_x = x - pad_l - ed_w / 4;
    if (x - scroll_x > ed_w - 3 * f_tx.cw) scroll_x = x - ed_w * 3 / 4;
  } else
    scroll_x = 0;
  clamp_scroll();
}

/* ------------------------------------------------------------ arquivo */
static void title_update(void) {
  char t[160];
  const char* nm = "Sem titulo";
  int i;
  if (doc_path[0]) {
    nm = doc_path;
    for (i = 0; doc_path[i]; i++)
      if (doc_path[i] == '\\' || doc_path[i] == '/' || doc_path[i] == ':') nm = doc_path + i + 1;
  }
  snprintf(t, sizeof(t), "%s%s - Bloco de notas", dirty ? "*" : "", nm);
  /* o titulo do WIN so tem ASCII */
  for (i = 0; t[i]; i++)
    if ((unsigned char)t[i] >= 127) t[i] = '?';
  gui_title(t);
}

/* nome do documento para as mensagens (UTF-8) */
static void doc_name(char* out, int max) {
  int i;
  const char* nm = "Sem título";
  if (doc_path[0]) {
    nm = doc_path;
    for (i = 0; doc_path[i]; i++)
      if (doc_path[i] == '\\' || doc_path[i] == '/' || doc_path[i] == ':') nm = doc_path + i + 1;
  }
  scopy(out, nm, max);
}

static void doc_new(void) {
  tn = 0;
  cur = anc = 0;
  top_row = scroll_x = 0;
  doc_path[0] = 0;
  doc_enc = ENC_UTF8;
  doc_eol = EOL_CRLF;
  dirty = 0;
  undo_clear();
  lay_dirty = 1;
  goal_x = -1;
  title_update();
}

/* bytes -> texto. 0 = ok. */
static int decode_into(const unsigned char* b, int n) {
  int i = 0, o = 0, crlf = 0, lf = 0, cr = 0;
  int enc;
  if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
    enc = ENC_UTF8BOM;
    i = 3;
  } else if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
    enc = ENC_U16LE;
    i = 2;
  } else if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
    enc = ENC_U16BE;
    i = 2;
  } else if (utf8_valid(b, n)) {
    enc = ENC_UTF8;
  } else {
    enc = ENC_ANSI;
  }
  if (!tx_reserve(n + 4)) return -1;
  if (enc == ENC_U16LE || enc == ENC_U16BE) {
    for (; i + 1 < n; i += 2)
      tx[o++] = enc == ENC_U16LE ? (wch)(b[i] | (b[i + 1] << 8)) : (wch)((b[i] << 8) | b[i + 1]);
  } else if (enc == ENC_ANSI) {
    for (; i < n; i++) tx[o++] = (wch)cp1252_to_uni(b[i]);
  } else {
    const unsigned char* p = b + i;
    const unsigned char* e = b + n;
    while (p < e) {
      unsigned u = u8_next(&p);
      if (u >= 0x10000) {
        u -= 0x10000;
        tx[o++] = (wch)(0xD800 | (u >> 10));
        tx[o++] = (wch)(0xDC00 | (u & 0x3FF));
      } else
        tx[o++] = (wch)u;
    }
  }
  /* fim de linha: o mais comum vira o do documento; por dentro, so '\n' */
  {
    int j, k = 0;
    for (j = 0; j < o; j++) {
      if (tx[j] == '\r') {
        if (j + 1 < o && tx[j + 1] == '\n') {
          crlf++;
          j++;
        } else
          cr++;
        tx[k++] = '\n';
      } else {
        if (tx[j] == '\n') lf++;
        tx[k++] = tx[j];
      }
    }
    o = k;
  }
  tn = o;
  doc_enc = enc;
  doc_eol = (lf > crlf && lf >= cr) ? EOL_LF : (cr > crlf && cr > lf) ? EOL_CR : EOL_CRLF;
  return 0;
}

static void msg_box(const char* title, const char* text);

static int doc_load(const char* path) {
  int n = 0;
  unsigned char* b = load_bytes(path, &n);
  char m[400];
  if (!b) {
    snprintf(m, sizeof(m), "Não foi possível abrir o arquivo:\n%s", path);
    msg_box("Bloco de notas", m);
    return -1;
  }
  decode_into(b, n);
  free(b);
  scopy(doc_path, path, (int)sizeof(doc_path));
  cur = anc = 0;
  top_row = scroll_x = 0;
  dirty = 0;
  undo_clear();
  lay_dirty = 1;
  goal_x = -1;
  title_update();
  return 0;
}

/* texto -> bytes na codificacao e fim de linha do documento */
static char* encode_doc(int* outn, int* lossy) {
  int cap = tn * 4 + 16, o = 0, i;
  char* b = (char*)malloc((size_t)cap);
  *lossy = 0;
  if (!b) return 0;
  if (doc_enc == ENC_UTF8BOM) {
    b[o++] = (char)0xEF;
    b[o++] = (char)0xBB;
    b[o++] = (char)0xBF;
  } else if (doc_enc == ENC_U16LE) {
    b[o++] = (char)0xFF;
    b[o++] = (char)0xFE;
  } else if (doc_enc == ENC_U16BE) {
    b[o++] = (char)0xFE;
    b[o++] = (char)0xFF;
  }
  for (i = 0; i < tn; i++) {
    unsigned u = tx[i];
    if (u == '\n') {
      wch eo[2];
      int k, ne = 0;
      if (doc_eol == EOL_CRLF) {
        eo[ne++] = '\r';
        eo[ne++] = '\n';
      } else
        eo[ne++] = doc_eol == EOL_CR ? '\r' : '\n';
      for (k = 0; k < ne; k++) {
        if (doc_enc == ENC_U16LE) {
          b[o++] = (char)eo[k];
          b[o++] = 0;
        } else if (doc_enc == ENC_U16BE) {
          b[o++] = 0;
          b[o++] = (char)eo[k];
        } else
          b[o++] = (char)eo[k];
      }
      continue;
    }
    if (doc_enc == ENC_U16LE) {
      b[o++] = (char)(u & 0xFF);
      b[o++] = (char)(u >> 8);
    } else if (doc_enc == ENC_U16BE) {
      b[o++] = (char)(u >> 8);
      b[o++] = (char)(u & 0xFF);
    } else if (doc_enc == ENC_ANSI) {
      int c = uni_to_1252(u);
      if (c < 0) {
        c = '?';
        *lossy = 1;
      }
      b[o++] = (char)c;
    } else {
      if (u >= 0xD800 && u < 0xDC00 && i + 1 < tn && tx[i + 1] >= 0xDC00 && tx[i + 1] < 0xE000) {
        u = 0x10000 + ((u - 0xD800) << 10) + (tx[i + 1] - 0xDC00);
        i++;
      }
      o += u8_put(u, b + o);
    }
  }
  *outn = o;
  return b;
}

static int doc_save_to(const char* path) {
  int n = 0, lossy = 0, rc;
  char* b = encode_doc(&n, &lossy);
  char m[400];
  if (!b) {
    msg_box("Bloco de notas", "Sem memória para gravar o arquivo.");
    return -1;
  }
  rc = k_fsave(path, b, n);
  free(b);
  if (rc < 0) {
    snprintf(m, sizeof(m), "Não foi possível salvar o arquivo:\n%s\n(erro %d)", path, rc);
    msg_box("Bloco de notas", m);
    return -1;
  }
  scopy(doc_path, path, (int)sizeof(doc_path));
  dirty = 0;
  u_break = 1;
  title_update();
  if (lossy)
    msg_box("Bloco de notas",
            "Algumas letras não existem em ANSI e foram gravadas como \"?\".\n"
            "Use UTF-8 para guardar o texto como está.");
  return 0;
}

/* ------------------------------------------------------------ area de transferencia */
static wch* clip_own;     /* o ultimo Copiar daqui, inteiro (o do kernel corta em 4 KB) */
static int clip_own_n;
static char* clip_own_u8;
static int clip_own_u8n;

static void clip_put(const wch* s, int n) {
  char* u = (char*)malloc((size_t)n * 4 + 8);
  int o = 0, i;
  if (!u) return;
  for (i = 0; i < n; i++) {
    unsigned c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    }
    o += u8_put(c, u + o);
  }
  u[o] = 0;
  k_clip_set(u, o);
  if (clip_own) free(clip_own);
  if (clip_own_u8) free(clip_own_u8);
  clip_own = wdup(s, n);
  clip_own_n = n;
  clip_own_u8 = u;
  clip_own_u8n = o;
}

/* texto da area de transferencia (devolve malloc, *n codigos) */
static wch* clip_take(int* n) {
  static char b[4100];
  int k = k_clip_get(b, (int)sizeof(b));
  wch* w;
  int o = 0;
  if (k < 0) k = 0;
  b[k] = 0;
  /* veio daqui: o texto inteiro (o kernel guarda so os primeiros 4 KB) */
  if (clip_own && clip_own_u8 && k > 0 && k <= clip_own_u8n &&
      memcmp(b, clip_own_u8, (size_t)k) == 0 && (k == clip_own_u8n || k >= 4000)) {
    *n = clip_own_n;
    return wdup(clip_own, clip_own_n);
  }
  w = (wch*)malloc((size_t)k * 2 + 4);
  if (!w) return 0;
  if (utf8_valid((const unsigned char*)b, k)) {
    const unsigned char* p = (const unsigned char*)b;
    const unsigned char* e = p + k;
    while (p < e) {
      unsigned u = u8_next(&p);
      if (u >= 0x10000) {
        u -= 0x10000;
        w[o++] = (wch)(0xD800 | (u >> 10));
        w[o++] = (wch)(0xDC00 | (u & 0x3FF));
      } else
        w[o++] = (wch)u;
    }
  } else {
    int i;
    for (i = 0; i < k; i++) w[o++] = (wch)cp850_to_uni((unsigned char)b[i]);
  }
  /* CRLF e CR -> '\n' */
  {
    int i, j = 0;
    for (i = 0; i < o; i++) {
      if (w[i] == '\r') {
        if (i + 1 < o && w[i + 1] == '\n') i++;
        w[j++] = '\n';
      } else
        w[j++] = w[i];
    }
    o = j;
  }
  *n = o;
  return w;
}

/* ------------------------------------------------------------ comandos de texto */
static void ins_text(const wch* s, int n, int kind) {
  edit(sel_lo(), sel_hi() - sel_lo(), s, n, kind);
  ensure_visible();
}

static void cmd_copy(void) {
  if (has_sel()) clip_put(tx + sel_lo(), sel_hi() - sel_lo());
}
static void cmd_cut(void) {
  if (!has_sel()) return;
  cmd_copy();
  ins_text(0, 0, 0);
}
static void cmd_paste(void) {
  int n = 0;
  wch* w = clip_take(&n);
  if (w && n > 0) ins_text(w, n, 0);
  if (w) free(w);
}
static void cmd_delete(void) {
  if (has_sel()) ins_text(0, 0, 0);
  else if (cur < tn) {
    edit(cur, 1, 0, 0, 3);
    ensure_visible();
  }
}
static void cmd_selall(void) {
  anc = 0;
  cur = tn;
  goal_x = -1;
}

static void cmd_time(void) {
  int t = k_time(), d = k_date();
  char b[40];
  wch w[40];
  int i, n;
  if (t < 0) t = 0;
  if (d <= 0) d = 20260101;
  n = snprintf(b, sizeof(b), "%02d:%02d %02d/%02d/%04d", t / 10000, (t / 100) % 100, d % 100,
               (d / 100) % 100, d / 10000);
  for (i = 0; i < n; i++) w[i] = (wch)(unsigned char)b[i];
  ins_text(w, n, 0);
}

static void cmd_case(int upper) {
  int a = sel_lo(), b = sel_hi(), i, c0 = cur, a0 = anc;
  wch* w;
  if (a == b) return;
  w = wdup(tx + a, b - a);
  if (!w) return;
  for (i = 0; i < b - a; i++) w[i] = upper ? w_upper(w[i]) : w_lower(w[i]);
  edit(a, b - a, w, b - a, 0);
  free(w);
  cur = c0;
  anc = a0;
}

/* palavra: Ctrl+setas */
static int word_left(int p) {
  while (p > 0 && w_isspace(tx[p - 1])) p--;
  if (p > 0 && w_isword(tx[p - 1]))
    while (p > 0 && w_isword(tx[p - 1])) p--;
  else if (p > 0)
    p--;
  return p;
}
static int word_right(int p) {
  if (p < tn && w_isword(tx[p]))
    while (p < tn && w_isword(tx[p])) p++;
  else if (p < tn && !w_isspace(tx[p]))
    p++;
  while (p < tn && (tx[p] == ' ' || tx[p] == '\t')) p++;
  if (p < tn && tx[p] == '\n' && p == cur) p++;
  return p;
}

/* ------------------------------------------------------------ localizar */
static wch find_s[256], repl_s[256];
static int find_n, repl_n;
static int find_case = 0, find_wrap = 1, find_down = 1;

static int match_at(int p) {
  int i;
  if (p < 0 || p + find_n > tn) return 0;
  for (i = 0; i < find_n; i++) {
    wch a = tx[p + i], b = find_s[i];
    if (!find_case) {
      a = w_lower(a);
      b = w_lower(b);
    }
    if (a != b) return 0;
  }
  return 1;
}

static void not_found(void) {
  char m[400], q[300];
  int i, o = 0;
  for (i = 0; i < find_n && o < (int)sizeof(q) - 5; i++) o += u8_put(find_s[i], q + o);
  q[o] = 0;
  snprintf(m, sizeof(m), "Não foi possível localizar \"%s\"", q);
  msg_box("Bloco de notas", m);
}

/* 1 achou (e marcou) */
static int find_next(int down) {
  int p, start;
  if (!find_n) return 0;
  if (down) {
    start = sel_hi();
    for (p = start; p + find_n <= tn; p++)
      if (match_at(p)) goto ok;
    if (find_wrap)
      for (p = 0; p < start && p + find_n <= tn; p++)
        if (match_at(p)) goto ok;
  } else {
    start = sel_lo();
    for (p = start - 1; p >= 0; p--)
      if (match_at(p)) goto ok;
    if (find_wrap)
      for (p = tn - find_n; p >= start; p--)
        if (match_at(p)) goto ok;
  }
  not_found();
  return 0;
ok:
  anc = p;
  cur = p + find_n;
  goal_x = -1;
  ensure_visible();
  return 1;
}

static void replace_one(void) {
  if (!find_n) return;
  if (has_sel() && sel_hi() - sel_lo() == find_n && match_at(sel_lo())) {
    ins_text(repl_s, repl_n, 0);
  }
  find_next(1);
}

static int replace_all(void) {
  int p = 0, cnt = 0, o = 0;
  wch* nb;
  if (!find_n) return 0;
  for (p = 0; p + find_n <= tn; p++)
    if (match_at(p)) {
      cnt++;
      p += find_n - 1;
    }
  if (!cnt) {
    not_found();
    return 0;
  }
  nb = (wch*)malloc((size_t)(tn + cnt * (repl_n > find_n ? repl_n - find_n : 0) + 1) * sizeof(wch));
  if (!nb) return 0;
  for (p = 0; p < tn;) {
    if (p + find_n <= tn && match_at(p)) {
      memcpy(nb + o, repl_s, (size_t)repl_n * sizeof(wch));
      o += repl_n;
      p += find_n;
    } else
      nb[o++] = tx[p++];
  }
  {
    int c = cur;
    edit(0, tn, nb, o, 0);
    cur = anc = c > tn ? tn : c;
  }
  free(nb);
  ensure_visible();
  return cnt;
}

/* ------------------------------------------------------------ menus */
enum {
  ID_NONE, ID_NEW, ID_NEWWIN, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_EXIT,
  ID_UNDO, ID_REDO, ID_CUT, ID_COPY, ID_PASTE, ID_DEL, ID_FIND, ID_FINDNEXT, ID_FINDPREV,
  ID_REPLACE, ID_GOTO, ID_SELALL, ID_TIME,
  ID_WRAP, ID_STATUS, ID_ZOOMIN, ID_ZOOMOUT, ID_ZOOM0, ID_UPPER, ID_LOWER, ID_STATS, ID_ABOUT,
  ID_FONT0 = 100, ID_ENC0 = 120, ID_EOL0 = 140
};
#define SUB_FONT 1
#define SUB_ZOOM 2
#define SUB_ENC 3
#define SUB_EOL 4
#define SUB_CASE 5

struct mi {
  const char* t;   /* 0 = fim, "-" = separador */
  const char* k;   /* atalho */
  int id;
  int sub;
};

static const struct mi m_file[] = {
  {"Novo", "Ctrl+N", ID_NEW, 0},
  {"Nova janela", "Ctrl+Shift+N", ID_NEWWIN, 0},
  {"Abrir...", "Ctrl+O", ID_OPEN, 0},
  {"Salvar", "Ctrl+S", ID_SAVE, 0},
  {"Salvar como...", "Ctrl+Shift+S", ID_SAVEAS, 0},
  {"-", 0, 0, 0},
  {"Sair", "", ID_EXIT, 0},
  {0, 0, 0, 0},
};
static const struct mi m_edit[] = {
  {"Desfazer", "Ctrl+Z", ID_UNDO, 0},
  {"Refazer", "Ctrl+Y", ID_REDO, 0},
  {"-", 0, 0, 0},
  {"Recortar", "Ctrl+X", ID_CUT, 0},
  {"Copiar", "Ctrl+C", ID_COPY, 0},
  {"Colar", "Ctrl+V", ID_PASTE, 0},
  {"Excluir", "Del", ID_DEL, 0},
  {"-", 0, 0, 0},
  {"Localizar...", "Ctrl+F", ID_FIND, 0},
  {"Localizar próxima", "F3", ID_FINDNEXT, 0},
  {"Localizar anterior", "Shift+F3", ID_FINDPREV, 0},
  {"Substituir...", "Ctrl+H", ID_REPLACE, 0},
  {"Ir para...", "Ctrl+G", ID_GOTO, 0},
  {"-", 0, 0, 0},
  {"Selecionar tudo", "Ctrl+A", ID_SELALL, 0},
  {"Hora/Data", "F5", ID_TIME, 0},
  {0, 0, 0, 0},
};
static const struct mi m_tools[] = {
  {"Quebra automática de linha", "", ID_WRAP, 0},
  {"Fonte", "", 0, SUB_FONT},
  {"Zoom", "", 0, SUB_ZOOM},
  {"Barra de status", "", ID_STATUS, 0},
  {"-", 0, 0, 0},
  {"Codificação", "", 0, SUB_ENC},
  {"Fim de linha", "", 0, SUB_EOL},
  {"Converter seleção", "", 0, SUB_CASE},
  {"-", 0, 0, 0},
  {"Estatísticas...", "", ID_STATS, 0},
  {"Sobre o Bloco de notas", "F1", ID_ABOUT, 0},
  {0, 0, 0, 0},
};
static const struct mi m_font[] = {
  {"Noto Mono", "", ID_FONT0 + 0, 0},
  {"JetBrains Mono", "", ID_FONT0 + 1, 0},
  {"Fira Mono", "", ID_FONT0 + 2, 0},
  {0, 0, 0, 0},
};
static const struct mi m_zoom[] = {
  {"Ampliar", "", ID_ZOOMIN, 0},
  {"Reduzir", "", ID_ZOOMOUT, 0},
  {"Restaurar zoom padrão", "", ID_ZOOM0, 0},
  {0, 0, 0, 0},
};
static const struct mi m_enc[] = {
  {"UTF-8", "", ID_ENC0 + ENC_UTF8, 0},
  {"UTF-8 com BOM", "", ID_ENC0 + ENC_UTF8BOM, 0},
  {"UTF-16 LE", "", ID_ENC0 + ENC_U16LE, 0},
  {"UTF-16 BE", "", ID_ENC0 + ENC_U16BE, 0},
  {"ANSI (Windows-1252)", "", ID_ENC0 + ENC_ANSI, 0},
  {0, 0, 0, 0},
};
static const struct mi m_eol[] = {
  {"Windows (CRLF)", "", ID_EOL0 + EOL_CRLF, 0},
  {"Unix (LF)", "", ID_EOL0 + EOL_LF, 0},
  {"Macintosh (CR)", "", ID_EOL0 + EOL_CR, 0},
  {0, 0, 0, 0},
};
static const struct mi m_case[] = {
  {"MAIÚSCULAS", "", ID_UPPER, 0},
  {"minúsculas", "", ID_LOWER, 0},
  {0, 0, 0, 0},
};
static const struct mi m_ctx[] = {
  {"Desfazer", "", ID_UNDO, 0},
  {"-", 0, 0, 0},
  {"Recortar", "", ID_CUT, 0},
  {"Copiar", "", ID_COPY, 0},
  {"Colar", "", ID_PASTE, 0},
  {"Excluir", "", ID_DEL, 0},
  {"-", 0, 0, 0},
  {"Selecionar tudo", "", ID_SELALL, 0},
  {0, 0, 0, 0},
};

#define NTOP 3
static const char* const top_names[NTOP] = {"Arquivo", "Editar", "Ferramentas"};
static const struct mi* const top_menus[NTOP] = {m_file, m_edit, m_tools};
#define MENU_CTX 9

static const struct mi* sub_menu(int s) {
  switch (s) {
    case SUB_FONT: return m_font;
    case SUB_ZOOM: return m_zoom;
    case SUB_ENC: return m_enc;
    case SUB_EOL: return m_eol;
    case SUB_CASE: return m_case;
  }
  return 0;
}

static int mi_enabled(int id) {
  switch (id) {
    case ID_UNDO: return upos > 0;
    case ID_REDO: return upos < un;
    case ID_CUT:
    case ID_COPY:
    case ID_DEL: return has_sel();
    case ID_UPPER:
    case ID_LOWER: return has_sel();
    case ID_FINDNEXT:
    case ID_FINDPREV: return find_n > 0;
    case ID_ZOOMIN: return opt_zoom < 500;
    case ID_ZOOMOUT: return opt_zoom > 10;
  }
  if (id >= ID_FONT0 && id < ID_FONT0 + NMONO) return 1;
  return 1;
}

static int mi_checked(int id) {
  if (id == ID_WRAP) return opt_wrap;
  if (id == ID_STATUS) return opt_status;
  if (id >= ID_FONT0 && id < ID_FONT0 + NMONO) return opt_font == id - ID_FONT0 && mono_loaded >= 0;
  if (id >= ID_ENC0 && id < ID_ENC0 + NENC) return doc_enc == id - ID_ENC0;
  if (id >= ID_EOL0 && id < ID_EOL0 + 3) return doc_eol == id - ID_EOL0;
  return 0;
}

/* estado dos menus */
static int m_open = -1;          /* 0..NTOP-1 barra, MENU_CTX contexto, -1 fechado */
static int m_hot = -1;           /* item marcado no menu aberto */
static int m_sub = -1;           /* item do menu aberto cujo submenu esta aberto */
static int m_subhot = -1;
static int m_x, m_y;             /* canto do menu aberto */
static int mb_hot = -1;          /* realce do rato na barra */
static int mx_mouse = -1, my_mouse = -1;

static int item_h(void) { return f_ui.h + 10; }
static int sep_h(void) { return 9; }

static int mb_item_x(int i) {
  int x = 0, k;
  for (k = 0; k < i; k++) x += text_w(&f_ui, top_names[k]) + 16;
  return x;
}
static int mb_item_w(int i) { return text_w(&f_ui, top_names[i]) + 16; }

static const struct mi* open_menu_items(void) {
  if (m_open == MENU_CTX) return m_ctx;
  if (m_open >= 0 && m_open < NTOP) return top_menus[m_open];
  return 0;
}

struct pg {
  int x, y, w, h, n;
};

static void pop_size(const struct mi* m, int* w, int* h, int* n) {
  int i, lw = 0, kw = 0, hh = 4;
  for (i = 0; m[i].t; i++) {
    if (m[i].t[0] == '-' && !m[i].t[1]) {
      hh += sep_h();
      continue;
    }
    if (text_w(&f_ui, m[i].t) > lw) lw = text_w(&f_ui, m[i].t);
    if (m[i].k && text_w(&f_ui, m[i].k) > kw) kw = text_w(&f_ui, m[i].k);
    hh += item_h();
  }
  *n = i;
  *w = 32 + lw + (kw ? 44 + kw : 20) + 22;
  if (*w < 160) *w = 160;
  *h = hh;
}

static void pop_geom(const struct mi* m, int ax, int ay, struct pg* g) {
  pop_size(m, &g->w, &g->h, &g->n);
  g->x = ax;
  g->y = ay;
  if (g->x + g->w > fbw - 2) g->x = fbw - 2 - g->w;
  if (g->y + g->h > fbh - 2) g->y = fbh - 2 - g->h;
  if (g->x < 0) g->x = 0;
  if (g->y < 0) g->y = 0;
}

/* y do item i (e altura) */
static int pop_item_y(const struct mi* m, const struct pg* g, int i, int* h) {
  int k, y = g->y + 2;
  for (k = 0; k < i; k++) y += (m[k].t[0] == '-' && !m[k].t[1]) ? sep_h() : item_h();
  *h = (m[i].t[0] == '-' && !m[i].t[1]) ? sep_h() : item_h();
  return y;
}

static int pop_hit(const struct mi* m, const struct pg* g, int x, int y) {
  int i;
  if (x < g->x || x >= g->x + g->w || y < g->y || y >= g->y + g->h) return -2;
  for (i = 0; i < g->n; i++) {
    int h, iy = pop_item_y(m, g, i, &h);
    if (y >= iy && y < iy + h) return (m[i].t[0] == '-' && !m[i].t[1]) ? -1 : i;
  }
  return -1;
}

static void open_geom(struct pg* g) {
  const struct mi* m = open_menu_items();
  if (m) pop_geom(m, m_x, m_y, g);
}

static void sub_geom(struct pg* g) {
  const struct mi* m = open_menu_items();
  struct pg pg0;
  const struct mi* sm;
  int h, iy;
  if (!m || m_sub < 0) return;
  sm = sub_menu(m[m_sub].sub);
  open_geom(&pg0);
  iy = pop_item_y(m, &pg0, m_sub, &h);
  pop_size(sm, &g->w, &g->h, &g->n);
  g->x = pg0.x + pg0.w - 3;
  g->y = iy - 2;
  if (g->x + g->w > fbw - 2) g->x = pg0.x - g->w + 3;
  if (g->y + g->h > fbh - 2) g->y = fbh - 2 - g->h;
  if (g->x < 0) g->x = 0;
  if (g->y < 0) g->y = 0;
}

static void draw_check(int x, int y, int s, unsigned rgb) {
  /* visto de duas retas grossas */
  int i;
  for (i = 0; i < s / 3; i++) {
    fill(x + i, y + s / 2 + i - 1, 2, 2, rgb);
  }
  for (i = 0; i < s - s / 3; i++) {
    fill(x + s / 3 + i, y + s / 2 + s / 3 - i - 2, 2, 2, rgb);
  }
}

static void draw_tri_right(int x, int y, int s, unsigned rgb) {
  int i;
  for (i = 0; i < s; i++) fill(x + i, y + i, 1, 2 * (s - i) - 1, rgb);
}
static void draw_tri_down(int x, int y, int s, unsigned rgb) {
  int i;
  for (i = 0; i < s; i++) fill(x + i, y + i, 2 * (s - i) - 1, 1, rgb);
}
static void draw_tri_up(int x, int y, int s, unsigned rgb) {
  int i;
  for (i = 0; i < s; i++) fill(x + s - 1 - i, y + i, 2 * i + 1, 1, rgb);
}
static void draw_tri_left(int x, int y, int s, unsigned rgb) {
  int i;
  for (i = 0; i < s; i++) fill(x + s - 1 - i, y + s - 1 - i, 1, 2 * i + 1, rgb);
}

static void draw_pop(const struct mi* m, const struct pg* g, int hot, int subidx) {
  int i;
  shadow(g->x, g->y, g->w, g->h, 4);
  fill(g->x, g->y, g->w, g->h, C_POP_BG);
  frame(g->x, g->y, g->w, g->h, C_POP_BORDER);
  for (i = 0; i < g->n; i++) {
    int h, iy = pop_item_y(m, g, i, &h);
    int en, on;
    unsigned fg;
    if (m[i].t[0] == '-' && !m[i].t[1]) {
      fill(g->x + 30, iy + h / 2, g->w - 34, 1, C_POP_SEP);
      continue;
    }
    en = m[i].sub ? 1 : mi_enabled(m[i].id);
    on = (i == hot || i == subidx);
    if (on && en) fill(g->x + 2, iy, g->w - 4, h, C_POP_HOT);
    else if (on) frame(g->x + 2, iy, g->w - 4, h, C_POP_HOT);
    fg = en ? P.text : C_DIS;
    if (!m[i].sub && mi_checked(m[i].id)) draw_check(g->x + 9, iy + (h - 12) / 2, 12, fg);
    text_draw(&f_ui, g->x + 32, iy + (h - f_ui.h) / 2, m[i].t, fg);
    if (m[i].sub)
      draw_tri_right(g->x + g->w - 16, iy + h / 2 - 4, 4, fg);
    else if (m[i].k && m[i].k[0])
      text_draw(&f_ui, g->x + g->w - 22 - text_w(&f_ui, m[i].k), iy + (h - f_ui.h) / 2, m[i].k,
                fg);
  }
}

static void menu_close(void) {
  m_open = -1;
  m_hot = m_sub = m_subhot = -1;
}

static void menu_open_top(int i, int hot) {
  m_open = i;
  m_x = mb_item_x(i);
  m_y = menu_h - 1;
  m_hot = hot;
  m_sub = m_subhot = -1;
}

/* ------------------------------------------------------------ dialogos */
struct fld {
  wch t[260];
  int n, cur, anc, scroll;
};

#define CT_LABEL 1
#define CT_EDIT 2
#define CT_BTN 3
#define CT_CHECK 4
#define CT_RADIO 5
#define CT_COMBO 6

struct ctl {
  int type, x, y, w, h, id;
  char text[80];
  int* val;    /* check, radio, combo */
  int mine;    /* radio: valor deste; combo: quantas opcoes */
  const char* const* opts;  /* combo */
  struct fld* f;
  int def;     /* botao padrao (Enter) */
};

#define DLG_MSG 1
#define DLG_SAVEASK 2
#define DLG_FIND 3
#define DLG_REPLACE 4
#define DLG_GOTO 5
#define DLG_OPEN 6
#define DLG_SAVEAS 7
#define DLG_OVERWRITE 8

#define B_OK 1
#define B_CANCEL 2
#define B_SAVE 3
#define B_NOSAVE 4
#define B_FINDNEXT 5
#define B_REPLACE 6
#define B_REPLACEALL 7
#define B_GOTO 8
#define B_YES 9
#define B_NO 10
#define B_UP 11
#define B_FILEOK 12

struct dlg {
  int kind;
  int x, y, w, h;
  char title[64];
  struct ctl c[20];
  int nc;
  int focus;       /* indice do controle com foco */
  int down;        /* botao apertado (indice) */
  char msg[600];
  int pend;        /* acao que espera a resposta */
};
static struct dlg dst[4];
static int ndlg;
static struct fld f_find, f_repl, f_goto, f_fname, f_addr;

#define ACT_NEW 1
#define ACT_OPEN 2
#define ACT_EXIT 3
static int quit_now;

static int dlg_title_h(void) { return f_ui.h + 14; }

static struct dlg* dlg_top(void) { return ndlg ? &dst[ndlg - 1] : 0; }

static struct ctl* add_ctl(struct dlg* d, int type, int x, int y, int w, int h, int id,
                           const char* text) {
  struct ctl* c;
  if (d->nc >= 20) return &d->c[19];
  c = &d->c[d->nc++];
  memset(c, 0, sizeof(*c));
  c->type = type;
  c->x = x;
  c->y = y;
  c->w = w;
  c->h = h;
  c->id = id;
  scopy(c->text, text ? text : "", (int)sizeof(c->text));
  return c;
}

static void fld_set_utf8(struct fld* f, const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  f->n = 0;
  while (*p && f->n < 255) f->t[f->n++] = (wch)u8_next(&p);
  f->cur = f->n;
  f->anc = 0;
  f->scroll = 0;
}

static void fld_set_w(struct fld* f, const wch* s, int n) {
  if (n > 255) n = 255;
  memcpy(f->t, s, (size_t)n * sizeof(wch));
  f->n = n;
  f->cur = n;
  f->anc = 0;
  f->scroll = 0;
}

static void fld_get_utf8(const struct fld* f, char* out, int max) {
  int i, o = 0;
  for (i = 0; i < f->n && o < max - 5; i++) o += u8_put(f->t[i], out + o);
  out[o] = 0;
}

/* nome de arquivo do campo: os bytes como o FAT do SODOS guarda (Latin-1 /
 * ASCII; o que nao cabe num byte vira '_') */
static void fld_get_path(const struct fld* f, char* out, int max) {
  int i, o = 0;
  for (i = 0; i < f->n && o < max - 1; i++) out[o++] = f->t[i] < 256 ? (char)f->t[i] : '_';
  out[o] = 0;
}

static void fld_set_path(struct fld* f, const char* s) {
  int i;
  f->n = 0;
  for (i = 0; s[i] && f->n < 255; i++) f->t[f->n++] = (wch)(unsigned char)s[i];
  f->cur = f->n;
  f->anc = 0;
  f->scroll = 0;
}

static int fld_key(struct fld* f, int k, int mods) {
  int lo = f->cur < f->anc ? f->cur : f->anc, hi = f->cur < f->anc ? f->anc : f->cur;
  int shift = mods & GUI_MOD_SHIFT;
  if (k == 1) {
    f->anc = 0;
    f->cur = f->n;
    return 1;
  }
  if (k == 3 || k == 24) {
    if (hi > lo) clip_put(f->t + lo, hi - lo);
    if (k == 24 && hi > lo) {
      memmove(f->t + lo, f->t + hi, (size_t)(f->n - hi) * sizeof(wch));
      f->n -= hi - lo;
      f->cur = f->anc = lo;
    }
    return 1;
  }
  if (k == 22) {
    int n = 0, i, j = 0;
    wch* w = clip_take(&n);
    if (!w) return 1;
    for (i = 0; i < n; i++)
      if (w[i] != '\n' && w[i] != '\r') w[j++] = w[i];
    n = j;
    if (hi > lo) {
      memmove(f->t + lo, f->t + hi, (size_t)(f->n - hi) * sizeof(wch));
      f->n -= hi - lo;
      f->cur = lo;
    }
    if (n > 255 - f->n) n = 255 - f->n;
    memmove(f->t + f->cur + n, f->t + f->cur, (size_t)(f->n - f->cur) * sizeof(wch));
    memcpy(f->t + f->cur, w, (size_t)n * sizeof(wch));
    f->n += n;
    f->cur += n;
    f->anc = f->cur;
    free(w);
    return 1;
  }
  if (k == 8 || k == GUI_KEY_DEL) {
    if (hi > lo) {
      memmove(f->t + lo, f->t + hi, (size_t)(f->n - hi) * sizeof(wch));
      f->n -= hi - lo;
      f->cur = f->anc = lo;
    } else if (k == 8 && f->cur > 0) {
      memmove(f->t + f->cur - 1, f->t + f->cur, (size_t)(f->n - f->cur) * sizeof(wch));
      f->n--;
      f->cur--;
      f->anc = f->cur;
    } else if (k == GUI_KEY_DEL && f->cur < f->n) {
      memmove(f->t + f->cur, f->t + f->cur + 1, (size_t)(f->n - f->cur - 1) * sizeof(wch));
      f->n--;
      f->anc = f->cur;
    }
    return 1;
  }
  if (k == GUI_KEY_LEFT || k == GUI_KEY_RIGHT || k == GUI_KEY_HOME || k == GUI_KEY_END) {
    if (!shift && hi > lo && (k == GUI_KEY_LEFT || k == GUI_KEY_RIGHT)) {
      f->cur = f->anc = k == GUI_KEY_LEFT ? lo : hi;
      return 1;
    }
    if (k == GUI_KEY_LEFT && f->cur > 0) f->cur--;
    else if (k == GUI_KEY_RIGHT && f->cur < f->n) f->cur++;
    else if (k == GUI_KEY_HOME) f->cur = 0;
    else if (k == GUI_KEY_END) f->cur = f->n;
    if (!shift) f->anc = f->cur;
    return 1;
  }
  if ((k >= 32 && k < 127) || (k >= 0x80 && k <= 0xFF)) {
    wch c = (wch)cp850_to_uni(k);
    if (hi > lo) {
      memmove(f->t + lo, f->t + hi, (size_t)(f->n - hi) * sizeof(wch));
      f->n -= hi - lo;
      f->cur = lo;
    }
    if (f->n >= 255) return 1;
    memmove(f->t + f->cur + 1, f->t + f->cur, (size_t)(f->n - f->cur) * sizeof(wch));
    f->t[f->cur++] = c;
    f->n++;
    f->anc = f->cur;
    return 1;
  }
  return 0;
}

static void fld_draw(struct fld* f, int x, int y, int w, int h, int focused) {
  int tx0 = x + 5, ty = y + (h - f_ui.h) / 2, i, cx = tx0;
  int lo = f->cur < f->anc ? f->cur : f->anc, hi = f->cur < f->anc ? f->anc : f->cur;
  int avail = w - 10;
  fill(x, y, w, h, P.field_bg);
  frame(x, y, w, h, focused ? C_FOCUS : C_FIELD_B);
  /* rola para o cursor caber */
  if (f->scroll > f->cur) f->scroll = f->cur;
  while (f->scroll < f->cur && wtext_w(&f_ui, f->t + f->scroll, f->cur - f->scroll) > avail)
    f->scroll++;
  clip_to(x + 2, y + 1, w - 4, h - 2);
  for (i = f->scroll; i < f->n; i++) {
    int idx = gmap(f->t[i]);
    int adv = (f_ui.g[idx].adv + 128) >> 8;
    int sel = focused && i >= lo && i < hi;
    if (cx > x + w) break;
    if (sel) fill(cx, y + 3, adv, h - 6, C_SELBG);
    glyph_draw(&f_ui, idx, cx, ty, sel ? C_SELFG : P.text);
    cx += adv;
  }
  if (focused) {
    int px = tx0 + wtext_w(&f_ui, f->t + f->scroll, f->cur - f->scroll);
    fill(px, y + 4, 1, h - 8, P.text);
  }
  clip_all();
}

static void fld_click(struct fld* f, int x, int px) {
  int i, cx = x + 5;
  for (i = f->scroll; i < f->n; i++) {
    int adv = (f_ui.g[gmap(f->t[i])].adv + 128) >> 8;
    if (px < cx + adv / 2) break;
    cx += adv;
  }
  f->cur = f->anc = i;
}

/* ------------------------------------------------------------ dialogo de arquivo */
#define FMAX 1024
static struct k_direntl fents[FMAX];
static int fidx[FMAX];
static int fn_all, fn_vis;
static char fd_dir[260];
static int fd_sel = -1, fd_top = 0, fd_type = 0, fd_enc = 0, fd_hot = -1;
static int fd_mode;   /* DLG_OPEN ou DLG_SAVEAS */
static unsigned fd_last_ms;
static int fd_last_i = -1;
static const char* const fd_types[2] = {"Documentos de texto (*.txt)", "Todos os arquivos (*.*)"};
static char places[16][40];
static char place_path[16][40];
static int nplaces;
static int fd_lx, fd_ly, fd_lw, fd_lh, fd_rows, fd_px, fd_pw; /* geometria da lista */
static int fd_sb_drag = -1, fd_sb_off;

static int ext_txt(const char* n) {
  const char* d = strrchr(n, '.');
  return d && ci_eq(d, ".txt");
}

static int fent_cmp(const struct k_direntl* a, const struct k_direntl* b) {
  const char* x = a->name;
  const char* y = b->name;
  if (a->dir != b->dir) return a->dir ? -1 : 1;
  while (*x && *y && up_ascii((unsigned char)*x) == up_ascii((unsigned char)*y)) {
    x++;
    y++;
  }
  return up_ascii((unsigned char)*x) - up_ascii((unsigned char)*y);
}

static void fd_filter(void) {
  int i, j;
  fn_vis = 0;
  for (i = 0; i < fn_all; i++) {
    if (!strcmp(fents[i].name, ".") || !strcmp(fents[i].name, "..")) continue;
    if (!fents[i].dir && fd_type == 0 && !ext_txt(fents[i].name)) continue;
    fidx[fn_vis++] = i;
  }
  /* insercao: pastas primeiro, depois por nome */
  for (i = 1; i < fn_vis; i++) {
    int v = fidx[i];
    j = i - 1;
    while (j >= 0 && fent_cmp(&fents[fidx[j]], &fents[v]) > 0) {
      fidx[j + 1] = fidx[j];
      j--;
    }
    fidx[j + 1] = v;
  }
  fd_sel = -1;
  fd_top = 0;
}

static int fd_load(const char* dir) {
  char d[260];
  int n;
  scopy(d, dir, (int)sizeof(d));
  n = (int)strlen(d);
  if (n && d[n - 1] != '\\') scat(d, "\\", (int)sizeof(d));
  n = k_dir_long(d, fents, FMAX);
  if (n < 0) return -1;
  fn_all = n;
  scopy(fd_dir, d, (int)sizeof(fd_dir));
  fld_set_path(&f_addr, fd_dir);
  fd_filter();
  return 0;
}

static void fd_up(void) {
  char d[260];
  int n;
  scopy(d, fd_dir, (int)sizeof(d));
  n = (int)strlen(d);
  if (n <= 3) return;
  n--;   /* tira a barra do fim */
  while (n > 0 && d[n - 1] != '\\') n--;
  d[n] = 0;
  fd_load(d);
}

static void places_init(void) {
  unsigned mask = k_drive_mask();
  int L;
  nplaces = 0;
  {
    static const char* const dd[2] = {"D:\\USER\\DEFAULT\\DESKTOP\\", "U:\\USER\\DEFAULT\\DESKTOP\\"};
    int i;
    for (i = 0; i < 2; i++) {
      struct k_direntl e;
      char t[40];
      scopy(t, dd[i], (int)sizeof(t));
      t[strlen(t) - 1] = 0;
      if ((mask & (1u << (dd[i][0] - 'A'))) && k_dir_entry(t, &e) == 0 && e.dir) {
        scopy(places[nplaces], "Área de Trabalho", 40);
        scopy(place_path[nplaces], dd[i], 40);
        nplaces++;
        break;
      }
    }
  }
  for (L = 0; L < 26 && nplaces < 16; L++) {
    if (!(mask & (1u << L))) continue;
    if (L == 'A' - 'A' || L == 'B' - 'A') continue;
    snprintf(places[nplaces], 40, "Disco (%c:)", 'A' + L);
    snprintf(place_path[nplaces], 40, "%c:\\", 'A' + L);
    nplaces++;
  }
}

static void fmt_size(unsigned sz, char* out, int max) {
  if (sz >= 1024u * 1024u)
    snprintf(out, max, "%u,%u MB", sz / (1024u * 1024u), (sz / (1024u * 102u)) % 10u);
  else
    snprintf(out, max, "%u KB", (sz + 1023u) / 1024u);
}

static void fmt_date(unsigned short d, unsigned short t, char* out, int max) {
  if (!d || !(d & 31) || !((d >> 5) & 15)) {
    out[0] = 0;
    return;
  }
  snprintf(out, max, "%02d/%02d/%04d %02d:%02d", d & 31, (d >> 5) & 15, 1980 + (d >> 9), t >> 11,
           (t >> 5) & 63);
}

/* ------------------------------------------------------------ abrir dialogos */
static void dlg_center(struct dlg* d) {
  d->x = (fbw - d->w) / 2;
  d->y = (fbh - d->h) / 3;
  if (d->x < 0) d->x = 0;
  if (d->y < 0) d->y = 0;
}

static struct dlg* dlg_push(int kind, const char* title, int w, int h) {
  struct dlg* d;
  if (ndlg >= 4) return &dst[3];
  d = &dst[ndlg++];
  memset(d, 0, sizeof(*d));
  d->kind = kind;
  d->w = w > fbw - 8 ? fbw - 8 : w;
  d->h = h > fbh - 8 ? fbh - 8 : h;
  scopy(d->title, title, (int)sizeof(d->title));
  d->focus = -1;
  d->down = -1;
  dlg_center(d);
  menu_close();
  return d;
}

static void dlg_pop(void) {
  if (ndlg > 0) ndlg--;
}

static int btn_w(const char* t) {
  int w = text_w(&f_ui, t) + 30;
  return w < 88 ? 88 : w;
}

static int msg_lines(const char* m, int* maxw) {
  char line[200];
  int n = 0, k = 0, i, w;
  *maxw = 0;
  for (i = 0;; i++) {
    if (m[i] == '\n' || !m[i]) {
      line[k] = 0;
      w = text_w(&f_ui, line);
      if (w > *maxw) *maxw = w;
      n++;
      k = 0;
      if (!m[i]) break;
    } else if (k < 199)
      line[k++] = m[i];
  }
  return n;
}

/* caixa de mensagem com ate 3 botoes (ids e rotulos) */
static struct dlg* msg_dlg(int kind, const char* title, const char* text, int nb, const int* ids,
                           const char* const* labels, int defb) {
  int mw, nl = msg_lines(text, &mw), bw = 0, i, w, h, bx, by;
  struct dlg* d;
  for (i = 0; i < nb; i++) bw += btn_w(labels[i]) + 8;
  w = mw + 60;
  if (w < bw + 40) w = bw + 40;
  if (w < 320) w = 320;
  h = dlg_title_h() + 24 + nl * (f_ui.h + 4) + 24 + 30 + 18;
  d = dlg_push(kind, title, w, h);
  scopy(d->msg, text, (int)sizeof(d->msg));
  by = h - dlg_title_h() - 30 - 12;
  bx = d->w - 12 - bw + 8;
  for (i = 0; i < nb; i++) {
    struct ctl* c = add_ctl(d, CT_BTN, bx, by, btn_w(labels[i]), 28, ids[i], labels[i]);
    if (ids[i] == defb) {
      c->def = 1;
      d->focus = d->nc - 1;
    }
    bx += btn_w(labels[i]) + 8;
  }
  return d;
}

static void msg_box(const char* title, const char* text) {
  static const int ids[1] = {B_OK};
  static const char* const lb[1] = {"OK"};
  msg_dlg(DLG_MSG, title, text, 1, ids, lb, B_OK);
}

static void ask_save(int act) {
  static const int ids[3] = {B_SAVE, B_NOSAVE, B_CANCEL};
  static const char* const lb[3] = {"Salvar", "Não salvar", "Cancelar"};
  char m[400], nm[260];
  struct dlg* d;
  doc_name(nm, (int)sizeof(nm));
  snprintf(m, sizeof(m), "Deseja salvar as alterações de %s?", nm);
  d = msg_dlg(DLG_SAVEASK, "Bloco de notas", m, 3, ids, lb, B_SAVE);
  d->pend = act;
}

static void open_find(int replace) {
  struct dlg* d;
  int y0 = 14, lw = text_w(&f_ui, "Substituir por:") + 16, fw = 230, bx, bwid;
  int rows = replace ? 2 : 1, rh = 30;
  int bw0 = text_w(&f_ui, "Localizar próxima") + 34;
  int w, h;
  struct ctl* c;
  if (bw0 < 130) bw0 = 130;
  w = 14 + lw + fw + 14 + bw0 + 14;
  h = dlg_title_h() + (replace ? 14 + 4 * 34 + 12 : 14 + rows * rh + 16 + 52 + 14);
  /* o texto marcado vira a busca (uma linha so) */
  if (has_sel() && sel_hi() - sel_lo() < 200) {
    int a = sel_lo(), b = sel_hi(), i;
    for (i = a; i < b && tx[i] != '\n'; i++) { }
    if (i == b) fld_set_w(&f_find, tx + a, b - a);
  } else
    fld_set_w(&f_find, find_s, find_n);
  f_find.anc = 0;
  f_find.cur = f_find.n;
  d = dlg_push(replace ? DLG_REPLACE : DLG_FIND, replace ? "Substituir" : "Localizar", w, h);
  add_ctl(d, CT_LABEL, 14, y0 + 6, lw, 20, 0, "Localizar:");
  c = add_ctl(d, CT_EDIT, 14 + lw, y0, fw, 26, 0, "");
  c->f = &f_find;
  d->focus = d->nc - 1;
  if (replace) {
    add_ctl(d, CT_LABEL, 14, y0 + rh + 6, lw, 20, 0, "Substituir por:");
    c = add_ctl(d, CT_EDIT, 14 + lw, y0 + rh, fw, 26, 0, "");
    c->f = &f_repl;
    fld_set_w(&f_repl, repl_s, repl_n);
  }
  bx = 14 + lw + fw + 14;
  bwid = bw0;
  c = add_ctl(d, CT_BTN, bx, y0, bwid, 28, B_FINDNEXT, "Localizar próxima");
  c->def = 1;
  if (replace) {
    add_ctl(d, CT_BTN, bx, y0 + 34, bwid, 28, B_REPLACE, "Substituir");
    add_ctl(d, CT_BTN, bx, y0 + 68, bwid, 28, B_REPLACEALL, "Substituir tudo");
    add_ctl(d, CT_BTN, bx, y0 + 102, bwid, 28, B_CANCEL, "Cancelar");
  } else
    add_ctl(d, CT_BTN, bx, y0 + 34, bwid, 28, B_CANCEL, "Cancelar");
  {
    int cy = y0 + rows * rh + 16;
    c = add_ctl(d, CT_CHECK, 14, cy, 220, 22, 0, "Diferenciar maiúsculas");
    c->val = &find_case;
    c = add_ctl(d, CT_CHECK, 14, cy + 26, 220, 22, 0, "Voltar ao início");
    c->val = &find_wrap;
    if (!replace) {
      int rx = 14 + lw + fw - 122;
      add_ctl(d, CT_LABEL, rx, cy, 100, 20, 0, "Direção");
      c = add_ctl(d, CT_RADIO, rx, cy + 26, 70, 22, 0, "Acima");
      c->val = &find_down;
      c->mine = 0;
      c = add_ctl(d, CT_RADIO, rx + 74, cy + 26, 80, 22, 0, "Abaixo");
      c->val = &find_down;
      c->mine = 1;
    }
  }
}

static void open_goto(void) {
  struct dlg* d;
  struct ctl* c;
  char b[16];
  int line;
  lay_ok();
  line = rln[row_of(cur)] + 1;
  snprintf(b, sizeof(b), "%d", line);
  fld_set_utf8(&f_goto, b);
  f_goto.anc = 0;
  d = dlg_push(DLG_GOTO, "Ir para a linha", 340, dlg_title_h() + 130);
  add_ctl(d, CT_LABEL, 14, 14, 300, 20, 0, "Número da linha:");
  c = add_ctl(d, CT_EDIT, 14, 38, 312, 26, 0, "");
  c->f = &f_goto;
  d->focus = d->nc - 1;
  c = add_ctl(d, CT_BTN, 340 - 14 - 88 - 8 - 88, 80, 88, 28, B_GOTO, "Ir para");
  c->def = 1;
  add_ctl(d, CT_BTN, 340 - 14 - 88, 80, 88, 28, B_CANCEL, "Cancelar");
}

static void fd_layout(struct dlg* d);

static void open_file_dlg(int mode, int pend) {
  struct dlg* d;
  char start[260];
  int w = fbw * 86 / 100, h = fbh * 84 / 100;
  if (w > 860) w = 860;
  if (h > 600) h = 600;
  if (w < 520) w = 520;
  if (h < 360) h = 360;
  places_init();
  /* comeca na pasta do documento, senao na Area de Trabalho, senao no disco */
  start[0] = 0;
  if (doc_path[0]) {
    int i, s = 0;
    scopy(start, doc_path, (int)sizeof(start));
    for (i = 0; start[i]; i++)
      if (start[i] == '\\') s = i + 1;
    start[s] = 0;
  } else if (fd_dir[0])
    scopy(start, fd_dir, (int)sizeof(start));
  else if (nplaces)
    scopy(start, place_path[0], (int)sizeof(start));
  if (!start[0] || fd_load(start) != 0) {
    char r[4];
    r[0] = exe_drive;
    r[1] = ':';
    r[2] = '\\';
    r[3] = 0;
    fd_load(r);
  }
  fd_mode = mode;
  fd_enc = doc_enc;
  fd_type = 0;
  if (mode == DLG_SAVEAS) {
    char nm[260];
    if (doc_path[0]) {
      int i;
      const char* b = doc_path;
      for (i = 0; doc_path[i]; i++)
        if (doc_path[i] == '\\') b = doc_path + i + 1;
      scopy(nm, b, (int)sizeof(nm));
      if (!ext_txt(nm)) fd_type = 1;
    } else
      scopy(nm, "*.txt", (int)sizeof(nm));
    fld_set_path(&f_fname, nm);
  } else
    fld_set_path(&f_fname, "");
  f_fname.anc = 0;
  fd_filter();
  d = dlg_push(mode, mode == DLG_OPEN ? "Abrir" : "Salvar como", w, h);
  d->pend = pend;
  fd_layout(d);
}

/* controles do dialogo de arquivo (a lista e os lugares sao desenhados a parte) */
static void fd_layout(struct dlg* d) {
  int ch = d->h - dlg_title_h();
  int bw = 96, by = ch - 12 - 28;
  int ny = by - (fd_mode == DLG_SAVEAS ? 38 : 38);
  struct ctl* c;
  int lab = text_w(&f_ui, "Codificação:") + 14;
  d->nc = 0;
  c = add_ctl(d, CT_BTN, 12, 10, 34, 26, B_UP, "");
  c = add_ctl(d, CT_EDIT, 52, 10, d->w - 64, 26, 0, "");
  c->f = &f_addr;
  add_ctl(d, CT_LABEL, 12, ny + 4, lab, 20, 0, "Nome:");
  c = add_ctl(d, CT_EDIT, 12 + lab, ny, d->w - 12 - lab - 12 - 272, 26, 0, "");
  c->f = &f_fname;
  d->focus = d->nc - 1;
  c = add_ctl(d, CT_COMBO, d->w - 12 - 260, ny, 260, 26, 0, "");
  c->val = &fd_type;
  c->mine = 2;
  c->opts = fd_types;
  if (fd_mode == DLG_SAVEAS) {
    add_ctl(d, CT_LABEL, 12, by + 4, lab, 20, 0, "Codificação:");
    c = add_ctl(d, CT_COMBO, 12 + lab, by, 180, 28, 0, "");
    c->val = &fd_enc;
    c->mine = NENC;
    c->opts = enc_names;
  }
  c = add_ctl(d, CT_BTN, d->w - 12 - bw - 8 - bw, by, bw, 28, B_FILEOK,
              fd_mode == DLG_OPEN ? "Abrir" : "Salvar");
  c->def = 1;
  add_ctl(d, CT_BTN, d->w - 12 - bw, by, bw, 28, B_CANCEL, "Cancelar");
  /* lista: entre o endereco e o Nome */
  fd_px = d->x + 12;
  fd_pw = 170;
  fd_lx = d->x + 12 + fd_pw + 8;
  fd_ly = d->y + dlg_title_h() + 46;
  fd_lw = d->w - 24 - fd_pw - 8;
  fd_lh = ny - 10 - 46;
  fd_rows = (fd_lh - (f_ui.h + 10)) / (f_ui.h + 8);
  if (fd_rows < 1) fd_rows = 1;
}

/* ------------------------------------------------------------ acoes */
static void do_cmd(int id);
static void tx_font_apply(void);

static void act_do(int act) {
  if (act == ACT_NEW) doc_new();
  else if (act == ACT_OPEN) open_file_dlg(DLG_OPEN, 0);
  else if (act == ACT_EXIT) quit_now = 1;
}

/* pede para salvar antes, se precisar */
static void act_req(int act) {
  if (dirty) ask_save(act);
  else act_do(act);
}

/* Salvar: 1 gravou; 0 abriu o Salvar como (pend continua la); -1 falhou */
static int save_doc(int pend) {
  if (!doc_path[0]) {
    open_file_dlg(DLG_SAVEAS, pend);
    return 0;
  }
  return doc_save_to(doc_path) == 0 ? 1 : -1;
}

static void zoom_set(int z) {
  int c = cur;
  z = clampi(z, 10, 500);
  if (z == opt_zoom) return;
  opt_zoom = z;
  tx_font_apply();
  geometry();
  lay_dirty = 1;
  cur = c;
  ensure_visible();
  cfg_save();
}

static void show_stats(void) {
  int lines = 1, words = 0, chars = 0, nospace = 0, i, inw = 0, bytes = 0, lossy;
  char m[400];
  char* b;
  for (i = 0; i < tn; i++) {
    wch c = tx[i];
    if (c == '\n') lines++;
    else {
      chars++;
      if (!w_isspace(c)) nospace++;
    }
    if (w_isspace(c)) inw = 0;
    else if (!inw) {
      inw = 1;
      words++;
    }
  }
  b = encode_doc(&bytes, &lossy);
  if (b) free(b);
  snprintf(m, sizeof(m),
           "Linhas: %d\nPalavras: %d\nCaracteres: %d\nCaracteres (sem espaços): %d\n"
           "Tamanho no disco: %d bytes (%s, %s)",
           lines, words, chars, nospace, bytes, enc_names[doc_enc], eol_names[doc_eol]);
  msg_box("Estatísticas", m);
}

static void do_cmd(int id) {
  menu_close();
  switch (id) {
    case ID_NEW: act_req(ACT_NEW); break;
    case ID_NEWWIN:
      if (self_path[0]) k_win_launch(self_path);
      break;
    case ID_OPEN: act_req(ACT_OPEN); break;
    case ID_SAVE: save_doc(0); break;
    case ID_SAVEAS: open_file_dlg(DLG_SAVEAS, 0); break;
    case ID_EXIT: act_req(ACT_EXIT); break;
    case ID_UNDO: do_undo(); break;
    case ID_REDO: do_redo(); break;
    case ID_CUT: cmd_cut(); break;
    case ID_COPY: cmd_copy(); break;
    case ID_PASTE: cmd_paste(); break;
    case ID_DEL: cmd_delete(); break;
    case ID_FIND: open_find(0); break;
    case ID_REPLACE: open_find(1); break;
    case ID_FINDNEXT:
      if (find_n) find_next(1);
      else open_find(0);
      break;
    case ID_FINDPREV:
      if (find_n) find_next(0);
      else open_find(0);
      break;
    case ID_GOTO: open_goto(); break;
    case ID_SELALL: cmd_selall(); break;
    case ID_TIME: cmd_time(); break;
    case ID_WRAP:
      opt_wrap = !opt_wrap;
      scroll_x = 0;
      geometry();
      lay_dirty = 1;
      ensure_visible();
      cfg_save();
      break;
    case ID_STATUS:
      opt_status = !opt_status;
      geometry();
      ensure_visible();
      cfg_save();
      break;
    case ID_ZOOMIN: zoom_set(opt_zoom + 10); break;
    case ID_ZOOMOUT: zoom_set(opt_zoom - 10); break;
    case ID_ZOOM0: zoom_set(100); break;
    case ID_UPPER: cmd_case(1); break;
    case ID_LOWER: cmd_case(0); break;
    case ID_STATS: show_stats(); break;
    case ID_ABOUT:
      msg_box("Sobre o Bloco de notas",
              "Bloco de notas do SODOS\n"
              "Versão 1.0 (NOTEPAD.EXE)\n"
              " \n"
              "Abre e grava UTF-8, UTF-16 e ANSI, com fim de linha\n"
              "Windows, Unix ou Macintosh. Fonte TrueType suavizada.");
      break;
    default:
      if (id >= ID_FONT0 && id < ID_FONT0 + NMONO) {
        int c = cur;
        opt_font = id - ID_FONT0;
        tx_font_apply();
        geometry();
        lay_dirty = 1;
        cur = c;
        ensure_visible();
        cfg_save();
      } else if (id >= ID_ENC0 && id < ID_ENC0 + NENC) {
        if (doc_enc != id - ID_ENC0) {
          doc_enc = id - ID_ENC0;
          dirty = 1;
          title_update();
        }
      } else if (id >= ID_EOL0 && id < ID_EOL0 + 3) {
        if (doc_eol != id - ID_EOL0) {
          doc_eol = id - ID_EOL0;
          dirty = 1;
          title_update();
        }
      }
      break;
  }
}

/* respostas dos dialogos */
static void fd_accept(struct dlg* d);

static void dlg_button(struct dlg* d, int id) {
  int kind = d->kind, pend = d->pend;
  switch (kind) {
    case DLG_MSG:
      dlg_pop();
      break;
    case DLG_SAVEASK:
      dlg_pop();
      if (id == B_SAVE) {
        int r = save_doc(pend);
        if (r == 1) act_do(pend);
      } else if (id == B_NOSAVE)
        act_do(pend);
      break;
    case DLG_FIND:
    case DLG_REPLACE:
      if (id == B_CANCEL) {
        dlg_pop();
        break;
      }
      find_n = f_find.n;
      memcpy(find_s, f_find.t, (size_t)find_n * sizeof(wch));
      if (kind == DLG_REPLACE) {
        repl_n = f_repl.n;
        memcpy(repl_s, f_repl.t, (size_t)repl_n * sizeof(wch));
      }
      if (!find_n) break;
      if (id == B_FINDNEXT) find_next(kind == DLG_REPLACE ? 1 : find_down);
      else if (id == B_REPLACE) replace_one();
      else if (id == B_REPLACEALL) {
        int n = replace_all();
        if (n > 0) {
          char m[80];
          snprintf(m, sizeof(m), "%d ocorrência(s) substituída(s).", n);
          msg_box("Substituir", m);
        }
      }
      break;
    case DLG_GOTO:
      if (id == B_GOTO) {
        char b[20];
        int line, r;
        fld_get_utf8(&f_goto, b, (int)sizeof(b));
        line = atoi(b);
        lay_ok();
        if (line < 1 || line > rln[nrows - 1] + 1) {
          msg_box("Bloco de notas - Ir para a linha", "O número da linha está além do total de linhas");
          break;
        }
        dlg_pop();
        for (r = 0; r < nrows && rln[r] != line - 1; r++) { }
        cur = anc = rs[r < nrows ? r : nrows - 1];
        goal_x = -1;
        ensure_visible();
      } else
        dlg_pop();
      break;
    case DLG_OPEN:
    case DLG_SAVEAS:
      if (id == B_CANCEL) dlg_pop();
      else if (id == B_UP) fd_up();
      else if (id == B_FILEOK) fd_accept(d);
      break;
    case DLG_OVERWRITE:
      dlg_pop();
      if (id == B_YES) {
        /* embaixo esta o Salvar como; o caminho ficou em fd_target */
        struct dlg* fdg = dlg_top();
        int pe = fdg ? fdg->pend : 0;
        doc_enc = fd_enc;
        if (doc_save_to(fd_target) == 0) {
          dlg_pop();
          if (pe) act_do(pe);
        }
      }
      break;
  }
}

static int dir_ok(const char* p) {
  struct k_direntl e;
  char t[300];
  int n;
  scopy(t, p, (int)sizeof(t));
  n = (int)strlen(t);
  if (n == 3 && t[1] == ':') return (k_drive_mask() >> (up_ascii(t[0]) - 'A')) & 1;
  while (n > 3 && t[n - 1] == '\\') t[--n] = 0;
  return k_dir_entry(t, &e) == 0 && e.dir;
}

static void fd_accept(struct dlg* d) {
  char nm[260], full[300];
  int n;
  fld_get_path(&f_fname, nm, (int)sizeof(nm));
  /* sem espacos nas pontas */
  while (nm[0] == ' ') memmove(nm, nm + 1, strlen(nm));
  n = (int)strlen(nm);
  while (n > 0 && nm[n - 1] == ' ') nm[--n] = 0;
  if (!n) return;
  if (strchr(nm, '*') || strchr(nm, '?')) {
    /* filtro: *.* mostra tudo, *.txt so texto */
    fd_type = ci_eq(nm, "*.txt") ? 0 : 1;
    fd_filter();
    return;
  }
  if (nm[1] == ':') scopy(full, nm, (int)sizeof(full));
  else if (nm[0] == '\\') {
    full[0] = fd_dir[0];
    full[1] = ':';
    scopy(full + 2, nm, (int)sizeof(full) - 2);
  } else {
    scopy(full, fd_dir, (int)sizeof(full));
    scat(full, nm, (int)sizeof(full));
  }
  if (dir_ok(full)) {
    fd_load(full);
    fld_set_path(&f_fname, "");
    return;
  }
  if (d->kind == DLG_OPEN) {
    int pend = d->pend;
    if (k_fsize(full) < 0) {
      char m[400];
      if (!strchr(nm, '.') && fd_type == 0) {
        char t2[300];
        scopy(t2, full, (int)sizeof(t2));
        scat(t2, ".txt", (int)sizeof(t2));
        if (k_fsize(t2) >= 0) {
          dlg_pop();
          doc_load(t2);
          (void)pend;
          return;
        }
      }
      snprintf(m, sizeof(m), "%s\nArquivo não encontrado.\nVerifique o nome e tente de novo.", nm);
      msg_box("Abrir", m);
      return;
    }
    dlg_pop();
    doc_load(full);
    return;
  }
  /* Salvar como */
  if (!strchr(nm, '.') && fd_type == 0) scat(full, ".txt", (int)sizeof(full));
  scopy(fd_target, full, (int)sizeof(fd_target));
  if (k_fsize(full) >= 0) {
    static const int ids[2] = {B_YES, B_NO};
    static const char* const lb[2] = {"Sim", "Não"};
    char m[400];
    snprintf(m, sizeof(m), "%s já existe.\nDeseja substituí-lo?", nm);
    msg_dlg(DLG_OVERWRITE, "Confirmar Salvar como", m, 2, ids, lb, B_NO);
    return;
  }
  doc_enc = fd_enc;
  if (doc_save_to(full) == 0) {
    int pe = d->pend;
    dlg_pop();
    if (pe) act_do(pe);
  }
}

/* ------------------------------------------------------------ desenho */
static int caret_on = 1;
static int sb_hot = 0;      /* 1 polegar vertical sob o rato, 2 horizontal */
static int sb_drag = 0;     /* 1 arrastando o vertical, 2 o horizontal */
static int sb_drag_off;
static int focus_text = 1;

struct sbg {
  int x, y, w, h;      /* barra inteira */
  int tpos, tlen;      /* polegar (ao longo da barra, sem as setas) */
  int track0, track_len;
};

static void vsb_geom(struct sbg* g) {
  int total, vis, mt;
  lay_ok();
  g->x = fbw - sbw;
  g->y = ed_y;
  g->w = sbw;
  g->h = ed_h;
  g->track0 = g->y + sbw;
  g->track_len = g->h - 2 * sbw;
  if (g->track_len < 8) g->track_len = 8;
  total = nrows + vis_rows - 1;
  vis = vis_rows;
  mt = max_top();
  if (total < 1) total = 1;
  g->tlen = (int)((long long)g->track_len * vis / (total > vis ? total : vis));
  if (g->tlen < 18) g->tlen = 18;
  if (g->tlen > g->track_len) g->tlen = g->track_len;
  g->tpos = mt > 0 ? (int)((long long)(g->track_len - g->tlen) * top_row / mt) : 0;
}

static void hsb_geom(struct sbg* g) {
  int cw = content_w(), mx;
  g->x = 0;
  g->y = ed_y + ed_h;
  g->w = ed_w;
  g->h = sbw;
  g->track0 = g->x + sbw;
  g->track_len = g->w - 2 * sbw;
  if (g->track_len < 8) g->track_len = 8;
  mx = cw - ed_w;
  if (mx < 0) mx = 0;
  g->tlen = cw > ed_w ? (int)((long long)g->track_len * ed_w / cw) : g->track_len;
  if (g->tlen < 18) g->tlen = 18;
  if (g->tlen > g->track_len) g->tlen = g->track_len;
  g->tpos = mx > 0 ? (int)((long long)(g->track_len - g->tlen) * scroll_x / mx) : 0;
}

static void draw_vsb(void) {
  struct sbg g;
  int can = max_top() > 0;
  unsigned th = sb_drag == 1 ? C_SB_DRAG : sb_hot == 1 ? C_SB_HOT : C_SB_THUMB;
  vsb_geom(&g);
  fill(g.x, g.y, g.w, g.h, C_SB_TRACK);
  draw_tri_up(g.x + (sbw - 7) / 2, g.y + sbw / 2 - 2, 4, can ? C_SB_ARROW : P.sb_dis);
  draw_tri_down(g.x + (sbw - 7) / 2, g.y + g.h - sbw / 2 - 2, 4, can ? C_SB_ARROW : P.sb_dis);
  if (can) fill(g.x + 2, g.track0 + g.tpos, g.w - 4, g.tlen, th);
}

static void draw_hsb(void) {
  struct sbg g;
  int can = content_w() > ed_w;
  unsigned th = sb_drag == 2 ? C_SB_DRAG : sb_hot == 2 ? C_SB_HOT : C_SB_THUMB;
  hsb_geom(&g);
  fill(g.x, g.y, g.w, g.h, C_SB_TRACK);
  draw_tri_left(g.x + sbw / 2 - 2, g.y + (sbw - 7) / 2, 4, can ? C_SB_ARROW : P.sb_dis);
  draw_tri_right(g.x + g.w - sbw / 2 - 2, g.y + (sbw - 7) / 2, 4, can ? C_SB_ARROW : P.sb_dis);
  if (can) fill(g.track0 + g.tpos, g.y + 2, g.tlen, g.h - 4, th);
  /* canto entre as duas barras */
  fill(fbw - sbw, g.y, sbw, sbw, C_SB_TRACK);
}

static void draw_text_area(void) {
  int r, s0 = sel_lo(), s1 = sel_hi();
  int x0 = ed_x + pad_l - scroll_x;
  int cw = f_tx.cw;
  lay_ok();
  clip_to(ed_x, ed_y, ed_w, ed_h);
  fill(ed_x, ed_y, ed_w, ed_h, P.ed_bg);
  for (r = top_row; r < nrows && r < top_row + vis_rows + 1; r++) {
    int y = ed_y + (r - top_row) * lh;
    int p = rs[r], e = row_end(r), col = 0;
    int gy = y + (lh - f_tx.h) / 2;
    for (; p < e; p++) {
      wch c = tx[p];
      int w = ch_cols(c, col);
      int x = x0 + col * cw;
      int sel = p >= s0 && p < s1;
      if (x >= ed_x + ed_w) break;
      if (x + w * cw > ed_x) {
        if (sel) fill(x, y, w * cw, lh, C_SELBG);
        if (c != ' ' && c != '\t') {
          if (c < 32 || c == 0x7F) {
            /* controle: caixinha */
            frame(x + 1, gy + 2, cw - 2, f_tx.h - 4, sel ? C_SELFG : P.dim);
          } else
            glyph_draw(&f_tx, gmap(c), x, gy, sel ? C_SELFG : P.ed_fg);
        }
      }
      col += w;
    }
    /* quebra de linha marcada: meia letra azul */
    if (e < tn && tx[e] == '\n' && e >= s0 && e < s1)
      fill(x0 + col * cw, y, cw / 2 + 1, lh, C_SELBG);
  }
  /* cursor */
  if (caret_on && focus_text && !ndlg && m_open < 0) {
    int cr = row_of(cur);
    if (cr >= top_row && cr < top_row + vis_rows + 1) {
      int x = x0 + col_of(cur) * cw;
      int y = ed_y + (cr - top_row) * lh;
      fill(x, y, f_tx.px >= 20 ? 2 : 1, lh, P.ed_fg);
    }
  }
  clip_all();
}

static void draw_menubar(void) {
  int i;
  fill(0, 0, fbw, menu_h, P.bar_bg);
  fill(0, menu_h, fbw, 1, P.bar_line);
  for (i = 0; i < NTOP; i++) {
    int x = mb_item_x(i), w = mb_item_w(i);
    if (m_open == i) {
      fill(x, 1, w, menu_h - 2, C_MB_OPEN);
      frame(x, 1, w, menu_h - 2, C_MB_OPENB);
    } else if (mb_hot == i && m_open < 0) {
      fill(x, 1, w, menu_h - 2, C_MB_HOT);
      frame(x, 1, w, menu_h - 2, C_MB_HOTB);
    }
    text_draw(&f_ui, x + 8, (menu_h - f_ui.h) / 2, top_names[i], P.text);
  }
}

static void draw_status(void) {
  char b[64];
  int y = fbh - stat_h, x = fbw, r, line, col, p;
  static const int widths[4] = {150, 70, 140, 130};
  const char* parts[4];
  char zb[16];
  if (!stat_h) return;
  fill(0, y, fbw, stat_h, C_STAT_BG);
  fill(0, y, fbw, 1, C_STAT_LINE);
  lay_ok();
  r = row_of(cur);
  line = rln[r] + 1;
  col = 1;
  for (p = line_start(cur); p < cur; p++) col++;
  snprintf(b, sizeof(b), "Ln %d, Col %d", line, col);
  snprintf(zb, sizeof(zb), "%d%%", opt_zoom);
  parts[0] = b;
  parts[1] = zb;
  parts[2] = eol_names[doc_eol];
  parts[3] = enc_names[doc_enc];
  /* da direita para a esquerda: codificacao, fim de linha, zoom, posicao */
  {
    int i;
    for (i = 3; i >= 0; i--) {
      int w = widths[i];
      if (i == 3 && text_w(&f_ui, parts[3]) + 24 > w) w = text_w(&f_ui, parts[3]) + 24;
      x -= w;
      if (x < 0) break;
      fill(x, y + 3, 1, stat_h - 5, C_STAT_LINE);
      text_draw(&f_ui, x + 10, y + (stat_h - f_ui.h) / 2 + 1, parts[i], P.text);
    }
  }
}

static void draw_btn(const struct ctl* c, int ox, int oy, int hot, int down, int focus) {
  int x = ox + c->x, y = oy + c->y;
  unsigned bg = C_BTN, bd = C_BTN_B;
  if (down) {
    bg = C_BTN_DOWN;
    bd = C_BTN_DOWNB;
  } else if (hot) {
    bg = C_BTN_HOT;
    bd = C_FOCUS;
  } else if (c->def || focus)
    bd = C_FOCUS;
  fill(x, y, c->w, c->h, bg);
  frame(x, y, c->w, c->h, bd);
  if ((c->def || focus) && !down) frame(x + 1, y + 1, c->w - 2, c->h - 2, bd);
  if (c->id == B_UP) {
    /* seta para cima */
    int cx = x + c->w / 2, cy = y + 7, i;
    for (i = 0; i < 6; i++) fill(cx - i, cy + i, 2 * i + 1, 1, P.text);
    fill(cx - 1, cy + 6, 3, 7, P.text);
  } else
    text_draw(&f_ui, x + (c->w - text_w(&f_ui, c->text)) / 2, y + (c->h - f_ui.h) / 2, c->text,
              P.text);
}

static int hit_ctl(const struct dlg* d, int x, int y) {
  int i, ox = d->x, oy = d->y + dlg_title_h();
  for (i = 0; i < d->nc; i++) {
    const struct ctl* c = &d->c[i];
    if (c->type == CT_LABEL) continue;
    if (x >= ox + c->x && x < ox + c->x + c->w && y >= oy + c->y && y < oy + c->y + c->h) return i;
  }
  return -1;
}

static void draw_file_lists(struct dlg* d) {
  int i, ih = f_ui.h + 8, hh = f_ui.h + 10;
  int col_size = fd_lw - 96;
  int col_date = col_size - text_w(&f_ui, "Data de modificação") - 40;
  int best = -1, bestn = 0;
  /* lugares: marca o de prefixo mais longo da pasta aberta */
  for (i = 0; i < nplaces; i++) {
    int n = (int)strlen(place_path[i]), k;
    for (k = 0; k < n && fd_dir[k] && up_ascii((unsigned char)fd_dir[k]) ==
                                          up_ascii((unsigned char)place_path[i][k]); k++) { }
    if (k == n && n > bestn) {
      best = i;
      bestn = n;
    }
  }
  fill(fd_px, fd_ly, fd_pw, fd_lh, P.list_bg);
  frame(fd_px, fd_ly, fd_pw, fd_lh, P.list_b);
  for (i = 0; i < nplaces; i++) {
    int y = fd_ly + 6 + i * (ih + 2);
    int on = i == best;
    if (on) fill(fd_px + 2, y, fd_pw - 4, ih, C_LIST_SEL);
    /* icone: disco ou pasta */
    fill(fd_px + 10, y + ih / 2 - 5, 14, 10, i == 0 && places[0][0] != 'D' ? 0x4DA3E0 : 0x9AA0A6);
    fill(fd_px + 10, y + ih / 2 + 2, 14, 2, 0x5F6368);
    text_fit(&f_ui, fd_px + 30, y + (ih - f_ui.h) / 2, places[i], fd_pw - 36, P.text);
  }
  /* lista */
  fill(fd_lx, fd_ly, fd_lw, fd_lh, P.list_bg);
  frame(fd_lx, fd_ly, fd_lw, fd_lh, P.list_b);
  text_draw(&f_ui, fd_lx + 30, fd_ly + (hh - f_ui.h) / 2, "Nome", C_HEAD_TEXT);
  if (col_date > 200) text_draw(&f_ui, fd_lx + col_date, fd_ly + (hh - f_ui.h) / 2, "Data de modificação", C_HEAD_TEXT);
  text_draw(&f_ui, fd_lx + col_size, fd_ly + (hh - f_ui.h) / 2, "Tamanho", C_HEAD_TEXT);
  fill(fd_lx + 1, fd_ly + hh, fd_lw - 2, 1, P.list_b);
  if (fd_top > fn_vis - fd_rows) fd_top = fn_vis - fd_rows;
  if (fd_top < 0) fd_top = 0;
  clip_to(fd_lx + 1, fd_ly + hh + 1, fd_lw - 2 - 14, fd_lh - hh - 2);
  for (i = 0; i < fd_rows && fd_top + i < fn_vis; i++) {
    int k = fd_top + i;
    const struct k_direntl* e = &fents[fidx[k]];
    int y = fd_ly + hh + 2 + i * ih;
    char nm[300], sz[32], dt[32];
    int j, o = 0;
    if (k == fd_sel) fill(fd_lx + 2, y, fd_lw - 18, ih, C_LIST_SEL);
    else if (k == fd_hot) fill(fd_lx + 2, y, fd_lw - 18, ih, C_LIST_HOT);
    /* icone */
    if (e->dir) {
      fill(fd_lx + 8, y + ih / 2 - 5, 16, 11, 0xF8D775);
      fill(fd_lx + 8, y + ih / 2 - 7, 7, 3, 0xE8B739);
    } else {
      fill(fd_lx + 10, y + ih / 2 - 7, 12, 15, 0xFFFFFF);
      frame(fd_lx + 10, y + ih / 2 - 7, 12, 15, 0x8A8A8A);
      fill(fd_lx + 12, y + ih / 2 - 3, 8, 1, 0x8A8A8A);
      fill(fd_lx + 12, y + ih / 2, 8, 1, 0x8A8A8A);
      fill(fd_lx + 12, y + ih / 2 + 3, 6, 1, 0x8A8A8A);
    }
    /* nome do FAT (bytes Latin-1) -> UTF-8 para desenhar */
    for (j = 0; e->name[j] && o < (int)sizeof(nm) - 4; j++)
      o += u8_put((unsigned char)e->name[j], nm + o);
    nm[o] = 0;
    text_fit(&f_ui, fd_lx + 30, y + (ih - f_ui.h) / 2, nm,
             (col_date > 200 ? col_date : col_size) - 40, P.text);
    if (col_date > 200) {
      fmt_date(e->date, e->time, dt, (int)sizeof(dt));
      text_draw(&f_ui, fd_lx + col_date, y + (ih - f_ui.h) / 2, dt, P.dim);
    }
    if (!e->dir) {
      fmt_size(e->size, sz, (int)sizeof(sz));
      text_draw(&f_ui, fd_lx + col_size, y + (ih - f_ui.h) / 2, sz, P.dim);
    }
  }
  clip_all();
  if (fn_vis == 0)
    text_draw(&f_ui, fd_lx + 30, fd_ly + hh + 10, "Nenhum item corresponde à pesquisa.", P.dim);
  /* barra de rolagem da lista */
  if (fn_vis > fd_rows) {
    int tx0 = fd_lx + fd_lw - 14, ty0 = fd_ly + hh + 1, th = fd_lh - hh - 2;
    int tl = th * fd_rows / fn_vis, tp;
    if (tl < 16) tl = 16;
    tp = (th - tl) * fd_top / (fn_vis - fd_rows);
    fill(tx0, ty0, 13, th, C_SB_TRACK);
    fill(tx0 + 2, ty0 + tp, 9, tl, fd_sb_drag >= 0 ? C_SB_DRAG : C_SB_THUMB);
  }
  (void)d;
}

static void draw_dlg(struct dlg* d, int top) {
  int i, ox = d->x, oy = d->y + dlg_title_h(), th = dlg_title_h();
  int hot = top ? hit_ctl(d, mx_mouse, my_mouse) : -1;
  shadow(d->x, d->y, d->w, d->h, 6);
  fill(d->x, d->y, d->w, d->h, C_DLG_BG);
  fill(d->x, d->y, d->w, th, P.dlg_title);
  frame(d->x, d->y, d->w, d->h, top ? P.dlg_frame : P.dlg_frame_in);
  text_draw(&f_ui, d->x + 12, d->y + (th - f_ui.h) / 2, d->title, top ? P.text : P.dim);
  /* X do dialogo */
  {
    int bx = d->x + d->w - 46, bh = th - 1;
    int on = top && mx_mouse >= bx && mx_mouse < bx + 45 && my_mouse >= d->y && my_mouse < d->y + bh;
    int cx = bx + 22, cy = d->y + th / 2, k;
    if (on) fill(bx, d->y + 1, 45, bh - 1, 0xE81123);
    for (k = -5; k <= 5; k++) {
      fill(cx + k, cy + k, 1, 1, on ? 0xFFFFFF : P.text);
      fill(cx + k, cy - k, 1, 1, on ? 0xFFFFFF : P.text);
    }
  }
  if (d->kind == DLG_MSG || d->kind == DLG_SAVEASK || d->kind == DLG_OVERWRITE) {
    /* corpo branco e faixa cinza dos botoes, como no Windows 10 */
    char line[200];
    int k = 0, ly = oy + 22, j;
    fill(d->x + 1, oy, d->w - 2, d->h - th - 52, P.dlg_body);
    for (j = 0;; j++) {
      if (d->msg[j] == '\n' || !d->msg[j]) {
        line[k] = 0;
        text_draw(&f_ui, d->x + 24, ly, line, d->kind == DLG_SAVEASK ? P.ask : P.text);
        ly += f_ui.h + 4;
        k = 0;
        if (!d->msg[j]) break;
      } else if (k < 199)
        line[k++] = d->msg[j];
    }
  }
  if (d->kind == DLG_OPEN || d->kind == DLG_SAVEAS) draw_file_lists(d);
  for (i = 0; i < d->nc; i++) {
    struct ctl* c = &d->c[i];
    int x = ox + c->x, y = oy + c->y;
    int foc = top && d->focus == i;
    switch (c->type) {
      case CT_LABEL: text_draw(&f_ui, x, y, c->text, P.text); break;
      case CT_EDIT: fld_draw(c->f, x, y, c->w, c->h, foc); break;
      case CT_BTN: draw_btn(c, ox, oy, hot == i, top && d->down == i && hot == i, foc); break;
      case CT_CHECK: {
        int s = 13, by = y + (c->h - s) / 2;
        fill(x, by, s, s, P.field_bg);
        frame(x, by, s, s, hot == i ? C_FOCUS : P.check);
        if (*c->val) draw_check(x + 2, by + 1, 9, P.text);
        text_draw(&f_ui, x + s + 8, y + (c->h - f_ui.h) / 2, c->text, P.text);
        if (foc) frame(x + s + 5, y + 1, text_w(&f_ui, c->text) + 6, c->h - 2, P.dim);
        break;
      }
      case CT_RADIO: {
        int r = 6, cx = x + r, cy = y + c->h / 2, yy;
        for (yy = -r; yy <= r; yy++) {
          int xo = 0, xi = -1;
          while ((xo + 1) * (xo + 1) + yy * yy <= r * r + r) xo++;
          if (yy * yy <= (r - 1) * (r - 1)) {
            xi = 0;
            while ((xi + 1) * (xi + 1) + yy * yy <= (r - 1) * (r - 1) + (r - 1)) xi++;
          }
          fill(cx - xo, cy + yy, 2 * xo + 1, 1, hot == i ? C_FOCUS : P.check);
          if (xi >= 0) fill(cx - xi, cy + yy, 2 * xi + 1, 1, P.field_bg);
        }
        if (*c->val == c->mine) {
          fill(cx - 2, cy - 3, 5, 7, P.text);
          fill(cx - 3, cy - 2, 7, 5, P.text);
        }
        text_draw(&f_ui, x + 2 * r + 8, y + (c->h - f_ui.h) / 2, c->text, P.text);
        break;
      }
      case CT_COMBO: {
        const char* t = c->opts[clampi(*c->val, 0, c->mine - 1)];
        fill(x, y, c->w, c->h, hot == i ? C_BTN_HOT : C_BTN);
        frame(x, y, c->w, c->h, hot == i || foc ? C_FOCUS : C_BTN_B);
        text_fit(&f_ui, x + 8, y + (c->h - f_ui.h) / 2, t, c->w - 30, P.text);
        draw_tri_down(x + c->w - 16, y + c->h / 2 - 2, 4, P.text);
        break;
      }
    }
  }
}

static void render(void) {
  int i;
  clip_all();
  geometry();
  lay_ok();
  clamp_scroll();
  draw_menubar();
  draw_text_area();
  draw_vsb();
  if (hsb_on) draw_hsb();
  draw_status();
  for (i = 0; i < ndlg; i++) draw_dlg(&dst[i], i == ndlg - 1);
  if (m_open >= 0) {
    struct pg g;
    open_geom(&g);
    draw_pop(open_menu_items(), &g, m_hot, m_sub);
    if (m_sub >= 0) {
      struct pg sg;
      sub_geom(&sg);
      draw_pop(sub_menu(open_menu_items()[m_sub].sub), &sg, m_subhot, -1);
    }
  }
  clip_all();
}

static void present(void) {
  gui_pixels(0, 0, fbw, fbh, fb, fbw * 4);
  gui_present();
}

/* Como o gui_wait, mas a janela que sumiu (k_gui_event < 0) vira -1. */
static int wait_ev(gui_event_t* ev, int ms) {
  int raw[4];
  int t = k_gui_event(raw, ms);
  if (t < 0) {
    ev->type = -1;
    return -1;
  }
  ev->type = raw[0];
  ev->x = raw[1];
  ev->y = raw[2];
  ev->key = raw[3];
  return ev->type;
}

static int fb_alloc(void) {
  int w = 0, h = 0;
  k_gui_size(&w, &h);
  unsigned* n;
  if (w < 64) w = 64;
  if (h < 64) h = 64;
  if (fb && w == fbw && h == fbh) return 1;
  n = (unsigned*)malloc((size_t)w * (size_t)h * 4);
  if (!n) return 0;
  if (fb) free(fb);
  fb = n;
  fbw = w;
  fbh = h;
  clip_all();
  for (w = 0; w < ndlg; w++) {
    dlg_center(&dst[w]);
    if (dst[w].kind == DLG_OPEN || dst[w].kind == DLG_SAVEAS) fd_layout(&dst[w]);
  }
  return 1;
}

/* ------------------------------------------------------------ teclado no texto */
static void move_caret(int np, int shift) {
  cur = clampi(np, 0, tn);
  if (!shift) anc = cur;
  u_break = 1;
}

static void key_text(int k, int mods) {
  int shift = mods & GUI_MOD_SHIFT, ctrl = mods & GUI_MOD_CTRL;
  lay_ok();
  if (k == GUI_KEY_LEFT) {
    if (!shift && has_sel() && !ctrl) move_caret(sel_lo(), 0);
    else move_caret(ctrl ? word_left(cur) : cur - 1 - (cur >= 2 && tx[cur - 1] >= 0xDC00 && tx[cur - 1] < 0xE000), shift);
    goal_x = -1;
  } else if (k == GUI_KEY_RIGHT) {
    if (!shift && has_sel() && !ctrl) move_caret(sel_hi(), 0);
    else move_caret(ctrl ? word_right(cur) : cur + 1 + (cur + 1 < tn && tx[cur] >= 0xD800 && tx[cur] < 0xDC00), shift);
    goal_x = -1;
  } else if (k == GUI_KEY_UP || k == GUI_KEY_DOWN || k == GUI_KEY_PGUP || k == GUI_KEY_PGDN) {
    int r = row_of(cur), nr;
    int d = (k == GUI_KEY_UP) ? -1 : (k == GUI_KEY_DOWN) ? 1 : (k == GUI_KEY_PGUP) ? -vis_rows : vis_rows;
    if (ctrl && (k == GUI_KEY_UP || k == GUI_KEY_DOWN)) {
      top_row = clampi(top_row + d, 0, max_top());
      return;
    }
    if (goal_x < 0) goal_x = col_of(cur);
    nr = r + d;
    if (nr < 0) {
      move_caret(0, shift);
    } else if (nr >= nrows) {
      move_caret(tn, shift);
    } else {
      move_caret(pos_at(nr, goal_x), shift);
    }
    if (k == GUI_KEY_PGUP || k == GUI_KEY_PGDN) top_row = clampi(top_row + d, 0, max_top());
    {
      int g = goal_x;
      ensure_visible();
      goal_x = g;
    }
    return;
  } else if (k == GUI_KEY_HOME) {
    if (ctrl) move_caret(0, shift);
    else {
      /* Home: vai ao primeiro nao-espaco; de novo, ao comeco da linha */
      int r = row_of(cur), p = rs[r], e = row_end(r), q = p;
      while (q < e && (tx[q] == ' ' || tx[q] == '\t')) q++;
      if (rln[r] != (r > 0 ? rln[r - 1] : -1) && cur != q) move_caret(q, shift);
      else move_caret(p, shift);
    }
    goal_x = -1;
  } else if (k == GUI_KEY_END) {
    if (ctrl) move_caret(tn, shift);
    else {
      int r = row_of(cur), e = row_end(r);
      if (r + 1 < nrows && rln[r + 1] == rln[r] && e > rs[r]) e--;
      move_caret(e, shift);
    }
    goal_x = -1;
  } else if (k == GUI_KEY_DEL) {
    if (ctrl && !has_sel()) {
      int e = word_right(cur);
      if (e > cur) edit(cur, e - cur, 0, 0, 0);
    } else
      cmd_delete();
  } else if (k == 8) {
    if (has_sel()) ins_text(0, 0, 0);
    else if (cur > 0) {
      int n = 1;
      if (cur >= 2 && tx[cur - 1] >= 0xDC00 && tx[cur - 1] < 0xE000 && tx[cur - 2] >= 0xD800 &&
          tx[cur - 2] < 0xDC00)
        n = 2;
      edit(cur - n, n, 0, 0, 2);
    }
  } else if (k == '\r' || k == '\n') {
    wch nl = '\n';
    ins_text(&nl, 1, 1);
  } else if (k == '\t' || (k >= 32 && k < 127) || (k >= 0x80 && k <= 0xFF)) {
    wch c = (wch)cp850_to_uni(k);
    ins_text(&c, 1, 1);
  }
  ensure_visible();
}

/* Ctrl+letra (1..26) e teclas F */
static int key_command(int k, int mods) {
  int shift = mods & GUI_MOD_SHIFT;
  switch (k) {
    case 1: do_cmd(ID_SELALL); return 1;
    case 3: do_cmd(ID_COPY); return 1;
    case 24: do_cmd(ID_CUT); return 1;
    case 22: do_cmd(ID_PASTE); return 1;
    case 26: do_cmd(ID_UNDO); return 1;
    case 25: do_cmd(ID_REDO); return 1;
    case 19: do_cmd(shift ? ID_SAVEAS : ID_SAVE); return 1;
    case 15: do_cmd(ID_OPEN); return 1;
    case 14: do_cmd(shift ? ID_NEWWIN : ID_NEW); return 1;
    case 6: do_cmd(ID_FIND); return 1;
    case 7: do_cmd(ID_GOTO); return 1;
    case GUI_KEY_CTRL + 8: do_cmd(ID_REPLACE); return 1;
    case GUI_KEY_F1: do_cmd(ID_ABOUT); return 1;
    case GUI_KEY_F1 + 2: do_cmd(shift ? ID_FINDPREV : ID_FINDNEXT); return 1;
    case GUI_KEY_F1 + 4: do_cmd(ID_TIME); return 1;
    case GUI_KEY_F1 + 9:
      menu_open_top(0, 0);
      return 1;
  }
  return 0;
}

/* proximo controle focavel */
static void dlg_tab(struct dlg* d, int back) {
  int i, n = d->nc;
  for (i = 1; i <= n; i++) {
    int k = ((d->focus < 0 ? 0 : d->focus) + (back ? -i : i) + n * 4) % n;
    if (d->c[k].type != CT_LABEL) {
      d->focus = k;
      return;
    }
  }
}

static void dlg_activate(struct dlg* d, int i) {
  struct ctl* c = &d->c[i];
  if (c->type == CT_BTN) dlg_button(d, c->id);
  else if (c->type == CT_CHECK) *c->val = !*c->val;
  else if (c->type == CT_RADIO) *c->val = c->mine;
  else if (c->type == CT_COMBO) {
    *c->val = (*c->val + 1) % c->mine;
    if (c->val == &fd_type) fd_filter();
  }
}

static int dlg_default(struct dlg* d) {
  int i;
  for (i = 0; i < d->nc; i++)
    if (d->c[i].type == CT_BTN && d->c[i].def) return i;
  return -1;
}

static void dlg_key(struct dlg* d, int k, int mods) {
  struct ctl* fc = d->focus >= 0 && d->focus < d->nc ? &d->c[d->focus] : 0;
  if (k == 27) {
    if (d->kind == DLG_SAVEASK) dlg_button(d, B_CANCEL);
    else if (d->kind == DLG_MSG) dlg_button(d, B_OK);
    else if (d->kind == DLG_OVERWRITE) dlg_button(d, B_NO);
    else dlg_button(d, B_CANCEL);
    return;
  }
  if (k == '\t') {
    dlg_tab(d, mods & GUI_MOD_SHIFT);
    return;
  }
  if (k == '\r' || k == '\n') {
    int di;
    /* Enter no endereco do dialogo de arquivo: vai para a pasta */
    if (fc && fc->type == CT_EDIT && fc->f == &f_addr) {
      char p[300];
      fld_get_path(&f_addr, p, (int)sizeof(p));
      if (dir_ok(p)) fd_load(p);
      else msg_box("Abrir", "Pasta não encontrada.");
      return;
    }
    if (fc && fc->type == CT_BTN) {
      dlg_button(d, fc->id);
      return;
    }
    di = dlg_default(d);
    if (di >= 0) dlg_button(d, d->c[di].id);
    return;
  }
  if ((d->kind == DLG_OPEN || d->kind == DLG_SAVEAS) &&
      (k == GUI_KEY_UP || k == GUI_KEY_DOWN || k == GUI_KEY_PGUP || k == GUI_KEY_PGDN) &&
      !(fc && fc->f == &f_addr)) {
    int s = fd_sel < 0 ? -1 : fd_sel;
    int dd = k == GUI_KEY_UP ? -1 : k == GUI_KEY_DOWN ? 1 : k == GUI_KEY_PGUP ? -fd_rows : fd_rows;
    if (!fn_vis) return;
    s = clampi(s + dd, 0, fn_vis - 1);
    fd_sel = s;
    if (fd_sel < fd_top) fd_top = fd_sel;
    if (fd_sel >= fd_top + fd_rows) fd_top = fd_sel - fd_rows + 1;
    if (!fents[fidx[s]].dir) fld_set_path(&f_fname, fents[fidx[s]].name);
    return;
  }
  if (fc && fc->type == CT_EDIT) {
    fld_key(fc->f, k, mods);
    return;
  }
  if (fc && k == ' ') {
    dlg_activate(d, d->focus);
    return;
  }
  if (fc && (fc->type == CT_RADIO || fc->type == CT_COMBO) &&
      (k == GUI_KEY_LEFT || k == GUI_KEY_RIGHT || k == GUI_KEY_UP || k == GUI_KEY_DOWN)) {
    dlg_activate(d, d->focus);
    return;
  }
  if (fc && fc->type == CT_BTN && (k == GUI_KEY_LEFT || k == GUI_KEY_RIGHT)) {
    dlg_tab(d, k == GUI_KEY_LEFT);
    return;
  }
}

/* clique na lista do dialogo de arquivo (1 tratou) */
static int fd_click(struct dlg* d, int x, int y, int is_down) {
  int ih = f_ui.h + 8, hh = f_ui.h + 10, i;
  if (!is_down) return 0;
  /* lugares */
  if (x >= fd_px && x < fd_px + fd_pw && y >= fd_ly && y < fd_ly + fd_lh) {
    for (i = 0; i < nplaces; i++) {
      int yy = fd_ly + 6 + i * (ih + 2);
      if (y >= yy && y < yy + ih) {
        fd_load(place_path[i]);
        return 1;
      }
    }
    return 1;
  }
  if (x >= fd_lx && x < fd_lx + fd_lw && y >= fd_ly && y < fd_ly + fd_lh) {
    int row;
    /* barra de rolagem */
    if (x >= fd_lx + fd_lw - 15 && fn_vis > fd_rows) {
      int ty0 = fd_ly + hh + 1, th = fd_lh - hh - 2;
      int tl = th * fd_rows / fn_vis, tp;
      if (tl < 16) tl = 16;
      tp = (th - tl) * fd_top / (fn_vis - fd_rows);
      if (y >= ty0 + tp && y < ty0 + tp + tl) {
        fd_sb_drag = 1;
        fd_sb_off = y - (ty0 + tp);
      } else if (y < ty0 + tp)
        fd_top -= fd_rows;
      else
        fd_top += fd_rows;
      return 1;
    }
    if (y < fd_ly + hh + 2) return 1;
    row = (y - (fd_ly + hh + 2)) / ih;
    i = fd_top + row;
    if (row >= fd_rows || i >= fn_vis) {
      fd_sel = -1;
      return 1;
    }
    {
      unsigned now = k_ms();
      int dbl = fd_last_i == i && now - fd_last_ms < 500;
      fd_last_i = i;
      fd_last_ms = now;
      fd_sel = i;
      if (fents[fidx[i]].dir) {
        if (dbl) {
          char p[300];
          scopy(p, fd_dir, (int)sizeof(p));
          scat(p, fents[fidx[i]].name, (int)sizeof(p));
          fd_load(p);
          fd_last_i = -1;
        }
      } else {
        fld_set_path(&f_fname, fents[fidx[i]].name);
        if (dbl) {
          fd_last_i = -1;
          fd_accept(d);
        }
      }
    }
    return 1;
  }
  return 0;
}

static void fd_drag(int y) {
  int hh = f_ui.h + 10;
  int ty0 = fd_ly + hh + 1, th = fd_lh - hh - 2;
  int tl, span;
  if (fn_vis <= fd_rows) return;
  tl = th * fd_rows / fn_vis;
  if (tl < 16) tl = 16;
  span = th - tl;
  if (span < 1) span = 1;
  fd_top = (y - fd_sb_off - ty0) * (fn_vis - fd_rows) / span;
  fd_top = clampi(fd_top, 0, fn_vis - fd_rows);
}

/* ------------------------------------------------------------ rato no texto */
static unsigned last_click_ms;
static int last_click_x, last_click_y, click_count;
static int text_drag;
static int word_anchor0, word_anchor1;

static int pos_from_xy(int x, int y) {
  int r, col;
  lay_ok();
  r = top_row + (y - ed_y) / lh;
  if (y < ed_y) r = top_row - 1 + (y - ed_y) / lh;
  r = clampi(r, 0, nrows - 1);
  col = (x - ed_x - pad_l + scroll_x);
  col = col < 0 ? 0 : (col + f_tx.cw / 2) / f_tx.cw;
  {
    /* pos_at arredonda pela metade da letra; aqui ja arredondamos */
    int p, c = 0, e = row_end(r);
    for (p = rs[r]; p < e; p++) {
      int w = ch_cols(tx[p], c);
      if (c + w > col || (w > 1 && c + w / 2 >= col)) break;
      c += w;
    }
    if (p == e && r + 1 < nrows && rln[r + 1] == rln[r] && e > rs[r]) p = e - 1;
    return p;
  }
}

static void select_word_at(int p) {
  int a = p, b = p;
  if (p < tn && w_isword(tx[p])) {
    while (a > 0 && w_isword(tx[a - 1])) a--;
    while (b < tn && w_isword(tx[b])) b++;
  } else if (p < tn && tx[p] != '\n') {
    b = p + 1;
  }
  while (b < tn && tx[b] == ' ') b++;
  anc = a;
  cur = b;
  word_anchor0 = a;
  word_anchor1 = b;
}

static void sb_click(int x, int y) {
  struct sbg g;
  vsb_geom(&g);
  if (x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h) {
    if (y < g.track0) top_row--;
    else if (y >= g.track0 + g.track_len) top_row++;
    else if (y < g.track0 + g.tpos) top_row -= vis_rows - 1;
    else if (y >= g.track0 + g.tpos + g.tlen) top_row += vis_rows - 1;
    else {
      sb_drag = 1;
      sb_drag_off = y - (g.track0 + g.tpos);
    }
    clamp_scroll();
    return;
  }
  if (hsb_on) {
    hsb_geom(&g);
    if (x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h) {
      int page = ed_w * 3 / 4;
      if (x < g.track0) scroll_x -= f_tx.cw * 4;
      else if (x >= g.track0 + g.track_len) scroll_x += f_tx.cw * 4;
      else if (x < g.track0 + g.tpos) scroll_x -= page;
      else if (x >= g.track0 + g.tpos + g.tlen) scroll_x += page;
      else {
        sb_drag = 2;
        sb_drag_off = x - (g.track0 + g.tpos);
      }
      clamp_scroll();
    }
  }
}

static void sb_drag_to(int x, int y) {
  struct sbg g;
  if (sb_drag == 1) {
    int span, mt = max_top();
    vsb_geom(&g);
    span = g.track_len - g.tlen;
    if (span < 1) span = 1;
    top_row = (int)((long long)(y - sb_drag_off - g.track0) * mt / span);
  } else if (sb_drag == 2) {
    int span, mx = content_w() - ed_w;
    hsb_geom(&g);
    span = g.track_len - g.tlen;
    if (span < 1) span = 1;
    scroll_x = (int)((long long)(x - sb_drag_off - g.track0) * mx / span);
  }
  clamp_scroll();
}

/* ------------------------------------------------------------ eventos do menu */
static int menubar_hit(int x, int y) {
  int i;
  if (y < 0 || y >= menu_h) return -1;
  for (i = 0; i < NTOP; i++)
    if (x >= mb_item_x(i) && x < mb_item_x(i) + mb_item_w(i)) return i;
  return -1;
}

/* rato com o menu aberto. down: 1 clique, 0 movimento. 1 = o menu tratou. */
static int menu_mouse(int x, int y, int down) {
  struct pg g, sg;
  const struct mi* m = open_menu_items();
  int h, hb;
  if (!m) return 0;
  open_geom(&g);
  if (m_sub >= 0) {
    const struct mi* sm = sub_menu(m[m_sub].sub);
    sub_geom(&sg);
    h = pop_hit(sm, &sg, x, y);
    if (h != -2) {
      m_subhot = h;
      if (down && h >= 0 && mi_enabled(sm[h].id)) do_cmd(sm[h].id);
      return 1;
    }
  }
  h = pop_hit(m, &g, x, y);
  if (h != -2) {
    m_hot = h;
    if (h >= 0 && m[h].sub) {
      if (m_sub != h) {
        m_sub = h;
        m_subhot = -1;
      }
    } else if (h >= 0)
      m_sub = -1;   /* outro item: o submenu fecha, como no Windows */
    if (down && h >= 0 && !m[h].sub && mi_enabled(m[h].id)) do_cmd(m[h].id);
    return 1;
  }
  hb = menubar_hit(x, y);
  if (hb >= 0 && m_open != MENU_CTX) {
    if (down) {
      if (hb == m_open) menu_close();
      else menu_open_top(hb, -1);
    } else if (hb != m_open)
      menu_open_top(hb, -1);
    return 1;
  }
  if (down) {
    menu_close();
    return 1;   /* clique fora so fecha (como no Windows) */
  }
  return 1;
}

static void menu_key(int k) {
  const struct mi* m = open_menu_items();
  int n, i;
  if (!m) return;
  for (n = 0; m[n].t; n++) { }
  if (m_sub >= 0 && m_subhot >= -1) {
    const struct mi* sm = sub_menu(m[m_sub].sub);
    int sn;
    for (sn = 0; sm[sn].t; sn++) { }
    if (m_subhot >= 0 || k == GUI_KEY_RIGHT) {
      if (k == GUI_KEY_DOWN || k == GUI_KEY_UP) {
        int d = k == GUI_KEY_DOWN ? 1 : -1, s = m_subhot;
        for (i = 0; i < sn; i++) {
          s = (s + d + sn) % sn;
          if (!(sm[s].t[0] == '-' && !sm[s].t[1])) break;
        }
        m_subhot = s;
        return;
      }
      if (k == GUI_KEY_LEFT || k == 27) {
        m_subhot = -1;
        m_sub = -1;
        return;
      }
      if ((k == '\r' || k == '\n' || k == ' ') && m_subhot >= 0) {
        if (mi_enabled(sm[m_subhot].id)) do_cmd(sm[m_subhot].id);
        return;
      }
      if (k == GUI_KEY_RIGHT && m_subhot < 0) {
        m_subhot = 0;
        return;
      }
    }
  }
  if (k == 27) {
    menu_close();
    return;
  }
  if (k == GUI_KEY_DOWN || k == GUI_KEY_UP) {
    int d = k == GUI_KEY_DOWN ? 1 : -1, s = m_hot;
    for (i = 0; i < n; i++) {
      s = (s + d + n) % n;
      if (!(m[s].t[0] == '-' && !m[s].t[1])) break;
    }
    m_hot = s;
    m_sub = -1;
    return;
  }
  if (k == GUI_KEY_RIGHT) {
    if (m_hot >= 0 && m[m_hot].sub) {
      m_sub = m_hot;
      m_subhot = 0;
    } else if (m_open < NTOP)
      menu_open_top((m_open + 1) % NTOP, 0);
    return;
  }
  if (k == GUI_KEY_LEFT) {
    if (m_open < NTOP) menu_open_top((m_open + NTOP - 1) % NTOP, 0);
    return;
  }
  if ((k == '\r' || k == '\n' || k == ' ') && m_hot >= 0) {
    if (m[m_hot].sub) {
      m_sub = m_hot;
      m_subhot = 0;
    } else if (mi_enabled(m[m_hot].id))
      do_cmd(m[m_hot].id);
  }
}

/* ------------------------------------------------------------ principal */
int main(int argc, char** argv) {
  gui_event_t ev;
  unsigned blink_ms;
  int need = 1;
  int scr_w = 0, scr_h = 0;
  int (*hover)(int) = 0;

  /* disco e pasta do .EXE: fontes e NOTEPAD.CFG */
  {
    int d = k_exe_drive();
    if (d >= 'a' && d <= 'z') d -= 32;
    if (d >= 'A' && d <= 'Z') exe_drive = (char)d;
    if (argv && argv[0] && argv[0][0] && argv[0][1] == ':') {
      int i, s = 0;
      scopy(cfg_path, argv[0], (int)sizeof(cfg_path));
      for (i = 0; cfg_path[i]; i++)
        if (cfg_path[i] == '\\') s = i + 1;
      cfg_path[s] = 0;
      scat(cfg_path, "NOTEPAD.CFG", (int)sizeof(cfg_path));
      exe_drive = (char)up_ascii(argv[0][0]);
      /* Nova janela abre outra copia deste mesmo .EXE */
      scopy(self_path, argv[0], (int)sizeof(self_path));
    } else
      snprintf(cfg_path, sizeof(cfg_path), "%c:\\SODOS\\NOTEPAD.CFG", exe_drive);
  }
  cfg_load();
  k_win_size(&scr_w, &scr_h);
  base_px = scr_h >= 1000 ? 17 : scr_h >= 700 ? 16 : 14;

  if (gui_open_pct("Sem titulo - Bloco de notas", 62, 72) != 0) {
    printf("NOTEPAD: nao abriu a janela (rode pelo WIN)\n");
    return 1;
  }
  k_gui_clip(1);   /* Ctrl+V chega como 22: a colagem e daqui */
  hover = (int (*)(int))k_dl_sym(0, "k_gui_hover", 0);
  if (hover) hover(1);
  undo_st = (struct urec*)malloc(sizeof(struct urec) * UMAX);
  if (!undo_st) {
    gui_close();
    return 1;
  }
  memset(undo_st, 0, sizeof(struct urec) * UMAX);
  pal_sync();
  ui_font_init(scr_h >= 1000 ? 16 : scr_h >= 700 ? 15 : 13);
  tx_font_apply();
  if (!fb_alloc()) {
    gui_close();
    return 1;
  }
  tx_reserve(4096);
  doc_new();
  if (argc > 1 && argv[1] && argv[1][0]) {
    if (k_fsize(argv[1]) >= 0) doc_load(argv[1]);
    else {
      /* arquivo novo com esse nome: grava no primeiro Salvar */
      scopy(doc_path, argv[1], (int)sizeof(doc_path));
      title_update();
    }
  }
  geometry();
  blink_ms = k_ms();

  while (!quit_now) {
    int t, any = 0;
    if (need) {
      render();
      present();
      need = 0;
    }
    t = wait_ev(&ev, 530);
    if (t < 0) break;   /* a janela sumiu (WIN fechou) */
    if (pal_sync()) need = 1;   /* Configuracoes > Temas mudou: cores novas */
    if (t == GUI_EV_NONE) {
      /* cursor piscando */
      caret_on = !caret_on;
      need = 1;
      continue;
    }
    do {
      any = 1;
      if (ev.type == GUI_EV_CLOSE) {
        /* X da janela: com dialogo aberto, nada (como o Windows) */
        if (!ndlg) {
          menu_close();
          act_req(ACT_EXIT);
        }
      } else if (ev.type == GUI_EV_RESIZE) {
        fb_alloc();
        geometry();
        lay_dirty = 1;
        ensure_visible();
      } else if (ev.type == GUI_EV_MOVE || ev.type == GUI_EV_DRAG) {
        mx_mouse = ev.x;
        my_mouse = ev.y;
        if (ev.type == GUI_EV_DRAG || (ev.key & 1)) {
          if (sb_drag) sb_drag_to(ev.x, ev.y);
          else if (fd_sb_drag >= 0) fd_drag(ev.y);
          else if (text_drag) {
            int p = pos_from_xy(ev.x, ev.y);
            if (ev.y < ed_y) top_row--;
            else if (ev.y >= ed_y + ed_h) top_row++;
            clamp_scroll();
            if (click_count >= 2) {
              /* arrasto depois do duplo clique: por palavras */
              int a = word_anchor0, b = word_anchor1;
              if (p < a) {
                int q = p;
                while (q > 0 && w_isword(tx[q - 1])) q--;
                anc = b;
                cur = q;
              } else {
                int q = p;
                while (q < tn && w_isword(tx[q])) q++;
                anc = a;
                cur = q > b ? q : b;
              }
            } else
              cur = p;
            if (!opt_wrap) {
              int x = pad_l + col_of(cur) * f_tx.cw;
              if (x - scroll_x < pad_l) scroll_x = x - pad_l;
              if (x - scroll_x > ed_w - 2 * f_tx.cw) scroll_x = x - ed_w + 2 * f_tx.cw;
              clamp_scroll();
            }
          }
        } else {
          /* so realce: barra de menus, menu aberto, barra de rolagem, lista */
          int nh = menubar_hit(ev.x, ev.y);
          if (m_open >= 0) menu_mouse(ev.x, ev.y, 0);
          else if (nh != mb_hot) mb_hot = nh;
          {
            struct sbg g;
            int h2 = 0;
            vsb_geom(&g);
            if (ev.x >= g.x && ev.y >= g.track0 + g.tpos && ev.y < g.track0 + g.tpos + g.tlen)
              h2 = 1;
            if (hsb_on) {
              hsb_geom(&g);
              if (ev.y >= g.y && ev.x >= g.track0 + g.tpos && ev.x < g.track0 + g.tpos + g.tlen)
                h2 = 2;
            }
            sb_hot = h2;
          }
          if (ndlg && (dlg_top()->kind == DLG_OPEN || dlg_top()->kind == DLG_SAVEAS)) {
            int ih = f_ui.h + 8, hh = f_ui.h + 10, row;
            fd_hot = -1;
            if (ev.x >= fd_lx && ev.x < fd_lx + fd_lw - 15 && ev.y >= fd_ly + hh + 2 &&
                ev.y < fd_ly + fd_lh) {
              row = (ev.y - (fd_ly + hh + 2)) / ih;
              if (row < fd_rows && fd_top + row < fn_vis) fd_hot = fd_top + row;
            }
          }
        }
      } else if (ev.type == GUI_EV_UP) {
        mx_mouse = ev.x;
        my_mouse = ev.y;
        sb_drag = 0;
        fd_sb_drag = -1;
        text_drag = 0;
        if (ndlg) {
          struct dlg* d = dlg_top();
          int h = hit_ctl(d, ev.x, ev.y);
          if (d->down >= 0 && h == d->down) {
            int di = d->down;
            d->down = -1;
            dlg_activate(d, di);
          } else
            d->down = -1;
        }
      } else if (ev.type == GUI_EV_DOWN) {
        mx_mouse = ev.x;
        my_mouse = ev.y;
        caret_on = 1;
        if (m_open >= 0) {
          menu_mouse(ev.x, ev.y, 1);
        } else if (ndlg) {
          struct dlg* d = dlg_top();
          int h = hit_ctl(d, ev.x, ev.y);
          int th = dlg_title_h();
          if (ev.x >= d->x + d->w - 46 && ev.x < d->x + d->w && ev.y >= d->y && ev.y < d->y + th) {
            dlg_key(d, 27, 0);
          } else if (h >= 0) {
            struct ctl* c = &d->c[h];
            if (c->type == CT_EDIT) {
              d->focus = h;
              fld_click(c->f, d->x + c->x, ev.x);
            } else if (c->type == CT_BTN) {
              d->down = h;
              d->focus = h;
            } else {
              d->focus = h;
              dlg_activate(d, h);
            }
          } else if (d->kind == DLG_OPEN || d->kind == DLG_SAVEAS) {
            fd_click(d, ev.x, ev.y, 1);
          }
        } else if (ev.key == 2) {
          /* botao direito no texto: menu de contexto */
          if (ev.y >= ed_y && ev.y < ed_y + ed_h && ev.x < ed_x + ed_w) {
            int p = pos_from_xy(ev.x, ev.y);
            if (!(p >= sel_lo() && p < sel_hi())) cur = anc = p;
            m_open = MENU_CTX;
            m_x = ev.x;
            m_y = ev.y;
            m_hot = -1;
            m_sub = m_subhot = -1;
          }
        } else if (ev.y < menu_h) {
          int hb = menubar_hit(ev.x, ev.y);
          if (hb >= 0) menu_open_top(hb, -1);
        } else if (ev.x >= ed_x + ed_w || (hsb_on && ev.y >= ed_y + ed_h && ev.y < ed_y + ed_h + sbw)) {
          sb_click(ev.x, ev.y);
        } else if (ev.y >= ed_y && ev.y < ed_y + ed_h) {
          unsigned now = k_ms();
          int p = pos_from_xy(ev.x, ev.y);
          int dx = ev.x - last_click_x, dy = ev.y - last_click_y;
          if (now - last_click_ms < 450 && dx < 5 && dx > -5 && dy < 5 && dy > -5)
            click_count++;
          else
            click_count = 1;
          last_click_ms = now;
          last_click_x = ev.x;
          last_click_y = ev.y;
          if (click_count == 2) select_word_at(p);
          else if (click_count >= 3) {
            /* triplo: a linha inteira */
            int a = line_start(p), b = line_end(p);
            if (b < tn) b++;
            anc = a;
            cur = b;
            click_count = 3;
          } else {
            cur = anc = p;
          }
          goal_x = -1;
          u_break = 1;
          text_drag = 1;
        }
      } else if (ev.type == GUI_EV_WHEEL) {
        if (ndlg && (dlg_top()->kind == DLG_OPEN || dlg_top()->kind == DLG_SAVEAS))
          fd_top = clampi(fd_top + ev.key * 3, 0, fn_vis - fd_rows > 0 ? fn_vis - fd_rows : 0);
        else if (!ndlg && m_open < 0) {
          top_row += ev.key * 3;
          clamp_scroll();
        }
      } else if (ev.type == GUI_EV_KEY) {
        int k = ev.key, mods = ev.x;
        caret_on = 1;
        if (m_open >= 0) {
          if (k == GUI_KEY_F1 + 9) menu_close();
          else menu_key(k);
        } else if (ndlg) {
          struct dlg* d = dlg_top();
          /* F3 com o Localizar aberto: proxima, como no Windows */
          if (k == GUI_KEY_F1 + 2 && (d->kind == DLG_FIND || d->kind == DLG_REPLACE))
            dlg_button(d, B_FINDNEXT);
          else
            dlg_key(d, k, mods);
        } else if (!key_command(k, mods)) {
          if (k == 27) {
          } else
            key_text(k, mods);
        }
      }
      if (quit_now) break;
      t = wait_ev(&ev, 0);
      if (t < 0) quit_now = 1;
    } while (t > 0);
    (void)any;
    (void)blink_ms;
    need = 1;
  }
  gui_close();
  return 0;
}
