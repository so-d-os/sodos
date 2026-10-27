/* fontadv.c - Fontes avancadas do WIN.
 * Lista D:\SODOS\FONTS, troca a face e desenha a frase com o contorno TTF.
 * Compilar: cc -o FONTADV.EXE fontadv.c ttfrast.c GUI.DLL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include "ttfrast.h"

#define NFONTS 4
#define LIST_W 168

static const char* paths[NFONTS] = {
  "D:\\SODOS\\FONTS\\NOTOSANS.TTF",
  "D:\\SODOS\\FONTS\\NOTOSERF.TTF",
  "D:\\SODOS\\FONTS\\NOTOMONO.TTF",
  "D:\\SODOS\\FONTS\\SANSCOLL.TTF"
};

static const char* labels[NFONTS] = {
  "Noto Sans",
  "Noto Serif",
  "Noto Sans Mono",
  "Sans Serif"
};

static const char* files[NFONTS] = {
  "NOTOSANS.TTF",
  "NOTOSERF.TTF",
  "NOTOMONO.TTF",
  "SANSCOLL.TTF"
};

static int list_row;
static int edit_y, edit_h, edit_w;
static int by_size, bh, bx_minus, bx_plus;
static int preview_y, meta_y;

static int load_file(const char* path, uint8_t** data, int* len) {
  FILE* f;
  long n;
  uint8_t* buf;
  *data = 0;
  *len = 0;
  f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 12 || n > 8 * 1024 * 1024) {
    fclose(f);
    return -1;
  }
  buf = (uint8_t*)malloc((size_t)n);
  if (!buf) {
    fclose(f);
    return -1;
  }
  if ((long)fread(buf, 1, (size_t)n, f) != n) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  *data = buf;
  *len = (int)n;
  return 0;
}

static void layout(void) {
  int ch = gui_char_h();
  int cw = gui_char_w();
  int H = gui_height();
  int W = gui_width();
  list_row = ch + 8;
  edit_y = 8 + (ch + 6) + (ch + 8);
  edit_h = ch + 8;
  edit_w = W - LIST_W - 28;
  if (edit_w < 80) edit_w = 80;
  by_size = edit_y + edit_h + 10;
  bh = ch + 6;
  bx_minus = LIST_W + 12;
  bx_plus = bx_minus + 36 + 14 * cw;
  preview_y = by_size + bh + 12;
  meta_y = H - 6 * (ch + 3) - 8;
  if (meta_y < preview_y + ch) meta_y = preview_y + ch;
}

static void paint(int sel, int px, gui_edit_t* ed, uint8_t* rgba, int rw, int rh,
                  const char* name, const char* full, const char* ver,
                  const char* copyr, const char* tm, const char* err) {
  int W = gui_width();
  int H = gui_height();
  int cw = gui_char_w();
  int ch = gui_char_h();
  int i;
  int y;
  int show_h;
  char line[80];
  unsigned ink = 0xF4EAF2;
  unsigned muted = 0xC8B0C4;
  unsigned bg = 0x261C23;
  unsigned panel = 0x1A1218;
  layout();
  gui_fill(0, 0, W, H, bg);
  gui_fill(0, 0, LIST_W, H, panel);
  for (i = 0; i < NFONTS; i++) {
    unsigned row = (i == sel) ? 0x5C3A62 : panel;
    gui_fill(4, 8 + i * list_row, LIST_W - 8, list_row - 2, row);
    gui_text(12, 8 + i * list_row + 4, labels[i], -1, ink, row, 0);
  }
  y = 8;
  gui_text(LIST_W + 12, y, "Personalizacao  >  Fontes", -1, muted, bg, 0);
  y += ch + 6;
  gui_text(LIST_W + 12, y, name[0] ? name : labels[sel], -1, ink, bg, 0);
  if (full[0]) gui_text(LIST_W + 12 + 22 * cw, y, full, -1, muted, bg, 0);
  gui_edit_draw(ed, LIST_W + 12, edit_y, edit_w, edit_h, 1);
  gui_fill(bx_minus, by_size, 28, bh, 0x3A2434);
  gui_frame(bx_minus, by_size, 28, bh, 0xA275A8);
  gui_text(bx_minus + 10, by_size + 3, "-", -1, ink, 0x3A2434, 0);
  sprintf(line, "%d pontos", px);
  gui_text(bx_minus + 36, by_size + 3, line, -1, ink, bg, 0);
  gui_fill(bx_plus, by_size, 28, bh, 0x3A2434);
  gui_frame(bx_plus, by_size, 28, bh, 0xA275A8);
  gui_text(bx_plus + 10, by_size + 3, "+", -1, ink, 0x3A2434, 0);
  if (err[0]) gui_text(LIST_W + 12, preview_y, err, -1, 0xA275A8, bg, 0);
  show_h = rh;
  if (rgba && rw > 0 && show_h > 0) {
    if (preview_y + show_h > meta_y - 4) show_h = meta_y - 4 - preview_y;
    if (show_h > 0) gui_pixels(LIST_W + 12, preview_y, rw, show_h, rgba, rw * 4);
  }
  y = meta_y;
  gui_text(LIST_W + 12, y, "Nome", -1, muted, bg, 0);
  gui_text(LIST_W + 12 + 22 * cw, y, name[0] ? name : "-", -1, ink, bg, 0);
  y += ch + 3;
  gui_text(LIST_W + 12, y, "Arquivo de fonte", -1, muted, bg, 0);
  gui_text(LIST_W + 12 + 22 * cw, y, files[sel], -1, ink, bg, 0);
  y += ch + 3;
  gui_text(LIST_W + 12, y, "Versao", -1, muted, bg, 0);
  gui_text(LIST_W + 12 + 22 * cw, y, ver[0] ? ver : "-", -1, ink, bg, 0);
  y += ch + 3;
  gui_text(LIST_W + 12, y, "Direitos autorais", -1, muted, bg, 0);
  gui_text_fit(LIST_W + 12 + 22 * cw, y, copyr[0] ? copyr : "-", W - (LIST_W + 24 + 22 * cw), ink, bg, 0);
  y += ch + 3;
  gui_text(LIST_W + 12, y, "Marcas registradas", -1, muted, bg, 0);
  gui_text_fit(LIST_W + 12 + 22 * cw, y, tm[0] ? tm : "-", W - (LIST_W + 24 + 22 * cw), ink, bg, 0);
  y += ch + 3;
  gui_text(LIST_W + 12, y, "Licenca SIL OFL 1.1", -1, muted, bg, 0);
}

int main(void) {
  gui_event_t ev;
  gui_edit_t ed;
  int sel = 0;
  int px = 24;
  int loaded = -1;
  int flen = 0;
  int dirty = 1;
  int rw = 0;
  int rh = 0;
  uint8_t* file = 0;
  uint8_t* rgba = 0;
  char name[96];
  char full[96];
  char ver[160];
  char copy[200];
  char tm[160];
  char err[80];
  name[0] = full[0] = ver[0] = copy[0] = tm[0] = err[0] = 0;
  if (gui_open("Fontes avancadas") != 0) {
    printf("Abra pelo menu Iniciar, Configuracoes, Fontes avancadas.\n");
    return 1;
  }
  gui_edit_set(&ed, "Exemplo de fonte digitada");
  for (;;) {
    if (dirty) {
      strcpy(err, "desenhando...");
      paint(sel, px, &ed, 0, 0, 0, name, full, ver, copy, tm, err);
      gui_present();
      if (loaded != sel) {
        free(file);
        file = 0;
        flen = 0;
        loaded = -1;
        name[0] = full[0] = ver[0] = copy[0] = tm[0] = 0;
        if (load_file(paths[sel], &file, &flen) == 0) loaded = sel;
      }
      free(rgba);
      rgba = 0;
      rw = rh = 0;
      err[0] = 0;
      if (!file) {
        strcpy(err, "fonte ausente em D:\\SODOS\\FONTS");
      } else if (ttf_phrase(file, flen, ed.text, px, &rgba, &rw, &rh,
                            name, (int)sizeof(name), full, (int)sizeof(full),
                            ver, (int)sizeof(ver), copy, (int)sizeof(copy),
                            tm, (int)sizeof(tm)) != 0) {
        strcpy(err, "nao consegui desenhar esta fonte");
      }
      dirty = 0;
    }
    paint(sel, px, &ed, rgba, rw, rh, name, full, ver, copy, tm, err);
    gui_present();
    if (gui_wait(&ev, -1) == GUI_EV_CLOSE || ev.type == GUI_EV_CLOSE) break;
    if (ev.type == GUI_EV_KEY) {
      if (ev.key == GUI_KEY_UP) {
        if (sel > 0) {
          sel--;
          dirty = 1;
        }
      } else if (ev.key == GUI_KEY_DOWN) {
        if (sel + 1 < NFONTS) {
          sel++;
          dirty = 1;
        }
      } else if (ev.key == GUI_KEY_PGUP) {
        if (px > 8) {
          px -= 2;
          if (px < 8) px = 8;
          dirty = 1;
        }
      } else if (ev.key == GUI_KEY_PGDN) {
        if (px < 64) {
          px += 2;
          if (px > 64) px = 64;
          dirty = 1;
        }
      } else {
        char prev[512];
        strcpy(prev, ed.text);
        gui_edit_key(&ed, ev.key);
        if (strcmp(prev, ed.text) != 0 && ed.text[0]) dirty = 1;
      }
    } else if (ev.type == GUI_EV_DOWN && ev.key == 1) {
      layout();
      if (ev.x < LIST_W) {
        int i = (ev.y - 8) / list_row;
        if (i >= 0 && i < NFONTS && i != sel) {
          sel = i;
          dirty = 1;
        }
      } else if (gui_hit(bx_minus, by_size, 28, bh, ev.x, ev.y)) {
        if (px > 8) {
          px -= 2;
          if (px < 8) px = 8;
          dirty = 1;
        }
      } else if (gui_hit(bx_plus, by_size, 28, bh, ev.x, ev.y)) {
        if (px < 64) {
          px += 2;
          if (px > 64) px = 64;
          dirty = 1;
        }
      } else if (gui_hit(LIST_W + 12, edit_y, edit_w, edit_h, ev.x, ev.y)) {
        gui_edit_click(&ed, LIST_W + 12, ev.x);
      }
    }
  }
  gui_close();
  free(file);
  free(rgba);
  return 0;
}
