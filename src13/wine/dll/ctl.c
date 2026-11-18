/*
 * ctl.c - Controles padrao do Windows: Button, Static, Edit, ListBox,
 * ComboBox, msctls_progress32, SysLink e um generico para as classes do
 * comctl32 que ainda nao existem. Visual do Windows 10 (claro e chapado).
 */
#include <so.h>
#include <string.h>
#include "gui_int.h"

#define C_FACE 0xF0F0F0
#define C_TEXT 0x000000
#define C_GRAY 0x838383
#define C_BORDER 0x7A7A7A
#define C_ACCENT 0xD77800   /* azul do Windows (BGR) */
#define C_BTN 0xE1E1E1
#define C_BTN_BORDER 0xADADAD
#define C_BTN_DOWN 0xF7E4CC

static void notify(struct wwin* w, int code) {
  if (w->parent) wsend(w->parent, WM_COMMAND, ((uintptr_t)(code & 0xFFFF) << 16) | (w->id & 0xFFFF), (intptr_t)w);
}

static int has_focus(struct wwin* w) {
  struct wwin* t = win_top(w);
  return t && t->focus == w;
}

/* Cores pedidas a mae (WM_CTLCOLOR*). *bg_null = 1: fundo transparente. */
static void ctl_colors(struct wwin* w, DWORD msg, DWORD def_bg, DWORD* bg, DWORD* fg, int* bg_null) {
  struct wdc* d = &w->dc;
  intptr_t br;
  d->text_color = w->enabled ? C_TEXT : C_GRAY;
  d->bk_color = def_bg;
  *bg = def_bg;
  *bg_null = 0;
  br = w->parent ? wsend(w->parent, msg, (uintptr_t)d, (intptr_t)w) : 0;
  if (br) {
    int null;
    DWORD c = brush_color((HANDLE)br, &null);
    if (null) *bg_null = 1;
    else *bg = c;
  }
  *fg = w->enabled ? d->text_color : C_GRAY;
}

static void ctl_text(struct wwin* w, int l, int t, int r, int b, DWORD fmt, DWORD color) {
  struct wdc* d = &w->dc;
  struct surf* s = dc_surf(d);
  struct rect rr = {l + d->ox, t + d->oy, r + d->ox, b + d->oy};
  if (!s || !w->text) return;
  text_box(s, &d->clip, &rr, w->text, -1, color, w->font && w->font->bold, fmt);
}

/* ---------- Button ---------- */
static void draw_check_mark(struct wdc* d, int x, int y, DWORD c) {
  int i;
  for (i = 0; i < 3; i++) dc_line(d, x + 2, y + 6 + i - 1, x + 5, y + 9 + i - 1, c);
  for (i = 0; i < 3; i++) dc_line(d, x + 5, y + 9 + i - 1, x + 10, y + 3 + i - 1, c);
}
static void draw_circle(struct wdc* d, int cx, int cy, int r, DWORD c) {
  int dy;
  for (dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
    dc_fill(d, cx - dx, cy + dy, cx + dx + 1, cy + dy + 1, c);
  }
}

static void button_click(struct wwin* w) {
  int type = (int)(w->style & 0xF);
  if (type == 3) w->check = !w->check;                          /* AUTOCHECKBOX */
  else if (type == 6) w->check = (w->check + 1) % 3;             /* AUTO3STATE */
  else if (type == 9 && w->parent) {                             /* AUTORADIOBUTTON: o grupo */
    struct wwin* c;
    struct wwin* start = w->parent->child;
    for (c = w->parent->child; c && c != w; c = c->sibling)
      if (c->style & WS_GROUP) start = c;
    for (c = start; c; c = c->sibling) {
      if (c != start && (c->style & WS_GROUP)) break;
      if (c->cls->kind == CK_BUTTON && (c->style & 0xF) == 9 && c != w && c->check) {
        c->check = 0;
        win_invalidate(c);
      }
    }
    w->check = 1;
  }
  win_invalidate(w);
  notify(w, BN_CLICKED);
}

