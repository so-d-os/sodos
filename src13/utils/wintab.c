/* wintab.c - WINTAB.EXE: a pilha do Alt+Tab do WIN.
 *
 * O WIN abre este programa (D:\SODOS\WINTAB.EXE) no primeiro Alt+Tab. Depois
 * de cada escolha o painel fecha mas o programa fica na memoria, parado em
 * k_alttab_wait (sem CPU), e o proximo Alt+Tab so o acorda: nada de ler o
 * EXE e a LIBC.DLL do disco de novo. As
 * janelas e as miniaturas vem do kernel (k_win_list/k_win_thumb, tiradas no
 * momento do Alt+Tab). O painel fica na esquerda, por cima do que estava na
 * tela (k_win_shot) escurecido, como um vidro.
 *
 * Alt segurado + Tab (Shift+Tab volta) anda na pilha; soltar o Alt escolhe.
 * Setas tambem andam, Enter escolhe, Esc desiste, clique escolhe a janela.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define MAXW 32
#define EV_NONE 0
#define EV_DOWN 1
#define EV_KEY 2
#define EV_CLOSE 3
#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define TEXT_NOBG 1

#define C_TINT 0x0E1726u   /* vidro: azul bem escuro */
#define TINT_A 190         /* 0..255: quanto do vidro cobre o fundo */
#define C_CARD 0x1E293Bu
#define C_SEL 0x3B82F6u
#define C_TEXT 0xF1F5F9u
#define C_DIM 0x94A3B8u

static k_win_t g_w[MAXW];
static int g_n;
static int g_sw, g_sh;          /* tela */
static int g_pw, g_ph;          /* painel */
static int g_cw, g_chh;         /* fonte */
static unsigned char* g_bg;     /* fundo do painel ja escurecido, RGBA */
static unsigned char* g_frame;  /* quadro montado, RGBA */
static unsigned char* g_th[MAXW];
static int g_tw[MAXW], g_tht[MAXW]; /* tamanho de cada miniatura no cartao */
static int g_box_w, g_box_h;    /* area da miniatura no cartao */
static int g_card_h;
static int g_top;               /* rolagem: primeiro cartao visivel */

#define PAD 14
#define GAP 10
#define HEAD 40

static int mod(int a, int n) {
  a %= n;
  return a < 0 ? a + n : a;
}

static void put(unsigned char* p, unsigned rgb) {
  p[0] = (unsigned char)(rgb >> 16);
  p[1] = (unsigned char)(rgb >> 8);
  p[2] = (unsigned char)rgb;
  p[3] = 255;
}

static void fill(int x, int y, int w, int h, unsigned rgb) {
  int i, j;
  for (j = y; j < y + h; j++) {
    if (j < 0 || j >= g_ph) continue;
    for (i = x; i < x + w; i++) {
      if (i < 0 || i >= g_pw) continue;
      put(g_frame + ((size_t)j * g_pw + i) * 4, rgb);
    }
  }
}

/* Retangulo com cantos cortados (raio pequeno, sem antialias). */
static void round_rect(int x, int y, int w, int h, int r, unsigned rgb) {
  int j;
  for (j = 0; j < h; j++) {
    int d = 0;
    if (j < r) d = r - j;
    else if (j >= h - r) d = j - (h - r) + 1;
    if (d > 0) d = d * d / (r + 1);
    fill(x + d, y + j, w - 2 * d, 1, rgb);
  }
}

