/* widgets.c - controles reutilizaveis do SODOS (parte da GUI.DLL).
 *
 * Sem alocacao de estado escondido: o app guarda o que precisa (selecao,
 * texto, foco) e passa em cada chamada. O rato vem de wg_input().
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
  10, 8, 26, 18,
  0, 0, 0x707070
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
  10, 8, 26, 18,
  0, 0, 0x16161A
};

static const wg_theme_t* g_theme = &theme_light;
static int g_custom;          /* o app passou um tema proprio (wg_set_theme) */
static int g_want_dark;       /* wg_theme_use(1) */
static wg_theme_t theme_sys;  /* tema moderno do sistema (k_ui_theme) */
static unsigned g_sys_gen = 0xFFFFFFFFu;
static k_ui_theme_t g_kt;

static unsigned mix(unsigned a, unsigned b, int t);

/* Segue Configuracoes > Temas: com o tema do sistema moderno, os controles
 * passam a ser chapados e redondos, com as cores do TEMA.CFG. */
static void theme_sync(void) {
  unsigned gen = (unsigned)k_ui_theme(0, 0);
  if (gen == g_sys_gen) return;
  g_sys_gen = gen;
  memset(&g_kt, 0, sizeof(g_kt));
  k_ui_theme(0, &g_kt);
  if (g_custom) return;
  if (!g_kt.modern) {
    g_theme = g_want_dark ? &theme_dark : &theme_light;
    return;
  }
  theme_sys.face = g_kt.face;
  theme_sys.bg = g_kt.bg;
  theme_sys.panel = g_kt.panel;
  theme_sys.text = g_kt.text;
  theme_sys.text_dim = g_kt.text_dim;
  theme_sys.text_dis = g_kt.text_dis;
  theme_sys.sel_bg = g_kt.sel_bg;
  theme_sys.sel_text = g_kt.sel_text;
  theme_sys.field_bg = g_kt.field_bg;
  theme_sys.field_text = g_kt.text;
  theme_sys.accent = g_kt.accent;
  theme_sys.border = g_kt.border;
  /* face_hi/face_lo: quem desenha relevo por conta propria ainda acha cores */
  theme_sys.face_hi = mix(g_kt.face, 0xFFFFFF, 128);
  theme_sys.face_lo = g_kt.border;
  theme_sys.pad = 12;
  theme_sys.gap = 10;
  theme_sys.btn_h = g_kt.btn_h > 0 ? g_kt.btn_h : 30;
  theme_sys.row_h = g_kt.row_h > 0 ? g_kt.row_h : 26;
  theme_sys.style = 1;
  theme_sys.radius = g_kt.radius > 0 ? g_kt.radius : 8;
  g_theme = &theme_sys;
}

__export const wg_theme_t* wg_theme(void) {
  theme_sync();
  return g_theme;
}
__export void wg_set_theme(const wg_theme_t* t) {
  if (t) {
    g_theme = t;
    g_custom = 1;
  }
}
__export int wg_theme_is_dark(void) {
  unsigned b = g_theme->bg;
  return ((b >> 16 & 255) + (b >> 8 & 255) + (b & 255)) < 3 * 110;
}
__export void wg_theme_use(int dark) {
  g_want_dark = dark;
  g_custom = 0;
  g_sys_gen = 0xFFFFFFFFu;
  theme_sync();
  if (!g_kt.modern) g_theme = dark ? &theme_dark : &theme_light;
}
__export int wg_modern(void) {
  theme_sync();
  return g_theme->style == 1;
}

#define M (g_theme->style == 1)
#define RAD (g_theme->radius)

/* --------------------------------------------------------------- entrada --- */
static int g_mx, g_my, g_down;
static const void* g_active;
static const void* g_focus;

__export void wg_input(int mx, int my, int down) {
  g_mx = mx;
  g_my = my;
  g_down = down;
  theme_sync();
}
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

