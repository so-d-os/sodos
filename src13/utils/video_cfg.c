/* video_cfg.c - configurador de tela do SODOS (VIDEOCFG.EXE).
 *
 * Escolhe o modelo de renderizacao (retro, baixo consumo / avancado, com
 * mistura alfa), a cor de fundo do desktop e um papel de parede. Aplica na
 * hora (k_gui_quality/k_win_bg/k_win_wallpaper) e grava D:\SODOS\VIDEO.CFG
 * (+ D:\SODOS\WALL.RAW) para o WIN reaplicar no proximo boot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <img.h>
#include <so.h>

#define WALLS_MAX 24
#define WALLS_DIR "D:\\SODOS\\WALLS"
#define CFG_PATH  "D:\\SODOS\\VIDEO.CFG"
#define RAW_PATH  "D:\\SODOS\\WALL.RAW"

static const unsigned cores[16] = {
  0x008080, 0x0A2E5C, 0x101820, 0x2A2A30,
  0x1E4D2B, 0x4A3B0E, 0x5C1F1F, 0x3A1F4D,
  0x805500, 0x005580, 0x1F5C4D, 0x444444,
  0xFFFFFF, 0xC8C8C8, 0x808080, 0x000000
};

static int g_mode;
static int g_menupos;   /* 0 esquerda, 1 centro, 2 direita */
static int g_glass;     /* menu translucido */
static int g_ia;        /* painel de IA na lateral */
static unsigned g_bg;
static char g_wall[32];          /* nome escolhido ("" = nenhum) */
static int g_wall_sel = -1;
static char g_names[WALLS_MAX][32];
static int g_n;
static unsigned char* g_rgba;    /* imagem decodificada para o preview */
static int g_rw, g_rh;
static char g_status[160];
static int g_mx, g_my, g_down;
static int g_ready;   /* so aceita cliques depois de a janela assentar */

static const char id_mode0, id_mode1;
static const char id_apply, id_reset, id_exit, id_list, id_default;
static const char id_pos0, id_pos1, id_pos2, id_glass, id_ia;

static void set_status(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(g_status) - 1) {
    g_status[i] = s[i];
    i++;
  }
  g_status[i] = 0;
}

static int name_eq(const char* a, const char* b) {
  int i;
  for (i = 0; a[i] || b[i]; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'a' && ca <= 'z') ca -= 32;
    if (cb >= 'a' && cb <= 'z') cb -= 32;
    if (ca != cb) return 0;
  }
  return 1;
}

static int is_img_name(const char* n) {
  int l = (int)strlen(n);
  if (l < 5) return 0;
  if (n[l - 4] != '.') return 0;
  if ((n[l - 3] == 'P' || n[l - 3] == 'p') && (n[l - 2] == 'N' || n[l - 2] == 'n') &&
      (n[l - 1] == 'G' || n[l - 1] == 'g')) return 1;
  if ((n[l - 3] == 'J' || n[l - 3] == 'j') && (n[l - 2] == 'P' || n[l - 2] == 'p') &&
      (n[l - 1] == 'G' || n[l - 1] == 'g')) return 1;
  return 0;
}

static void scan_walls(void) {
  struct k_dirent e[64];
  int i, n;
  g_n = 0;
  n = k_dir(WALLS_DIR, e, 64);
  if (n < 0) n = 0;
  for (i = 0; i < n && g_n < WALLS_MAX; i++) {
    if (e[i].dir) continue;
    if (!is_img_name(e[i].name)) continue;
    {
      int k = 0;
      while (e[i].name[k] && k < 31) {
        g_names[g_n][k] = e[i].name[k];
        k++;
      }
      g_names[g_n][k] = 0;
      g_n++;
    }
  }
}

static void free_wall(void) {
  if (g_rgba) {
    img_t im;
    im.w = g_rw;
    im.h = g_rh;
    im.rgba = g_rgba;
    img_free(&im);
    g_rgba = 0;
  }
  g_rw = 0;
  g_rh = 0;
}