/* Fundo: a tela de antes, borrada de leve e coberta pelo vidro. */
static void make_bg(void) {
  int x, y, n = g_pw * g_ph;
  unsigned char* raw = (unsigned char*)malloc((size_t)n * 4);
  g_bg = (unsigned char*)malloc((size_t)n * 4);
  if (!g_bg) return;
  if (!raw || k_win_shot(0, 0, g_pw, g_ph, raw) != 0) {
    for (x = 0; x < n; x++) put(g_bg + x * 4, C_TINT);
    free(raw);
    return;
  }
  for (y = 0; y < g_ph; y++)
    for (x = 0; x < g_pw; x++) {
      unsigned s[3] = { 0, 0, 0 };
      int k, dx, dy, cnt = 0;
      /* borrao 5x5 em passos de 2: so para tirar o detalhe de baixo */
      for (dy = -4; dy <= 4; dy += 4)
        for (dx = -4; dx <= 4; dx += 4) {
          int sx = x + dx, sy = y + dy;
          const unsigned char* p;
          if (sx < 0) sx = 0;
          if (sy < 0) sy = 0;
          if (sx >= g_pw) sx = g_pw - 1;
          if (sy >= g_ph) sy = g_ph - 1;
          p = raw + ((size_t)sy * g_pw + sx) * 4;
          s[0] += p[0];
          s[1] += p[1];
          s[2] += p[2];
          cnt++;
        }
      for (k = 0; k < 3; k++) {
        unsigned t = (C_TINT >> (16 - 8 * k)) & 0xFF;
        g_bg[((size_t)y * g_pw + x) * 4 + k] =
            (unsigned char)((s[k] / cnt * (255 - TINT_A) + t * TINT_A) / 255);
      }
      g_bg[((size_t)y * g_pw + x) * 4 + 3] = 255;
    }
  free(raw);
}

