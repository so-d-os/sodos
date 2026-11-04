/* widgets.c - controles reutilizaveis do SODOS (parte da GUI.DLL).
 *
 * Sem alocacao de estado escondido: o app guarda o que precisa (selecao,
 * texto, foco) e passa em cada chamada. O rato vem de wg_input().
 */
#include <string.h>
#include <so.h>
#include <gui.h>
#include <widgets.h>

/* ---------------------------------------------------------------- tema --- */
static const wg_theme_t theme_light = {
  0xC8C8C8,   /* face */
  0xF4F4F4,   /* face_hi */
  0x707070,   /* face_lo */
  0xE4E4E4,   /* bg */
  0xD8D8D8,   /* panel */
  0x141414,   /* text */
  0x585858,   /* text_dim */
  0x9A9A9A,   /* text_dis */
  0x2A5DB0,   /* sel_bg */
  0xFFFFFF,   /* sel_text */
  0xFFFFFF,   /* field_bg */
  0x101010,   /* field_text */
  0x2A7FD4,   /* accent */
  10, 8, 26, 18
};

static const wg_theme_t theme_dark = {
  0x3A3A42,   /* face */
  0x55555F,   /* face_hi */
  0x16161A,   /* face_lo */
  0x1E1E22,   /* bg */
  0x2A2A30,   /* panel */
  0xE8E8F0,   /* text */
  0xA0A0AC,   /* text_dim */
  0x6A6A74,   /* text_dis */
  0x3E6FB0,   /* sel_bg */
  0xFFFFFF,   /* sel_text */
  0x14141A,   /* field_bg */
  0xE8E8F0,   /* field_text */
  0x4C9BE8,   /* accent */
  10, 8, 26, 18
};

static const wg_theme_t* g_theme = &theme_light;

__export const wg_theme_t* wg_theme(void) { return g_theme; }
__export void wg_set_theme(const wg_theme_t* t) { if (t) g_theme = t; }
__export int wg_theme_is_dark(void) { return g_theme == &theme_dark; }
__export void wg_theme_use(int dark) { g_theme = dark ? &theme_dark : &theme_light; }

/* --------------------------------------------------------------- entrada --- */
static int g_mx, g_my, g_down;
static const void* g_active;
static const void* g_focus;

__export void wg_input(int mx, int my, int down) { g_mx = mx; g_my = my; g_down = down; }
__export int wg_mouse_x(void) { return g_mx; }
__export int wg_mouse_y(void) { return g_my; }
__export int wg_mouse_down(void) { return g_down; }
__export void wg_focus_set(const void* id) { g_focus = id; }
__export const void* wg_focus_get(void) { return g_focus; }

static int hit(int x, int y, int w, int h, int px, int py) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

static int inside_now(int x, int y, int w, int h) {
  return hit(x, y, w, h, g_mx, g_my);
}

/* Logica IMGUI de clique: aperta dentro, solta dentro = clique. */
static int click(const void* id, int x, int y, int w, int h, int enabled) {
  int in = enabled && inside_now(x, y, w, h);
  int clicked = 0;
  if (in && g_down && !g_active) g_active = id;
  if (g_active == id) {
    if (!g_down) {
      clicked = in;
      g_active = 0;
    }
  }
  return clicked;
}

static int pressed(const void* id) { return g_active == id && g_down; }
static int hovered(const void* id, int x, int y, int w, int h) {
  return g_active ? g_active == id : inside_now(x, y, w, h);
}

/* ------------------------------------------------------------ primitivas --- */
__export int wg_text_w(const char* s) { return (int)strlen(s) * gui_char_w(); }

__export void wg_label(int x, int y, const char* s) {
  gui_text(x, y, s, -1, g_theme->text, g_theme->bg, GUI_TEXT_NOBG);
}

__export void wg_label_dim(int x, int y, const char* s) {
  gui_text(x, y, s, -1, g_theme->text_dim, g_theme->bg, GUI_TEXT_NOBG);
}

__export void wg_label_c(int x, int y, int w, const char* s) {
  int t = wg_text_w(s);
  int tx = x + (w - t) / 2;
  if (tx < x) tx = x;
  gui_text(tx, y, s, -1, g_theme->text, g_theme->bg, GUI_TEXT_NOBG);
}

__export void wg_bevel(int x, int y, int w, int h, int raised) {
  unsigned tl = raised ? g_theme->face_hi : g_theme->face_lo;
  unsigned br = raised ? g_theme->face_lo : g_theme->face_hi;
  gui_fill(x, y, w, h, g_theme->face);
  gui_fill(x, y, w, 1, tl);
  gui_fill(x, y, 1, h, tl);
  gui_fill(x, y + h - 1, w, 1, br);
  gui_fill(x + w - 1, y, 1, h, br);
  /* segundo tom, 1px para dentro, da o ar de 3D */
  gui_fill(x + 1, y + 1, w - 2, 1, tl);
  gui_fill(x + 1, y + 1, 1, h - 2, tl);
  gui_fill(x + 1, y + h - 2, w - 2, 1, br);
  gui_fill(x + w - 2, y + 1, 1, h - 2, br);
}

