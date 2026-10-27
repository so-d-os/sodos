/* gui.c - GUI.DLL: janela canvas do WIN e alguns controles simples. */
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <gui.h>

static int win_w, win_h, char_w = 8, char_h = 16;

__export int gui_open(const char* title) {
  int rc = k_gui_open(title);
  if (rc != 0) return rc;
  k_gui_size(&win_w, &win_h);
  k_gui_font(&char_w, &char_h);
  return 0;
}

__export void gui_close(void) { k_gui_close(); }
__export void gui_title(const char* title) { k_gui_title(title); }
__export int gui_width(void) { return win_w; }
__export int gui_height(void) { return win_h; }
__export int gui_char_w(void) { return char_w; }
__export int gui_char_h(void) { return char_h; }
__export void gui_fill(int x, int y, int w, int h, unsigned rgb) { k_gui_fill(x, y, w, h, rgb); }

__export void gui_pixels(int x, int y, int w, int h, const void* rgba, int stride) {
  k_gui_pixels(x, y, w, h, rgba, stride);
}

__export void gui_image(int x, int y, int w, int h, const void* rgba, int iw, int ih,
                        int cx, int cy, int cw, int ch) {
  int y0, y1, x0, x1, dw, row;
  unsigned char* line;
  if (w < 1 || h < 1 || iw < 1 || ih < 1 || !rgba) return;
  y0 = y > cy ? y : cy;
  y1 = y + h;
  if (y1 > cy + ch) y1 = cy + ch;
  x0 = x > cx ? x : cx;
  x1 = x + w;
  if (x1 > cx + cw) x1 = cx + cw;
  if (y0 >= y1 || x0 >= x1) return;
  dw = x1 - x0;
  line = (unsigned char*)malloc((size_t)dw * 4);
  if (!line) return;
  for (row = y0; row < y1; row++) {
    int sy = (row - y) * ih / h;
    int col;
    const unsigned char* src;
    if (sy < 0) sy = 0;
    if (sy >= ih) sy = ih - 1;
    src = (const unsigned char*)rgba + (size_t)sy * (size_t)iw * 4;
    for (col = 0; col < dw; col++) {
      int sx = (x0 + col - x) * iw / w;
      const unsigned char* p;
      if (sx < 0) sx = 0;
      if (sx >= iw) sx = iw - 1;
      p = src + sx * 4;
      line[col * 4] = p[0];
      line[col * 4 + 1] = p[1];
      line[col * 4 + 2] = p[2];
      line[col * 4 + 3] = p[3];
    }
    k_gui_pixels(x0, row, dw, 1, line, dw * 4);
  }
  free(line);
}

__export void gui_present(void) { k_gui_present(); }

__export void gui_frame(int x, int y, int w, int h, unsigned rgb) {
  k_gui_fill(x, y, w, 1, rgb);
  k_gui_fill(x, y + h - 1, w, 1, rgb);
  k_gui_fill(x, y, 1, h, rgb);
  k_gui_fill(x + w - 1, y, 1, h, rgb);
}

__export int gui_text(int x, int y, const char* s, int n, unsigned fg, unsigned bg, int flags) {
  if (n < 0) n = (int)strlen(s);
  k_gui_text(x, y, s, n, fg, bg, flags);
  return n * char_w;
}

__export int gui_text_fit(int x, int y, const char* s, int max_w, unsigned fg, unsigned bg, int flags) {
  int n = (int)strlen(s);
  int fit = max_w / char_w;
  if (fit < 0) fit = 0;
  if (n > fit) n = fit;
  k_gui_text(x, y, s, n, fg, bg, flags);
  return n * char_w;
}

__export int gui_wait(gui_event_t* ev, int timeout_ms) {
  int raw[4];
  int t = k_gui_event(raw, timeout_ms);
  ev->type = raw[0];
  ev->x = raw[1];
  ev->y = raw[2];
  ev->key = raw[3];
  return t < 0 ? GUI_EV_CLOSE : ev->type;
}

__export int gui_hit(int x, int y, int w, int h, int px, int py) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