static void button_paint(struct wwin* w) {
  struct wdc* d = &w->dc;
  int type = (int)(w->style & 0xF);
  DWORD bg, fg;
  int bg_null, th = text_height();
  dc_prepare(d, w);
  if (type == 0xB) { /* BS_OWNERDRAW */
    struct {
      uint32_t type, id, item, action, state, pad;
      HANDLE hwnd, hdc;
      struct rect rc;
      uintptr_t data;
    } di;
    memset(&di, 0, sizeof(di));
    di.type = 4; /* ODT_BUTTON */
    di.id = (uint32_t)w->id;
    di.action = 1;
    di.state = (w->pressed ? 1 : 0) | (has_focus(w) ? 0x10 : 0) | (w->enabled ? 0 : 4);
    di.hwnd = w;
    di.hdc = d;
    di.rc.right = w->w;
    di.rc.bottom = w->h;
    if (w->parent) wsend(w->parent, WM_DRAWITEM, w->id, (intptr_t)&di);
    return;
  }
  if (type == 0 || type == 1 || type == 0xE || type == 0xC || type == 0xD) {
    int def = type == 1 || type == 0xD || has_focus(w);
    dc_fill(d, 0, 0, w->w, w->h, w->pressed ? C_BTN_DOWN : C_BTN);
    dc_frame(d, 0, 0, w->w, w->h, def && w->enabled ? C_ACCENT : C_BTN_BORDER);
    if (def && w->enabled) dc_frame(d, 1, 1, w->w - 1, w->h - 1, C_ACCENT);
    ctl_text(w, 4, 0, w->w - 4, w->h, DT_SINGLELINE | DT_VCENTER | DT_CENTER, w->enabled ? C_TEXT : C_GRAY);
    return;
  }
  ctl_colors(w, WM_CTLCOLORBTN, C_FACE, &bg, &fg, &bg_null);
  if (type == 7) { /* GROUPBOX */
    int ty = th / 2, tw = w->text ? text_width(w->text, -1) : 0;
    dc_frame(d, 0, ty, w->w, w->h, 0xDCDCDC);
    if (tw) {
      if (!bg_null) dc_fill(d, 6, 0, 6 + tw + 6, th, bg);
      ctl_text(w, 9, 0, 9 + tw + 4, th, DT_SINGLELINE, fg);
    }
    return;
  }
  if (!bg_null) dc_fill(d, 0, 0, w->w, w->h, bg);
  {
    int box = 13, by = (w->h - box) / 2, tx = box + 5;
    DWORD bc = w->enabled ? 0x333333 : 0xBCBCBC;
    if (type == 4 || type == 9) { /* RADIO */
      draw_circle(d, box / 2, by + box / 2, 6, bc);
      draw_circle(d, box / 2, by + box / 2, 5, 0xFFFFFF);
      if (w->check) draw_circle(d, box / 2, by + box / 2, 3, bc);
    } else {
      dc_fill(d, 0, by, box, by + box, 0xFFFFFF);
      dc_frame(d, 0, by, box, by + box, bc);
      if (w->check == 1) draw_check_mark(d, 0, by, bc);
      else if (w->check == 2) dc_fill(d, 3, by + 3, box - 3, by + box - 3, bc);
    }
    ctl_text(w, tx, 0, w->w, w->h, DT_SINGLELINE | DT_VCENTER | ((w->style & 0x2000) ? DT_WORDBREAK : 0), fg);
    if (has_focus(w) && w->text) {
      int tw = text_width(w->text, -1);
      if (tw > w->w - tx) tw = w->w - tx;
      dc_frame(d, tx - 2, (w->h - th) / 2 - 1, tx + tw + 2, (w->h + th) / 2 + 1, 0xA0A0A0);
    }
  }
}

static intptr_t button_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  int type = (int)(w->style & 0xF);
  switch (m) {
    case WM_PAINT: button_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
      w->pressed = 1;
      if (type != 7) win_set_focus(w);
      win_invalidate(w);
      return 0;
    case WM_LBUTTONUP: {
      int x = (int)(int16_t)(lp & 0xFFFF), y = (int)(int16_t)((lp >> 16) & 0xFFFF);
      int was = w->pressed;
      w->pressed = 0;
      win_invalidate(w);
      if (was && x >= 0 && y >= 0 && x < w->w && y < w->h) button_click(w);
      return 0;
    }
    case WM_CHAR:
      if (wp_ == ' ') button_click(w);
      return 0;
    case WM_GETDLGCODE:
      if (type == 1) return 0x2010;          /* DLGC_BUTTON | DLGC_DEFPUSHBUTTON */
      if (type == 0) return 0x2020;          /* DLGC_UNDEFPUSHBUTTON */
      if (type == 4 || type == 9) return 0x2040; /* DLGC_RADIOBUTTON */
      if (type == 7) return 0x0100;          /* DLGC_STATIC */
      return 0x2000;
    case 0xF0: return w->check;              /* BM_GETCHECK */
    case 0xF1:                               /* BM_SETCHECK */
      w->check = (int)wp_;
      win_invalidate(w);
      return 0;
    case 0xF2: return (w->check ? 3 : 0) | (w->pressed ? 4 : 0) | (has_focus(w) ? 8 : 0); /* BM_GETSTATE */
    case 0xF3: w->pressed = wp_ != 0; win_invalidate(w); return 0;                        /* BM_SETSTATE */
    case 0xF4:                               /* BM_SETSTYLE */
      w->style = (w->style & ~0xFFFFu) | (DWORD)(wp_ & 0xFFFF);
      if (lp) win_invalidate(w);
      return 0;
    case 0xF5: button_click(w); return 0;    /* BM_CLICK */
    case 0xF6: return 0;                     /* BM_GETIMAGE */
    case 0xF7: return 0;                     /* BM_SETIMAGE */
  }
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ---------- Static ---------- */
static void static_paint(struct wwin* w) {
  struct wdc* d = &w->dc;
  int type = (int)(w->style & 0x1F);
  DWORD bg, fg, fmt;
  int bg_null;
  dc_prepare(d, w);
  if (type == 0xD) { /* SS_OWNERDRAW */
    struct {
      uint32_t type, id, item, action, state, pad;
      HANDLE hwnd, hdc;
      struct rect rc;
      uintptr_t data;
    } di;
    memset(&di, 0, sizeof(di));
    di.type = 5; /* ODT_STATIC */
    di.id = (uint32_t)w->id;
    di.action = 1;
    di.hwnd = w;
    di.hdc = d;
    di.rc.right = w->w;
    di.rc.bottom = w->h;
    if (w->parent) wsend(w->parent, WM_DRAWITEM, w->id, (intptr_t)&di);
    return;
  }
  ctl_colors(w, WM_CTLCOLORSTATIC, C_FACE, &bg, &fg, &bg_null);
  switch (type) {
    case 4: dc_fill(d, 0, 0, w->w, w->h, 0x000000); return;
    case 5: dc_fill(d, 0, 0, w->w, w->h, 0x808080); return;
    case 6: dc_fill(d, 0, 0, w->w, w->h, 0xFFFFFF); return;
    case 7: dc_frame(d, 0, 0, w->w, w->h, 0x000000); return;
    case 8: dc_frame(d, 0, 0, w->w, w->h, 0x808080); return;
    case 9: dc_frame(d, 0, 0, w->w, w->h, 0xFFFFFF); return;
    case 0x10: dc_fill(d, 0, 0, w->w, 1, 0xA0A0A0); dc_fill(d, 0, 1, w->w, 2, 0xFFFFFF); return;
    case 0x11: dc_fill(d, 0, 0, 1, w->h, 0xA0A0A0); dc_fill(d, 1, 0, 2, w->h, 0xFFFFFF); return;
    case 0x12: dc_frame(d, 0, 0, w->w, w->h, 0xA0A0A0); return;
  }
  if (!bg_null) dc_fill(d, 0, 0, w->w, w->h, bg);
  if (type == 0xE || type == 3) { /* SS_BITMAP / SS_ICON */
    if (w->image) {
      struct surf* s = dc_surf(d);
      int iw = w->image->s.w, ih = w->image->s.h, x = 0, y = 0;
      if (w->style & 0x200) { /* SS_CENTERIMAGE */
        x = (w->w - iw) / 2;
        y = (w->h - ih) / 2;
      }
      if (s) surf_blit(s, d->ox + x, d->oy + y, iw, ih, &w->image->s, 0, 0, iw, ih, &d->clip);
    }
    return;
  }
  fmt = (w->style & 0x80) ? DT_NOPREFIX : 0; /* SS_NOPREFIX */
  if (type == 1) fmt |= DT_CENTER;
  else if (type == 2) fmt |= DT_RIGHT;
  if (type == 0xB || type == 0xC) fmt |= DT_SINGLELINE;
  else fmt |= DT_WORDBREAK;
  if (w->style & 0x200) fmt |= DT_VCENTER | DT_SINGLELINE; /* SS_CENTERIMAGE em texto: centro vertical */
  ctl_text(w, 0, 0, w->w, w->h, fmt, fg);
  if (w->style & 0x1000) dc_frame(d, 0, 0, w->w, w->h, 0xA0A0A0); /* SS_SUNKEN */
}

