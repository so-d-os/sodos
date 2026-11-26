/* winmenu.c - WINMENU.EXE: o menu da tecla Win do WIN.
 *
 * A tecla Win (bandeirinha) mostra o desktop e abre este painel: da metade
 * da tela para baixo, transparente (k_gui_transp), com a caixa de busca e a
 * lupa embaixo. Onde o painel e invisivel o clique passa para os icones do
 * desktop. Digitar procura programas (.EXE em D:\SODOS, D:\ e no desktop);
 * a lista aparece num cartao acima da caixa. Enter (ou clique) abre, setas
 * escolhem, Esc fecha e devolve as janelas; a tecla Win de novo tambem fecha.
 *
 * Texto em TrueType (Inter, de \SODOS\FONTS) pelo mesmo rasterizador do WIN
 * (video/wfont.c, 4x4 amostras por pixel), desenhado com alfa no quadro do
 * painel: a caixa tem ~2,4x a altura da letra. Os glifos sao feitos uma vez
 * (o programa fica residente). Sem os TTF, cai na fonte de bitmap.
 *
 * O WIN abre o programa (D:\SODOS\WINMENU.EXE) na primeira tecla Win. Depois
 * ele fica na memoria, parado em k_winmenu(K_WM_ESPERA) (sem CPU), e as
 * teclas seguintes so o acordam, como o WINTAB do Alt+Tab.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define WFONT_LAST 255   /* Latin-1, como no WINE.DLL */
#include "../video/wfont.c"

#define EV_DOWN 1
#define EV_KEY 2
#define EV_CLOSE 3
#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define TEXT_NOBG 1
#define TEXT_BOLD 2

#define C_GLASS 0x0E1726u  /* vidro: azul bem escuro */
#define A_GLASS 228        /* 0..255 */
#define C_CAP 0x1E293Bu    /* tecla desenhada (Esc, Enter) */
#define C_EDGE 0x2E4A6Cu
#define C_ACCENT 0x2F80D0u
#define C_TEXT 0xF1F5F9u
#define C_DIM 0x93A8C4u

#define NPROG 256           /* programas conhecidos */
#define MAXR 8             /* linhas de resultado */
#define QMAX 60

struct prog {
  char nome[40];   /* sem o .EXE */
  char path[160];
};

static struct prog g_p[NPROG];
static int g_np;
static int g_res[MAXR], g_nres, g_sel;
static char g_q[QMAX + 1];
static int g_nq;

static int g_pw, g_ph;          /* painel */
static int g_cw, g_chh;         /* fonte de bitmap (reserva) */
static unsigned char* g_frame;  /* RGBA do painel */
static int g_bx, g_by, g_bw, g_bh;   /* caixa de busca */
static int g_rowh;

/* ------------------------------------------------------------- fontes */

struct fonte {
  int ok, px, asc, desc;
  struct wfont_glyph g[256 - 32];
};
static struct fonte F_BUSCA, F_NOME, F_DICA;   /* texto digitado, nomes, pastas e teclas */
static int g_ttf;                              /* as tres prontas */
static int g_px;                               /* tamanho base: mede a caixa e as linhas */

/* O wfont libera o pool de glifos ao montar o proximo tamanho: aqui o pool
 * (a 2a alocacao de cada wfont_build) fica, para guardar os tres tamanhos. */
static void* g_keep[8];
static int g_nkeep, g_nalloc;

static void* f_alloc(uint32_t n) {
  void* p = malloc(n);
  if (++g_nalloc == 2 && p && g_nkeep < 8) g_keep[g_nkeep++] = p;
  return p;
}

static void f_free(void* p) {
  int i;
  for (i = 0; i < g_nkeep; i++)
    if (g_keep[i] == p) return;
  free(p);
}

/* TTF de \SODOS\FONTS (D:, U: ou C:) inteiro na memoria. */
static unsigned char* le_ttf(const char* nome, int* len) {
  static const char drv[] = "DUC";
  int d;
  for (d = 0; drv[d]; d++) {
    char path[64];
    struct k_direntl e;
    unsigned char* b;
    sprintf(path, "%c:\\SODOS\\FONTS\\%s", drv[d], nome);
    if (k_dir_entry(path, &e) != 0 || e.dir || e.size < 1024 || e.size > 8u * 1024 * 1024) continue;
    b = (unsigned char*)malloc(e.size);
    if (!b) return 0;
    if (k_fread(path, 0, (char*)b, (int)e.size) == (int)e.size) {
      *len = (int)e.size;
      return b;
    }
    free(b);
  }
  return 0;
}