__export void gui_button(int x, int y, int w, int h, const char* label, int enabled) {
  int n = (int)strlen(label);
  int tx = x + (w - n * char_w) / 2;
  int ty = y + (h - char_h) / 2;
  k_gui_fill(x, y, w, h, 0xC0C0C0);
  k_gui_fill(x, y, w, 1, 0xFFFFFF);
  k_gui_fill(x, y, 1, h, 0xFFFFFF);
  k_gui_fill(x, y + h - 1, w, 1, 0x404040);
  k_gui_fill(x + w - 1, y, 1, h, 0x404040);
  k_gui_text(tx, ty, label, n, enabled ? 0x000000 : 0x808080, 0xC0C0C0, GUI_TEXT_NOBG);
}

__export void gui_edit_set(gui_edit_t* e, const char* s) {
  int n = (int)strlen(s);
  if (n > (int)sizeof(e->text) - 1) n = (int)sizeof(e->text) - 1;
  memcpy(e->text, s, (size_t)n);
  e->text[n] = 0;
  e->len = n;
  e->caret = n;
  e->scroll = 0;
  e->all = 0;
}

__export void gui_edit_select_all(gui_edit_t* e) {
  e->all = e->len > 0;
  e->caret = e->len;
}

__export int gui_edit_key(gui_edit_t* e, int key) {
  if (key == '\r' || key == '\n') return 2;
  if (e->all) {
    e->all = 0;
    if (key == '\b' || key == GUI_KEY_DEL || (key >= 32 && key < 127)) {
      e->len = e->caret = e->scroll = 0;
      e->text[0] = 0;
      if (key < 32 || key >= 127) return 1;
    }
  }
  if (key == '\b') {
    if (e->caret == 0) return 0;
    memmove(e->text + e->caret - 1, e->text + e->caret, (size_t)(e->len - e->caret + 1));
    e->caret--;
    e->len--;
    return 1;
  }
  if (key == GUI_KEY_DEL) {
    if (e->caret >= e->len) return 0;
    memmove(e->text + e->caret, e->text + e->caret + 1, (size_t)(e->len - e->caret));
    e->len--;
    return 1;
  }
  if (key == GUI_KEY_LEFT) {
    if (e->caret > 0) e->caret--;
    return 1;
  }
  if (key == GUI_KEY_RIGHT) {
    if (e->caret < e->len) e->caret++;
    return 1;
  }
  if (key == GUI_KEY_HOME) {
    e->caret = 0;
    return 1;
  }
  if (key == GUI_KEY_END) {
    e->caret = e->len;
    return 1;
  }
  if (key >= 32 && key < 127 && e->len < (int)sizeof(e->text) - 1) {
    memmove(e->text + e->caret + 1, e->text + e->caret, (size_t)(e->len - e->caret + 1));
    e->text[e->caret++] = (char)key;
    e->len++;
    return 1;
  }
  return 0;
}

__export void gui_edit_click(gui_edit_t* e, int x, int px) {
  int c = e->scroll + (px - x - 4 + char_w / 2) / char_w;
  if (c < 0) c = 0;
  if (c > e->len) c = e->len;
  e->caret = c;
  e->all = 0;
}

__export void gui_edit_draw(gui_edit_t* e, int x, int y, int w, int h, int focused) {
  int vis = (w - 8) / char_w;
  int ty = y + (h - char_h) / 2;
  int n;
  if (vis < 1) vis = 1;
  if (e->caret < e->scroll) e->scroll = e->caret;
  if (e->caret > e->scroll + vis) e->scroll = e->caret - vis;
  k_gui_fill(x, y, w, h, 0xFFFFFF);
  gui_frame(x, y, w, h, focused ? 0x000080 : 0x808080);
  n = e->len - e->scroll;
  if (n > vis) n = vis;
  if (n > 0) {
    int sel = focused && e->all;
    k_gui_text(x + 4, ty, e->text + e->scroll, n, sel ? 0xFFFFFF : 0x000000,
               sel ? 0x000080 : 0xFFFFFF, 0);
  }
  if (focused) k_gui_fill(x + 4 + (e->caret - e->scroll) * char_w, ty, 2, char_h, 0x000000);
}