static intptr_t static_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  switch (m) {
    case WM_PAINT: static_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONUP:
      if (w->style & 0x100) notify(w, 0); /* SS_NOTIFY: STN_CLICKED */
      return 0;
    case WM_GETDLGCODE: return 0x100; /* DLGC_STATIC */
    case 0x170: /* STM_SETICON */
    case 0x172: { /* STM_SETIMAGE */
      struct wgdi* old = w->image;
      struct wgdi* img = gdi_of((HANDLE)(m == 0x170 ? wp_ : lp));
      w->image = img && img->kind == G_BITMAP ? img : 0;
      win_invalidate(w);
      return (intptr_t)old;
    }
    case 0x171:
    case 0x173: return (intptr_t)w->image;
  }
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ---------- Edit ---------- */
static int etext_len(struct wwin* w) { return w->text ? (int)wstrlen16(w->text) : 0; }

static void edit_set(struct wwin* w, const wchar16* t, int notify_change) {
  wchar16* old = w->text;
  size_t n = t ? wstrlen16(t) : 0;
  w->text = (wchar16*)k_malloc((unsigned)(n + 1) * 2u);
  if (w->text) {
    if (n) memcpy(w->text, t, n * 2);
    w->text[n] = 0;
  }
  if (old) k_free(old);
  w->sel_a = w->sel_b = (int)n;
  if (!(w->style & 4)) w->scroll = 0;
  win_invalidate(w);
  if (notify_change) notify(w, EN_CHANGE);
}

/* Troca a selecao por s (n caracteres). */
static void edit_replace(struct wwin* w, const wchar16* s, int n) {
  int len = etext_len(w), a = w->sel_a < w->sel_b ? w->sel_a : w->sel_b, b = w->sel_a < w->sel_b ? w->sel_b : w->sel_a;
  wchar16* nt;
  if (a < 0) a = 0;
  if (b > len) b = len;
  if (w->limit && len - (b - a) + n > w->limit) n = w->limit - (len - (b - a));
  if (n < 0) n = 0;
  nt = (wchar16*)k_malloc((unsigned)(len - (b - a) + n + 1) * 2u);
  if (!nt) return;
  if (a) memcpy(nt, w->text, (size_t)a * 2);
  if (n) memcpy(nt + a, s, (size_t)n * 2);
  if (len > b) memcpy(nt + a + n, w->text + b, (size_t)(len - b) * 2);
  nt[len - (b - a) + n] = 0;
  if (w->text) k_free(w->text);
  w->text = nt;
  w->sel_a = w->sel_b = a + n;
  win_invalidate(w);
  notify(w, EN_CHANGE);
}