/* -------------------------------------------------- desenho moderno (AA) --- */
/* t = 0..256: quanto de b entra */
static unsigned mix(unsigned a, unsigned b, int t) {
  int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  return (unsigned)(((ar + (br - ar) * t / 256) << 16) | ((ag + (bg - ag) * t / 256) << 8) |
                    (ab + (bb - ab) * t / 256));
}

/* Cobertura (0..16) do pixel (i,j) do canto de raio r (centro em r,r). */
static int corner_cov(int i, int j, int r) {
  int s, t, k = 0, C = 8 * r, R2 = 64 * r * r;
  for (t = 0; t < 4; t++)
    for (s = 0; s < 4; s++) {
      int X = 8 * i + 2 * s + 1 - C, Y = 8 * j + 2 * t + 1 - C;
      if (X * X + Y * Y <= R2) k++;
    }
  return k;
}

/* Retangulo redondo com borda suave sobre o fundo bg (cor conhecida). */
static void rr_fill(int x, int y, int w, int h, int r, unsigned c, unsigned bg) {
  int i, j;
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r < 1) {
    gui_fill(x, y, w, h, c);
    return;
  }
  gui_fill(x, y + r, w, h - 2 * r, c);
  for (j = 0; j < r; j++) {
    int full = r;
    for (i = 0; i < r; i++) {
      int k = corner_cov(i, j, r);
      if (k >= 16) {
        full = i;
        break;
      }
      if (k) {
        unsigned m = mix(bg, c, k * 16);
        gui_fill(x + i, y + j, 1, 1, m);
        gui_fill(x + w - 1 - i, y + j, 1, 1, m);
        gui_fill(x + i, y + h - 1 - j, 1, 1, m);
        gui_fill(x + w - 1 - i, y + h - 1 - j, 1, 1, m);
      }
    }
    gui_fill(x + full, y + j, w - 2 * full, 1, c);
    gui_fill(x + full, y + h - 1 - j, w - 2 * full, 1, c);
  }
}

/* Contorno de 1px (borda) com miolo fill. */
static void rr_box(int x, int y, int w, int h, int r, unsigned border, unsigned fill,
                   unsigned bg) {
  rr_fill(x, y, w, h, r, border, bg);
  rr_fill(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill, border);
}

/* Anel de foco de 2px na cor de destaque, por fora do controle. */
static void focus_ring(int x, int y, int w, int h, int r, unsigned inner, unsigned bg) {
  unsigned ring = mix(bg, g_theme->accent, 150);
  rr_fill(x - 2, y - 2, w + 4, h + 4, r + 2, ring, bg);
  rr_fill(x, y, w, h, r, inner, ring);
}

/* Sombra curta embaixo (1-2px), para botoes e cartoes. */
static void soft_shadow(int x, int y, int w, int h, int r, unsigned bg) {
  unsigned s1 = mix(bg, 0x000000, wg_theme_is_dark() ? 90 : 22);
  unsigned s2 = mix(bg, 0x000000, wg_theme_is_dark() ? 50 : 12);
  rr_fill(x, y + 2, w, h, r, s2, bg);
  rr_fill(x, y + 1, w, h, r, s1, s2);
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
  unsigned tl, br;
  if (M) {
    rr_box(x, y, w, h, RAD, g_theme->border,
           raised ? g_theme->face : mix(g_theme->face, g_theme->accent, 28), g_theme->bg);
    return;
  }
  tl = raised ? g_theme->face_hi : g_theme->face_lo;
  br = raised ? g_theme->face_lo : g_theme->face_hi;
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
  if (M) {
    rr_box(x, y, w, h, RAD, g_theme->border, g_theme->panel, g_theme->bg);
    return;
  }
  gui_fill(x, y, w, h, g_theme->panel);
}