/* Carrega o papel de parede i (ou nenhum, i < 0) e aplica no kernel. */
static void pick_wall(int i) {
  char path[96];
  FILE* f;
  unsigned char* data;
  long n;
  img_t im;

  free_wall();
  g_wall_sel = i;
  g_wall[0] = 0;
  if (i < 0 || i >= g_n) {
    k_win_wallpaper(0, 0, 0);
    set_status("fundo sem imagem");
    return;
  }
  path[0] = 0;
  {
    int k = 0;
    while (WALLS_DIR[k] && k < 90) {
      path[k] = WALLS_DIR[k];
      k++;
    }
    path[k++] = '\\';
    {
      int j = 0;
      while (g_names[i][j] && k < 95) path[k++] = g_names[i][j++];
    }
    path[k] = 0;
  }
  f = fopen(path, "rb");
  if (!f) {
    set_status("nao abriu a imagem");
    return;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 8 || n > 4 * 1024 * 1024) {
    fclose(f);
    set_status("imagem grande demais");
    return;
  }
  data = (unsigned char*)malloc((size_t)n);
  if (!data) {
    fclose(f);
    set_status("sem memoria");
    return;
  }
  if ((long)fread(data, 1, (size_t)n, f) != n) {
    free(data);
    fclose(f);
    set_status("erro lendo a imagem");
    return;
  }
  fclose(f);
  im.w = im.h = 0;
  im.rgba = 0;
  if (img_decode(data, (int)n, &im) != 0 || !im.rgba) {
    free(data);
    set_status("formato nao suportado");
    return;
  }
  free(data);
  g_rgba = im.rgba;
  g_rw = im.w;
  g_rh = im.h;
  {
    int k = 0;
    while (g_names[i][k] && k < 31) {
      g_wall[k] = g_names[i][k];
      k++;
    }
    g_wall[k] = 0;
  }
  if (k_win_wallpaper(g_rgba, g_rw, g_rh) != 0) set_status("kernel recusou a imagem");
  else set_status("papel de parede aplicado");
}

/* Grava o estado atual em VIDEO.CFG e o papel de parede em WALL.RAW. */
static void apply_all(void) {
  char buf[128];
  FILE* f;
  int n = 0;

  k_gui_quality(g_mode);
  k_ui_menu_pos(g_menupos);
  k_ui_glass(g_glass);
  k_ui_ia(g_ia);
  if (g_mode < 1) {
    /* no retro a cor fica valendo; o papel de parede continua, mas sem
     * mistura (o kernel so muda o tratamento do alfa). */
  }
  if (g_bg) k_win_bg(g_bg);

  if (g_wall[0] && g_rgba) {
    static unsigned char hdr[8];
    unsigned long total = 8ul + (unsigned long)g_rw * g_rh * 3ul;
    unsigned char* raw;
    int i;
    if (total > (20u << 20)) {
      set_status("imagem grande demais para gravar");
      return;
    }
    raw = (unsigned char*)malloc((size_t)total);
    if (!raw) {
      set_status("sem memoria para gravar");
      return;
    }
    hdr[0] = 'S'; hdr[1] = 'O'; hdr[2] = 'D'; hdr[3] = 'W';
    hdr[4] = (unsigned char)(g_rw & 0xFF);
    hdr[5] = (unsigned char)((g_rw >> 8) & 0xFF);
    hdr[6] = (unsigned char)(g_rh & 0xFF);
    hdr[7] = (unsigned char)((g_rh >> 8) & 0xFF);
    memcpy(raw, hdr, 8);
    for (i = 0; i < g_rw * g_rh; i++) {
      raw[8 + i * 3] = g_rgba[i * 4];
      raw[9 + i * 3] = g_rgba[i * 4 + 1];
      raw[10 + i * 3] = g_rgba[i * 4 + 2];
    }
    if (k_fsave(RAW_PATH, (const char*)raw, (int)total) != 0)
      set_status("nao gravou WALL.RAW");
    free(raw);
  }

  buf[0] = 0;
  n = snprintf(buf, sizeof(buf),
               "mode=%d\nbg=%06X\nwall=%s\nmenupos=%d\nglass=%d\nia=%d\n",
               g_mode, g_bg, (g_wall[0] && g_rgba) ? g_wall : "",
               g_menupos, g_glass, g_ia);
  f = fopen(CFG_PATH, "wb");
  if (f) {
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    set_status("configuracao gravada");
  } else {
    set_status("nao gravou VIDEO.CFG");
  }
}