static void edit_paint(struct wwin* w) {
  struct wdc* d = &w->dc;
  struct surf* s;
  DWORD bg, fg;
  int bg_null, multi = (w->style & 4) != 0, th = text_height();
  int ro = (w->style & 0x800) != 0;
  int border = (w->exstyle & 0x200) || (w->style & WS_BORDER); /* WS_EX_CLIENTEDGE */
  int pad = border ? 3 : 1;
  wchar16 stars[512];
  const wchar16* txt = w->text;
  int len = etext_len(w);
  dc_prepare(d, w);
  s = dc_surf(d);
  ctl_colors(w, ro || !w->enabled ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT, ro || !w->enabled ? C_FACE : 0xFFFFFF, &bg,
             &fg, &bg_null);
  if (!bg_null) dc_fill(d, 0, 0, w->w, w->h, bg);
  if (border) dc_frame(d, 0, 0, w->w, w->h, has_focus(w) ? C_ACCENT : C_BORDER);
  if (!s) return;
  if (w->style & 0x20 && txt) { /* ES_PASSWORD */
    int i;
    if (len > 511) len = 511;
    for (i = 0; i < len; i++) stars[i] = '*';
    stars[len] = 0;
    txt = stars;
  }
  if (multi) {
    struct rect rr = {d->ox + pad, d->oy + pad - w->scroll * th, d->ox + w->w - pad, d->oy + w->h + 100000};
    struct rect clip = {d->ox + pad, d->oy + pad, d->ox + w->w - pad, d->oy + w->h - pad};
    if (clip.right > d->clip.right) clip.right = d->clip.right;
    if (clip.bottom > d->clip.bottom) clip.bottom = d->clip.bottom;
    if (txt) text_box(s, &clip, &rr, txt, len, fg, 0, DT_WORDBREAK | DT_NOPREFIX);
    return;
  }
  {
    int caret_x = text_width(txt, w->sel_b), avail = w->w - 2 * pad - 2, x0, y0 = (w->h - th) / 2;
    struct rect clip = {d->ox + pad, d->oy, d->ox + w->w - pad, d->oy + w->h};
    if (caret_x - w->scroll > avail) w->scroll = caret_x - avail;
    if (caret_x < w->scroll) w->scroll = caret_x;
    if (w->scroll < 0) w->scroll = 0;
    x0 = pad + 1 - w->scroll;
    if (w->style & 1) x0 = (w->w - text_width(txt, len)) / 2;        /* ES_CENTER */
    else if (w->style & 2) x0 = w->w - pad - 1 - text_width(txt, len); /* ES_RIGHT */
    if (clip.right > d->clip.right) clip.right = d->clip.right;
    if (w->sel_a != w->sel_b && has_focus(w)) {
      int a = w->sel_a < w->sel_b ? w->sel_a : w->sel_b, b = w->sel_a < w->sel_b ? w->sel_b : w->sel_a;
      dc_fill(d, x0 + text_width(txt, a), y0, x0 + text_width(txt, b), y0 + th, 0xFFD9B3);
    }
    if (txt) text_draw(s, &clip, d->ox + x0, d->oy + y0, txt, len, fg, 0);
    if (has_focus(w) && !ro) dc_fill(d, x0 + caret_x, y0, x0 + caret_x + 1, y0 + th, 0x000000);
  }
}

static int edit_hit(struct wwin* w, int x) {
  int len = etext_len(w), i, pad = 3, x0 = pad + 1 - w->scroll;
  for (i = 0; i < len; i++) {
    int cw = text_width(w->text + i, 1);
    if (x < x0 + cw / 2) return i;
    x0 += cw;
  }
  return len;
}