static int monta(struct fonte* f, const unsigned char* ttf, int len, int px) {
  int c;
  f->ok = 0;
  if (!ttf) return 0;
  g_nalloc = 0;
  if (wfont_build(ttf, len, px, f_alloc, f_free) != 0) return 0;
  for (c = 32; c < 256; c++) f->g[c - 32] = *wfont_glyph((unsigned char)c);
  f->px = px;
  f->asc = wfont_ascent();
  f->desc = wfont_height() - f->asc;
  f->ok = 1;
  return 1;
}

/* Primeiro arquivo da lista que existe. */
static unsigned char* le_um(const char* a, const char* b, const char* c, int* len) {
  unsigned char* t = le_ttf(a, len);
  if (!t && b) t = le_ttf(b, len);
  if (!t && c) t = le_ttf(c, len);
  return t;
}

/* As tres fontes no tamanho desta tela (uma vez; de novo se a tela mudar). */
static void fontes(int sh) {
  static int feito_sh;
  int px = sh * 37 / 1000, lr = 0, lb = 0;
  unsigned char *reg, *neg;
  if (px < 22) px = 22;
  if (px > 56) px = 56;
  if (feito_sh == sh) return;
  feito_sh = sh;
  reg = le_um("INTER.TTF", "ROBOTO.TTF", "DEJAVU.TTF", &lr);
  neg = le_um("INTERBD.TTF", "DEJAVUBD.TTF", 0, &lb);
  if (!neg) {
    neg = reg;
    lb = lr;
  }
  /* a busca com 80% do tamanho base (a caixa continua medida pelo base) */
  g_px = px;
  g_ttf = monta(&F_BUSCA, reg, lr, px * 8 / 10) && monta(&F_NOME, neg, lb, px * 6 / 10) &&
          monta(&F_DICA, reg, lr, px * 45 / 100);
  if (neg != reg) free(neg);
  free(reg);
}

/* CP850 (teclado do SODOS) -> Latin-1 (os glifos); 0 = sem equivalente. */
static const unsigned char cp850_lat1[128] = {
  0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
  0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xF8, 0xA3, 0xD8, 0xD7, 0,
  0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, 0xAE, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB,
  0, 0, 0, 0, 0, 0xC1, 0xC2, 0xC0, 0xA9, 0, 0, 0, 0, 0xA2, 0xA5, 0,
  0, 0, 0, 0, 0, 0, 0xE3, 0xC3, 0, 0, 0, 0, 0, 0, 0, 0xA4,
  0xF0, 0xD0, 0xCA, 0xCB, 0xC8, 0, 0xCD, 0xCE, 0xCF, 0, 0, 0, 0, 0xA6, 0xCC, 0,
  0xD3, 0xDF, 0xD4, 0xD2, 0xF5, 0xD5, 0xB5, 0xFE, 0xDE, 0xDA, 0xDB, 0xD9, 0xFD, 0xDD, 0xAF, 0xB4,
  0xAD, 0xB1, 0, 0xBE, 0xB6, 0xA7, 0xF7, 0xB8, 0xB0, 0xA8, 0xB7, 0xB9, 0xB3, 0xB2, 0, 0xA0,
};

static const struct wfont_glyph* glifo(const struct fonte* f, unsigned char c) {
  if (c >= 0x80) c = cp850_lat1[c - 0x80] ? cp850_lat1[c - 0x80] : '?';
  if (c < 32) c = '?';
  return &f->g[c - 32];
}

static int txt_larg(const struct fonte* f, const char* s, int n) {
  int i, pen = 0;
  for (i = 0; i < n; i++) pen += glifo(f, (unsigned char)s[i])->adv;
  return (pen + 128) >> 8;
}

/* ------------------------------------------------------------- desenho */