static void layout(void) {
  int i, avail;
  g_box_w = g_pw - 2 * PAD - 12;
  g_box_h = g_box_w * 10 / 16;
  g_card_h = 6 + g_box_h + 6 + g_chh + 8;
  avail = g_ph - HEAD - PAD;
  /* muitas janelas: cartoes menores antes de precisar rolar */
  while (g_n * (g_card_h + GAP) > avail && g_box_h > 60) {
    g_box_h -= 10;
    g_card_h = 6 + g_box_h + 6 + g_chh + 8;
  }
  for (i = 0; i < g_n; i++) {
    int tw = g_box_w, th;
    th = g_w[i].tw > 0 ? tw * g_w[i].th / g_w[i].tw : g_box_h;
    if (th > g_box_h) {
      th = g_box_h;
      tw = g_w[i].th > 0 ? th * g_w[i].tw / g_w[i].th : g_box_w;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    g_tw[i] = tw;
    g_tht[i] = th;
    g_th[i] = (unsigned char*)malloc((size_t)tw * th * 4);
    if (g_th[i] && k_win_thumb(i, g_th[i], tw, th) != 0) {
      free(g_th[i]);
      g_th[i] = 0;
    }
  }
}

static int card_y(int i) { return HEAD + (i - g_top) * (g_card_h + GAP); }

static int visible(void) {
  int v = (g_ph - HEAD - PAD + GAP) / (g_card_h + GAP);
  return v < 1 ? 1 : v;
}

static void draw(int sel) {
  int i, v = visible();
  char head[48];
  if (sel < g_top) g_top = sel;
  if (sel >= g_top + v) g_top = sel - v + 1;
  if (g_bg) memcpy(g_frame, g_bg, (size_t)g_pw * g_ph * 4);
  else fill(0, 0, g_pw, g_ph, C_TINT);
  /* risco claro na beira direita do vidro */
  fill(g_pw - 1, 0, 1, g_ph, 0x334155u);
  for (i = g_top; i < g_n && i < g_top + v; i++) {
    int x = PAD, y = card_y(i), w = g_pw - 2 * PAD;
    int tx, ty, k;
    if (i == sel) round_rect(x - 3, y - 3, w + 6, g_card_h + 6, 8, C_SEL);
    round_rect(x, y, w, g_card_h, 6, C_CARD);
    tx = x + 6 + (g_box_w - g_tw[i]) / 2;
    ty = y + 6 + (g_box_h - g_tht[i]) / 2;
    if (g_th[i]) {
      for (k = 0; k < g_tht[i]; k++)
        if (ty + k >= 0 && ty + k < g_ph)
          memcpy(g_frame + ((size_t)(ty + k) * g_pw + tx) * 4, g_th[i] + (size_t)k * g_tw[i] * 4,
                 (size_t)g_tw[i] * 4);
    } else {
      fill(tx, ty, g_tw[i], g_tht[i], 0x475569u);
    }
  }
  k_gui_pixels(0, 0, g_pw, g_ph, g_frame, g_pw * 4);
  sprintf(head, "Janelas  %d/%d", sel + 1, g_n);
  k_gui_text(PAD, (HEAD - g_chh) / 2, head, (int)strlen(head), C_TEXT, 0, TEXT_NOBG);
  for (i = g_top; i < g_n && i < g_top + v; i++) {
    int y = card_y(i) + 6 + g_box_h + 6;
    int maxc = (g_pw - 2 * PAD - 12) / g_cw;
    char t[64];
    strncpy(t, g_w[i].title[0] ? g_w[i].title : "(sem titulo)", sizeof(t) - 1);
    t[sizeof(t) - 1] = 0;
    if ((int)strlen(t) > maxc && maxc > 3) {
      t[maxc - 3] = '.';
      t[maxc - 2] = '.';
      t[maxc - 1] = '.';
      t[maxc] = 0;
    }
    k_gui_text(PAD + 6, y, t, (int)strlen(t), i == sel ? C_TEXT : C_DIM, 0, TEXT_NOBG);
  }
  k_gui_present();
}

/* Cartao sob o clique, ou -1. */
static int hit(int px, int py) {
  int i, v = visible();
  for (i = g_top; i < g_n && i < g_top + v; i++) {
    int y = card_y(i);
    if (px >= PAD && px < g_pw - PAD && py >= y && py < y + g_card_h) return i;
  }
  return -1;
}

/* Um Alt+Tab: do aperto ate a escolha. */
static void session(void) {
  int tabs = 0, base = 0, sel, alt, i, pick = -1;
  g_top = 0;
  g_bg = 0;
  g_frame = 0;
  for (i = 0; i < MAXW; i++) g_th[i] = 0;
  g_n = k_win_list(g_w, MAXW);
  if (g_n > MAXW) g_n = MAXW;
  if (g_n < 2) return;
  alt = k_alttab(&tabs);
  if (!alt) {
    /* Alt+Tab rapido: nem mostra, so troca */
    k_win_activate(g_w[mod(tabs, g_n)].id);
    return;
  }
  if (k_win_size(&g_sw, &g_sh) != 0) {
    k_win_activate(g_w[mod(tabs, g_n)].id);
    return;
  }
  k_gui_font(&g_cw, &g_chh);
  if (g_cw < 1) g_cw = 8;
  if (g_chh < 1) g_chh = 16;
  g_pw = g_sw / 5;
  if (g_pw < 240) g_pw = 240;
  if (g_pw > 420) g_pw = 420;
  if (g_pw > g_sw) g_pw = g_sw;
  g_ph = g_sh;
  make_bg();
  g_frame = (unsigned char*)malloc((size_t)g_pw * g_ph * 4);
  if (!g_frame) {
    k_win_activate(g_w[mod(tabs, g_n)].id);
    free(g_bg);
    return;
  }
  layout();
  if (k_gui_panel("Alt+Tab", 0, 0, g_pw, g_ph) != 0) {
    k_win_activate(g_w[mod(tabs, g_n)].id);
    pick = -2;
  }
  sel = mod(tabs, g_n);
  if (pick == -1) draw(sel);
  for (; pick == -1;) {
    int ev[4], t, now;
    t = k_gui_event(ev, 15);
    if (t == EV_CLOSE || t < 0) break;
    if (t == EV_KEY) {
      if (ev[3] == 27) {
        pick = 0; /* desiste: a janela da frente de antes volta ao foco */
        break;
      }
      if (ev[3] == KEY_UP) base--;
      else if (ev[3] == KEY_DOWN) base++;
      else if (ev[3] == '\r' || ev[3] == '\n' || ev[3] == ' ') {
        pick = sel;
        break;
      }
    } else if (t == EV_DOWN) {
      int c = hit(ev[1], ev[2]);
      if (c >= 0) {
        pick = c;
        break;
      }
    }
    alt = k_alttab(&tabs);
    now = mod(base + tabs, g_n);
    if (!alt) {
      pick = now;
      break;
    }
    if (now != sel) {
      sel = now;
      draw(sel);
    }
  }
  if (pick >= 0) k_win_activate(g_w[pick].id);
  if (pick != -2) k_gui_close();
  for (i = 0; i < g_n; i++) free(g_th[i]);
  free(g_frame);
  free(g_bg);
}

int main(void) {
  int seq = k_alttab_wait(-1, 0); /* o Alt+Tab que abriu o programa */
  for (;;) {
    int s;
    session();
    if (seq < 0) return 0;      /* fora do WIN: so este Alt+Tab */
    do {
      s = k_alttab_wait(seq, 60000);
      if (s < 0) return 0;
    } while (s == seq);
    seq = s;
  }
}