static intptr_t edit_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  int len = etext_len(w), ro = (w->style & 0x800) != 0, multi = (w->style & 4) != 0;
  switch (m) {
    case WM_CREATE:
      if (w->text) w->sel_a = w->sel_b = 0;
      return 0;
    case WM_PAINT: edit_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETTEXT: edit_set(w, (const wchar16*)lp, 0); return 1;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: win_invalidate(w); return 0;
    case WM_LBUTTONDOWN:
      win_set_focus(w);
      if (!multi) w->sel_a = w->sel_b = edit_hit(w, (int)(int16_t)(lp & 0xFFFF));
      win_invalidate(w);
      return 0;
    case WM_MOUSEWHEEL:
      if (multi) {
        int lines = (int)(int16_t)(wp_ >> 16) / 120 * -3;
        w->scroll += lines;
        if (w->scroll < 0) w->scroll = 0;
        win_invalidate(w);
      }
      return 0;
    case WM_VSCROLL:
      if (multi) {
        int code = (int)(wp_ & 0xFFFF);
        if (code == 0) w->scroll--;      /* SB_LINEUP */
        else if (code == 1) w->scroll++; /* SB_LINEDOWN */
        else if (code == 2) w->scroll -= 5;
        else if (code == 3) w->scroll += 5;
        if (w->scroll < 0) w->scroll = 0;
        win_invalidate(w);
      }
      return 0;
    case WM_GETDLGCODE: return multi ? 0x8D : 0x89; /* WANTCHARS|HASSETSEL|WANTARROWS (+WANTALLKEYS) */
    case WM_KEYDOWN:
      switch (wp_) {
        case 0x25: if (w->sel_b > 0) w->sel_b--; w->sel_a = w->sel_b; break; /* esquerda */
        case 0x27: if (w->sel_b < len) w->sel_b++; w->sel_a = w->sel_b; break;
        case 0x24: w->sel_a = w->sel_b = 0; break;   /* Home */
        case 0x23: w->sel_a = w->sel_b = len; break; /* End */
        case 0x26: if (multi && w->scroll > 0) w->scroll--; break;
        case 0x28: if (multi) w->scroll++; break;
        case 0x21: if (multi) { w->scroll -= 5; if (w->scroll < 0) w->scroll = 0; } break;
        case 0x22: if (multi) w->scroll += 5; break;
        case 0x2E: /* Delete */
          if (!ro) {
            if (w->sel_a == w->sel_b && w->sel_b < len) w->sel_b++;
            edit_replace(w, 0, 0);
          }
          break;
        default: return 0;
      }
      win_invalidate(w);
      return 0;
    case WM_CHAR: {
      wchar16 c = (wchar16)wp_;
      if (c == 1) { /* Ctrl+A */
        w->sel_a = 0;
        w->sel_b = len;
        win_invalidate(w);
        return 0;
      }
      if (ro) return 0;
      if (c == 8) {
        if (w->sel_a == w->sel_b && w->sel_a > 0) w->sel_a--;
        edit_replace(w, 0, 0);
        return 0;
      }
      if (c == '\r') {
        if (multi && (w->style & 0x1000)) { /* ES_WANTRETURN */
          static const wchar16 nl[2] = {'\r', '\n'};
          edit_replace(w, nl, 2);
        }
        return 0;
      }
      if (c < 32 || c == 127) return 0;
      if ((w->style & 0x2000) && (c < '0' || c > '9')) return 0; /* ES_NUMBER */
      edit_replace(w, &c, 1);
      return 0;
    }
    case 0xB0: /* EM_GETSEL */
      if (wp_) *(DWORD*)wp_ = (DWORD)(w->sel_a < w->sel_b ? w->sel_a : w->sel_b);
      if (lp) *(DWORD*)lp = (DWORD)(w->sel_a < w->sel_b ? w->sel_b : w->sel_a);
      return ((intptr_t)w->sel_b << 16) | (w->sel_a & 0xFFFF);
    case 0xB1: { /* EM_SETSEL */
      int a = (int)(intptr_t)wp_, b = (int)lp;
      if (a < 0) {
        w->sel_a = w->sel_b;
      } else {
        if (b < 0 || b > len) b = len;
        if (a > len) a = len;
        w->sel_a = a;
        w->sel_b = b;
      }
      win_invalidate(w);
      return 0;
    }
    case 0xC2: /* EM_REPLACESEL */
      edit_replace(w, (const wchar16*)lp, lp ? (int)wstrlen16((const wchar16*)lp) : 0);
      return 0;
    case 0xC5: /* EM_LIMITTEXT */
    case 0x435: w->limit = (int)wp_ == 0 ? 0 : (int)(m == 0x435 ? lp : (intptr_t)wp_); return 0; /* EM_EXLIMITTEXT */
    case 0xD5: return w->limit ? w->limit : 30000; /* EM_GETLIMITTEXT */
    case 0xCF: /* EM_SETREADONLY */
      if (wp_) w->style |= 0x800;
      else w->style &= ~0x800u;
      win_invalidate(w);
      return 1;
    case 0xB8: return 0;                                         /* EM_GETMODIFY */
    case 0xB9: return 0;                                         /* EM_SETMODIFY */
    case 0xBA: {                                                 /* EM_GETLINECOUNT */
      int i, n = 1;
      for (i = 0; i < len; i++)
        if (w->text[i] == '\n') n++;
      return n;
    }
    case 0xB6: w->scroll += (int)lp; if (w->scroll < 0) w->scroll = 0; win_invalidate(w); return 1; /* EM_LINESCROLL */
    case 0xB7: return 0;                                         /* EM_SCROLLCARET */
    case 0xCE: return w->scroll;                                 /* EM_GETFIRSTVISIBLELINE */
    case 0x443: case 0x445: case 0x448: case 0x45B: case 0x1501: return 0; /* RichEdit/cue: nada */
    case 0x449: { /* EM_STREAMIN: le o texto (RTF vira texto puro, grosseiro) */
      struct {
        uintptr_t cookie;
        DWORD err;
        long (WINAPI* cb)(uintptr_t, uint8_t*, long, long*);
      }* es = (void*)lp;
      uint8_t buf[4096];
      char* all = (char*)k_malloc(65536);
      long got = 0;
      int n = 0, depth = 0, i, o = 0;
      if (!all || !es || !es->cb) return 0;
      while (n < 65000 && es->cb(es->cookie, buf, (long)sizeof(buf), &got) == 0 && got > 0) {
        if (n + got > 65000) got = 65000 - n;
        memcpy(all + n, buf, (size_t)got);
        n += (int)got;
      }
      all[n] = 0;
      if (wp_ & 2) { /* SF_RTF: tira grupos de controle e comandos */
        for (i = 0; i < n; i++) {
          char c = all[i];
          if (c == '{') depth++;
          else if (c == '}') depth--;
          else if (c == '\\') {
            if (all[i + 1] == 'p' && all[i + 2] == 'a' && all[i + 3] == 'r' && (all[i + 4] < 'a' || all[i + 4] > 'z')) {
              all[o++] = '\n';
            }
            if (all[i + 1] == '\'' && i + 3 < n) {
              int hi = all[i + 2] <= '9' ? all[i + 2] - '0' : (all[i + 2] | 32) - 'a' + 10;
              int lo = all[i + 3] <= '9' ? all[i + 3] - '0' : (all[i + 3] | 32) - 'a' + 10;
              all[o++] = (char)(hi * 16 + lo);
              i += 3;
              continue;
            }
            i++;
            while (i < n && ((all[i] >= 'a' && all[i] <= 'z') || (all[i] >= '0' && all[i] <= '9') || all[i] == '-')) i++;
            if (i < n && all[i] != ' ') i--;
          } else if (c != '\r' && c != '\n' && depth <= 1) {
            all[o++] = c;
          }
        }
        all[o] = 0;
      }
      {
        wchar16* t = (wchar16*)k_malloc((unsigned)(strlen(all) + 1) * 2u);
        int k;
        if (t) {
          for (k = 0; all[k]; k++) t[k] = (wchar16)(uint8_t)all[k];
          t[k] = 0;
          edit_set(w, t, 0);
          w->sel_a = w->sel_b = 0;
          k_free(t);
        }
      }
      k_free(all);
      return 0;
    }
  }
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ---------- itens (ListBox / ComboBox) ---------- */
static int items_insert(struct wwin* w, int at, const wchar16* s) {
  wchar16* d;
  size_t n = s ? wstrlen16(s) : 0;
  if (w->nitems == w->cap_items) {
    int nc = w->cap_items ? w->cap_items * 2 : 16;
    wchar16** ni = (wchar16**)k_realloc(w->items, (unsigned)(sizeof(wchar16*) * (size_t)nc));
    intptr_t* nd;
    if (!ni) return -1;
    w->items = ni;
    nd = (intptr_t*)k_realloc(w->item_data, (unsigned)(sizeof(intptr_t) * (size_t)nc));
    if (!nd) return -1;
    w->item_data = nd;
    w->cap_items = nc;
  }
  d = (wchar16*)k_malloc((unsigned)(n + 1) * 2u);
  if (!d) return -1;
  if (n) memcpy(d, s, n * 2);
  d[n] = 0;
  if (at < 0 || at > w->nitems) at = w->nitems;
  memmove(w->items + at + 1, w->items + at, sizeof(wchar16*) * (size_t)(w->nitems - at));
  memmove(w->item_data + at + 1, w->item_data + at, sizeof(intptr_t) * (size_t)(w->nitems - at));
  w->items[at] = d;
  w->item_data[at] = 0;
  w->nitems++;
  win_invalidate(w);
  return at;
}
static void items_delete(struct wwin* w, int i) {
  if (i < 0 || i >= w->nitems) return;
  k_free(w->items[i]);
  memmove(w->items + i, w->items + i + 1, sizeof(wchar16*) * (size_t)(w->nitems - i - 1));
  memmove(w->item_data + i, w->item_data + i + 1, sizeof(intptr_t) * (size_t)(w->nitems - i - 1));
  w->nitems--;
  if (w->cur >= w->nitems) w->cur = w->nitems - 1;
  win_invalidate(w);
}
static void items_clear(struct wwin* w) {
  while (w->nitems) items_delete(w, w->nitems - 1);
  w->cur = -1;
}
static int items_find(struct wwin* w, int start, const wchar16* s, int exact) {
  int i, n = s ? (int)wstrlen16(s) : 0;
  for (i = start + 1; i < w->nitems; i++) {
    const wchar16* t = w->items[i];
    int k, ok = 1;
    for (k = 0; k < n && ok; k++) {
      wchar16 a = t[k], b = s[k];
      if (a >= 'a' && a <= 'z') a = (wchar16)(a - 32);
      if (b >= 'a' && b <= 'z') b = (wchar16)(b - 32);
      if (a != b) ok = 0;
    }
    if (ok && (!exact || !t[n])) return i;
  }
  return -1;
}
static int item_text(struct wwin* w, int i, wchar16* buf) {
  int n;
  if (i < 0 || i >= w->nitems) return -1;
  n = (int)wstrlen16(w->items[i]);
  if (buf) memcpy(buf, w->items[i], (size_t)(n + 1) * 2);
  return n;
}