/* "over": cor rgb com alfa a (0..255) por cima do que ja esta no quadro */
static void px_over(int x, int y, unsigned rgb, int a) {
  unsigned char* p;
  int da, oa, k;
  if (x < 0 || y < 0 || x >= g_pw || y >= g_ph || a <= 0) return;
  p = g_frame + ((size_t)y * g_pw + x) * 4;
  if (a >= 255 || !p[3]) {   /* opaco, ou nada embaixo: so grava */
    p[0] = (unsigned char)(rgb >> 16);
    p[1] = (unsigned char)(rgb >> 8);
    p[2] = (unsigned char)rgb;
    p[3] = (unsigned char)(a >= 255 ? 255 : a);
    return;
  }
  da = p[3];
  oa = a + da * (255 - a) / 255;
  if (oa <= 0) return;
  for (k = 0; k < 3; k++) {
    int s = (int)((rgb >> (16 - 8 * k)) & 0xFF);
    p[k] = (unsigned char)((s * a + p[k] * da * (255 - a) / 255) / oa);
  }
  p[3] = (unsigned char)oa;
}

/* Texto TrueType com a linha de base em yb; nenhuma letra passa de x_lim
 * (0 = sem limite). Devolve a largura. */
static int txt(const struct fonte* f, int x, int yb, const char* s, int n, unsigned rgb, int a,
               int x_lim) {
  int i, pen = x * 256;
  for (i = 0; i < n; i++) {
    const struct wfont_glyph* g = glifo(f, (unsigned char)s[i]);
    int gx = ((pen + 128) >> 8) + g->left, gy = yb - g->top, r, c;
    if (x_lim && gx + g->w > x_lim) break;
    if (g->a)
      for (r = 0; r < g->h; r++)
        for (c = 0; c < g->w; c++) {
          int m = g->a[r * g->w + c];
          if (m) px_over(gx + c, gy + r, rgb, a * m / 255);
        }
    pen += g->adv;
  }
  return ((pen + 128) >> 8) - x;
}

/* Cobertura (0..16) do pixel (x,y) num retangulo de cantos redondos, por 4x4
 * amostras so no canto (o meio e 16). Contas em oitavos de pixel: a amostra
 * (sx,sy) do pixel x fica em x*8 + sx*2 + 1, sem raiz quadrada. */
static int rr_cov(int x, int y, int rx, int ry, int w, int h, int r) {
  int cx, cy, sx, sy, n = 0, r8 = r * 8;
  if (r < 1) return x >= rx && y >= ry && x < rx + w && y < ry + h ? 16 : 0;
  if (x < rx || y < ry || x >= rx + w || y >= ry + h) return 0;
  if (x >= rx + r && x < rx + w - r) return 16;
  if (y >= ry + r && y < ry + h - r) return 16;
  cx = (x < rx + r ? rx + r : rx + w - r) * 8;   /* centro do canto */
  cy = (y < ry + r ? ry + r : ry + h - r) * 8;
  for (sy = 0; sy < 4; sy++)
    for (sx = 0; sx < 4; sx++) {
      int dx = x * 8 + sx * 2 + 1 - cx, dy = y * 8 + sy * 2 + 1 - cy;
      if (dx * dx + dy * dy <= r8 * r8) n++;
    }
  return n;
}

static void rrect(int x, int y, int w, int h, int r, unsigned rgb, int a) {
  int i, j;
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  for (j = y; j < y + h; j++)
    for (i = x; i < x + w; i++) {
      int c = rr_cov(i, j, x, y, w, h, r);
      if (c) px_over(i, j, rgb, a * c / 16);
    }
}

/* Contorno de 1 px do retangulo redondo: o anel entre ele e o de dentro. */
static void rframe(int x, int y, int w, int h, int r, unsigned rgb, int a) {
  int i, j;
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  for (j = y; j < y + h; j++)
    for (i = x; i < x + w; i++) {
      int c = rr_cov(i, j, x, y, w, h, r) - rr_cov(i, j, x + 1, y + 1, w - 2, h - 2, r - 1);
      if (c > 0) px_over(i, j, rgb, a * c / 16);
    }
}

