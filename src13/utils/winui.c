/* winui.c - a camada visual do NOTEPAD.EXE, comum aos programas do SODOS que
 * querem o mesmo visual (NOTEPAD.EXE e EXPLORER.EXE): incluir com
 *   #include "winui.c"
 * (o cc do SODOS compila um .c por vez; tudo aqui e static).
 *
 *   - tudo e desenhado numa imagem propria (fb, fbw x fbh, R,G,B,A) e vai
 *     para a janela com um gui_pixels: a letra suavizada fica igual no modo
 *     retro e no avancado
 *   - fonte TrueType suavizada (video/wfont.c, o rasterizador dos titulos do
 *     WIN) com Latin-1 e as letras extras do Windows-1252; f_ui = a dos menus
 *   - cores do tema do sistema (Configuracoes > Temas, k_ui_theme): Classico =
 *     o claro do Windows 10; Moderno e Escuro = as cores do TEMA.CFG. pal_sync()
 *     a cada volta do laco: 1 quando o tema mudou (redesenhar)
 *   - menus suspensos (struct mi, draw_pop), campos de texto (struct fld) e a
 *     area de transferencia com o texto inteiro do ultimo Copiar daqui
 *
 * Quem inclui define mi_enabled(id) e mi_checked(id) (estado dos itens).
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

/* Unicode -> byte do CP850 (nomes do FAT do SODOS); -1 sem equivalente */
static int uni_to_cp850(unsigned u) {
  int k;
  if (u < 0x80) return (int)u;
  for (k = 0; k < 128; k++)
    if (cp850_uni[k] == u) return 0x80 + k;
  return -1;
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

/* Nome do FAT (CP850) -> UTF-8 para desenhar */
static void cp_to_u8(const char* in, char* out, int max) {
  int o = 0;
  while (*in && o < max - 4) o += u8_put(cp850_to_uni((unsigned char)*in++), out + o);
  out[o] = 0;
}

/* UTF-8 (rotulos, argumentos) -> CP850; o que nao cabe vira '_' */
static void u8_to_cp(const char* in, char* out, int max) {
  const unsigned char* p = (const unsigned char*)in;
  int o = 0;
  while (*p && o < max - 1) {
    int c = uni_to_cp850(u8_next(&p));
    out[o++] = c > 0 ? (char)c : '_';
  }
  out[o] = 0;
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
static struct font f_ui;

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

static wch* wdup(const wch* s, int n) {
  wch* d = (wch*)malloc((size_t)(n ? n : 1) * sizeof(wch));
  if (d && n) memcpy(d, s, (size_t)n * sizeof(wch));
  return d;
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

/* ------------------------------------------------------------ menus suspensos */
struct mi {
  const char* t;   /* 0 = fim, "-" = separador */
  const char* k;   /* atalho */
  int id;
  int sub;
};

/* estado de cada item: quem inclui o winui.c define */
static int mi_enabled(int id);
static int mi_checked(int id);

static int item_h(void) { return f_ui.h + 10; }
static int sep_h(void) { return 9; }

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

/* ------------------------------------------------------------ campos de texto */
struct fld {
  wch t[260];
  int n, cur, anc, scroll;
};

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
  for (i = 0; i < f->n && o < max - 1; i++) {
    int c = uni_to_cp850(f->t[i]);
    out[o++] = c > 0 ? (char)c : '_';
  }
  out[o] = 0;
}

static void fld_set_path(struct fld* f, const char* s) {
  int i;
  f->n = 0;
  for (i = 0; s[i] && f->n < 255; i++) f->t[f->n++] = (wch)cp850_to_uni((unsigned char)s[i]);
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