static void list_paint(struct wwin* w) {
  struct wdc* d = &w->dc;
  struct surf* s;
  int ih = text_height() + 2, i, y;
  DWORD bg, fg;
  int bg_null;
  dc_prepare(d, w);
  s = dc_surf(d);
  ctl_colors(w, WM_CTLCOLORLISTBOX, 0xFFFFFF, &bg, &fg, &bg_null);
  if (!bg_null) dc_fill(d, 0, 0, w->w, w->h, bg);
  dc_frame(d, 0, 0, w->w, w->h, C_BORDER);
  if (!s) return;
  for (i = w->scroll, y = 1; i < w->nitems && y < w->h; i++, y += ih) {
    struct rect clip = {d->ox + 1, d->oy + 1, d->ox + w->w - 1, d->oy + w->h - 1};
    if (i == w->cur) dc_fill(d, 1, y, w->w - 1, y + ih, C_ACCENT);
    if (clip.right > d->clip.right) clip.right = d->clip.right;
    if (clip.bottom > d->clip.bottom) clip.bottom = d->clip.bottom;
    text_draw(s, &clip, d->ox + 4, d->oy + y + 1, w->items[i], -1, i == w->cur ? 0xFFFFFF : fg, 0);
  }
}

static intptr_t list_common(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp, int combo, int* handled) {
  *handled = 1;
  /* CB_* = LB_* - 0x3D, em boa parte */
  switch (combo ? m : 0) {
    case 0x143: return items_insert(w, -1, (const wchar16*)lp);          /* CB_ADDSTRING */
    case 0x14A: return items_insert(w, (int)(intptr_t)wp_, (const wchar16*)lp); /* CB_INSERTSTRING */
    case 0x144: items_delete(w, (int)wp_); return w->nitems;           /* CB_DELETESTRING */
    case 0x146: return w->nitems;                                      /* CB_GETCOUNT */
    case 0x147: return w->cur;                                         /* CB_GETCURSEL */
    case 0x148: return item_text(w, (int)wp_, (wchar16*)lp);           /* CB_GETLBTEXT */
    case 0x149: return item_text(w, (int)wp_, 0);                      /* CB_GETLBTEXTLEN */
    case 0x14B: items_clear(w); return 0;                              /* CB_RESETCONTENT */
    case 0x14C: return items_find(w, (int)(intptr_t)wp_, (const wchar16*)lp, 0); /* CB_FINDSTRING */
    case 0x158: return items_find(w, (int)(intptr_t)wp_, (const wchar16*)lp, 1); /* CB_FINDSTRINGEXACT */
    case 0x14D: {                                                      /* CB_SELECTSTRING */
      int i = items_find(w, (int)(intptr_t)wp_, (const wchar16*)lp, 0);
      if (i >= 0) w->cur = i;
      win_invalidate(w);
      return i;
    }
    case 0x14E:                                                        /* CB_SETCURSEL */
      w->cur = (int)(intptr_t)wp_ < w->nitems ? (int)(intptr_t)wp_ : -1;
      win_invalidate(w);
      return w->cur;
    case 0x150: return ((int)wp_ >= 0 && (int)wp_ < w->nitems) ? w->item_data[wp_] : -1; /* CB_GETITEMDATA */
    case 0x151: if ((int)wp_ >= 0 && (int)wp_ < w->nitems) w->item_data[wp_] = lp; return 0;  /* CB_SETITEMDATA */
    case 0x14F: case 0x153: case 0x141: case 0x142: case 0x157: case 0x154: case 0x15B: return 0;
  }
  switch (combo ? 0 : m) {
    case 0x180: return items_insert(w, -1, (const wchar16*)lp);          /* LB_ADDSTRING */
    case 0x181: return items_insert(w, (int)(intptr_t)wp_, (const wchar16*)lp); /* LB_INSERTSTRING */
    case 0x182: items_delete(w, (int)wp_); return w->nitems;           /* LB_DELETESTRING */
    case 0x184: items_clear(w); return 0;                              /* LB_RESETCONTENT */
    case 0x185: if ((int)lp >= 0) w->cur = wp_ ? (int)lp : -1; win_invalidate(w); return 0; /* LB_SETSEL */
    case 0x186: w->cur = (int)(intptr_t)wp_ < w->nitems ? (int)(intptr_t)wp_ : -1; win_invalidate(w); return w->cur; /* LB_SETCURSEL */
    case 0x187: return (int)wp_ == w->cur;                             /* LB_GETSEL */
    case 0x188: return w->cur;                                         /* LB_GETCURSEL */
    case 0x189: return item_text(w, (int)wp_, (wchar16*)lp);           /* LB_GETTEXT */
    case 0x18A: return item_text(w, (int)wp_, 0);                      /* LB_GETTEXTLEN */
    case 0x18B: return w->nitems;                                      /* LB_GETCOUNT */
    case 0x18E: return w->scroll;                                      /* LB_GETTOPINDEX */
    case 0x197: w->scroll = (int)wp_; win_invalidate(w); return 0;     /* LB_SETTOPINDEX */
    case 0x18F: return items_find(w, (int)(intptr_t)wp_, (const wchar16*)lp, 0); /* LB_FINDSTRING */
    case 0x1A2: return items_find(w, (int)(intptr_t)wp_, (const wchar16*)lp, 1); /* LB_FINDSTRINGEXACT */
    case 0x199: return ((int)wp_ >= 0 && (int)wp_ < w->nitems) ? w->item_data[wp_] : -1; /* LB_GETITEMDATA */
    case 0x19A: if ((int)wp_ >= 0 && (int)wp_ < w->nitems) w->item_data[wp_] = lp; return 0;  /* LB_SETITEMDATA */
    case 0x1A1: return text_height() + 2;                              /* LB_GETITEMHEIGHT */
    case 0x190: return w->cur >= 0 ? 1 : 0;                            /* LB_GETSELCOUNT */
  }
  *handled = 0;
  return 0;
}

