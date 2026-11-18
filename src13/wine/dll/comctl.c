/*
 * comctl.c - Controles do comctl32 que a janela principal de um programa
 * "de explorador" usa: SysListView32 (modo relatorio, inclusive LVS_OWNERDATA
 * com LVN_GETDISPINFO), ToolbarWindow32, msctls_statusbar32, ReBarWindow32 e
 * as mensagens CBEM_* do ComboBoxEx32. Sem icones ainda (ImageList guarda so
 * a contagem): o texto ocupa o lugar.
 *
 * O estado de cada controle fica num bloco proprio, com o ponteiro no fim do
 * extra[] da janela (as classes embutidas nao tem cbWndExtra do programa).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "gui_int.h"

#define C_FACE 0xF0F0F0
#define C_TEXT 0x000000
#define C_GRAY 0x838383
#define C_LINE 0xE5E5E5
#define C_BORDER 0x7A7A7A
#define C_SEL 0xF7E4CC      /* selecao sem foco (BGR) */
#define C_SELF 0xD77800     /* selecao com foco */
#define C_HOT 0xFBF1E5

#define WM_NOTIFY_ 0x004E
#define WM_LBUTTONDBLCLK_ 0x0203

static void** state_slot(struct wwin* w) { return (void**)(w->extra + WEXTRA - 8); }

struct nmhdr {
  HANDLE from;
  uintptr_t id;
  DWORD code;
};
static intptr_t notify(struct wwin* w, struct nmhdr* h, int code) {
  h->from = (HANDLE)w;
  h->id = w->id;
  h->code = (DWORD)code;
  return w->parent ? wsend(w->parent, WM_NOTIFY_, w->id, (intptr_t)h) : 0;
}
static void command(struct wwin* w, DWORD id, int code) {
  if (w->parent) wsend(w->parent, WM_COMMAND, ((uintptr_t)(code & 0xFFFF) << 16) | (id & 0xFFFF), (intptr_t)w);
}

static wchar16* dup16(const wchar16* s) {
  size_t n;
  wchar16* d;
  if (!s || (uintptr_t)s < 0x10000 || (intptr_t)s == -1) return 0;
  n = (wstrlen16(s) + 1) * 2;
  d = (wchar16*)k_malloc((unsigned)n);
  if (d) memcpy(d, s, n);
  return d;
}
static int copy_out(const wchar16* s, wchar16* buf, int n) {
  int len = s ? (int)wstrlen16(s) : 0;
  if (buf && n > 0) {
    int k = len < n - 1 ? len : n - 1;
    if (k) memcpy(buf, s, (size_t)k * 2);
    buf[k] = 0;
    return k;
  }
  return len;
}