static void reset_default(void) {
  g_mode = 0;
  g_menupos = 0;
  g_glass = 0;
  g_ia = 0;
  k_ui_menu_pos(0);
  k_ui_glass(0);
  k_ui_ia(0);
  g_bg = 0;
  free_wall();
  g_wall_sel = -1;
  g_wall[0] = 0;
  k_gui_quality(0);
  k_win_bg(0);
  k_win_wallpaper(0, 0, 0);
  set_status("padrao restaurado (teal, retro)");
}

/* caixa de "vidro" com alfa, para mostrar a diferenca entre os modos */
static void glass_box(int x, int y, int w, int h) {
  static unsigned char* buf;
  static int bw, bh;
  int i, j;
  if (!buf || bw != w || bh != h) {
    if (buf) free(buf);
    buf = (unsigned char*)malloc((size_t)w * h * 4);
    bw = w;
    bh = h;
  }
  if (!buf) return;
  for (j = 0; j < h; j++) {
    for (i = 0; i < w; i++) {
      int k = (j * w + i) * 4;
      int edge = (i < 2 || j < 2 || i >= w - 2 || j >= h - 2);
      buf[k] = edge ? 255 : 240;
      buf[k + 1] = edge ? 255 : 250;
      buf[k + 2] = edge ? 255 : 255;
      buf[k + 3] = edge ? 210 : 96;      /* bem transparente no meio */
    }
  }
  gui_pixels(x, y, w, h, buf, w * 4);
}

static void draw_preview(int x, int y, int w, int h) {
  int tw = 260, th = 150;
  int tx, ty;

  /* fundo: cor escolhida ou a imagem */
  if (g_rgba && g_rw > 0 && g_rh > 0) {
    int iw = w, ih = h;
    if (g_rw * h > g_rh * w) ih = g_rh * w / g_rw;
    else iw = g_rw * h / g_rh;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    gui_fill(x, y, w, h, g_bg ? (int)g_bg : 0x008080);
    gui_image(x + (w - iw) / 2, y + (h - ih) / 2, iw, ih, g_rgba, g_rw, g_rh,
              0, 0, w, h);
  } else {
    gui_fill(x, y, w, h, g_bg ? g_bg : 0x008080);
  }

  /* janela de exemplo */
  tx = x + (w - tw) / 2;
  ty = y + (h - th) / 2;
  gui_fill(tx, ty, tw, 22, 0x000080);
  gui_text(tx + 6, ty + 3, "Janela de exemplo", -1, 0xFFFFFF, 0x000080, GUI_TEXT_NOBG);
  gui_fill(tx, ty + 22, tw, th - 22, 0xD0D0D0);
  gui_fill(tx + 1, ty + 1, tw - 2, 1, 0xFFFFFF);
  gui_fill(tx, ty + th - 1, tw, 1, 0x404040);

  /* vidro translucido sobre a janela: em modo avancado mistura */
  glass_box(tx + 16, ty + 40, tw - 32, 60);
  gui_text(tx + 28, ty + 52, g_mode ? "avancado: alfa misturado"
                                    : "retro: alfa binario",
           -1, 0x101820, 0xD0D0D0, GUI_TEXT_NOBG);
  gui_text(tx + 28, ty + 72, "texto sobre o vidro", -1, 0x101820, 0xD0D0D0,
           GUI_TEXT_NOBG);

  gui_frame(x, y, w, h, 0x000000);
}