static intptr_t list_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  int handled;
  intptr_t r = list_common(w, m, wp_, lp, 0, &handled);
  if (handled) return r;
  switch (m) {
    case WM_PAINT: list_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
      int y = (int)(int16_t)((lp >> 16) & 0xFFFF), i = w->scroll + (y - 1) / (text_height() + 2);
      win_set_focus(w);
      if (i >= 0 && i < w->nitems && i != w->cur) {
        w->cur = i;
        win_invalidate(w);
        if (w->style & 1) notify(w, LBN_SELCHANGE); /* LBS_NOTIFY */
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK:
      if (w->style & 1) notify(w, 2); /* LBN_DBLCLK */
      return 0;
    case WM_MOUSEWHEEL:
      w->scroll -= (int)(int16_t)(wp_ >> 16) / 120;
      if (w->scroll < 0) w->scroll = 0;
      if (w->scroll > w->nitems - 1) w->scroll = w->nitems > 0 ? w->nitems - 1 : 0;
      win_invalidate(w);
      return 0;
    case WM_KEYDOWN:
      if ((wp_ == 0x26 && w->cur > 0) || (wp_ == 0x28 && w->cur < w->nitems - 1)) {
        w->cur += wp_ == 0x26 ? -1 : 1;
        win_invalidate(w);
        if (w->style & 1) notify(w, LBN_SELCHANGE);
      }
      return 0;
    case WM_GETDLGCODE: return 0x81; /* WANTARROWS | WANTCHARS */
  }
  return wDefWindowProcW(w, m, wp_, lp);
}

static void combo_paint(struct wwin* w) {
  struct wdc* d = &w->dc;
  int bh = text_height() + 8, ax;
  struct surf* s;
  const wchar16* t = w->cur >= 0 && w->cur < w->nitems ? w->items[w->cur] : w->text;
  dc_prepare(d, w);
  s = dc_surf(d);
  if (bh > w->h) bh = w->h;
  dc_fill(d, 0, 0, w->w, bh, w->enabled ? 0xFFFFFF : C_FACE);
  dc_frame(d, 0, 0, w->w, bh, has_focus(w) ? C_ACCENT : C_BORDER);
  ax = w->w - 14;
  dc_line(d, ax - 4, bh / 2 - 2, ax, bh / 2 + 2, 0x606060);
  dc_line(d, ax, bh / 2 + 2, ax + 4, bh / 2 - 2, 0x606060);
  if (s && t) {
    struct rect clip = {d->ox + 2, d->oy, d->ox + w->w - 20, d->oy + bh};
    if (clip.right > d->clip.right) clip.right = d->clip.right;
    text_draw(s, &clip, d->ox + 4, d->oy + (bh - text_height()) / 2, t, -1, w->enabled ? C_TEXT : C_GRAY, 0);
  }
}