/* Cartao: painel redondo com sombra suave (moderno); no classico, painel. */
__export void wg_card(int x, int y, int w, int h) {
  if (M) {
    soft_shadow(x, y, w, h, RAD + 2, g_theme->bg);
    rr_box(x, y, w, h, RAD + 2, mix(g_theme->border, g_theme->panel, 96), g_theme->panel,
           g_theme->bg);
    return;
  }
  wg_bevel(x, y, w, h, 1);
}

__export void wg_group(int x, int y, int w, int h, const char* title) {
  int tw = title ? wg_text_w(title) + 8 : 0;
  if (M) {
    /* cartao com o titulo dentro, em cima (os controles comecam em y+26) */
    rr_box(x, y + 2, w, h - 2, RAD + 2, g_theme->border, g_theme->panel, g_theme->bg);
    if (title)
      gui_text(x + 12, y + 7, title, -1, g_theme->text_dim, g_theme->panel, GUI_TEXT_NOBG);
    return;
  }
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
  if (M) {
    gui_fill(x, y, w, 1, g_theme->border);
    return;
  }
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
  if (M) {
    gui_fill(x, y, w, 1, g_theme->border);
  } else {
    gui_fill(x, y, w, 1, g_theme->face_hi);
    gui_fill(x, y + 1, w, 1, g_theme->face_lo);
  }
  {
    /* janela estreita: o texto da esquerda e cortado antes do da direita */
    int rw = right ? wg_text_w(right) + 16 : 0;
    if (left && w - rw - 16 > 0)
      gui_text_fit(x + 8, y + 5, left, w - rw - 16, g_theme->text, g_theme->face,
                   GUI_TEXT_NOBG);
  }
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

  if (M) {
    int hov = enabled && hovered(id, x, y, w, h);
    unsigned face = g_theme->face;
    if (pressed_now) face = mix(face, g_theme->accent, 46);
    else if (hov) face = mix(face, g_theme->accent, 20);
    if (!enabled) face = mix(g_theme->face, g_theme->bg, 128);
    if (focused) {
      focus_ring(x, y, w, h, RAD, g_theme->border, g_theme->bg);
      rr_fill(x + 1, y + 1, w - 2, h - 2, RAD - 1, face, g_theme->border);
    } else {
      if (enabled && !pressed_now) soft_shadow(x, y, w, h, RAD, g_theme->bg);
      rr_box(x, y, w, h, RAD, hov ? mix(g_theme->border, g_theme->accent, 90) : g_theme->border,
             face, g_theme->bg);
    }
    gui_text(tx, ty, label, -1, fg, face, GUI_TEXT_NOBG);
    return clicked;
  }
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

  if (M) {
    int hov = hovered(id, x, y, w, h);
    unsigned face = *on ? g_theme->accent : g_theme->face;
    unsigned fg = *on ? 0xFFFFFF : g_theme->text;
    if (pressed_now) face = mix(face, *on ? 0x000000 : g_theme->accent, 40);
    else if (hov) face = mix(face, *on ? 0xFFFFFF : g_theme->accent, 24);
    rr_box(x, y, w, h, RAD, *on ? face : g_theme->border, face, g_theme->bg);
    gui_text(tx, ty, label, -1, fg, face, GUI_TEXT_NOBG);
    if (g_focus == id) gui_fill(x + 8, y + h - 3, w - 16, 2, *on ? 0xFFFFFF : g_theme->accent);
    if (clicked) {
      *on = !*on;
      return 1;
    }
    return 0;
  }
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

  if (M) {
    int b = 18, yy = y + (gui_char_h() - b) / 2;
    if (*value) {
      int i;
      rr_fill(x, yy, b, b, 5, g_theme->accent, g_theme->bg);
      /* visto branco de 2px */
      for (i = 0; i < 4; i++) gui_fill(x + 4 + i, yy + 8 + i, 2, 2, 0xFFFFFF);
      for (i = 0; i < 7; i++) gui_fill(x + 7 + i, yy + 11 - i, 2, 2, 0xFFFFFF);
    } else {
      rr_box(x, yy, b, b, 5,
             hovered(id, x, by, gui_char_w() * 30, box) ? g_theme->accent : g_theme->border,
             g_theme->field_bg, g_theme->bg);
    }
    gui_text(x + b + 8, y, label, -1, g_theme->text, g_theme->bg, GUI_TEXT_NOBG);
    if (focused) gui_fill(x + b + 8, y + gui_char_h(), wg_text_w(label), 2, g_theme->accent);
    if (clicked) {
      *value = !*value;
      return 1;
    }
    return 0;
  }
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

  if (M) {
    int b = 18, yy = y + (gui_char_h() - b) / 2;
    if (on) {
      rr_fill(x, yy, b, b, b / 2, g_theme->accent, g_theme->bg);
      rr_fill(x + 6, yy + 6, 6, 6, 3, 0xFFFFFF, g_theme->accent);
    } else {
      rr_box(x, yy, b, b, b / 2,
             hovered(id, x, by, gui_char_w() * 30, box) ? g_theme->accent : g_theme->border,
             g_theme->field_bg, g_theme->bg);
    }
    gui_text(x + b + 8, y, label, -1, on ? g_theme->text : g_theme->text_dim, g_theme->bg,
             GUI_TEXT_NOBG);
    if (focused) gui_fill(x + b + 8, y + gui_char_h(), wg_text_w(label), 2, g_theme->accent);
    if (clicked && !on) {
      *value = mine;
      return 1;
    }
    return 0;
  }
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

  if (M) {
    unsigned track = mix(g_theme->bg, g_theme->border, 160);
    int r = h / 2;
    rr_fill(x, y, w, h, r, track, g_theme->bg);
    fill_w = w * pct / 100;
    if (fill_w > 0) {
      if (fill_w < h) fill_w = h;
      rr_fill(x, y, fill_w, h, r, g_theme->accent, track);
    }
    if (caption && h >= gui_char_h()) {
      int tw = wg_text_w(caption);
      int tx = x + (w - tw) / 2;
      int ty = y + (h - gui_char_h()) / 2;
      gui_text(tx, ty, caption, -1, tx + tw / 2 < x + fill_w ? 0xFFFFFF : g_theme->text,
               g_theme->accent, GUI_TEXT_NOBG);
    }
    return;
  }
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

  if (M) {
    gui_fill(x, y, w, h, g_theme->field_bg);
  } else {
    gui_fill(x, y, w, h, g_theme->face);
    gui_fill(x, y, w, 1, g_theme->face_lo);
    gui_fill(x, y, 1, h, g_theme->face_lo);
  }

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
    if (M) {
      int act = g_active == id || hit(x, y + pos, w, thumb_h, g_mx, g_my);
      rr_fill(x + 3, y + pos + 2, w - 6, thumb_h - 4, (w - 6) / 2,
              mix(g_theme->field_bg, g_theme->text, act ? 120 : 70), g_theme->field_bg);
    } else {
      wg_bevel(x + 1, y + pos, w - 2, thumb_h, 1);
    }
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
    if (M) {
      int act = g_active == id || hit(x + pos, y, thumb_w, h, g_mx, g_my);
      rr_fill(x + pos + 2, y + 3, thumb_w - 4, h - 6, (h - 6) / 2,
              mix(g_theme->field_bg, g_theme->text, act ? 120 : 70), g_theme->field_bg);
    } else {
      wg_bevel(x + pos, y + 1, thumb_w, h - 2, 1);
    }
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

  if (M) {
    rr_box(x, y, w, h, RAD, g_theme->border, g_theme->field_bg, g_theme->bg);
  } else {
    gui_fill(x, y, w, h, g_theme->field_bg);
    gui_fill(x, y, w, 1, g_theme->face_lo);
    gui_fill(x, y, 1, h, g_theme->face_lo);
    gui_fill(x, y + h - 1, w, 1, g_theme->face_hi);
    gui_fill(x + w - 1, y, 1, h, g_theme->face_hi);
  }

  for (i = 0; i < rows; i++) {
    int idx = l->top + i;
    int iy = y + 2 + i * rh;
    int row_hit;
    if (idx >= l->n) break;
    row_hit = hit(x + 1, iy, inner_w - 2, rh, g_mx, g_my);
    if (M) {
      if (idx == l->sel)
        rr_fill(x + 4, iy + 1, inner_w - 8, rh - 2, 6, g_theme->sel_bg, g_theme->field_bg);
      else if (row_hit)
        rr_fill(x + 4, iy + 1, inner_w - 8, rh - 2, 6,
                mix(g_theme->field_bg, g_theme->accent, 26), g_theme->field_bg);
    } else if (idx == l->sel) {
      gui_fill(x + 1, iy, inner_w - 2, rh, g_theme->sel_bg);
    } else if (row_hit) {
      gui_fill(x + 1, iy, inner_w - 2, rh,
               g_theme->face_hi == 0xFFFFFF ? 0xE8EEF8 : 0x3E4048);
    }
    gui_text(x + (M ? 12 : 6), iy + (rh - gui_char_h()) / 2, l->items[idx], -1,
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
    if (wg_scrollbar(id, &s, x + w - 13, y + (M ? 4 : 1), 12, h - (M ? 8 : 2)))
      l->top = s.pos;
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
  if (M) {
    int cw = gui_char_w(), chh = gui_char_h();
    int vis = (w - 16) / cw, ty = y + (h - chh) / 2, n;
    if (vis < 1) vis = 1;
    if (e->caret < e->scroll) e->scroll = e->caret;
    if (e->caret > e->scroll + vis) e->scroll = e->caret - vis;
    if (focused) {
      focus_ring(x, y, w, h, RAD, g_theme->accent, g_theme->bg);
      rr_fill(x + 1, y + 1, w - 2, h - 2, RAD - 1, g_theme->field_bg, g_theme->accent);
    } else {
      rr_box(x, y, w, h, RAD, g_theme->border, g_theme->field_bg, g_theme->bg);
    }
    n = e->len - e->scroll;
    if (n > vis) n = vis;
    if (n > 0) {
      int sel = focused && e->all;
      if (sel) gui_fill(x + 8, ty, n * cw, chh, mix(g_theme->field_bg, g_theme->accent, 70));
      gui_text(x + 8, ty, e->text + e->scroll, n, g_theme->field_text, g_theme->field_bg,
               GUI_TEXT_NOBG);
    }
    if (focused) gui_fill(x + 8 + (e->caret - e->scroll) * cw, ty, 2, chh, g_theme->accent);
    return focused;
  }
  gui_edit_draw(e, x, y, w, h, focused);
  return focused;
}

__export int wg_click_area(const void* id, int x, int y, int w, int h,
                           int enabled) {
  return click(id, x, y, w, h, enabled);
}

__export int wg_key_is_activate(int key) {
  return key == '\r' || key == '\n' || key == ' ';
}

/* ------------------------------------------------------ widgets modernos --- */
/* Botao de destaque (acao principal): cheio na cor de destaque. No tema
 * classico e um wg_button comum. */
__export int wg_button_primary(const void* id, int x, int y, int w, int h,
                               const char* label, int enabled) {
  int pressed_now, tw, tx, ty, clicked, hov;
  unsigned face = g_theme->accent;
  if (!M) return wg_button(id, x, y, w, h, label, enabled);
  pressed_now = enabled && pressed(id);
  tw = wg_text_w(label);
  tx = x + (w - tw) / 2;
  ty = y + (h - gui_char_h()) / 2;
  clicked = click(id, x, y, w, h, enabled);
  hov = enabled && hovered(id, x, y, w, h);
  if (!enabled) face = mix(face, g_theme->bg, 150);
  else if (pressed_now) face = mix(face, 0x000000, 50);
  else if (hov) face = mix(face, 0xFFFFFF, 30);
  if (g_focus == id) {
    focus_ring(x, y, w, h, RAD, face, g_theme->bg);
  } else {
    if (enabled && !pressed_now) soft_shadow(x, y, w, h, RAD, g_theme->bg);
    rr_fill(x, y, w, h, RAD, face, g_theme->bg);
  }
  gui_text(tx, ty, label, -1, 0xFFFFFF, face, GUI_TEXT_NOBG);
  return clicked;
}

/* Interruptor (liga/desliga) com o rotulo a direita. Devolve 1 se mudou. */
__export int wg_switch(const void* id, int x, int y, const char* label, int* on) {
  int tw = 40, th = 22;
  int ty = y + (gui_char_h() - th) / 2;
  int lw = label ? wg_text_w(label) + 10 : 0;
  int clicked = click(id, x, ty, tw + lw, th, 1);
  int hov = hovered(id, x, ty, tw + lw, th);
  unsigned track = *on ? g_theme->accent : mix(g_theme->bg, g_theme->border, 220);
  int kx;
  if (hov) track = mix(track, *on ? 0xFFFFFF : g_theme->text, 30);
  if (M) {
    rr_fill(x, ty, tw, th, th / 2, track, g_theme->bg);
    kx = *on ? x + tw - th + 2 : x + 2;
    rr_fill(kx, ty + 2, th - 4, th - 4, (th - 4) / 2, 0xFFFFFF, track);
  } else {
    wg_bevel(x, ty, tw, th, 0);
    gui_fill(x + 2, ty + 2, tw - 4, th - 4, *on ? g_theme->accent : g_theme->field_bg);
    kx = *on ? x + tw / 2 : x + 2;
    wg_bevel(kx, ty + 2, tw / 2 - 2, th - 4, 1);
  }
  if (label)
    gui_text(x + tw + 10, y, label, -1, g_theme->text, g_theme->bg, GUI_TEXT_NOBG);
  if (g_focus == id && label)
    gui_fill(x + tw + 10, y + gui_char_h(), wg_text_w(label), 2, g_theme->accent);
  if (clicked) {
    *on = !*on;
    return 1;
  }
  return 0;
}

/* Controle deslizante horizontal (altura 22). Devolve 1 se *value mudou. */
__export int wg_slider(const void* id, int x, int y, int w, int* value, int vmin, int vmax) {
  int h = 22, k = 18, old = *value, pos, cy = y + h / 2;
  int act;
  unsigned track = mix(g_theme->bg, g_theme->border, 200);
  if (vmax <= vmin) vmax = vmin + 1;
  if (inside_now(x, y, w, h) && g_down && !g_active) g_active = id;
  if (g_active == id) {
    if (g_down) {
      int rel = g_mx - x - k / 2;
      int range = w - k;
      int v = range > 0 ? vmin + (rel * (vmax - vmin) + range / 2) / range : vmin;
      if (v < vmin) v = vmin;
      if (v > vmax) v = vmax;
      *value = v;
    } else {
      g_active = 0;
    }
  }
  if (*value < vmin) *value = vmin;
  if (*value > vmax) *value = vmax;
  pos = (w - k) * (*value - vmin) / (vmax - vmin);
  act = g_active == id || g_focus == id;
  if (M) {
    unsigned halo = mix(g_theme->bg, g_theme->accent, 60);
    rr_fill(x, cy - 2, w, 4, 2, track, g_theme->bg);
    if (pos > 0) rr_fill(x, cy - 2, pos + k / 2, 4, 2, g_theme->accent, g_theme->bg);
    if (act) rr_fill(x + pos - 3, cy - k / 2 - 3, k + 6, k + 6, (k + 6) / 2, halo, g_theme->bg);
    rr_fill(x + pos, cy - k / 2, k, k, k / 2, g_theme->accent, act ? halo : g_theme->bg);
    rr_fill(x + pos + 5, cy - k / 2 + 5, k - 10, k - 10, (k - 10) / 2, 0xFFFFFF,
            g_theme->accent);
  } else {
    gui_fill(x, cy - 2, w, 4, g_theme->face_lo);
    gui_fill(x, cy - 2, pos + k / 2, 4, g_theme->accent);
    wg_bevel(x + pos, y + 2, k / 2 + 2, h - 4, 1);
  }
  return *value != old;
}

/* Etiqueta em pilula (contagem, estado). rgb 0 = cor de destaque.
 * Devolve a largura usada. */
__export int wg_badge(int x, int y, const char* text, unsigned rgb) {
  int w = wg_text_w(text) + 16, h = gui_char_h() + 4;
  if (!rgb) rgb = g_theme->accent;
  if (M) rr_fill(x, y, w, h, h / 2, rgb, g_theme->bg);
  else gui_fill(x, y, w, h, rgb);
  gui_text(x + 8, y + 2, text, -1, 0xFFFFFF, rgb, GUI_TEXT_NOBG);
  return w;
}

/* --------------------------------------------------------- icones do tema --- */
/* \SODOS\ICONS\<tema>\NOME.ICN ("ICN1", u16 w, u16 h, RGBA). Cache por nome,
 * com uma copia reduzida no ultimo tamanho pedido; limpo na troca de tema. */
#define WI_MAX 48
static struct {
  char name[12];
  unsigned char* px;   /* RGBA original */
  int w, h;
  unsigned char* sc;   /* RGBA reduzida */
  int ss, sw, sh;
  int miss;
} g_icn[WI_MAX];
static int g_icn_n;
static unsigned g_icn_gen = 0xFFFFFFFFu;

static void icn_reset(void) {
  int i;
  for (i = 0; i < g_icn_n; i++) {
    free(g_icn[i].px);
    free(g_icn[i].sc);
  }
  g_icn_n = 0;
}

static int icn_find(const char* name) {
  char path[64];
  FILE* f;
  unsigned char hd[8];
  int i, w, h;
  size_t n;
  theme_sync();
  if (g_icn_gen != g_sys_gen) {
    icn_reset();
    g_icn_gen = g_sys_gen;
  }
  if (!g_kt.icons || !g_kt.dir[0] || !name || !name[0]) return -1;
  for (i = 0; i < g_icn_n; i++)
    if (strcmp(g_icn[i].name, name) == 0) return g_icn[i].miss ? -1 : i;
  if (g_icn_n >= WI_MAX) return -1;
  i = g_icn_n++;
  memset(&g_icn[i], 0, sizeof(g_icn[i]));
  strncpy(g_icn[i].name, name, sizeof(g_icn[i].name) - 1);
  g_icn[i].miss = 1;
  snprintf(path, sizeof(path), "%s%s.ICN", g_kt.dir, name);
  f = fopen(path, "rb");
  if (!f) return -1;
  if (fread(hd, 1, 8, f) != 8 || memcmp(hd, "ICN1", 4) != 0) {
    fclose(f);
    return -1;
  }
  w = hd[4] | (hd[5] << 8);
  h = hd[6] | (hd[7] << 8);
  if (w < 1 || h < 1 || w > 128 || h > 128) {
    fclose(f);
    return -1;
  }
  n = (size_t)w * (size_t)h * 4;
  g_icn[i].px = (unsigned char*)malloc(n);
  if (!g_icn[i].px || fread(g_icn[i].px, 1, n, f) != n) {
    free(g_icn[i].px);
    g_icn[i].px = 0;
    fclose(f);
    return -1;
  }
  fclose(f);
  g_icn[i].w = w;
  g_icn[i].h = h;
  g_icn[i].miss = 0;
  return i;
}

/* Desenha o icone NOME do tema centrado no quadrado (x,y,s). Devolve 0 se o
 * tema nao tem esse icone (o app desenha o seu). */
__export int wg_icon(const char* name, int x, int y, int s) {
  int i = icn_find(name), iw, ih, xx, yy;
  if (i < 0 || s < 1) return 0;
  if (!g_icn[i].sc || g_icn[i].ss != s) {
    int w = g_icn[i].w, h = g_icn[i].h;
    free(g_icn[i].sc);
    g_icn[i].sc = 0;
    if (w >= h) {
      iw = s;
      ih = h * s / w;
    } else {
      ih = s;
      iw = w * s / h;
    }
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    g_icn[i].sc = (unsigned char*)malloc((size_t)iw * (size_t)ih * 4);
    if (!g_icn[i].sc) return 0;
    for (yy = 0; yy < ih; yy++) {
      int y0 = yy * h / ih, y1 = (yy + 1) * h / ih;
      if (y1 <= y0) y1 = y0 + 1;
      for (xx = 0; xx < iw; xx++) {
        int x0 = xx * w / iw, x1 = (xx + 1) * w / iw, sx, sy;
        unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
        unsigned char* o = g_icn[i].sc + (yy * iw + xx) * 4;
        if (x1 <= x0) x1 = x0 + 1;
        for (sy = y0; sy < y1; sy++)
          for (sx = x0; sx < x1; sx++) {
            const unsigned char* p = g_icn[i].px + (sy * w + sx) * 4;
            r += p[0] * p[3];
            g += p[1] * p[3];
            b += p[2] * p[3];
            a += p[3];
            n++;
          }
        o[0] = (unsigned char)(a ? r / a : 0);
        o[1] = (unsigned char)(a ? g / a : 0);
        o[2] = (unsigned char)(a ? b / a : 0);
        o[3] = (unsigned char)(a / n);
      }
    }
    g_icn[i].ss = s;
    g_icn[i].sw = iw;
    g_icn[i].sh = ih;
  }
  iw = g_icn[i].sw;
  ih = g_icn[i].sh;
  gui_pixels(x + (s - iw) / 2, y + (s - ih) / 2, iw, ih, g_icn[i].sc, iw * 4);
  return 1;
}

/* Bloco com icone em cima e rotulo embaixo (lancadores, painel de controle).
 * Sem o icone no tema, um quadrado vazio. Devolve 1 no clique. */
__export int wg_icon_button(const void* id, int x, int y, int w, int h, const char* icon,
                            const char* label) {
  int clicked = click(id, x, y, w, h, 1);
  int hov = hovered(id, x, y, w, h);
  int s = h - gui_char_h() - 18;
  unsigned face = g_theme->bg;
  if (s > w - 16) s = w - 16;
  if (pressed(id)) face = mix(g_theme->bg, g_theme->accent, 60);
  else if (hov) face = mix(g_theme->bg, g_theme->accent, 28);
  if (face != g_theme->bg) {
    if (M) rr_fill(x, y, w, h, RAD + 2, face, g_theme->bg);
    else gui_fill(x, y, w, h, face);
  }
  if (!wg_icon(icon, x + (w - s) / 2, y + 6, s)) {
    if (M) rr_box(x + (w - s) / 2, y + 6, s, s, s / 4, g_theme->border, g_theme->panel, face);
    else wg_bevel(x + (w - s) / 2, y + 6, s, s, 1);
  }
  if (label) {
    int tw = wg_text_w(label);
    int lx = x + (w - tw) / 2;
    if (lx < x + 2) lx = x + 2;
    gui_text_fit(lx, y + h - gui_char_h() - 6, label, w - 4, g_theme->text, face,
                 GUI_TEXT_NOBG);
  }
  if (g_focus == id) gui_fill(x + 8, y + h - 2, w - 16, 2, g_theme->accent);
  return clicked;
}