/* Brilho do vidro: o contorno so na parte de cima, sumindo ate a linha rows. */
static void brilho(int x, int y, int w, int h, int r, unsigned rgb, int a, int rows) {
  int i, j;
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  for (j = y; j < y + rows && j < y + h; j++) {
    int aj = a * (rows - (j - y)) / rows;
    for (i = x; i < x + w; i++) {
      int c = rr_cov(i, j, x, y, w, h, r) - rr_cov(i, j, x + 1, y + 1, w - 2, h - 2, r - 1);
      if (c > 0) px_over(i, j, rgb, aj * c / 16);
    }
  }
}

/* Lupa: anel de raio r (espessura t) com centro no meio do pixel (cx,cy) e o
 * cabo a 45 graus para baixo e a direita, de 0,75r a 1,9r do centro. 4x4
 * amostras em oitavos de pixel; u = dx + dy e v = dx - dy sao as distancias
 * ao longo e de lado da diagonal vezes raiz de 2. */
static void lupa(int cx, int cy, int r, int t, unsigned rgb) {
  int x, y, sx, sy;
  int ro = (2 * r + t) * 4, ri = (2 * r - t) * 4;     /* raios externo e interno */
  int u0 = r * 85 / 10, u1 = r * 215 / 10;            /* 0,75r e 1,9r, x8 x raiz 2 */
  int vh = (t + 1) * 566 / 100;                       /* meia largura do cabo */
  for (y = cy - r - t - 2; y <= cy + 2 * r + t; y++)
    for (x = cx - r - t - 2; x <= cx + 2 * r + t; x++) {
      int n = 0;
      for (sy = 0; sy < 4; sy++)
        for (sx = 0; sx < 4; sx++) {
          int dx = x * 8 + sx * 2 + 1 - (cx * 8 + 4), dy = y * 8 + sy * 2 + 1 - (cy * 8 + 4);
          int d2 = dx * dx + dy * dy, u = dx + dy, v = dx - dy;
          if ((d2 <= ro * ro && d2 >= ri * ri) || (u >= u0 && u <= u1 && v <= vh && v >= -vh))
            n++;
        }
      if (n) px_over(x, y, rgb, 255 * n / 16);
    }
}

/* Tecla desenhada ("Esc", "Enter") com a direita em xr, centrada em cy.
 * Devolve a largura. */
static int tecla(int xr, int cy, const char* s) {
  int n = (int)strlen(s), tw = txt_larg(&F_DICA, s, n), w = tw + F_DICA.px, h = F_DICA.px * 17 / 10;
  int x = xr - w, y = cy - h / 2;
  rrect(x, y, w, h, h / 4, C_CAP, 235);
  rframe(x, y, w, h, h / 4, C_EDGE, 255);
  txt(&F_DICA, x + (w - tw) / 2, cy + (F_DICA.asc - F_DICA.desc) / 2, s, n, C_DIM, 255, 0);
  return w;
}

/* Quadrado com a inicial do programa (cor pelo nome, como na barra). */
static void icone(int x, int y, int s, const char* nome) {
  static const unsigned cores[6] = { 0x2F6FB0, 0x7A4FB0, 0x2E8B57, 0xB06A2F, 0xA8324A, 0x3A8C99 };
  unsigned h = 0;
  char l[2];
  int i;
  for (i = 0; nome[i]; i++) h = h * 31u + (unsigned char)(nome[i] >= 'a' && nome[i] <= 'z' ? nome[i] - 32 : nome[i]);
  rrect(x, y, s, s, s / 4, cores[h % 6u], 255);
  l[0] = nome[0] >= 'a' && nome[0] <= 'z' ? (char)(nome[0] - 32) : nome[0];
  l[1] = 0;
  txt(&F_NOME, x + (s - txt_larg(&F_NOME, l, 1)) / 2, y + s / 2 + (F_NOME.asc - F_NOME.desc) / 2, l,
      1, 0xFFFFFFu, 255, 0);
}

/* ------------------------------------------------------------- programas */

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int ends_exe(const char* s) {
  int n = (int)strlen(s);
  return n > 4 && s[n - 4] == '.' && up(s[n - 3]) == 'E' && up(s[n - 2]) == 'X' && up(s[n - 1]) == 'E';
}

static int ieq(const char* a, const char* b) {
  while (*a && *b && up(*a) == up(*b)) {
    a++;
    b++;
  }
  return !*a && !*b;
}