static intptr_t combo_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  int handled;
  intptr_t r = list_common(w, m, wp_, lp, 1, &handled);
  if (handled) return r;
  switch (m) {
    case WM_PAINT: combo_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: /* sem lista suspensa: cada clique passa ao proximo item */
      win_set_focus(w);
      if (w->nitems) {
        w->cur = (w->cur + 1) % w->nitems;
        win_invalidate(w);
        notify(w, CBN_SELCHANGE);
      }
      return 0;
    case WM_KEYDOWN:
      if ((wp_ == 0x26 && w->cur > 0) || (wp_ == 0x28 && w->cur < w->nitems - 1)) {
        w->cur += wp_ == 0x26 ? -1 : 1;
        win_invalidate(w);
        notify(w, CBN_SELCHANGE);
      }
      return 0;
    case WM_GETTEXT:
      if (w->cur >= 0 && w->cur < w->nitems) {
        int n = (int)wstrlen16(w->items[w->cur]);
        if ((int)wp_ <= 0) return 0;
        if (n >= (int)wp_) n = (int)wp_ - 1;
        memcpy((wchar16*)lp, w->items[w->cur], (size_t)n * 2);
        ((wchar16*)lp)[n] = 0;
        return n;
      }
      break;
    case WM_GETTEXTLENGTH:
      if (w->cur >= 0 && w->cur < w->nitems) return (intptr_t)wstrlen16(w->items[w->cur]);
      break;
    case WM_GETDLGCODE: return 0x81;
  }
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ---------- Progress ---------- */
static intptr_t progress_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  int64_t old = w->ppos;
  switch (m) {
    case WM_PAINT: {
      struct wdc* d = &w->dc;
      int64_t span = w->pmax - w->pmin;
      int fill;
      dc_prepare(d, w);
      dc_fill(d, 0, 0, w->w, w->h, 0xE6E6E6);
      dc_frame(d, 0, 0, w->w, w->h, 0xBCBCBC);
      fill = span > 0 ? (int)((w->ppos - w->pmin) * (w->w - 2) / span) : 0;
      if (fill < 0) fill = 0;
      if (fill > w->w - 2) fill = w->w - 2;
      dc_fill(d, 1, 1, 1 + fill, w->h - 1, w->extra[WEXTRA - 1] ? *(DWORD*)(w->extra + WEXTRA - 8) : 0x25B006);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case 0x401: /* PBM_SETRANGE */
      w->pmin = lp & 0xFFFF;
      w->pmax = (lp >> 16) & 0xFFFF;
      win_invalidate(w);
      return 0;
    case 0x406: w->pmin = (int)wp_; w->pmax = (int)lp; win_invalidate(w); return 0; /* PBM_SETRANGE32 */
    case 0x407: return wp_ ? w->pmin : w->pmax;                                    /* PBM_GETRANGE */
    case 0x402: w->ppos = (int)(intptr_t)wp_; break;                               /* PBM_SETPOS */
    case 0x403: w->ppos += (int)(intptr_t)wp_; break;                              /* PBM_DELTAPOS */
    case 0x404: { int o = w->limit; w->limit = (int)wp_; return o; }               /* PBM_SETSTEP */
    case 0x405: w->ppos += w->limit ? w->limit : 10; break;                         /* PBM_STEPIT */
    case 0x408: return (intptr_t)w->ppos;                                          /* PBM_GETPOS */
    case 0x409: {                                                                   /* PBM_SETBARCOLOR */
      DWORD c = (DWORD)lp;
      memcpy(w->extra + WEXTRA - 8, &c, 4);
      w->extra[WEXTRA - 1] = 1;
      win_invalidate(w);
      return 0;
    }
    case 0x40A: case 0x410: case 0x2001: return 0; /* MARQUEE, SETSTATE, SETBKCOLOR */
    default: return wDefWindowProcW(w, m, wp_, lp);
  }
  if (w->ppos < w->pmin) w->ppos = w->pmin;
  if (w->ppos > w->pmax) w->ppos = w->pmax;
  win_invalidate(w);
  return (intptr_t)old;
}

/* ---------- SysLink e generico ---------- */
static intptr_t link_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  if (m == WM_PAINT) {
    struct wdc* d = &w->dc;
    struct surf* s;
    wchar16 buf[512];
    int i, o = 0, in_tag = 0;
    DWORD bg, fg;
    int bg_null;
    dc_prepare(d, w);
    s = dc_surf(d);
    ctl_colors(w, WM_CTLCOLORSTATIC, C_FACE, &bg, &fg, &bg_null);
    if (!bg_null) dc_fill(d, 0, 0, w->w, w->h, bg);
    for (i = 0; w->text && w->text[i] && o < 511; i++) {
      if (w->text[i] == '<') in_tag = 1;
      else if (w->text[i] == '>') in_tag = 0;
      else if (!in_tag) buf[o++] = w->text[i];
    }
    buf[o] = 0;
    if (s) {
      struct rect rr = {d->ox, d->oy, d->ox + w->w, d->oy + w->h};
      text_box(s, &d->clip, &rr, buf, o, 0xCC6600, 0, DT_WORDBREAK | DT_NOPREFIX);
    }
    return 0;
  }
  if (m == WM_ERASEBKGND) return 1;
  return wDefWindowProcW(w, m, wp_, lp);
}

static intptr_t other_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  if (m == WM_PAINT) {
    struct wdc* d = &w->dc;
    dc_prepare(d, w);
    dc_fill(d, 0, 0, w->w, w->h, 0xFFFFFF);
    dc_frame(d, 0, 0, w->w, w->h, 0xA0A0A0);
    ctl_text(w, 4, 2, w->w - 4, w->h - 2, DT_WORDBREAK, C_TEXT);
    return 0;
  }
  if (m == WM_ERASEBKGND) return 1;
  /* comctl32: muitas mensagens devolvem 0/-1 sem efeito */
  return wDefWindowProcW(w, m, wp_, lp);
}

intptr_t WINAPI ctl_proc(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wwin* w = win_of(h);
  if (!w) return 0;
  switch (w->cls->kind) {
    case CK_BUTTON: return button_proc(w, m, wp_, lp);
    case CK_STATIC: return static_proc(w, m, wp_, lp);
    case CK_EDIT: return edit_proc(w, m, wp_, lp);
    case CK_LISTBOX: return list_proc(w, m, wp_, lp);
    case CK_COMBO: {
      intptr_t r;
      if (m >= 0x400 && m < 0x420 && comboex_msg(w, m, wp_, lp, &r)) return r;
      return combo_proc(w, m, wp_, lp);
    }
    case CK_LISTVIEW: case CK_TOOLBAR: case CK_STATUS: case CK_REBAR: return comctl_proc(w, m, wp_, lp);
    case CK_PROGRESS: return progress_proc(w, m, wp_, lp);
    case CK_LINK: return link_proc(w, m, wp_, lp);
  }
  return other_proc(w, m, wp_, lp);
}