/* texto numa celula, cortado nela */
static void cell(struct wwin* w, int l, int t, int r, int b, const wchar16* s, DWORD color, DWORD fmt) {
  struct wdc* d = &w->dc;
  struct surf* sf = dc_surf(d);
  struct rect rr = {l + d->ox, t + d->oy, r + d->ox, b + d->oy};
  struct rect clip = rr;
  if (!sf || !s || !*s || r <= l) return;
  if (clip.left < d->clip.left) clip.left = d->clip.left;
  if (clip.top < d->clip.top) clip.top = d->clip.top;
  if (clip.right > d->clip.right) clip.right = d->clip.right;
  if (clip.bottom > d->clip.bottom) clip.bottom = d->clip.bottom;
  if (clip.right <= clip.left || clip.bottom <= clip.top) return;
  text_box(sf, &clip, &rr, s, -1, color, w->font && w->font->bold, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

static void move_child(struct wwin* c, int x, int y, int cx, int cy) {
  typedef BOOL(WINAPI * swp)(HANDLE, HANDLE, int, int, int, int, DWORD);
  static swp f;
  if (!c) return;
  if (!f) f = (swp)builtin_lookup("SetWindowPos");
  if (c->x == x && c->y == y && c->w == cx && c->h == cy) return;
  if (f) f((HANDLE)c, 0, x, y, cx, cy, 0x14);
}

/* ======================= SysListView32 ======================= */
#define LV_MAXCOL 24
struct lvitemw {
  DWORD mask;
  int item, sub;
  DWORD state, state_mask;
  wchar16* text;
  int cch;
  int image;
  intptr_t param;
  int indent, group_id;
  DWORD ncols;
  DWORD* cols;
  int* colfmt;
  int group;
};
struct lvcolumnw {
  DWORD mask;
  int fmt, cx;
  wchar16* text;
  int cch;
  int sub, image, order;
};
struct nmlvdispinfo {
  struct nmhdr h;
  struct lvitemw item;
};
struct nmlistview {
  struct nmhdr h;
  int item, sub;
  DWORD newst, oldst, changed;
  struct point pt;
  intptr_t param;
  DWORD keyflags; /* NMITEMACTIVATE */
};
struct __attribute__((packed)) nmlvkeydown {
  struct nmhdr h;
  uint16_t vk;
  DWORD flags;
};
struct lvhit {
  struct point pt;
  DWORD flags;
  int item, sub, group;
};
struct lvrow {
  wchar16* sub[LV_MAXCOL]; /* (wchar16*)-1 = LPSTR_TEXTCALLBACK */
  intptr_t param;
  int image;
  DWORD state;
};
struct lvcol {
  wchar16* text;
  int fmt, cx;
};
struct lv {
  struct lvcol col[LV_MAXCOL];
  int ncol;
  struct lvrow* rows;   /* sem LVS_OWNERDATA */
  uint8_t* ost;         /* estado por item com LVS_OWNERDATA */
  int n, cap;
  int top, focus, mark;
  DWORD exstyle, bk, fg;
  HANDLE il[3];
};
#define LVS_OWNERDATA 0x1000
#define LVS_NOCOLUMNHEADER 0x4000
#define LVS_SINGLESEL 0x0004
#define LVIS_FOCUSED 1
#define LVIS_SELECTED 2
#define LVIF_TEXT 1
#define LVIF_IMAGE 2
#define LVIF_PARAM 4
#define LVIF_STATE 8
#define LVN_ITEMCHANGED (-101)
#define LVN_COLUMNCLICK (-108)
#define LVN_GETDISPINFOW (-177)
#define LVN_KEYDOWN (-155)
#define LVN_ITEMACTIVATE (-114)
#define NM_CLICK (-2)
#define NM_DBLCLK (-3)
#define NM_RETURN (-4)
#define NM_RCLICK (-5)
#define NM_SETFOCUS (-7)
#define NM_KILLFOCUS (-8)

static struct lv* lv_of(struct wwin* w) {
  struct lv** s = (struct lv**)state_slot(w);
  if (!*s) {
    *s = (struct lv*)wzalloc(sizeof(struct lv));
    if (*s) {
      (*s)->focus = -1;
      (*s)->mark = -1;
      (*s)->bk = 0xFFFFFF;
    }
  }
  return *s;
}
static int lv_owner(struct wwin* w) { return (w->style & LVS_OWNERDATA) != 0; }
static int row_h(void) {
  int h = text_height() + 6;
  return h < 18 ? 18 : h;
}
static int head_h(struct wwin* w) {
  if ((w->style & LVS_NOCOLUMNHEADER) || (w->style & 3) != 1) return 0;
  return text_height() + 8;
}
static int lv_page(struct wwin* w) {
  int n = (w->h - head_h(w) - 2) / row_h();
  return n < 1 ? 1 : n;
}
static DWORD lv_state(struct lv* l, struct wwin* w, int i) {
  if (i < 0 || i >= l->n) return 0;
  if (lv_owner(w)) return l->ost ? l->ost[i] : 0;
  return l->rows[i].state;
}
static int lv_grow(struct lv* l, struct wwin* w, int n) {
  if (n <= l->cap) return 1;
  {
    int nc = l->cap ? l->cap : 64;
    while (nc < n) nc *= 2;
    if (lv_owner(w)) {
      uint8_t* ns = (uint8_t*)wzalloc((size_t)nc);
      if (!ns) return 0;
      if (l->ost) {
        memcpy(ns, l->ost, (size_t)l->cap);
        k_free(l->ost);
      }
      l->ost = ns;
    } else {
      struct lvrow* nr = (struct lvrow*)wzalloc(sizeof(struct lvrow) * (size_t)nc);
      if (!nr) return 0;
      if (l->rows) {
        memcpy(nr, l->rows, sizeof(struct lvrow) * (size_t)l->n);
        k_free(l->rows);
      }
      l->rows = nr;
    }
    l->cap = nc;
  }
  return 1;
}
static void row_free(struct lvrow* r) {
  int i;
  for (i = 0; i < LV_MAXCOL; i++)
    if (r->sub[i] && (intptr_t)r->sub[i] != -1) k_free(r->sub[i]);
}
/* muda o estado e avisa (LVN_ITEMCHANGED) */
static void lv_set_state(struct wwin* w, struct lv* l, int i, DWORD st, DWORD mask) {
  DWORD old = lv_state(l, w, i), nw = (old & ~mask) | (st & mask);
  struct nmlistview nm;
  if (old == nw) return;
  if (lv_owner(w)) {
    if (l->ost) l->ost[i] = (uint8_t)nw;
  } else {
    l->rows[i].state = nw;
  }
  if ((nw & LVIS_FOCUSED) && !(old & LVIS_FOCUSED)) {
    if (l->focus >= 0 && l->focus != i && l->focus < l->n) {
      if (lv_owner(w)) {
        if (l->ost) l->ost[l->focus] &= (uint8_t)~LVIS_FOCUSED;
      } else {
        l->rows[l->focus].state &= ~(DWORD)LVIS_FOCUSED;
      }
    }
    l->focus = i;
  }
  memset(&nm, 0, sizeof(nm));
  nm.item = i;
  nm.newst = nw;
  nm.oldst = old;
  nm.changed = LVIF_STATE;
  nm.param = lv_owner(w) ? 0 : l->rows[i].param;
  notify(w, &nm.h, LVN_ITEMCHANGED);
}
static void lv_select_only(struct wwin* w, struct lv* l, int i) {
  int k;
  for (k = 0; k < l->n; k++)
    if (k != i && (lv_state(l, w, k) & LVIS_SELECTED)) lv_set_state(w, l, k, 0, LVIS_SELECTED);
  if (i >= 0 && i < l->n) lv_set_state(w, l, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  l->mark = i;
}
static void lv_ensure(struct wwin* w, struct lv* l, int i) {
  int pg = lv_page(w);
  if (i < l->top) l->top = i;
  else if (i >= l->top + pg) l->top = i - pg + 1;
  if (l->top > l->n - pg) l->top = l->n - pg;
  if (l->top < 0) l->top = 0;
}
/* texto de uma celula: guardado ou pedido a mae */
static const wchar16* lv_text(struct wwin* w, struct lv* l, int i, int sub, wchar16* buf, int n) {
  const wchar16* t = 0;
  if (!lv_owner(w)) {
    t = l->rows[i].sub[sub];
    if ((intptr_t)t != -1) return t;
  }
  {
    struct nmlvdispinfo di;
    memset(&di, 0, sizeof(di));
    buf[0] = 0;
    di.item.mask = LVIF_TEXT;
    di.item.item = i;
    di.item.sub = sub;
    di.item.text = buf;
    di.item.cch = n;
    di.item.param = lv_owner(w) ? 0 : l->rows[i].param;
    notify(w, &di.h, LVN_GETDISPINFOW);
    t = di.item.text;
    if ((intptr_t)t == -1 || (uintptr_t)t < 0x10000) t = buf;
  }
  return t;
}
static void lv_paint(struct wwin* w) {
  struct lv* l = lv_of(w);
  struct wdc* d = &w->dc;
  int hh = head_h(w), rh = row_h(), i, c, x, y;
  int ncol;
  int focused;
  wchar16 buf[262];
  dc_prepare(d, w);
  if (!l) return;
  focused = win_top(w) && win_top(w)->focus == w;
  dc_fill(d, 0, 0, w->w, w->h, l->bk);
  ncol = l->ncol ? l->ncol : 1;
  if (hh) {
    dc_fill(d, 1, 1, w->w - 1, hh, 0xFFFFFF);
    dc_line(d, 1, hh, w->w - 1, hh, C_LINE);
    for (c = 0, x = 1; c < l->ncol; c++) {
      int cw = l->col[c].cx;
      cell(w, x + 6, 1, x + cw - 4, hh, l->col[c].text, 0x404040, (l->col[c].fmt & 3) == 1 ? DT_RIGHT : 0);
      x += cw;
      dc_line(d, x - 1, 3, x - 1, hh - 3, C_LINE);
    }
  }
  for (i = l->top, y = hh + 1; i < l->n && y < w->h; i++, y += rh) {
    DWORD st = lv_state(l, w, i);
    DWORD fg = l->fg;
    if (st & LVIS_SELECTED) {
      int full = (l->exstyle & 0x20) || 1; /* LVS_EX_FULLROWSELECT (sempre: fica mais legivel) */
      int right = full ? w->w - 1 : 1 + (l->ncol ? l->col[0].cx : w->w);
      dc_fill(d, 1, y, right, y + rh, focused ? C_SELF : C_SEL);
      if (focused) fg = 0xFFFFFF;
    }
    for (c = 0, x = 1; c < ncol; c++) {
      int cw = l->ncol ? l->col[c].cx : w->w - 2;
      const wchar16* t = lv_text(w, l, i, c, buf, 260);
      int pad = c == 0 ? 6 : 6;
      cell(w, x + pad, y, x + cw - 4, y + rh, t, fg,
           (l->ncol && (l->col[c].fmt & 3) == 1) ? DT_RIGHT | DT_END_ELLIPSIS : DT_END_ELLIPSIS);
      x += cw;
      if (x >= w->w) break;
    }
  }
  dc_frame(d, 0, 0, w->w, w->h, focused ? C_SELF : 0xA0A0A0);
}
static int lv_hit(struct wwin* w, struct lv* l, int x, int y, int* sub) {
  int hh = head_h(w), i, c, cx = 1;
  if (sub) *sub = 0;
  if (y < hh + 1) return -2; /* cabecalho */
  i = l->top + (y - hh - 1) / row_h();
  if (i < 0 || i >= l->n) return -1;
  if (sub)
    for (c = 0; c < l->ncol; c++) {
      if (x >= cx && x < cx + l->col[c].cx) *sub = c;
      cx += l->col[c].cx;
    }
  return i;
}
static void lv_item_get(struct wwin* w, struct lv* l, struct lvitemw* it) {
  int i = it->item;
  if (i < 0 || i >= l->n) return;
  if (it->mask & LVIF_STATE) it->state = lv_state(l, w, i) & it->state_mask;
  if (lv_owner(w)) return;
  if (it->mask & LVIF_PARAM) it->param = l->rows[i].param;
  if (it->mask & LVIF_IMAGE) it->image = l->rows[i].image;
  if ((it->mask & LVIF_TEXT) && it->sub >= 0 && it->sub < LV_MAXCOL) {
    wchar16* t = l->rows[i].sub[it->sub];
    if ((intptr_t)t == -1) it->text = (wchar16*)(intptr_t)-1;
    else copy_out(t, it->text, it->cch);
  }
}
static void lv_item_set(struct wwin* w, struct lv* l, const struct lvitemw* it) {
  int i = it->item;
  if (i < 0 || i >= l->n) return;
  if (it->mask & LVIF_STATE) lv_set_state(w, l, i, it->state, it->state_mask);
  if (lv_owner(w)) return;
  if (it->mask & LVIF_PARAM) l->rows[i].param = it->param;
  if (it->mask & LVIF_IMAGE) l->rows[i].image = it->image;
  if ((it->mask & LVIF_TEXT) && it->sub >= 0 && it->sub < LV_MAXCOL) {
    wchar16** p = &l->rows[i].sub[it->sub];
    if (*p && (intptr_t)*p != -1) k_free(*p);
    *p = (intptr_t)it->text == -1 ? (wchar16*)(intptr_t)-1 : dup16(it->text);
  }
}
static int lv_next(struct wwin* w, struct lv* l, int start, DWORD fl) {
  int i;
  DWORD want = fl & 3; /* LVNI_FOCUSED | LVNI_SELECTED */
  if (fl & 0x100 || fl & 0x200) { /* LVNI_ABOVE / BELOW */
    i = start + ((fl & 0x100) ? -1 : 1);
    return (i >= 0 && i < l->n) ? i : -1;
  }
  for (i = start + 1; i < l->n; i++) {
    DWORD st = lv_state(l, w, i);
    if (!want || (st & want) == want) return i;
  }
  return -1;
}
static void lv_activate(struct wwin* w, struct lv* l, int code, int item, int sub) {
  struct nmlistview nm;
  memset(&nm, 0, sizeof(nm));
  nm.item = item;
  nm.sub = sub;
  nm.param = (item >= 0 && item < l->n && !lv_owner(w)) ? l->rows[item].param : 0;
  notify(w, &nm.h, code);
}
static intptr_t lv_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct lv* l = lv_of(w);
  if (!l) return wDefWindowProcW(w, m, wp_, lp);
  switch (m) {
    case WM_PAINT: lv_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return 0x85; /* arrows | chars | wantallkeys */
    case WM_SETFOCUS: {
      struct nmhdr h;
      win_invalidate(w);
      notify(w, &h, NM_SETFOCUS);
      return 0;
    }
    case WM_KILLFOCUS: {
      struct nmhdr h;
      win_invalidate(w);
      notify(w, &h, NM_KILLFOCUS);
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONDBLCLK_: {
      int x = (int)(int16_t)(lp & 0xFFFF), y = (int)(int16_t)((lp >> 16) & 0xFFFF), sub = 0;
      int i = lv_hit(w, l, x, y, &sub);
      win_set_focus(w);
      if (i == -2) {
        int c, cx = 1;
        if (m != WM_LBUTTONDOWN) return 0;
        for (c = 0; c < l->ncol; c++) {
          if (x >= cx && x < cx + l->col[c].cx) {
            struct nmlistview nm;
            memset(&nm, 0, sizeof(nm));
            nm.item = -1;
            nm.sub = c;
            notify(w, &nm.h, LVN_COLUMNCLICK);
          }
          cx += l->col[c].cx;
        }
        return 0;
      }
      if (m != WM_LBUTTONDBLCLK_ && (i >= 0 || !(lv_state(l, w, l->focus) & LVIS_SELECTED) || 1)) {
        if (i >= 0 && (wp_ & 8) && !(w->style & LVS_SINGLESEL)) { /* MK_CONTROL */
          lv_set_state(w, l, i, lv_state(l, w, i) ^ LVIS_SELECTED, LVIS_SELECTED);
          lv_set_state(w, l, i, LVIS_FOCUSED, LVIS_FOCUSED);
        } else if (i >= 0 && (wp_ & 4) && l->mark >= 0 && !(w->style & LVS_SINGLESEL)) { /* MK_SHIFT */
          int a = l->mark < i ? l->mark : i, b = l->mark < i ? i : l->mark, k;
          for (k = 0; k < l->n; k++) lv_set_state(w, l, k, (k >= a && k <= b) ? LVIS_SELECTED : 0, LVIS_SELECTED);
          lv_set_state(w, l, i, LVIS_FOCUSED, LVIS_FOCUSED);
        } else if (!(m == WM_RBUTTONDOWN && i >= 0 && (lv_state(l, w, i) & LVIS_SELECTED))) {
          lv_select_only(w, l, i);
        }
        win_invalidate(w);
      }
      lv_activate(w, l, m == WM_LBUTTONDBLCLK_ ? NM_DBLCLK : m == WM_RBUTTONDOWN ? NM_RCLICK : NM_CLICK, i, sub);
      if (m == WM_LBUTTONDBLCLK_ && i >= 0) lv_activate(w, l, LVN_ITEMACTIVATE, i, sub);
      return 0;
    }
    case WM_MOUSEWHEEL:
      l->top -= 3 * (int)(int16_t)(wp_ >> 16) / 120;
      lv_ensure(w, l, l->top < 0 ? 0 : l->top);
      if (l->top > l->n - lv_page(w)) l->top = l->n - lv_page(w);
      if (l->top < 0) l->top = 0;
      win_invalidate(w);
      return 0;
    case WM_KEYDOWN: {
      struct nmlvkeydown kd;
      int i = l->focus, pg = lv_page(w);
      memset(&kd, 0, sizeof(kd));
      kd.vk = (uint16_t)wp_;
      notify(w, &kd.h, LVN_KEYDOWN);
      switch (wp_) {
        case 0x26: i = i > 0 ? i - 1 : 0; break;               /* cima */
        case 0x28: i = i < l->n - 1 ? i + 1 : l->n - 1; break; /* baixo */
        case 0x21: i = i - pg > 0 ? i - pg : 0; break;         /* PgUp */
        case 0x22: i = i + pg < l->n - 1 ? i + pg : l->n - 1; break;
        case 0x24: i = 0; break;                               /* Home */
        case 0x23: i = l->n - 1; break;                        /* End */
        case 0x0D: {
          struct nmhdr h;
          notify(w, &h, NM_RETURN);
          if (l->focus >= 0) lv_activate(w, l, LVN_ITEMACTIVATE, l->focus, 0);
          return 0;
        }
        default: return 0;
      }
      if (i >= 0 && i < l->n) {
        lv_select_only(w, l, i);
        lv_ensure(w, l, i);
        win_invalidate(w);
      }
      return 0;
    }
    case 0x1001: l->bk = (DWORD)lp; win_invalidate(w); return 1;   /* LVM_SETBKCOLOR */
    case 0x1000: return l->bk;                                      /* LVM_GETBKCOLOR */
    case 0x1024: l->fg = (DWORD)lp; win_invalidate(w); return 1;   /* LVM_SETTEXTCOLOR */
    case 0x1023: return l->fg;
    case 0x1026: case 0x1025: return 1;                            /* (GET)SETTEXTBKCOLOR */
    case 0x1003: {                                                 /* LVM_SETIMAGELIST */
      HANDLE old = l->il[wp_ % 3];
      l->il[wp_ % 3] = (HANDLE)lp;
      return (intptr_t)old;
    }
    case 0x1002: return (intptr_t)l->il[wp_ % 3];                  /* LVM_GETIMAGELIST */
    case 0x1004: return l->n;                                      /* LVM_GETITEMCOUNT */
    case 0x102F: {                                                 /* LVM_SETITEMCOUNT */
      int n = (int)wp_;
      if (lv_owner(w)) {
        if (!lv_grow(l, w, n)) return 0;
        if (n > l->n) memset(l->ost + l->n, 0, (size_t)(n - l->n));
        l->n = n;
        if (l->focus >= n) l->focus = -1;
        if (l->top > n - lv_page(w)) l->top = n - lv_page(w) > 0 ? n - lv_page(w) : 0;
        win_invalidate(w);
      } else {
        lv_grow(l, w, n);
      }
      return 1;
    }
    case 0x104D: {                                                 /* LVM_INSERTITEMW */
      const struct lvitemw* it = (const struct lvitemw*)lp;
      int i;
      if (!it || lv_owner(w) || !lv_grow(l, w, l->n + 1)) return -1;
      i = it->item < 0 || it->item > l->n ? l->n : it->item;
      memmove(&l->rows[i + 1], &l->rows[i], sizeof(struct lvrow) * (size_t)(l->n - i));
      memset(&l->rows[i], 0, sizeof(struct lvrow));
      l->n++;
      if (l->focus >= i) l->focus++;
      {
        struct lvitemw c = *it;
        c.item = i;
        c.sub = 0;
        lv_item_set(w, l, &c);
      }
      win_invalidate(w);
      return i;
    }
    case 0x104C:                                                   /* LVM_SETITEMW */
      if (lp) lv_item_set(w, l, (const struct lvitemw*)lp);
      win_invalidate(w);
      return 1;
    case 0x104B:                                                   /* LVM_GETITEMW */
      if (!lp || ((struct lvitemw*)lp)->item < 0 || ((struct lvitemw*)lp)->item >= l->n) return 0;
      lv_item_get(w, l, (struct lvitemw*)lp);
      return 1;
    case 0x1074: {                                                 /* LVM_SETITEMTEXTW */
      struct lvitemw c;
      if (!lp) return 0;
      c = *(const struct lvitemw*)lp;
      c.item = (int)wp_;
      c.mask = LVIF_TEXT;
      lv_item_set(w, l, &c);
      win_invalidate(w);
      return 1;
    }
    case 0x1073: {                                                 /* LVM_GETITEMTEXTW */
      struct lvitemw* it = (struct lvitemw*)lp;
      wchar16 buf[262];
      const wchar16* t;
      if (!it || (int)wp_ < 0 || (int)wp_ >= l->n || it->sub < 0 || it->sub >= LV_MAXCOL) return 0;
      t = lv_text(w, l, (int)wp_, it->sub, buf, 260);
      return copy_out(t, it->text, it->cch);
    }
    case 0x1008: {                                                 /* LVM_DELETEITEM */
      int i = (int)wp_;
      if (i < 0 || i >= l->n) return 0;
      if (!lv_owner(w)) {
        row_free(&l->rows[i]);
        memmove(&l->rows[i], &l->rows[i + 1], sizeof(struct lvrow) * (size_t)(l->n - i - 1));
      } else if (l->ost) {
        memmove(&l->ost[i], &l->ost[i + 1], (size_t)(l->n - i - 1));
      }
      l->n--;
      if (l->focus == i) l->focus = -1;
      else if (l->focus > i) l->focus--;
      win_invalidate(w);
      return 1;
    }
    case 0x1009: {                                                 /* LVM_DELETEALLITEMS */
      int i;
      if (!lv_owner(w))
        for (i = 0; i < l->n; i++) row_free(&l->rows[i]);
      l->n = 0;
      l->top = 0;
      l->focus = -1;
      l->mark = -1;
      win_invalidate(w);
      return 1;
    }
    case 0x100C: return lv_next(w, l, (int)(intptr_t)wp_, (DWORD)lp); /* LVM_GETNEXTITEM */
    case 0x102B: {                                                 /* LVM_SETITEMSTATE */
      const struct lvitemw* it = (const struct lvitemw*)lp;
      int i = (int)(intptr_t)wp_;
      if (!it) return 0;
      if (i == -1) {
        for (i = 0; i < l->n; i++) lv_set_state(w, l, i, it->state, it->state_mask);
      } else if (i >= 0 && i < l->n) {
        lv_set_state(w, l, i, it->state, it->state_mask);
      }
      win_invalidate(w);
      return 1;
    }
    case 0x102C: return lv_state(l, w, (int)wp_) & (DWORD)lp;      /* LVM_GETITEMSTATE */
    case 0x1032: {                                                 /* LVM_GETSELECTEDCOUNT */
      int i, c = 0;
      for (i = 0; i < l->n; i++)
        if (lv_state(l, w, i) & LVIS_SELECTED) c++;
      return c;
    }
    case 0x1013: lv_ensure(w, l, (int)wp_); win_invalidate(w); return 1; /* LVM_ENSUREVISIBLE */
    case 0x1027: return l->top;                                    /* LVM_GETTOPINDEX */
    case 0x1028: return lv_page(w);                                /* LVM_GETCOUNTPERPAGE */
    case 0x1014:                                                   /* LVM_SCROLL */
      l->top += (int)lp / row_h();
      lv_ensure(w, l, l->top < 0 ? 0 : l->top);
      win_invalidate(w);
      return 1;
    case 0x1036: {                                                 /* LVM_SETEXTENDEDLISTVIEWSTYLE */
      DWORD old = l->exstyle, mask = wp_ ? (DWORD)wp_ : 0xFFFFFFFFu;
      l->exstyle = (l->exstyle & ~mask) | ((DWORD)lp & mask);
      return old;
    }
    case 0x1037: return l->exstyle;                                /* LVM_GETEXTENDEDLISTVIEWSTYLE */
    case 0x1061: {                                                 /* LVM_INSERTCOLUMNW */
      const struct lvcolumnw* c = (const struct lvcolumnw*)lp;
      int i = (int)wp_;
      if (!c || l->ncol >= LV_MAXCOL) return -1;
      if (i < 0 || i > l->ncol) i = l->ncol;
      memmove(&l->col[i + 1], &l->col[i], sizeof(struct lvcol) * (size_t)(l->ncol - i));
      memset(&l->col[i], 0, sizeof(struct lvcol));
      l->ncol++;
      l->col[i].cx = (c->mask & 2) ? c->cx : 100;
      if (c->mask & 1) l->col[i].fmt = c->fmt;
      if (c->mask & 4) l->col[i].text = dup16(c->text);
      win_invalidate(w);
      return i;
    }
    case 0x1060: {                                                 /* LVM_SETCOLUMNW */
      const struct lvcolumnw* c = (const struct lvcolumnw*)lp;
      int i = (int)wp_;
      if (!c || i < 0 || i >= l->ncol) return 0;
      if (c->mask & 2) l->col[i].cx = c->cx;
      if (c->mask & 1) l->col[i].fmt = c->fmt;
      if (c->mask & 4) {
        k_free(l->col[i].text);
        l->col[i].text = dup16(c->text);
      }
      win_invalidate(w);
      return 1;
    }
    case 0x105F: {                                                 /* LVM_GETCOLUMNW */
      struct lvcolumnw* c = (struct lvcolumnw*)lp;
      int i = (int)wp_;
      if (!c || i < 0 || i >= l->ncol) return 0;
      if (c->mask & 2) c->cx = l->col[i].cx;
      if (c->mask & 1) c->fmt = l->col[i].fmt;
      if (c->mask & 4) copy_out(l->col[i].text, c->text, c->cch);
      if (c->mask & 8) c->sub = i;
      if (c->mask & 0x20) c->order = i;
      return 1;
    }
    case 0x101C: {                                                 /* LVM_DELETECOLUMN */
      int i = (int)wp_;
      if (i < 0 || i >= l->ncol) return 0;
      k_free(l->col[i].text);
      memmove(&l->col[i], &l->col[i + 1], sizeof(struct lvcol) * (size_t)(l->ncol - i - 1));
      l->ncol--;
      win_invalidate(w);
      return 1;
    }
    case 0x101D: return ((int)wp_ >= 0 && (int)wp_ < l->ncol) ? l->col[wp_].cx : 0; /* LVM_GETCOLUMNWIDTH */
    case 0x101E:                                                   /* LVM_SETCOLUMNWIDTH */
      if ((int)wp_ >= 0 && (int)wp_ < l->ncol) {
        int cx = (int)(int16_t)(lp & 0xFFFF);
        if (cx < 0) cx = (int)wp_ == l->ncol - 1 ? 120 : 100; /* LVSCW_AUTOSIZE* */
        l->col[wp_].cx = cx;
        win_invalidate(w);
      }
      return 1;
    case 0x103B: {                                                 /* LVM_GETCOLUMNORDERARRAY */
      int i, *a = (int*)lp;
      for (i = 0; a && i < (int)wp_ && i < l->ncol; i++) a[i] = i;
      return 1;
    }
    case 0x103A: return 1;                                         /* LVM_SETCOLUMNORDERARRAY */
    case 0x101F: return 0;                                         /* LVM_GETHEADER */
    case 0x100E:                                                   /* LVM_GETITEMRECT */
    case 0x1038: {                                                 /* LVM_GETSUBITEMRECT */
      struct rect* r = (struct rect*)lp;
      int i = (int)wp_, y;
      if (!r || i < 0 || i >= l->n) return 0;
      y = head_h(w) + 1 + (i - l->top) * row_h();
      r->left = 1;
      r->right = w->w - 1;
      if (m == 0x1038 && r->top > 0 && r->top < l->ncol) {
        int c, x = 1;
        for (c = 0; c < r->top; c++) x += l->col[c].cx;
        r->left = x;
        r->right = x + l->col[r->top].cx;
      }
      r->top = y;
      r->bottom = y + row_h();
      return 1;
    }
    case 0x1010: {                                                 /* LVM_GETITEMPOSITION */
      struct point* p = (struct point*)lp;
      if (!p) return 0;
      p->x = 1;
      p->y = head_h(w) + 1 + ((int)wp_ - l->top) * row_h();
      return 1;
    }
    case 0x1012:                                                   /* LVM_HITTEST */
    case 0x1039: {                                                 /* LVM_SUBITEMHITTEST */
      struct lvhit* h = (struct lvhit*)lp;
      int sub = 0, i;
      if (!h) return -1;
      i = lv_hit(w, l, h->pt.x, h->pt.y, &sub);
      h->item = i >= 0 ? i : -1;
      h->sub = sub;
      h->flags = i >= 0 ? 0x6 /* LVHT_ONITEM */ : 1 /* LVHT_NOWHERE */;
      return h->item;
    }
    case 0x1043: { int o = l->mark; l->mark = (int)lp; return o; } /* LVM_SETSELECTIONMARK */
    case 0x1042: return l->mark;                                   /* LVM_GETSELECTIONMARK */
    case 0x1057: return text_width((const wchar16*)lp, -1);        /* LVM_GETSTRINGWIDTHW */
    case 0x1053: return -1;                                        /* LVM_FINDITEMW */
    case 0x1015: case 0x102A: win_invalidate(w); return 1;         /* LVM_REDRAWITEMS / UPDATE */
    case 0x108F: return w->style & 3;                              /* LVM_GETVIEW */
    case 0x108E: w->style = (w->style & ~3u) | ((DWORD)wp_ & 3); win_invalidate(w); return 1; /* LVM_SETVIEW */
    case 0x1030: case 0x1051: case 0x106B: return 1;              /* SORTITEMS(EX) etc. */
    case 0x1029: {                                                 /* LVM_GETVIEWRECT */
      struct rect* r = (struct rect*)lp;
      if (r) {
        r->left = r->top = 0;
        r->right = w->w;
        r->bottom = l->n * row_h();
      }
      return 1;
    }
    case 0x1035: return MAKELONG_(16, row_h());                     /* LVM_GETITEMSPACING */
  }
  if (m >= 0x1000 && m < 0x1100) return 0;
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ======================= ToolbarWindow32 ======================= */
struct tbbutton {
  int bitmap, cmd;
  uint8_t state, style, res[6];
  uintptr_t data;
  intptr_t str;
};
struct tbinfo {
  DWORD size, mask;
  int cmd, image;
  uint8_t state, style;
  uint16_t cx;
  uintptr_t param;
  wchar16* text;
  int cch;
};
struct tbbtn {
  int bitmap, cmd;
  uint8_t state, style;
  uintptr_t data;
  wchar16* text;
  int x, w; /* calculados */
};
struct tb {
  struct tbbtn* b;
  int n, cap;
  wchar16** strs;
  int nstr, capstr;
  int bw, bh;   /* bitmap */
  int btn_w, btn_h;
  DWORD exstyle;
  int hot, down;
  HANDLE il[3];
};
#define TBSTATE_CHECKED 1
#define TBSTATE_PRESSED 2
#define TBSTATE_ENABLED 4
#define TBSTATE_HIDDEN 8
#define BTNS_SEP 1
#define BTNS_CHECK 2
#define BTNS_DROPDOWN 8
#define TBSTYLE_LIST 0x1000
#define TBSTYLE_FLAT 0x800
#define CCS_NORESIZE 4
#define CCS_NOPARENTALIGN 8
#define CCS_BOTTOM 3

static struct tb* tb_of(struct wwin* w) {
  struct tb** s = (struct tb**)state_slot(w);
  if (!*s) {
    *s = (struct tb*)wzalloc(sizeof(struct tb));
    if (*s) {
      (*s)->bw = 16;
      (*s)->bh = 15;
      (*s)->hot = (*s)->down = -1;
    }
  }
  return *s;
}
static const wchar16* tb_text(struct tb* t, struct tbbtn* b) {
  (void)t;
  return b->text;
}
static void tb_layout(struct wwin* w, struct tb* t) {
  int i, x = 0, maxw = 0, th = text_height(), anytext = 0;
  int list = (w->style & TBSTYLE_LIST) != 0;
  for (i = 0; i < t->n; i++)
    if (t->b[i].text && *t->b[i].text) {
      int tw = text_width(t->b[i].text, -1);
      anytext = 1;
      if (tw > maxw) maxw = tw;
    }
  if (list) {
    t->btn_h = (t->bh > th ? t->bh : th) + 8;
  } else {
    t->btn_h = t->bh + 8 + (anytext ? th + 2 : 0);
  }
  if (t->btn_h < 22) t->btn_h = 22;
  t->btn_w = list ? 0 : (maxw + 14 > t->bw + 8 ? maxw + 14 : t->bw + 8);
  if (t->btn_w > 96) t->btn_w = 96;
  for (i = 0; i < t->n; i++) {
    struct tbbtn* b = &t->b[i];
    b->x = x;
    if (b->state & TBSTATE_HIDDEN) b->w = 0;
    else if (b->style & BTNS_SEP) b->w = 8;
    else if (list) b->w = t->bw + 12 + (b->text && *b->text ? text_width(b->text, -1) + 8 : 0);
    else b->w = t->btn_w;
    if (!(b->style & BTNS_SEP) && (b->style & BTNS_DROPDOWN)) b->w += 12;
    x += b->w;
  }
}
static void tb_autosize(struct wwin* w, struct tb* t) {
  tb_layout(w, t);
  if (!(w->style & (CCS_NORESIZE | CCS_NOPARENTALIGN)) && w->parent) {
    int bottom = (w->style & 3) == CCS_BOTTOM;
    int h = t->btn_h + 4;
    move_child(w, 0, bottom ? w->parent->h - h : 0, w->parent->w, h);
  }
}
static void tb_paint(struct wwin* w) {
  struct tb* t = tb_of(w);
  struct wdc* d = &w->dc;
  int i;
  dc_prepare(d, w);
  dc_fill(d, 0, 0, w->w, w->h, C_FACE);
  if (!t) return;
  tb_layout(w, t);
  for (i = 0; i < t->n; i++) {
    struct tbbtn* b = &t->b[i];
    int x = b->x + 2, y = 2, bw = b->w, bh = t->btn_h;
    int en = (b->state & TBSTATE_ENABLED) != 0;
    DWORD fg = en ? C_TEXT : C_GRAY;
    const wchar16* s = tb_text(t, b);
    if (!b->w) continue;
    if (b->style & BTNS_SEP) {
      dc_line(d, x + 3, y + 3, x + 3, y + bh - 3, 0xC8C8C8);
      continue;
    }
    if (en && (i == t->down || (b->state & (TBSTATE_CHECKED | TBSTATE_PRESSED)))) {
      dc_fill(d, x, y, x + bw - 2, y + bh, 0xF7E4CC);
      dc_frame(d, x, y, x + bw - 2, y + bh, 0xD77800);
    } else if (en && i == t->hot) {
      dc_fill(d, x, y, x + bw - 2, y + bh, C_HOT);
      dc_frame(d, x, y, x + bw - 2, y + bh, 0xF0C080);
    }
    if (w->style & TBSTYLE_LIST) {
      /* simbolo no lugar do icone */
      dc_frame(d, x + 4, y + (bh - t->bh) / 2, x + 4 + t->bw, y + (bh - t->bh) / 2 + t->bh, en ? 0x9C6A3A : 0xC0C0C0);
      cell(w, x + t->bw + 8, y, x + bw - 4, y + bh, s, fg, 0);
    } else {
      int ix = x + (bw - 2 - t->bw) / 2;
      dc_fill(d, ix + 2, y + 5, ix + t->bw - 2, y + 4 + t->bh - 2, en ? 0xE8D3BE : 0xE6E6E6);
      dc_frame(d, ix + 2, y + 5, ix + t->bw - 2, y + 4 + t->bh - 2, en ? 0x9C6A3A : 0xC0C0C0);
      cell(w, x + 2, y + t->bh + 6, x + bw - 4, y + bh - 1, s, fg, DT_CENTER | DT_END_ELLIPSIS);
    }
    if (b->style & BTNS_DROPDOWN) {
      int ax = x + bw - 12, ay = y + bh / 2 - 1;
      dc_line(d, ax, ay, ax + 6, ay, fg);
      dc_line(d, ax + 1, ay + 1, ax + 5, ay + 1, fg);
      dc_line(d, ax + 2, ay + 2, ax + 4, ay + 2, fg);
    }
  }
}
static int tb_hit(struct tb* t, int x, int y) {
  int i;
  if (y < 2 || y > 2 + t->btn_h) return -1;
  for (i = 0; i < t->n; i++)
    if (t->b[i].w && !(t->b[i].style & BTNS_SEP) && x >= t->b[i].x + 2 && x < t->b[i].x + 2 + t->b[i].w - 2) return i;
  return -1;
}
static int tb_index(struct tb* t, int cmd) {
  int i;
  for (i = 0; i < t->n; i++)
    if (t->b[i].cmd == cmd && !(t->b[i].style & BTNS_SEP)) return i;
  return -1;
}
static int tb_addstr(struct tb* t, const wchar16* s) {
  if (t->nstr >= t->capstr) {
    int nc = t->capstr ? t->capstr * 2 : 16;
    wchar16** ns = (wchar16**)wzalloc(sizeof(wchar16*) * (size_t)nc);
    if (!ns) return -1;
    if (t->strs) {
      memcpy(ns, t->strs, sizeof(wchar16*) * (size_t)t->nstr);
      k_free(t->strs);
    }
    t->strs = ns;
    t->capstr = nc;
  }
  t->strs[t->nstr] = dup16(s);
  return t->nstr++;
}
static int tb_insert(struct wwin* w, struct tb* t, int at, const struct tbbutton* src) {
  struct tbbtn* b;
  (void)w;
  if (t->n >= t->cap) {
    int nc = t->cap ? t->cap * 2 : 16;
    struct tbbtn* nb = (struct tbbtn*)wzalloc(sizeof(struct tbbtn) * (size_t)nc);
    if (!nb) return 0;
    if (t->b) {
      memcpy(nb, t->b, sizeof(struct tbbtn) * (size_t)t->n);
      k_free(t->b);
    }
    t->b = nb;
    t->cap = nc;
  }
  if (at < 0 || at > t->n) at = t->n;
  memmove(&t->b[at + 1], &t->b[at], sizeof(struct tbbtn) * (size_t)(t->n - at));
  b = &t->b[at];
  memset(b, 0, sizeof(*b));
  b->bitmap = src->bitmap;
  b->cmd = src->cmd;
  b->state = src->state;
  b->style = src->style;
  b->data = src->data;
  if ((uintptr_t)src->str >= 0x10000) b->text = dup16((const wchar16*)src->str);
  else if (src->str >= 0 && src->str < t->nstr) b->text = dup16(t->strs[src->str]);
  t->n++;
  return 1;
}
static intptr_t tb_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct tb* t = tb_of(w);
  if (!t) return wDefWindowProcW(w, m, wp_, lp);
  switch (m) {
    case WM_PAINT: tb_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: return 0;
    case WM_MOUSEMOVE: {
      int i = tb_hit(t, (int)(int16_t)(lp & 0xFFFF), (int)(int16_t)((lp >> 16) & 0xFFFF));
      if (i != t->hot) {
        t->hot = i;
        win_invalidate(w);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      int i = tb_hit(t, (int)(int16_t)(lp & 0xFFFF), (int)(int16_t)((lp >> 16) & 0xFFFF));
      t->down = (i >= 0 && (t->b[i].state & TBSTATE_ENABLED)) ? i : -1;
      win_invalidate(w);
      return 0;
    }
    case WM_LBUTTONUP: {
      int i = tb_hit(t, (int)(int16_t)(lp & 0xFFFF), (int)(int16_t)((lp >> 16) & 0xFFFF)), was = t->down;
      t->down = -1;
      win_invalidate(w);
      if (i >= 0 && i == was) {
        struct tbbtn* b = &t->b[i];
        if (b->style & BTNS_CHECK) b->state ^= TBSTATE_CHECKED;
        command(w, (DWORD)b->cmd, 0);
      }
      return 0;
    }
    case 0x41E: return 0;                                          /* TB_BUTTONSTRUCTSIZE */
    case 0x420:                                                    /* TB_SETBITMAPSIZE */
      t->bw = (int)(lp & 0xFFFF);
      t->bh = (int)((lp >> 16) & 0xFFFF);
      if (t->bw > 64) t->bw = 64;
      if (t->bh > 64) t->bh = 64;
      return 1;
    case 0x41F: return 1;                                          /* TB_SETBUTTONSIZE */
    case 0x43A: tb_layout(w, t); return MAKELONG_(t->btn_w ? t->btn_w : t->bw + 8, t->btn_h); /* TB_GETBUTTONSIZE */
    case 0x413: return 0;                                          /* TB_ADDBITMAP */
    case 0x44D: {                                                  /* TB_ADDSTRINGW */
      int first = -1, r;
      if (wp_ && (uintptr_t)lp < 0x10000) {
        typedef int(WINAPI * ls)(HANDLE, DWORD, wchar16*, int);
        ls f = (ls)builtin_lookup("LoadStringW");
        wchar16 buf[512];
        int n = f ? f((HANDLE)wp_, (DWORD)lp, buf, 512) : 0;
        if (n > 1) { /* o 1o caractere separa as cadeias */
          wchar16 sep = buf[0];
          int i, s = 1;
          for (i = 1; i <= n; i++)
            if (i == n || buf[i] == sep) {
              buf[i] = 0;
              if (i > s) {
                r = tb_addstr(t, buf + s);
                if (first < 0) first = r;
              }
              s = i + 1;
            }
        }
        return first;
      }
      if (lp) {
        const wchar16* s = (const wchar16*)lp;
        while (*s) {
          r = tb_addstr(t, s);
          if (first < 0) first = r;
          s += wstrlen16(s) + 1;
        }
      }
      return first;
    }
    case 0x444:                                                    /* TB_ADDBUTTONSW */
    case 0x443: {                                                  /* TB_INSERTBUTTONW */
      const struct tbbutton* b = (const struct tbbutton*)lp;
      if (!b) return 0;
      if (m == 0x444) {
        DWORD i;
        for (i = 0; i < (DWORD)wp_; i++) tb_insert(w, t, -1, &b[i]);
      } else {
        tb_insert(w, t, (int)wp_, b);
      }
      tb_layout(w, t);
      win_invalidate(w);
      return 1;
    }
    case 0x416: {                                                  /* TB_DELETEBUTTON */
      int i = (int)wp_;
      if (i < 0 || i >= t->n) return 0;
      k_free(t->b[i].text);
      memmove(&t->b[i], &t->b[i + 1], sizeof(struct tbbtn) * (size_t)(t->n - i - 1));
      t->n--;
      win_invalidate(w);
      return 1;
    }
    case 0x418: return t->n;                                       /* TB_BUTTONCOUNT */
    case 0x417: {                                                  /* TB_GETBUTTON */
      struct tbbutton* o = (struct tbbutton*)lp;
      int i = (int)wp_;
      if (!o || i < 0 || i >= t->n) return 0;
      memset(o, 0, sizeof(*o));
      o->bitmap = t->b[i].bitmap;
      o->cmd = t->b[i].cmd;
      o->state = t->b[i].state;
      o->style = t->b[i].style;
      o->data = t->b[i].data;
      o->str = (intptr_t)t->b[i].text;
      return 1;
    }
    case 0x419: return tb_index(t, (int)wp_);                      /* TB_COMMANDTOINDEX */
    case 0x401: case 0x402: case 0x403: case 0x404: case 0x411: { /* ENABLE/CHECK/PRESS/HIDE, SETSTATE */
      int i = tb_index(t, (int)wp_);
      uint8_t bit = m == 0x401 ? TBSTATE_ENABLED : m == 0x402 ? TBSTATE_CHECKED : m == 0x403 ? TBSTATE_PRESSED
                                                                                           : TBSTATE_HIDDEN;
      if (i < 0) return 0;
      if (m == 0x411) t->b[i].state = (uint8_t)lp;
      else if (lp & 0xFFFF) t->b[i].state |= bit;
      else t->b[i].state &= (uint8_t)~bit;
      win_invalidate(w);
      return 1;
    }
    case 0x412: { int i = tb_index(t, (int)wp_); return i < 0 ? -1 : t->b[i].state; } /* TB_GETSTATE */
    case 0x409: { int i = tb_index(t, (int)wp_); return i >= 0 && (t->b[i].state & TBSTATE_ENABLED); }
    case 0x40A: { int i = tb_index(t, (int)wp_); return i >= 0 && (t->b[i].state & TBSTATE_CHECKED); }
    case 0x40B: { int i = tb_index(t, (int)wp_); return i >= 0 && (t->b[i].state & TBSTATE_PRESSED); }
    case 0x40C: { int i = tb_index(t, (int)wp_); return i >= 0 && (t->b[i].state & TBSTATE_HIDDEN); }
    case 0x421: tb_autosize(w, t); win_invalidate(w); return 0;   /* TB_AUTOSIZE */
    case 0x41D:                                                    /* TB_GETITEMRECT */
    case 0x433: {                                                  /* TB_GETRECT */
      struct rect* r = (struct rect*)lp;
      int i = m == 0x41D ? (int)wp_ : tb_index(t, (int)wp_);
      tb_layout(w, t);
      if (!r || i < 0 || i >= t->n) return 0;
      r->left = t->b[i].x + 2;
      r->right = r->left + t->b[i].w;
      r->top = 2;
      r->bottom = 2 + t->btn_h;
      return 1;
    }
    case 0x453:                                                    /* TB_GETMAXSIZE */
    case 0x463: {                                                  /* TB_GETIDEALSIZE */
      int32_t* sz = (int32_t*)lp;
      tb_layout(w, t);
      if (!sz) return 0;
      if (m == 0x463) {
        if (wp_) sz[1] = t->btn_h + 4;
        else sz[0] = t->n ? t->b[t->n - 1].x + t->b[t->n - 1].w + 4 : 0;
      } else {
        sz[0] = t->n ? t->b[t->n - 1].x + t->b[t->n - 1].w + 4 : 0;
        sz[1] = t->btn_h + 4;
      }
      return 1;
    }
    case 0x430: case 0x434: case 0x436: {                          /* TB_SET(HOT/DISABLED)IMAGELIST */
      int k = m == 0x430 ? 0 : m == 0x434 ? 1 : 2;
      HANDLE old = t->il[k];
      typedef BOOL(WINAPI * gis)(HANDLE, int*, int*);
      gis f = (gis)builtin_lookup("ImageList_GetIconSize");
      int cx, cy;
      t->il[k] = (HANDLE)lp;
      if (k == 0 && lp && f && f((HANDLE)lp, &cx, &cy)) {
        t->bw = cx > 64 ? 64 : cx;
        t->bh = cy > 64 ? 64 : cy;
      }
      return (intptr_t)old;
    }
    case 0x431: return (intptr_t)t->il[0];                         /* TB_GETIMAGELIST */
    case 0x454: { DWORD o = t->exstyle; t->exstyle = (DWORD)lp; return o; } /* TB_SETEXTENDEDSTYLE */
    case 0x455: return t->exstyle;
    case 0x43C: case 0x457: case 0x427: case 0x41C: return 0;     /* SETMAXTEXTROWS, SETPADDING, SETROWS... */
    case 0x423: return 0;                                          /* TB_GETTOOLTIPS */
    case 0x43F: case 0x440: {                                      /* TB_GET/SETBUTTONINFOW */
      struct tbinfo* bi = (struct tbinfo*)lp;
      int i;
      if (!bi) return -1;
      i = (bi->mask & 0x80000000u) ? (int)wp_ : tb_index(t, (int)wp_);
      if (i < 0 || i >= t->n) return -1;
      if (m == 0x43F) {
        if (bi->mask & 0x20) bi->cmd = t->b[i].cmd;
        if (bi->mask & 1) bi->image = t->b[i].bitmap;
        if (bi->mask & 4) bi->state = t->b[i].state;
        if (bi->mask & 8) bi->style = t->b[i].style;
        if (bi->mask & 0x10) bi->param = t->b[i].data;
        if (bi->mask & 0x40) bi->cx = (uint16_t)t->b[i].w;
        if (bi->mask & 2) copy_out(t->b[i].text, bi->text, bi->cch);
        return i;
      }
      if (bi->mask & 0x20) t->b[i].cmd = bi->cmd;
      if (bi->mask & 1) t->b[i].bitmap = bi->image;
      if (bi->mask & 4) t->b[i].state = bi->state;
      if (bi->mask & 8) t->b[i].style = bi->style;
      if (bi->mask & 0x10) t->b[i].data = bi->param;
      if (bi->mask & 2) {
        k_free(t->b[i].text);
        t->b[i].text = dup16(bi->text);
      }
      win_invalidate(w);
      return 1;
    }
    case 0x44B: {                                                  /* TB_GETBUTTONTEXTW */
      int i = tb_index(t, (int)wp_);
      if (i < 0) return -1;
      return copy_out(t->b[i].text, (wchar16*)lp, lp ? 1024 : 0);
    }
  }
  if (m >= 0x400 && m < 0x480) return 0;
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ======================= msctls_statusbar32 ======================= */
#define SB_MAXPART 16
struct sb {
  int n;
  int right[SB_MAXPART];
  wchar16* text[SB_MAXPART];
  wchar16* simple_text;
  int simple;
};
static struct sb* sb_of(struct wwin* w) {
  struct sb** s = (struct sb**)state_slot(w);
  if (!*s) {
    *s = (struct sb*)wzalloc(sizeof(struct sb));
    if (*s) {
      (*s)->n = 1;
      (*s)->right[0] = -1;
    }
  }
  return *s;
}
static int sb_height(void) { return text_height() + 8; }
static void sb_place(struct wwin* w) {
  if (w->parent && !(w->style & (CCS_NORESIZE | CCS_NOPARENTALIGN)))
    move_child(w, 0, w->parent->h - sb_height(), w->parent->w, sb_height());
}
static void sb_paint(struct wwin* w) {
  struct sb* s = sb_of(w);
  struct wdc* d = &w->dc;
  int i, x = 0;
  dc_prepare(d, w);
  dc_fill(d, 0, 0, w->w, w->h, C_FACE);
  dc_line(d, 0, 0, w->w, 0, 0xD8D8D8);
  if (!s) return;
  if (s->simple) {
    cell(w, 6, 1, w->w - 4, w->h, s->simple_text, C_TEXT, DT_END_ELLIPSIS);
    return;
  }
  for (i = 0; i < s->n; i++) {
    int r = s->right[i] < 0 || s->right[i] > w->w ? w->w : s->right[i];
    cell(w, x + 6, 1, r - 4, w->h, s->text[i], C_TEXT, DT_END_ELLIPSIS);
    if (i < s->n - 1) dc_line(d, r - 1, 4, r - 1, w->h - 4, 0xC8C8C8);
    x = r;
  }
}
static intptr_t sb_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct sb* s = sb_of(w);
  if (!s) return wDefWindowProcW(w, m, wp_, lp);
  switch (m) {
    case WM_PAINT: sb_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: sb_place(w); return 0;
    case WM_CREATE: sb_place(w); break;
    case 0x404: {                                                  /* SB_SETPARTS */
      int i, n = (int)wp_;
      const int* a = (const int*)lp;
      if (n < 1 || n > SB_MAXPART || !a) return 0;
      for (i = 0; i < n; i++) s->right[i] = a[i];
      s->n = n;
      win_invalidate(w);
      return 1;
    }
    case 0x406: {                                                  /* SB_GETPARTS */
      int i, *a = (int*)lp;
      for (i = 0; a && i < s->n && i < (int)wp_; i++) a[i] = s->right[i];
      return s->n;
    }
    case 0x40B: {                                                  /* SB_SETTEXTW */
      int i = (int)(wp_ & 0xFF);
      if (i == 0xFF) {
        k_free(s->simple_text);
        s->simple_text = dup16((const wchar16*)lp);
      } else if (i < SB_MAXPART) {
        k_free(s->text[i]);
        s->text[i] = (wp_ & 0x1000) ? 0 : dup16((const wchar16*)lp); /* SBT_OWNERDRAW */
      }
      win_invalidate(w);
      return 1;
    }
    case 0x40D:                                                    /* SB_GETTEXTW */
    case 0x40C: {                                                  /* SB_GETTEXTLENGTHW */
      int i = (int)(wp_ & 0xFF);
      const wchar16* t = i < SB_MAXPART ? s->text[i] : 0;
      int n = t ? (int)wstrlen16(t) : 0;
      if (m == 0x40D && lp) copy_out(t, (wchar16*)lp, n + 1);
      return n;
    }
    case 0x409: s->simple = wp_ != 0; win_invalidate(w); return 0; /* SB_SIMPLE */
    case 0x40E: return s->simple;                                  /* SB_ISSIMPLE */
    case 0x40A: {                                                  /* SB_GETRECT */
      struct rect* r = (struct rect*)lp;
      int i = (int)wp_;
      if (!r || i < 0 || i >= s->n) return 0;
      r->left = i ? s->right[i - 1] : 0;
      r->right = s->right[i] < 0 ? w->w : s->right[i];
      r->top = 2;
      r->bottom = w->h - 1;
      return 1;
    }
    case 0x407: {                                                  /* SB_GETBORDERS */
      int* a = (int*)lp;
      if (a) {
        a[0] = 0;
        a[1] = 2;
        a[2] = 2;
      }
      return 1;
    }
    case 0x408: case 0x40F: case 0x410: case 0x411: return 1;     /* SETMINHEIGHT, SETICON, TIPTEXT */
    case 0x2001: return 0;                                         /* SB_SETBKCOLOR */
  }
  if (m >= 0x400 && m < 0x420) return 0;
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ======================= ReBarWindow32 ======================= */
struct rbinfo {
  DWORD size, mask, style, fg, bg;
  wchar16* text;
  DWORD cch;
  int image;
  HANDLE child;
  DWORD cxmin, cymin, cx;
  HANDLE bmp;
  DWORD id, cychild, cymax, cyint, cxideal;
  intptr_t param;
  DWORD cxheader;
};
struct rbband {
  struct wwin* child;
  DWORD style, id, cxmin, cymin, cx;
  wchar16* text;
  intptr_t param;
};
#define RB_MAX 12
struct rb {
  struct rbband b[RB_MAX];
  int n;
  int height;
};
#define RBBIM_STYLE 1
#define RBBIM_TEXT 4
#define RBBIM_CHILD 0x10
#define RBBIM_CHILDSIZE 0x20
#define RBBIM_SIZE 0x40
#define RBBIM_ID 0x100
#define RBBIM_LPARAM 0x400
#define RBBS_BREAK 1
#define RBBS_HIDDEN 8
#define RBN_HEIGHTCHANGE (-831)

static struct rb* rb_of(struct wwin* w) {
  struct rb** s = (struct rb**)state_slot(w);
  if (!*s) *s = (struct rb*)wzalloc(sizeof(struct rb));
  return *s;
}
static int rb_head(struct rbband* b) { return b->text && *b->text ? text_width(b->text, -1) + 14 : 6; }
static void rb_layout(struct wwin* w, struct rb* r) {
  int i = 0, y = 0, width = w->w, oldh = r->height;
  if (w->parent && !(w->style & (CCS_NORESIZE | CCS_NOPARENTALIGN))) width = w->parent->w;
  while (i < r->n) {
    int j, rowh = 0, fixed = 0, nb = 0, x = 0, first = i;
    /* uma linha: ate o proximo RBBS_BREAK */
    for (j = i; j < r->n; j++) {
      if (j > i && (r->b[j].style & RBBS_BREAK)) break;
      if (r->b[j].style & RBBS_HIDDEN) continue;
      if ((int)r->b[j].cymin + 4 > rowh) rowh = (int)r->b[j].cymin + 4;
      nb++;
    }
    if (rowh < 8) rowh = 8;
    for (i = first; i < j; i++) {
      struct rbband* b = &r->b[i];
      int bw, last = (i == j - 1);
      if (b->style & RBBS_HIDDEN) {
        if (b->child) b->child->visible = 0;
        continue;
      }
      bw = last ? width - x : (int)(b->cx ? b->cx : b->cxmin + rb_head(b));
      if (bw < (int)b->cxmin + rb_head(b)) bw = (int)b->cxmin + rb_head(b);
      if (b->child) move_child(b->child, x + rb_head(b), y + (rowh - (int)b->cymin) / 2, bw - rb_head(b) - 4,
                               (int)b->cymin);
      x += bw;
      fixed++;
    }
    (void)nb;
    (void)fixed;
    y += rowh;
    i = j;
  }
  r->height = y;
  if (w->parent && !(w->style & (CCS_NORESIZE | CCS_NOPARENTALIGN))) move_child(w, 0, 0, width, y);
  if (oldh != y) {
    struct nmhdr h;
    notify(w, &h, RBN_HEIGHTCHANGE);
  }
}
static void rb_paint(struct wwin* w) {
  struct rb* r = rb_of(w);
  struct wdc* d = &w->dc;
  int i;
  dc_prepare(d, w);
  dc_fill(d, 0, 0, w->w, w->h, C_FACE);
  if (!r) return;
  for (i = 0; i < r->n; i++) {
    struct rbband* b = &r->b[i];
    if ((b->style & RBBS_HIDDEN) || !b->child) continue;
    if (b->text && *b->text) cell(w, b->child->x - rb_head(b) + 4, b->child->y, b->child->x - 2,
                                  b->child->y + b->child->h, b->text, C_TEXT, 0);
  }
  dc_line(d, 0, w->h - 1, w->w, w->h - 1, 0xD8D8D8);
}
static void rb_apply(struct rbband* b, const struct rbinfo* in) {
  if (in->mask & RBBIM_STYLE) b->style = in->style;
  if (in->mask & RBBIM_TEXT) {
    k_free(b->text);
    b->text = dup16(in->text);
  }
  if (in->mask & RBBIM_CHILD) {
    b->child = win_of(in->child);
    if (b->child) b->child->visible = 1;
  }
  if (in->mask & RBBIM_CHILDSIZE) {
    b->cxmin = in->cxmin;
    b->cymin = in->cymin;
  }
  if (in->mask & RBBIM_SIZE) b->cx = in->cx;
  if (in->mask & RBBIM_ID) b->id = in->id;
  if (in->mask & RBBIM_LPARAM) b->param = in->param;
  if (b->child && !b->cymin) b->cymin = (DWORD)b->child->h;
}
static intptr_t rb_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct rb* r = rb_of(w);
  if (!r) return wDefWindowProcW(w, m, wp_, lp);
  switch (m) {
    case WM_PAINT: rb_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
      static int busy;
      if (!busy) {
        busy = 1;
        rb_layout(w, r);
        busy = 0;
      }
      return 0;
    }
    case WM_NOTIFY_:
    case WM_COMMAND:
      /* os filhos (toolbar, combo) falam com a janela do programa */
      return w->parent ? wsend(w->parent, m, wp_, lp) : 0;
    case 0x401:                                                    /* RB_INSERTBANDA */
    case 0x40A: {                                                  /* RB_INSERTBANDW */
      const struct rbinfo* in = (const struct rbinfo*)lp;
      int at = (int)(intptr_t)wp_;
      if (!in || r->n >= RB_MAX) return 0;
      if (at < 0 || at > r->n) at = r->n;
      memmove(&r->b[at + 1], &r->b[at], sizeof(struct rbband) * (size_t)(r->n - at));
      memset(&r->b[at], 0, sizeof(struct rbband));
      rb_apply(&r->b[at], in);
      r->n++;
      rb_layout(w, r);
      win_invalidate(w);
      return 1;
    }
    case 0x40B:                                                    /* RB_SETBANDINFOW */
      if ((int)wp_ < 0 || (int)wp_ >= r->n || !lp) return 0;
      rb_apply(&r->b[wp_], (const struct rbinfo*)lp);
      rb_layout(w, r);
      win_invalidate(w);
      return 1;
    case 0x41C: {                                                  /* RB_GETBANDINFOW */
      struct rbinfo* o = (struct rbinfo*)lp;
      struct rbband* b;
      if ((int)wp_ < 0 || (int)wp_ >= r->n || !o) return 0;
      b = &r->b[wp_];
      if (o->mask & RBBIM_STYLE) o->style = b->style;
      if (o->mask & RBBIM_CHILD) o->child = (HANDLE)b->child;
      if (o->mask & RBBIM_CHILDSIZE) {
        o->cxmin = b->cxmin;
        o->cymin = b->cymin;
      }
      if (o->mask & RBBIM_SIZE) o->cx = b->cx;
      if (o->mask & RBBIM_ID) o->id = b->id;
      if (o->mask & RBBIM_LPARAM) o->param = b->param;
      if (o->mask & RBBIM_TEXT) copy_out(b->text, o->text, (int)o->cch);
      return 1;
    }
    case 0x402: {                                                  /* RB_DELETEBAND */
      int i = (int)wp_;
      if (i < 0 || i >= r->n) return 0;
      k_free(r->b[i].text);
      memmove(&r->b[i], &r->b[i + 1], sizeof(struct rbband) * (size_t)(r->n - i - 1));
      r->n--;
      rb_layout(w, r);
      return 1;
    }
    case 0x423:                                                    /* RB_SHOWBAND */
      if ((int)wp_ < 0 || (int)wp_ >= r->n) return 0;
      if (lp) r->b[wp_].style &= ~(DWORD)RBBS_HIDDEN;
      else r->b[wp_].style |= RBBS_HIDDEN;
      if (r->b[wp_].child) r->b[wp_].child->visible = lp != 0;
      rb_layout(w, r);
      win_invalidate(w);
      return 1;
    case 0x40C: return r->n;                                       /* RB_GETBANDCOUNT */
    case 0x41B: rb_layout(w, r); return r->height;                /* RB_GETBARHEIGHT */
    case 0x40D: return 1;                                          /* RB_GETROWCOUNT */
    case 0x40E: return r->height;                                  /* RB_GETROWHEIGHT */
    case 0x410: {                                                  /* RB_IDTOINDEX */
      int i;
      for (i = 0; i < r->n; i++)
        if (r->b[i].id == (DWORD)wp_) return i;
      return -1;
    }
    case 0x409: {                                                  /* RB_GETRECT */
      struct rect* rc = (struct rect*)lp;
      int i = (int)wp_;
      if (!rc || i < 0 || i >= r->n || !r->b[i].child) return 0;
      rc->left = r->b[i].child->x - rb_head(&r->b[i]);
      rc->top = r->b[i].child->y;
      rc->right = r->b[i].child->x + r->b[i].child->w;
      rc->bottom = r->b[i].child->y + r->b[i].child->h;
      return 1;
    }
    case 0x417: rb_layout(w, r); return 1;                         /* RB_SIZETORECT */
    case 0x404: case 0x406: case 0x41E: case 0x41F: case 0x413: case 0x414: case 0x415: case 0x425: return 1;
  }
  if (m >= 0x400 && m < 0x440) return 0;
  return wDefWindowProcW(w, m, wp_, lp);
}

/* ======================= ComboBoxEx32: CBEM_* ======================= */
struct cbexitem {
  DWORD mask;
  intptr_t item;
  wchar16* text;
  int cch;
  int image, simage, overlay, indent;
  intptr_t param;
};
/* devolve 1 se tratou */
int comboex_msg(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp, intptr_t* res) {
  struct cbexitem* it = (struct cbexitem*)lp;
  switch (m) {
    case 0x40B: {                                                  /* CBEM_INSERTITEMW */
      static const wchar16 empty[1] = {0};
      const wchar16* t = it && (it->mask & 1) && (uintptr_t)it->text >= 0x10000 && (intptr_t)it->text != -1
                             ? it->text : empty;
      *res = it ? wsend(w, 0x14A, (uintptr_t)it->item, (intptr_t)t) : -1;
      if (it && (it->mask & 0x20) && *res >= 0) wsend(w, 0x151, (uintptr_t)*res, it->param);
      return 1;
    }
    case 0x40C:                                                    /* CBEM_SETITEMW */
      if (it && it->item == -1 && (it->mask & 1) && (uintptr_t)it->text >= 0x10000 && (intptr_t)it->text != -1)
        win_set_text(w, it->text);
      *res = 1;
      win_invalidate(w);
      return 1;
    case 0x40D:                                                    /* CBEM_GETITEMW */
      if (!it) {
        *res = 0;
        return 1;
      }
      if (it->item == -1) {
        if (it->mask & 1) win_get_text(w, it->text, it->cch);
      } else {
        if (it->mask & 1) wsend(w, 0x148, (uintptr_t)it->item, (intptr_t)it->text);
        if (it->mask & 0x20) it->param = wsend(w, 0x150, (uintptr_t)it->item, 0);
      }
      *res = 1;
      return 1;
    case 0x406: case 0x407: *res = (intptr_t)w; return 1;          /* CBEM_GETCOMBOCONTROL/EDITCONTROL */
    case 0x402: case 0x403: *res = 0; return 1;                    /* CBEM_SET/GETIMAGELIST */
    case 0x40E: case 0x409: *res = 0; return 1;                    /* CBEM_SET/GETEXTENDEDSTYLE */
    case 0x40A: *res = 0; return 1;                                /* CBEM_HASEDITCHANGED */
  }
  return 0;
}

intptr_t comctl_proc(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  switch (w->cls->kind) {
    case CK_LISTVIEW: return lv_proc(w, m, wp_, lp);
    case CK_TOOLBAR: return tb_proc(w, m, wp_, lp);
    case CK_STATUS: return sb_proc(w, m, wp_, lp);
    case CK_REBAR: return rb_proc(w, m, wp_, lp);
  }
  return wDefWindowProcW(w, m, wp_, lp);
}