/* posicao de q em s sem ligar para maiusculas (-1 = nao tem) */
static int ifind(const char* s, const char* q) {
  int i, j;
  if (!q[0]) return 0;
  for (i = 0; s[i]; i++) {
    for (j = 0; q[j] && s[i + j] && up(s[i + j]) == up(q[j]); j++) {}
    if (!q[j]) return i;
  }
  return -1;
}

/* Nome 8.3 todo em maiusculas ("EXPLORER") fica "Explorer" na lista; nome
 * longo com minusculas fica como esta. */
static void bonito(char* s) {
  int i;
  for (i = 0; s[i]; i++)
    if (s[i] >= 'a' && s[i] <= 'z') return;
  for (i = 1; s[i]; i++)
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
}

static void coleta_pasta(const char* dir) {
  struct k_direntl* e;
  int n, i, k;
  e = (struct k_direntl*)malloc(sizeof(struct k_direntl) * 200);
  if (!e) return;
  n = k_dir_long(dir, e, 200);
  for (i = 0; i < n && g_np < NPROG; i++) {
    struct prog* p;
    char nome[40];
    int len;
    if (e[i].dir || !ends_exe(e[i].name)) continue;
    len = (int)strlen(e[i].name) - 4;
    if (len <= 0 || len >= (int)sizeof(nome)) continue;
    memcpy(nome, e[i].name, (size_t)len);
    nome[len] = 0;
    /* o proprio menu, o Alt+Tab e o que nao e para abrir pela busca */
    if (ieq(nome, "WINMENU") || ieq(nome, "WINTAB") || ieq(nome, "DISKSYNC")) continue;
    for (k = 0; k < g_np; k++)
      if (ieq(g_p[k].nome, nome)) break;
    if (k < g_np) continue;   /* mesmo nome em outra pasta: fica o primeiro */
    p = &g_p[g_np];
    {
      int dl = (int)strlen(dir), nl = (int)strlen(e[i].name);
      int bar = dir[dl - 1] == '\\' ? 0 : 1;
      if (dl + bar + nl >= (int)sizeof(p->path)) continue;   /* caminho comprido demais */
      memcpy(p->path, dir, (size_t)dl);
      if (bar) p->path[dl] = '\\';
      memcpy(p->path + dl + bar, e[i].name, (size_t)nl + 1);
    }
    strcpy(p->nome, nome);
    bonito(p->nome);
    g_np++;
  }
  free(e);
}

static void coleta(void) {
  g_np = 0;
  coleta_pasta("D:\\USER\\DEFAULT\\DESKTOP");
  coleta_pasta("D:\\SODOS");
  coleta_pasta("D:\\");
}

/* Linhas que cabem no cartao acima da caixa. */
static int cabe(void) {
  int c = (g_by - 18 - 16) / g_rowh;
  return c > MAXR ? MAXR : c;
}

/* Resultados: comeca com o texto antes, depois contem; em ordem alfabetica. */
static void busca(void) {
  int i, j, pos[NPROG], ord[NPROG], n = 0, max = cabe();
  g_nres = 0;
  g_sel = 0;
  if (!g_nq) return;
  for (i = 0; i < g_np; i++) {
    int p = ifind(g_p[i].nome, g_q);
    if (p < 0) continue;
    pos[n] = p == 0 ? 0 : 1;
    ord[n] = i;
    n++;
  }
  /* insercao: (comeca antes, nome) */
  for (i = 1; i < n; i++) {
    int pp = pos[i], oo = ord[i];
    for (j = i; j > 0; j--) {
      int c = pos[j - 1] - pp;
      if (!c) {
        const char* a = g_p[ord[j - 1]].nome;
        const char* b = g_p[oo].nome;
        while (*a && up(*a) == up(*b)) {
          a++;
          b++;
        }
        c = up(*a) - up(*b);
      }
      if (c <= 0) break;
      pos[j] = pos[j - 1];
      ord[j] = ord[j - 1];
    }
    pos[j] = pp;
    ord[j] = oo;
  }
  for (i = 0; i < n && g_nres < max; i++) g_res[g_nres++] = ord[i];
}

/* ------------------------------------------------------------- painel */

static int card_h(void) { return g_nres * g_rowh + 16; }
static int card_y(void) { return g_by - 18 - card_h(); }
static int row_y(int i) { return card_y() + 8 + i * g_rowh; }