int main(void) {
  gui_event_t ev;
  int run = 1, sw, sh, ch;
  int pad = 10;
  int lx, ly, lw;
  int px, py, pw, ph;
  int i;
  unsigned t_open = 0;

  if (gui_open("Configurar tela") != 0) {
    printf("VIDEOCFG: abra pelo WIN\n");
    return 1;
  }
  g_mode = k_gui_quality(-1) ? 1 : 0;
  g_menupos = k_ui_menu_pos(-1) ? 1 : 0;
  g_glass = k_ui_glass(-1) ? 1 : 0;
  g_ia = k_ui_ia(-1) ? 1 : 0;
  scan_walls();
  {
    /* le o cfg para o estado inicial da interface */
    FILE* f = fopen(CFG_PATH, "rb");
    if (f) {
      char b[128];
      size_t n = fread(b, 1, sizeof(b) - 1, f);
      fclose(f);
      b[n] = 0;
      {
        char* p = b;
        while (*p) {
          if (p[0] == 'm' && p[1] == 'o' && p[2] == 'd' && p[3] == 'e' &&
              p[4] == '=') {
            g_mode = p[5] == '1';
          } else if (p[0] == 'm' && p[1] == 'e' && p[2] == 'n' && p[3] == 'u' &&
                     p[4] == 'p' && p[5] == 'o' && p[6] == 's' && p[7] == '=') {
            g_menupos = p[8] == '2' ? 2 : (p[8] == '1' ? 1 : 0);
          } else if (p[0] == 'g' && p[1] == 'l' && p[2] == 'a' && p[3] == 's' &&
                     p[4] == 's' && p[5] == '=') {
            g_glass = p[6] == '1';
          } else if (p[0] == 'i' && p[1] == 'a' && p[2] == '=') {
            g_ia = p[3] == '1';
          } else if (p[0] == 'w' && p[1] == 'a' && p[2] == 'l' && p[3] == 'l' &&
                     p[4] == '=') {
            int k = 0;
            p += 5;
            while (*p && *p != '\n' && *p != '\r' && k < 31) g_wall[k++] = *p++;
            g_wall[k] = 0;
            p--;
          } else if (p[0] == 'b' && p[1] == 'g' && p[2] == '=') {
            int v = 0, k;
            for (k = 0; k < 6; k++) {
              char c = p[3 + k];
              int d;
              if (c >= '0' && c <= '9') d = c - '0';
              else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
              else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
              else break;
              v = v * 16 + d;
            }
            g_bg = (unsigned)v;
          }
          while (*p && *p != '\n') p++;
          if (*p == '\n') p++;
        }
      }
    }
  }
  for (i = 0; i < g_n; i++)
    if (g_wall[0] && name_eq(g_names[i], g_wall)) g_wall_sel = i;
  if (g_wall_sel >= 0) pick_wall(g_wall_sel);
  set_status("escolha o modelo e o fundo");
  wg_focus_set(&id_mode0);
  t_open = k_ms();

  while (run) {
    int rh = wg_theme()->row_h;
    int bh = wg_theme()->btn_h;
    char st[120];
    int rows;

    sw = gui_width();
    sh = gui_height();
    ch = gui_char_h();
    if (!g_ready && k_ms() - t_open > 400) g_ready = 1;
    wg_input(g_mx, g_my, g_down);
    gui_fill(0, 0, sw, sh, wg_theme()->bg);

    lx = pad;
    ly = pad;
    lw = 420;

    /* ---------------------------------------------------------- modelo */
    wg_group(lx, ly, lw, 96, "Modelo");
    if (wg_radio(&id_mode0, lx + 14, ly + 26, "Retro - baixo consumo",
                 &g_mode, 0)) {
      k_gui_quality(0);
      set_status("modelo retro: alfa binario, sem mistura");
    }
    if (wg_radio(&id_mode1, lx + 14, ly + 52, "Avancado - cores e transparencia",
                 &g_mode, 1)) {
      k_gui_quality(1);
      set_status("modelo avancado: alfa misturado");
    }

    /* ------------------------------------------------------ cor de fundo */
    ly += 96 + 8;
    wg_group(lx, ly, lw, 132, "Cor de fundo");
    for (i = 0; i < 16; i++) {
      int cx = lx + 16 + (i % 8) * 48;
      int cy = ly + 26 + (i / 8) * 44;
      if (wg_click_area(&cores[i], cx, cy, 40, 36, g_ready)) {
        g_bg = cores[i];
        k_win_bg(g_bg);
        set_status("cor de fundo alterada");
      }
      gui_fill(cx, cy, 40, 32, cores[i]);
      gui_frame(cx, cy, 40, 32, g_bg == cores[i] ? 0xFF0000 : 0x404040);
    }
    if (wg_button(&id_default, lx + 16, ly + 108, 150, bh - 8, "Cor padrao", g_ready)) {
      g_bg = 0;
      k_win_bg(0);
      set_status("cor padrao (teal)");
    }

    /* ------------------------------------------------ imagem de fundo */
    ly += 132 + 8;
    {
      int lh = sh - ly - (ch + 26) - 8 - bh - 12;
      if (lh < 120) lh = 120;
      wg_group(lx, ly, lw, lh, "Imagem de fundo");
      rows = (lh - 34) / rh;
      if (rows < 1) rows = 1;
      /* primeira linha: nenhuma */
      if (wg_click_area(&id_list, lx + 14, ly + 26, lw - 28, rh, g_ready)) {
        pick_wall(-1);
      }
      gui_text(lx + 20, ly + 26 + 1, "(nenhuma)", -1,
               g_wall_sel < 0 ? 0xFFFFFF : wg_theme()->text,
               g_wall_sel < 0 ? wg_theme()->sel_bg : wg_theme()->panel,
               GUI_TEXT_NOBG);
      for (i = 0; i < g_n && i < rows - 1; i++) {
        int iy = ly + 26 + (i + 1) * rh;
        int sel = (g_wall_sel == i);
        if (wg_click_area(&g_names[i], lx + 14, iy, lw - 28, rh, g_ready)) pick_wall(i);
        if (sel) gui_fill(lx + 14, iy, lw - 28, rh, wg_theme()->sel_bg);
        gui_text(lx + 20, iy + 1, g_names[i], -1,
                 sel ? wg_theme()->sel_text : wg_theme()->text,
                 sel ? wg_theme()->sel_bg : wg_theme()->panel, GUI_TEXT_NOBG);
      }
      if (g_n == 0)
        wg_label_dim(lx + 20, ly + 26 + rh + 4,
                     "coloque .PNG/.JPG em D:\\SODOS\\WALLS");
    }

    /* ------------------------------------------------------------ preview */
    px = lx + lw + 10;
    py = pad;
    pw = sw - px - pad;
    if (pw < 320) pw = 320;
    ph = sh - py - (ch + 26) - pad - bh - 12;
    if (ph < 200) ph = 200;
    wg_group(px, py, pw, ph, "Previa");
    draw_preview(px + 12, py + 26, pw - 24, ph - 40);

    /* ------------------------------------------------------------- botoes */
    {
      int by = sh - (ch + 26) - bh - 10;
      if (wg_button(&id_apply, px, by, 130, bh, "Aplicar", g_ready)) apply_all();
      if (wg_button(&id_reset, px + 140, by, 150, bh, "Restaurar", g_ready))
        reset_default();
      if (wg_button(&id_exit, sw - pad - 110, by, 110, bh, "Sair", 1)) run = 0;
    }

    snprintf(st, sizeof(st), "%s  %s", g_mode ? "avancado" : "retro",
             g_wall_sel >= 0 ? g_names[g_wall_sel] : "sem imagem");
    wg_status(0, sh - (ch + 26), sw, g_status, st);
    gui_present();

    if (gui_wait(&ev, 150) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_UP) { g_mx = ev.x; g_my = ev.y; g_down = 0; }
      else if (ev.type == GUI_EV_DRAG) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_KEY) {
        if (ev.key == 27) run = 0;
      }
    }
  }
  (void)id_exit;
  free_wall();
  gui_close();
  return 0;
}