__export void wg_panel(int x, int y, int w, int h) {
  gui_fill(x, y, w, h, g_theme->panel);
}

__export void wg_group(int x, int y, int w, int h, const char* title) {
  int tw = title ? wg_text_w(title) + 8 : 0;
  gui_fill(x, y + 6, w, h - 6, g_theme->panel);
  gui_fill(x, y + 6, w, 1, g_theme->face_lo);
  gui_fill(x, y + h - 1, w, 1, g_theme->face_hi);
  gui_fill(x, y + 6, 1, h - 6, g_theme->face_lo);
  gui_fill(x + w - 1, y + 6, 1, h - 6, g_theme->face_hi);
  if (tw) {
    gui_fill(x + 8, y + 1, tw, 12, g_theme->panel);
    gui_text(x + 12, y, title, -1, g_theme->text, g_theme->panel,
             GUI_TEXT_NOBG);
  }
}

__export void wg_separator(int x, int y, int w) {
  gui_fill(x, y, w, 1, g_theme->face_lo);
  gui_fill(x, y + 1, w, 1, g_theme->face_hi);
}

__export void wg_focus_rect(int x, int y, int w, int h) {
  int i;
  for (i = 0; i < w; i += 2) {
    gui_fill(x + i, y, 1, 1, g_theme->text);
    gui_fill(x + i, y + h - 1, 1, 1, g_theme->text);
  }
  for (i = 0; i < h; i += 2) {
    gui_fill(x, y + i, 1, 1, g_theme->text);
    gui_fill(x + w - 1, y + i, 1, 1, g_theme->text);
  }
}

__export void wg_dot(int cx, int cy, int r, unsigned rgb) {
  int dy;
  if (r < 1) return;
  for (dy = -r; dy <= r; dy++) {
    int dx = r;
    /* maior dx com dx*dx + dy*dy <= r*r (sem sqrt) */
    while (dx > 0 && dx * dx + dy * dy > r * r) dx--;
    gui_fill(cx - dx, cy + dy, dx * 2 + 1, 1, rgb);
  }
}

__export int wg_status(int x, int y, int w, const char* left, const char* right) {
  int h = gui_char_h() + 10;
  gui_fill(x, y, w, h, g_theme->face);
  gui_fill(x, y, w, 1, g_theme->face_hi);
  gui_fill(x, y + 1, w, 1, g_theme->face_lo);
  if (left) gui_text(x + 8, y + 5, left, -1, g_theme->text, g_theme->face,
                     GUI_TEXT_NOBG);
  if (right) {
    int rw = wg_text_w(right);
    gui_text(x + w - rw - 8, y + 5, right, -1, g_theme->text_dim,
             g_theme->face, GUI_TEXT_NOBG);
  }
  return h;
}

/* --------------------------------------------------------------- botoes --- */
__export int wg_button(const void* id, int x, int y, int w, int h, const char* label,
              int enabled) {
  int pressed_now = enabled && pressed(id);
  int focused = g_focus == id;
  int tw = wg_text_w(label);
  int tx = x + (w - tw) / 2;
  int ty = y + (h - gui_char_h()) / 2;
  int clicked = click(id, x, y, w, h, enabled);
  unsigned fg = enabled ? g_theme->text : g_theme->text_dis;

  wg_bevel(x, y, w, h, pressed_now ? 0 : 1);
  if (pressed_now) {
    tx += 1;
    ty += 1;
  }
  gui_text(tx, ty, label, -1, fg, g_theme->face, GUI_TEXT_NOBG);
  if (focused) wg_focus_rect(x + 2, y + 2, w - 4, h - 4);
  return clicked;
}

__export int wg_toggle(const void* id, int x, int y, int w, int h, const char* label,
              int* on) {
  int pressed_now = pressed(id);
  int tw = wg_text_w(label);
  int tx = x + (w - tw) / 2;
  int ty = y + (h - gui_char_h()) / 2;
  int clicked = click(id, x, y, w, h, 1);

  wg_bevel(x, y, w, h, (pressed_now || *on) ? 0 : 1);
  if (pressed_now) {
    tx += 1;
    ty += 1;
  }
  gui_text(tx, ty, label, -1, g_theme->text, g_theme->face, GUI_TEXT_NOBG);
  if (g_focus == id) wg_focus_rect(x + 2, y + 2, w - 4, h - 4);
  if (clicked) {
    *on = !*on;
    return 1;
  }
  return 0;
}