/* Pasta do programa i para mostrar: "D:\SODOS" ("D:\" na raiz). */
static int pasta(const struct prog* p, char* dir) {
  int dl;
  strcpy(dir, p->path);
  for (dl = (int)strlen(dir); dl > 0 && dir[dl - 1] != '\\'; dl--) {}
  if (dl > 1 && dir[dl - 2] != ':') dl--;
  dir[dl] = 0;
  return dl;
}

/* Com a fonte TrueType: tudo no quadro, com alfa. */
static void desenha_ttf(void) {
  int i, cy = g_by + g_bh / 2;
  int r = g_bh * 16 / 100, t = g_bh >= 80 ? 4 : 3;
  int lx = g_bx + g_bh * 46 / 100;                 /* centro da lupa */
  int tx = lx + r * 2 + g_bh * 22 / 100;           /* comeco do texto */
  int base = cy + (F_BUSCA.asc - F_BUSCA.desc) / 2;
  int xr = g_bx + g_bw - g_bh * 36 / 100;          /* direita das teclas */
  int fim;
  /* cartao dos resultados */
  if (g_nres) {
    int y0 = card_y(), ch = card_h(), s = g_rowh - g_rowh * 36 / 100;
    rrect(g_bx + 2, y0 + 6, g_bw - 4, ch, 20, 0x000000u, 50);   /* sombra */
    rrect(g_bx, y0, g_bw, ch, 20, C_GLASS, A_GLASS);
    rframe(g_bx, y0, g_bw, ch, 20, C_EDGE, 200);
    for (i = 0; i < g_nres; i++) {
      const struct prog* p = &g_p[g_res[i]];
      int y = row_y(i), nb = y + g_rowh / 2 + (F_NOME.asc - F_NOME.desc) / 2, nx, dl, dw;
      char dir[160];
      if (i == g_sel) rrect(g_bx + 8, y + 2, g_bw - 16, g_rowh - 4, 14, C_ACCENT, 235);
      icone(g_bx + 22, y + (g_rowh - s) / 2, s, p->nome);
      nx = g_bx + 22 + s + 18;
      dl = pasta(p, dir);
      dw = txt_larg(&F_DICA, dir, dl);
      /* a pasta a direita, se couber depois do nome */
      if (nx + txt_larg(&F_NOME, p->nome, (int)strlen(p->nome)) + 24 < g_bx + g_bw - 26 - dw)
        txt(&F_DICA, g_bx + g_bw - 26 - dw, y + g_rowh / 2 + (F_DICA.asc - F_DICA.desc) / 2, dir, dl,
            i == g_sel ? 0xDCE8F5u : C_DIM, 255, 0);
      txt(&F_NOME, nx, nb, p->nome, (int)strlen(p->nome), C_TEXT, 255, g_bx + g_bw - 20);
    }
  }
  /* caixa de busca: sombra macia, vidro, brilho em cima e contorno */
  rrect(g_bx - 4, g_by + 2, g_bw + 8, g_bh + 10, g_bh / 2 + 4, 0x000000u, 30);
  rrect(g_bx - 1, g_by + 4, g_bw + 2, g_bh + 2, g_bh / 2 + 1, 0x000000u, 50);
  rrect(g_bx, g_by, g_bw, g_bh, g_bh / 2, C_GLASS, A_GLASS);
  brilho(g_bx + 1, g_by + 1, g_bw - 2, g_bh - 2, g_bh / 2 - 1, 0xFFFFFFu, 60, g_bh / 2);
  rframe(g_bx, g_by, g_bw, g_bh, g_bh / 2, g_nq ? C_ACCENT : C_EDGE, 255);
  if (g_nq) rframe(g_bx + 1, g_by + 1, g_bw - 2, g_bh - 2, g_bh / 2 - 1, C_ACCENT, 140);
  lupa(lx, cy - r / 4, r, t, g_nq ? C_TEXT : 0xB8C7DAu);
  /* teclas a direita: Esc (e Enter quando ha o que abrir) */
  fim = xr - tecla(xr, cy, "Esc");
  if (g_nres) fim -= 12 + tecla(fim - 12, cy, "Enter");
  fim -= 20;
  /* texto: se nao cabe, mostra o fim (o que se esta digitando) */
  if (g_nq) {
    int ini = 0, w;
    while (ini < g_nq && tx + txt_larg(&F_BUSCA, g_q + ini, g_nq - ini) + 8 > fim) ini++;
    w = txt(&F_BUSCA, tx, base, g_q + ini, g_nq - ini, C_TEXT, 255, 0);
    rrect(tx + w + 3, base - F_BUSCA.asc + 2, 3, F_BUSCA.asc + F_BUSCA.desc / 2, 1, C_ACCENT, 255);
  } else {
    const char* ph = "Buscar programas";
    rrect(tx - 1, base - F_BUSCA.asc + 2, 3, F_BUSCA.asc + F_BUSCA.desc / 2, 1, C_ACCENT, 255);
    txt(&F_BUSCA, tx + 10, base, ph, (int)strlen(ph), C_DIM, 170, fim);
  }
  /* so a coluna do menu muda; o resto do painel fica invisivel */
  {
    int x0 = g_bx - 8, w = g_bw + 16;
    k_gui_pixels(x0, 0, w, g_ph, g_frame + (size_t)x0 * 4, g_pw * 4);
  }
  k_gui_present();
}

