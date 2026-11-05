/* menu_cfg.c - configuracao do menu Iniciar (MENUCFG.EXE).
 *
 * Iniciar > Configuracoes > Menu. Posicao do botao "S" (esquerda ou junto ao
 * painel), painel de IA ligado/desligado e transparencia do menu.
 * Grava D:\SODOS\VIDEO.CFG e aplica na hora.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <so.h>

#define CFG_PATH "D:\\SODOS\\VIDEO.CFG"

static int g_pos;     /* 0 esquerda, 1 junto ao painel */
static int g_ia;
static int g_glass;
static int g_mx, g_my, g_down;
static int g_ready;
static char g_status[96];
static const char id_pos0, id_pos1, id_ia, id_glass, id_apply, id_reset, id_exit;

static void set_status(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(g_status) - 1) {
    g_status[i] = s[i];
    i++;
  }
  g_status[i] = 0;
}

/* Le o cfg so para preservar mode/bg/wall ao gravar. */
static void apply_all(void) {
  char buf[160];
  char mode = '1', bg[8] = "000000", wall[32] = "";
  FILE* f = fopen(CFG_PATH, "rb");
  if (f) {
    char b[256];
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = 0;
    {
      char* p = b;
      while (*p) {
        if (p[0] == 'm' && p[1] == 'o' && p[2] == 'd' && p[3] == 'e' && p[4] == '=') {
          mode = p[5];
        } else if (p[0] == 'b' && p[1] == 'g' && p[2] == '=') {
          int k;
          for (k = 0; k < 6 && p[3 + k]; k++) bg[k] = p[3 + k];
          bg[k] = 0;
        } else if (p[0] == 'w' && p[1] == 'a' && p[2] == 'l' && p[3] == 'l' && p[4] == '=') {
          int k = 0;
          p += 5;
          while (*p && *p != '\n' && *p != '\r' && k < 31) wall[k++] = *p++;
          wall[k] = 0;
          p--;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
      }
    }
  }
  k_ui_menu_pos(g_pos);
  k_ui_ia(g_ia);
  k_ui_glass(g_glass);
  snprintf(buf, sizeof(buf),
           "mode=%c\nbg=%s\nwall=%s\nmenupos=%d\nglass=%d\nia=%d\n", mode, bg,
           wall, g_pos, g_glass, g_ia);
  f = fopen(CFG_PATH, "wb");
  if (f) {
    fwrite(buf, 1, strlen(buf), f);
    fclose(f);
    set_status("configuracao gravada");
  } else {
    set_status("nao gravou VIDEO.CFG");
  }
}

/* Mini barra: mostra onde o "S" fica e se ha painel. */
static void draw_preview(int x, int y, int w, int h) {
  int bar_h = 34;
  int lw = 34;
  int lx;
  int panel_w = w / 3;
  int g = wg_theme()->bg;
  gui_fill(x, y, w, h, g);
  /* area do desktop */
  gui_fill(x + 2, y + 2, w - 4, h - bar_h - 4, 0x0E2233);
  if (g_ia) {
    gui_fill(x + w - panel_w - 2, y + 2, panel_w, h - bar_h - 4, 0x101C2B);
    gui_text(x + w - panel_w, y + 8, "SODOS IA", -1, 0xE8F0FA, 0x101C2B,
             GUI_TEXT_NOBG);
  }
  /* barra */
  gui_fill(x + 2, y + h - bar_h - 2, w - 4, bar_h, 0x0B1522);
  if (g_pos == 1) lx = x + w - panel_w - lw - 12;
  else lx = x + 6;
  gui_fill(lx, y + h - bar_h + 2, lw, bar_h - 8, 0x2F80D0);
  gui_text(lx + 10, y + h - bar_h + 9, "S", 1, 0xE8F0FA, 0x2F80D0, GUI_TEXT_NOBG);
  if (g_pos == 1) gui_text(lx - 56, y + h - bar_h + 9, "SODOS", -1, 0xE8F0FA, 0x0B1522, GUI_TEXT_NOBG);
  else gui_text(lx + lw + 8, y + h - bar_h + 9, "SODOS", -1, 0xE8F0FA, 0x0B1522, GUI_TEXT_NOBG);
  /* amostra do menu */
  {
    int mw = 150, mh = 108;
    int mxx = (g_pos == 1) ? lx - mw + lw : lx;
    int myy = y + h - bar_h - mh - 2;
    if (g_glass) {
      static unsigned char* buf;
      static int bw, bh;
      int i, j;
      if (!buf || bw != mw || bh != mh) {
        if (buf) free(buf);
        buf = (unsigned char*)malloc((size_t)mw * mh * 4);
        bw = mw;
        bh = mh;
      }
      if (buf) {
        for (j = 0; j < mh; j++)
          for (i = 0; i < mw; i++) {
            int k = (j * mw + i) * 4;
            buf[k] = 0x10;
            buf[k + 1] = 0x1C;
            buf[k + 2] = 0x2B;
            buf[k + 3] = 205;
          }
        gui_pixels(mxx, myy, mw, mh, buf, mw * 4);
      } else {
        gui_fill(mxx, myy, mw, mh, 0x101C2B);
      }
    } else {
      gui_fill(mxx, myy, mw, mh, 0x101C2B);
    }
    gui_frame(mxx, myy, mw, mh, 0x2E4A6C);
    gui_text(mxx + 10, myy + 8, "Bloco de notas", -1, 0xE8F0FA, 0x101C2B, GUI_TEXT_NOBG);
    gui_text(mxx + 10, myy + 30, "Gerenciador", -1, 0xE8F0FA, 0x101C2B, GUI_TEXT_NOBG);
    gui_text(mxx + 10, myy + 52, "Explorer", -1, 0xE8F0FA, 0x101C2B, GUI_TEXT_NOBG);
    gui_text(mxx + 10, myy + 74, "Configuracoes", -1, 0xE8F0FA, 0x101C2B, GUI_TEXT_NOBG);
  }
}

int main(void) {
  gui_event_t ev;
  int run = 1, sw, sh, bh, pad = 12;

  if (gui_open("Menu") != 0) {
    printf("MENUCFG: abra pelo WIN\n");
    return 1;
  }
  g_pos = k_ui_menu_pos(-1) ? 1 : 0;
  g_ia = k_ui_ia(-1) ? 1 : 0;
  g_glass = k_ui_glass(-1) ? 1 : 0;
  set_status("escolha a posicao e os checks");
  wg_focus_set(&id_pos0);

  while (run) {
    sw = gui_width();
    sh = gui_height();
    bh = wg_theme()->btn_h;
    if (!g_ready && k_ms() > 400) g_ready = 1;
    wg_input(g_mx, g_my, g_down);
    gui_fill(0, 0, sw, sh, wg_theme()->bg);

    wg_group(pad, pad, sw - pad * 2, 96, "Posicao do botao do menu");
    if (wg_radio(&id_pos0, pad + 16, pad + 28, "1 - Esquerda", &g_pos, 0)) {
      k_ui_menu_pos(0);
      set_status("menu a esquerda");
    }
    if (wg_radio(&id_pos1, pad + 16, pad + 54, "2 - Junto ao painel", &g_pos, 1)) {
      k_ui_menu_pos(1);
      set_status("menu junto ao painel");
    }

    {
      int y = pad + 96 + 10;
      wg_group(pad, y, sw - pad * 2, 86, "Painel e transparencia");
      if (wg_check(&id_ia, pad + 16, y + 26, "Painel (IA na lateral direita)", &g_ia)) {
        k_ui_ia(g_ia);
        set_status(g_ia ? "painel ligado" : "painel desligado");
      }
      if (wg_check(&id_glass, pad + 16, y + 52, "Transparencia do menu", &g_glass)) {
        k_ui_glass(g_glass);
        set_status(g_glass ? "menu translucido" : "menu opaco");
      }
    }

    /* previa */
    {
      int y = pad + 96 + 10 + 86 + 10;
      int ph = sh - y - 62 - bh - 10;
      if (ph < 120) ph = 120;
      wg_group(pad, y, sw - pad * 2, ph, "Previa");
      draw_preview(pad + 12, y + 24, sw - pad * 2 - 24, ph - 34);
    }

    {
      int by = sh - 62;
      if (wg_button(&id_apply, pad, by, 120, bh, "Aplicar", g_ready)) apply_all();
      if (wg_button(&id_reset, pad + 130, by, 120, bh, "Restaurar", g_ready)) {
        g_pos = 0;
        g_ia = 0;
        g_glass = 0;
        k_ui_menu_pos(0);
        k_ui_ia(0);
        k_ui_glass(0);
        set_status("padrao restaurado");
      }
      if (wg_button(&id_exit, sw - pad - 100, by, 100, bh, "Sair", g_ready)) run = 0;
    }
    wg_status(0, sh - 24, sw, g_status, g_pos ? "junto ao painel" : "esquerda");

    gui_present();

    if (gui_wait(&ev, 150) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_UP) { g_mx = ev.x; g_my = ev.y; g_down = 0; }
      else if (ev.type == GUI_EV_DRAG) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_KEY && ev.key == 27) run = 0;
    }
  }
  gui_close();
  return 0;
}