/* ----------------------------------------------------------- check/radio --- */
__export int wg_check(const void* id, int x, int y, const char* label, int* value) {
  int box = 15;
  int by = y + (gui_char_h() - box) / 2;
  int clicked = click(id, x, by, gui_char_w() * 30, box, 1);
  int focused = g_focus == id;

  gui_fill(x, by, box, box, g_theme->field_bg);
  wg_bevel(x, by, box, box, 0);
  if (*value) {
    /* Check: dois traços */
    int i;
    for (i = 0; i < 4; i++)
      gui_fill(x + 4 + i, by + 6 + i, 2, 2, g_theme->accent);
    for (i = 0; i < 6; i++)
      gui_fill(x + 7 + i, by + 9 - i, 2, 2, g_theme->accent);
  }
  gui_text(x + box + 6, y, label, -1, g_theme->text, g_theme->bg,
           GUI_TEXT_NOBG);
  if (focused) wg_focus_rect(x + box + 2, y - 1, wg_text_w(label) + 4,
                             gui_char_h() + 2);
  if (clicked) {
    *value = !*value;
    return 1;
  }
  return 0;
}

__export int wg_radio(const void* id, int x, int y, const char* label, int* value,
             int mine) {
  int box = 15;
  int by = y + (gui_char_h() - box) / 2;
  int cx = x + box / 2;
  int cy = by + box / 2;
  int clicked = click(id, x, by, gui_char_w() * 30, box, 1);
  int on = (*value == mine);
  int focused = g_focus == id;

  gui_fill(x, by, box, box, g_theme->field_bg);
  gui_fill(x, by, box, 1, g_theme->face_lo);
  gui_fill(x, by, 1, box, g_theme->face_lo);
  gui_fill(x, by + box - 1, box, 1, g_theme->face_hi);
  gui_fill(x + box - 1, by, 1, box, g_theme->face_hi);
  wg_dot(cx, cy, 4, g_theme->field_bg);
  if (on) wg_dot(cx, cy, 3, g_theme->accent);
  gui_text(x + box + 6, y, label, -1,
           on ? g_theme->text : g_theme->text_dim, g_theme->bg,
           GUI_TEXT_NOBG);
  if (focused) wg_focus_rect(x + box + 2, y - 1, wg_text_w(label) + 4,
                             gui_char_h() + 2);
  if (clicked && !on) {
    *value = mine;
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------- progresso --- */
__export void wg_progress(int x, int y, int w, int h, int pct, const char* caption) {
  int inner_w = w - 4;
  int fill_w;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  fill_w = inner_w * pct / 100;

  gui_fill(x, y, w, h, g_theme->field_bg);
  wg_bevel(x, y, w, h, 0);
  if (fill_w > 0) {
    int row;
    /* barra com degrade vertical (3 tons do accent) */
    for (row = 0; row < h - 4; row++) {
      unsigned c = g_theme->accent;
      if (row < 2) c = g_theme->face_hi;
      gui_fill(x + 2, y + 2 + row, fill_w, 1,
               row < 2 ? g_theme->accent : c);
    }
    gui_fill(x + 2, y + 2, fill_w, 1, g_theme->face_hi);
  }
  if (caption) {
    int tw = wg_text_w(caption);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - gui_char_h()) / 2;
    gui_text(tx, ty, caption, -1, g_theme->text, g_theme->field_bg,
             GUI_TEXT_NOBG);
  }
}

/* ------------------------------------------------------------- scrollbar --- */
static int thumb_geom(int total, int vis, int track, int* out_len) {
  int len;
  if (total < 1) total = 1;
  if (vis < 1) vis = 1;
  len = track * vis / total;
  if (len < 12) len = 12;
  if (len > track) len = track;
  *out_len = len;
  return len;
}

__export int wg_scrollbar(const void* id, wg_scroll_t* s, int x, int y, int w, int h) {
  int vertical = h >= w;
  int track = vertical ? h : w;
  int len, range, pos, old = s->pos;
  int maxpos;

  if (!s || s->total <= s->vis) return 0;
  maxpos = s->total - s->vis;
  thumb_geom(s->total, s->vis, track, &len);
  range = track - len;
  if (s->pos < 0) s->pos = 0;
  if (s->pos > maxpos) s->pos = maxpos;

  gui_fill(x, y, w, h, g_theme->face);
  gui_fill(x, y, w, 1, g_theme->face_lo);
  gui_fill(x, y, 1, h, g_theme->face_lo);

  pos = range > 0 ? range * s->pos / maxpos : 0;
  if (vertical) {
    int thumb_h = len;
    int ty = y + pos;
    if (g_active == id && g_down) {
      int rel = g_my - y - len / 2;
      int np = range > 0 ? (rel * maxpos) / range : 0;
      if (np < 0) np = 0;
      if (np > maxpos) np = maxpos;
      s->pos = np;
    } else {
      click(id, x + 1, ty, w - 2, thumb_h, 1);
    }
    wg_bevel(x + 1, y + pos, w - 2, thumb_h, 1);
  } else {
    int thumb_w = len;
    if (g_active == id && g_down) {
      int rel = g_mx - x - len / 2;
      int np = range > 0 ? (rel * maxpos) / range : 0;
      if (np < 0) np = 0;
      if (np > maxpos) np = maxpos;
      s->pos = np;
    } else {
      click(id, x + pos, y + 1, thumb_w, h - 2, 1);
    }
    wg_bevel(x + pos, y + 1, thumb_w, h - 2, 1);
  }
  return s->pos != old;
}

/* --------------------------------------------------------------- listbox --- */
static int rows_in(int h) {
  int rh = g_theme->row_h;
  int n = (h - 4) / rh;
  if (n < 1) n = 1;
  return n;
}

static void list_clamp(wg_list_t* l, int rows) {
  if (l->sel < -1) l->sel = -1;
  if (l->sel >= l->n) l->sel = l->n - 1;
  if (l->top > l->n - rows) l->top = l->n - rows;
  if (l->top < 0) l->top = 0;
  if (l->sel >= 0) {
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
  }
}

__export int wg_listbox(const void* id, wg_list_t* l, int x, int y, int w, int h) {
  int rows = rows_in(h);
  int rh = g_theme->row_h;
  int need_sb = l->n > rows;
  int inner_w = need_sb ? w - 14 : w;
  int i, old = l->sel;

  list_clamp(l, rows);

  gui_fill(x, y, w, h, g_theme->field_bg);
  gui_fill(x, y, w, 1, g_theme->face_lo);
  gui_fill(x, y, 1, h, g_theme->face_lo);
  gui_fill(x, y + h - 1, w, 1, g_theme->face_hi);
  gui_fill(x + w - 1, y, 1, h, g_theme->face_hi);

  for (i = 0; i < rows; i++) {
    int idx = l->top + i;
    int iy = y + 2 + i * rh;
    int row_hit;
    if (idx >= l->n) break;
    row_hit = hit(x + 1, iy, inner_w - 2, rh, g_mx, g_my);
    if (idx == l->sel) {
      gui_fill(x + 1, iy, inner_w - 2, rh, g_theme->sel_bg);
    } else if (row_hit) {
      gui_fill(x + 1, iy, inner_w - 2, rh,
               g_theme->face_hi == 0xFFFFFF ? 0xE8EEF8 : 0x3E4048);
    }
    gui_text(x + 6, iy + (rh - gui_char_h()) / 2, l->items[idx], -1,
             idx == l->sel ? g_theme->sel_text : g_theme->field_text,
             idx == l->sel ? g_theme->sel_bg : g_theme->field_bg,
             GUI_TEXT_NOBG);
    if (row_hit && g_down && !g_active) {
      g_active = id;
      if (l->sel != idx) l->sel = idx;
    }
  }
  if (g_active == id && !g_down) g_active = 0;

  if (need_sb) {
    wg_scroll_t s;
    s.pos = l->top;
    s.vis = rows;
    s.total = l->n;
    if (wg_scrollbar(id, &s, x + w - 13, y + 1, 12, h - 2)) l->top = s.pos;
  }
  return l->sel != old;
}

__export int wg_list_key(wg_list_t* l, int key, int rows) {
  int old = l->sel;
  if (rows < 1) rows = 1;
  if (key == GUI_KEY_UP) l->sel--;
  else if (key == GUI_KEY_DOWN) l->sel++;
  else if (key == GUI_KEY_PGUP) l->sel -= rows;
  else if (key == GUI_KEY_PGDN) l->sel += rows;
  else if (key == GUI_KEY_HOME) l->sel = 0;
  else if (key == GUI_KEY_END) l->sel = l->n - 1;
  else return 0;
  list_clamp(l, rows);
  return l->sel != old;
}

__export int wg_list_wheel(wg_list_t* l, int delta, int rows) {
  int old = l->top;
  l->top += delta;
  list_clamp(l, rows);
  return l->top != old;
}

/* ------------------------------------------------------------------ edit --- */
__export int wg_edit_box(const void* id, gui_edit_t* e, int x, int y, int w, int h) {
  int focused = g_focus == id;
  if (inside_now(x, y, w, h) && g_down && !g_active) {
    g_active = id;
    g_focus = id;
    gui_edit_click(e, x, g_mx);
  }
  if (g_active == id && !g_down) g_active = 0;
  gui_edit_draw(e, x, y, w, h, focused);
  return focused;
}

__export int wg_key_is_activate(int key) {
  return key == '\r' || key == '\n' || key == ' ';
}