/* Sem os TTF: o desenho de antes, com a fonte de bitmap. */
static void desenha_bitmap(void) {
  int i, cy = g_by + g_bh / 2, ty = cy - g_chh / 2, tx = g_bx + g_bh;
  int r = g_bh / 5 < 6 ? 6 : g_bh / 5;
  if (g_nres) {
    rrect(g_bx, card_y(), g_bw, card_h(), 14, C_GLASS, A_GLASS);
    for (i = 0; i < g_nres; i++)
      if (i == g_sel) rrect(g_bx + 6, row_y(i) + 2, g_bw - 12, g_rowh - 4, 10, C_ACCENT, 230);
  }
  rrect(g_bx, g_by, g_bw, g_bh, g_bh / 2, C_GLASS, A_GLASS);
  rframe(g_bx, g_by, g_bw, g_bh, g_bh / 2, g_nq ? C_ACCENT : C_EDGE, 255);
  lupa(g_bx + g_bh / 2 + 2, cy - r / 3, r, 3, C_TEXT);
  {
    int x0 = g_bx - 8, w = g_bw + 16;
    k_gui_pixels(x0, 0, w, g_ph, g_frame + (size_t)x0 * 4, g_pw * 4);
  }
  if (g_nq) k_gui_text(tx, ty, g_q, g_nq, C_TEXT, 0, TEXT_NOBG | TEXT_BOLD);
  else k_gui_text(tx, ty, "Buscar programas", 16, C_DIM, 0, TEXT_NOBG);
  for (i = 0; i < g_nres; i++) {
    const struct prog* p = &g_p[g_res[i]];
    k_gui_text(g_bx + 18, row_y(i) + (g_rowh - g_chh) / 2, p->nome, (int)strlen(p->nome), C_TEXT, 0,
               TEXT_NOBG | TEXT_BOLD);
  }
  k_gui_present();
}

static void desenha(void) {
  memset(g_frame, 0, (size_t)g_pw * g_ph * 4);   /* tudo invisivel */
  if (g_ttf) desenha_ttf();
  else desenha_bitmap();
}

/* Linha de resultado sob o clique, ou -1. */
static int hit(int x, int y) {
  int i;
  for (i = 0; i < g_nres; i++) {
    int ry = row_y(i);
    if (x >= g_bx && x < g_bx + g_bw && y >= ry && y < ry + g_rowh) return i;
  }
  return -1;
}

/* D:\SODOS\WINMENU.CFG, "pb=1" ou "pb=0": com o menu aberto, o que esta
 * atras (desktop e icones) fica em preto e branco. Sem o arquivo: 1. Lido a
 * cada abertura, entao mudar o arquivo vale na proxima tecla Win. */
static int cfg_pb(void) {
  char b[512];
  int n = k_fread("D:\\SODOS\\WINMENU.CFG", 0, b, (int)sizeof(b) - 1), i;
  if (n <= 0) return 1;
  b[n] = 0;
  for (i = 0; i < n; i++) {
    if ((i == 0 || b[i - 1] == '\n') && up(b[i]) == 'P' && up(b[i + 1]) == 'B' && b[i + 2] == '=')
      return b[i + 3] != '0';
  }
  return 1;
}

/* Fecha o painel. devolve: as janelas que a tecla Win escondeu voltam. */
static void fecha(int devolve) {
  k_gui_close();
  k_winmenu(K_WM_FECHOU, devolve);
}

static void abre(int i) {
  const struct prog* p = &g_p[g_res[i]];
  k_win_launch(p->path);
  fecha(0);
}

/* Uma abertura do menu: da tecla Win ate fechar. */
static void sessao(void) {
  int sw, sh, bar, y0;
  if (k_win_primary(&sw, &sh) != 0 && k_win_size(&sw, &sh) != 0) return;
  bar = k_winmenu(K_WM_BARRA, 0);
  if (bar <= 0) bar = 40;
  k_gui_font(&g_cw, &g_chh);
  if (g_cw < 1) g_cw = 8;
  if (g_chh < 1) g_chh = 16;
  fontes(sh);
  y0 = sh / 2;
  g_pw = sw;
  g_ph = sh - y0 - bar;
  if (g_ph < 160) return;
  /* caixa com ~2,4x a altura da letra (o dobro da primeira versao) */
  g_bh = g_ttf ? g_px * 24 / 10 : 96;
  if (g_bh > g_ph / 3) g_bh = g_ph / 3;
  g_bw = sw * 52 / 100;
  if (g_bw > 940) g_bw = 940;
  if (g_bw < 360) g_bw = sw - 40;
  g_bx = (sw - g_bw) / 2;
  g_by = g_ph - g_bh - 34;
  g_rowh = g_ttf ? F_NOME.px * 24 / 10 : g_chh + 22;
  g_frame = (unsigned char*)malloc((size_t)g_pw * g_ph * 4);
  if (!g_frame) return;
  coleta();
  g_nq = 0;
  g_q[0] = 0;
  g_nres = 0;
  g_sel = 0;
  if (k_gui_panel("Busca", 0, y0, g_pw, g_ph) != 0) {
    free(g_frame);
    g_frame = 0;
    k_winmenu(K_WM_FECHOU, 1);
    return;
  }
  /* transparente; com pb=1 o que esta atras fica em preto e branco */
  k_gui_transp(cfg_pb() ? 1 | K_TRANSP_CINZA : 1);
  desenha();
  for (;;) {
    int ev[4], t;
    t = k_gui_event(ev, 1000);
    if (t < 0 || t == EV_CLOSE) {
      /* o WIN pediu (tecla Win de novo ou uma janela apareceu) */
      fecha(0);
      break;
    }
    if (t == EV_KEY) {
      int k = ev[3];
      if (k == 27) {
        fecha(1);
        break;
      }
      if (k == '\r' || k == '\n') {
        if (g_nres > 0) {
          abre(g_sel);
          break;
        }
        continue;
      }
      if (k == KEY_UP) {
        if (g_sel > 0) g_sel--;
      } else if (k == KEY_DOWN) {
        if (g_sel < g_nres - 1) g_sel++;
      } else if (k == 8 || k == 127) {
        if (g_nq > 0) g_q[--g_nq] = 0;
        busca();
      } else if (k >= 32 && k < 256 && g_nq < QMAX) {
        g_q[g_nq++] = (char)k;
        g_q[g_nq] = 0;
        busca();
      } else {
        continue;
      }
      /* a melhor fica em cima (selecao 0); as setas andam como na tela */
      desenha();
    } else if (t == EV_DOWN) {
      int c = hit(ev[1], ev[2]);
      if (c >= 0) {
        g_sel = c;
        abre(c);
        break;
      }
    }
  }
  free(g_frame);
  g_frame = 0;
}

int main(void) {
  int seq = k_winmenu(K_WM_ESPERA, -1);   /* a tecla Win que abriu o programa */
  if (seq < 0) {
    printf("WINMENU: e o menu da tecla Win; roda dentro do WIN.\n");
    return 0;
  }
  for (;;) {
    int s;
    sessao();
    do {
      s = k_winmenu(K_WM_ESPERA, seq);
      if (s < 0) return 0;
    } while (s == seq);
    seq = s;
  }
}
