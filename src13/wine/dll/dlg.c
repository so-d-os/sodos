/*
 * dlg.c - Dialogos do Windows: modelos de recurso (DLGTEMPLATE e
 * DLGTEMPLATEEX), DialogBox/CreateDialog, DefDlgProc, IsDialogMessage
 * (Tab, Enter, Esc), funcoes Dlg* e MessageBox como dialogo de verdade.
 *
 * Unidades de dialogo: base de 6 x 13 pixels (MS Shell Dlg 8 pt a 96 dpi),
 * para os controles ficarem onde o programa desenhou.
 */
#include <so.h>
#include <string.h>
#include "gui_int.h"

#define DWLP_MSGRESULT 0
#define DWLP_DLGPROC 8
#define DM_GETDEFID (WM_USER + 0)
#define DM_SETDEFID (WM_USER + 1)
#define IDOK 1
#define IDCANCEL 2

typedef intptr_t (WINAPI * dlgproc_fn)(HANDLE, DWORD, uintptr_t, intptr_t);

int dlg_units_x(int du) { return (du * 6 + 2) / 4; }
int dlg_units_y(int du) { return (du * 13 + 4) / 8; }

static dlgproc_fn dlg_proc_of(struct wwin* w) {
  dlgproc_fn f;
  memcpy(&f, w->extra + DWLP_DLGPROC, sizeof(f));
  return f;
}
static void set_result(struct wwin* w, intptr_t r) { memcpy(w->extra + DWLP_MSGRESULT, &r, sizeof(r)); }

static struct wwin* dlg_item(struct wwin* d, uintptr_t id) {
  struct wwin* c;
  if (!d) return 0;
  for (c = d->child; c; c = c->sibling)
    if (c->id == id && !c->destroyed) return c;
  return 0;
}

/* Proxima filha com WS_TABSTOP (visivel e habilitada), ciclica. */
static struct wwin* next_tab(struct wwin* d, struct wwin* from, int back) {
  struct wwin* list[256];
  int n = 0, i, cur = -1;
  struct wwin* c;
  for (c = d->child; c && n < 256; c = c->sibling) {
    if (c == from) cur = n;
    if (c->visible && c->enabled && (c->style & WS_TABSTOP)) list[n++] = c;
    else if (c == from) cur = n - 1 + (back ? 1 : 0);
  }
  if (!n) return 0;
  if (cur < 0) return back ? list[n - 1] : list[0];
  for (i = 0; i < n; i++)
    if (list[i] == from) return list[(i + (back ? n - 1 : 1)) % n];
  return list[(cur + (back ? n : 1) + n) % n];
}

static struct wwin* default_button(struct wwin* d) {
  struct wwin* c;
  intptr_t r = wsend(d, DM_GETDEFID, 0, 0);
  if ((r >> 16) == 0x534B) return dlg_item(d, (uintptr_t)(r & 0xFFFF));
  for (c = d->child; c; c = c->sibling)
    if (c->cls->kind == CK_BUTTON && (c->style & 0xF) == 1) return c;
  return dlg_item(d, IDOK);
}

static void press(struct wwin* d, uintptr_t id) {
  struct wwin* b = dlg_item(d, id);
  if (b && !b->enabled) return;
  wsend(d, WM_COMMAND, ((uintptr_t)BN_CLICKED << 16) | (id & 0xFFFF), (intptr_t)b);
}

/* ---------- DefDlgProc ---------- */
intptr_t WINAPI wDefDlgProcW(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wwin* w = win_of(h);
  dlgproc_fn f;
  intptr_t r;
  if (!w) return 0;
  f = dlg_proc_of(w);
  if (f && m != WM_NCCREATE) {
    set_result(w, 0);
    r = f(h, m, wp_, lp);
    if (r) {
      if ((m >= WM_CTLCOLOREDIT && m <= WM_CTLCOLORSTATIC) || m == WM_INITDIALOG || m == 0x0039 /* COMPAREITEM */ ||
          m == 0x002E /* VKEYTOITEM */ || m == 0x002F /* CHARTOITEM */ || m == 0x0037 /* QUERYDRAGICON */)
        return r;
      memcpy(&r, w->extra + DWLP_MSGRESULT, sizeof(r));
      return r;
    }
  }
  switch (m) {
    case WM_ERASEBKGND: {
      struct wdc* d = dc_of((HANDLE)wp_);
      DWORD c = sys_color(15);
      intptr_t br = f ? f(h, WM_CTLCOLORDLG, wp_, (intptr_t)h) : 0;
      if (br) {
        int null;
        DWORD bc = brush_color((HANDLE)br, &null);
        if (!null) c = bc;
      }
      if (d) dc_fill(d, 0, 0, w->w, w->h, c);
      return 1;
    }
    case WM_CLOSE:
      press(w, IDCANCEL);
      return 0;
    case WM_NEXTDLGCTL: {
      struct wwin* t = win_top(w);
      struct wwin* n = lp ? win_of((HANDLE)wp_) : next_tab(w, t ? t->focus : 0, wp_ != 0);
      if (n) win_set_focus(n);
      return 0;
    }
    case DM_GETDEFID: {
      struct wwin* c;
      for (c = w->child; c; c = c->sibling)
        if (c->cls->kind == CK_BUTTON && (c->style & 0xF) == 1) return (intptr_t)(0x534B0000u | (c->id & 0xFFFF));
      return 0;
    }
    case DM_SETDEFID: {
      struct wwin* c;
      for (c = w->child; c; c = c->sibling)
        if (c->cls->kind == CK_BUTTON && ((c->style & 0xF) == 0 || (c->style & 0xF) == 1)) {
          c->style = (c->style & ~0xFu) | (c->id == wp_ ? 1u : 0u);
          win_invalidate(c);
        }
      return 1;
    }
    case WM_GETDLGCODE: return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      return 0;
  }
  return wDefWindowProcW(h, m, wp_, lp);
}

/* ---------- IsDialogMessage ---------- */
BOOL WINAPI wIsDialogMessageW(HANDLE h, struct msg* m) {
  struct wwin* d = win_of(h);
  struct wwin* t;
  struct wwin* target = win_of(m->hwnd);
  struct wwin* p;
  if (!d || !target) return 0;
  for (p = target; p && p != d; p = p->parent) {
  }
  if (!p) return 0;
  t = win_top(d);
  if (m->message == WM_KEYDOWN) {
    intptr_t code = wsend(target, WM_GETDLGCODE, m->wparam, (intptr_t)m);
    switch (m->wparam) {
      case 0x09: { /* Tab */
        struct wwin* n = next_tab(d, t->focus, 0);
        if (n) {
          win_set_focus(n);
          if (n->cls->kind == CK_EDIT) wsend(n, 0xB1, 0, -1); /* EM_SETSEL: tudo */
        }
        return 1;
      }
      case 0x0D: { /* Enter */
        struct wwin* b;
        if (code & 4) break; /* DLGC_WANTALLKEYS: o controle quer o Enter */
        if (t->focus && t->focus->cls->kind == CK_BUTTON && ((t->focus->style & 0xF) <= 1)) {
          press(d, t->focus->id);
          return 1;
        }
        b = default_button(d);
        press(d, b ? b->id : IDOK);
        return 1;
      }
      case 0x1B: /* Esc */
        press(d, IDCANCEL);
        return 1;
    }
  } else if (m->message == WM_CHAR) {
    if (m->wparam == '\t' || m->wparam == 27) return 1;
    if (m->wparam == '\r') {
      intptr_t code = wsend(target, WM_GETDLGCODE, m->wparam, (intptr_t)m);
      if (!(code & 4)) return 1;
    }
  }
  wDispatchMessageW(m);
  return 1;
}

/* ---------- modelos ---------- */
static const uint8_t* skip_sz_or_ord(const uint8_t* p, uintptr_t* out) {
  const uint16_t* w = (const uint16_t*)p;
  if (w[0] == 0) {
    if (out) *out = 0;
    return p + 2;
  }
  if (w[0] == 0xFFFF) {
    if (out) *out = w[1];
    return p + 4;
  }
  if (out) *out = (uintptr_t)p;
  while (*w) w++;
  return (const uint8_t*)(w + 1);
}
static const uint8_t* align4(const uint8_t* p, const uint8_t* base) {
  return base + ((p - base + 3) & ~3);
}

static struct wwin* dialog_create(struct wmod* mod, const uint8_t* tpl, struct wwin* owner, dlgproc_fn proc,
                                  intptr_t param) {
  const uint8_t* p = tpl;
  int ex = *(const uint16_t*)(p + 2) == 0xFFFF;
  DWORD style, exstyle;
  int n, x, y, cx, cy, i;
  uintptr_t menu, cls, title;
  struct wwin* d;
  static const wchar16 dlgcls[] = {'#', '3', '2', '7', '7', '0', 0};
  struct wwin* first_tab = 0;
  if (ex) {
    exstyle = *(const DWORD*)(p + 8);
    style = *(const DWORD*)(p + 12);
    n = *(const uint16_t*)(p + 16);
    x = *(const int16_t*)(p + 18);
    y = *(const int16_t*)(p + 20);
    cx = *(const int16_t*)(p + 22);
    cy = *(const int16_t*)(p + 24);
    p += 26;
  } else {
    style = *(const DWORD*)p;
    exstyle = *(const DWORD*)(p + 4);
    n = *(const uint16_t*)(p + 8);
    x = *(const int16_t*)(p + 10);
    y = *(const int16_t*)(p + 12);
    cx = *(const int16_t*)(p + 14);
    cy = *(const int16_t*)(p + 16);
    p += 18;
  }
  p = skip_sz_or_ord(p, &menu);
  p = skip_sz_or_ord(p, &cls);
  p = skip_sz_or_ord(p, &title);
  if (style & 0x48) { /* DS_SETFONT | DS_SHELLFONT */
    p += ex ? 6 : 2;  /* tamanho (+ peso, italico, charset) */
    p = skip_sz_or_ord(p, 0);
  }
  d = win_create(exstyle, cls >= 0x10000 ? (const wchar16*)cls : dlgcls, title >= 0x10000 ? (const wchar16*)title : 0,
                 (style & ~WS_VISIBLE) | ((style & WS_CHILD) ? 0 : 0), dlg_units_x(x), dlg_units_y(y),
                 dlg_units_x(cx), dlg_units_y(cy), owner, 0, 0);
  if (!d) return 0;
  /* classe propria do programa: o dialogo continua com DefDlgProc por tras */
  memcpy(d->extra + DWLP_DLGPROC, &proc, sizeof(proc));
  for (i = 0; i < n; i++) {
    DWORD is, iex;
    int ix, iy, icx, icy;
    uintptr_t id, icls, ititle;
    uint16_t extra;
    struct wwin* c;
    p = align4(p, tpl);
    if (ex) {
      iex = *(const DWORD*)(p + 4);
      is = *(const DWORD*)(p + 8);
      ix = *(const int16_t*)(p + 12);
      iy = *(const int16_t*)(p + 14);
      icx = *(const int16_t*)(p + 16);
      icy = *(const int16_t*)(p + 18);
      id = *(const DWORD*)(p + 20);
      p += 24;
    } else {
      is = *(const DWORD*)p;
      iex = *(const DWORD*)(p + 4);
      ix = *(const int16_t*)(p + 8);
      iy = *(const int16_t*)(p + 10);
      icx = *(const int16_t*)(p + 12);
      icy = *(const int16_t*)(p + 14);
      id = *(const uint16_t*)(p + 16);
      p += 18;
    }
    p = skip_sz_or_ord(p, &icls);
    p = skip_sz_or_ord(p, &ititle);
    extra = *(const uint16_t*)p;
    p += 2;
    if (extra) p += extra;
    c = win_create(iex, (const wchar16*)icls,
                   ititle >= 0x10000 ? (const wchar16*)ititle : (const wchar16*)(ititle ? ititle : 0),
                   is | WS_CHILD, dlg_units_x(ix), dlg_units_y(iy), dlg_units_x(icx), dlg_units_y(icy), d, id, 0);
    if (!c) {
      if (wine_debug) wlogx("dialogo: controle nao criado, id ", id);
      continue;
    }
    if (c->res_id && c->cls->kind == CK_STATIC && (is & 0x1F) == 0xE && mod) { /* SS_BITMAP */
      uint32_t size = 0;
      const uint8_t* e = pe_find_resource(mod, 2, c->res_id, &size);
      if (e) c->image = bitmap_from_dib(mod->base + *(const uint32_t*)e, 0);
    }
    if (!first_tab && (is & WS_TABSTOP) && !(is & WS_DISABLED)) first_tab = c;
  }
  if (wsend(d, WM_INITDIALOG, (uintptr_t)first_tab, param) && first_tab) {
    win_set_focus(first_tab);
    if (first_tab->cls->kind == CK_EDIT) wsend(first_tab, 0xB1, 0, -1);
  }
  if (style & WS_VISIBLE) win_show(d, 1);
  return d;
}

static const uint8_t* find_template(HANDLE inst, const wchar16* name, struct wmod** mod) {
  struct wmod* m = pe_from_handle(inst);
  uint32_t size = 0;
  const uint8_t* e;
  *mod = m;
  if (!m) return 0;
  e = pe_find_resource(m, 5, (uintptr_t)name, &size); /* RT_DIALOG */
  if (!e) {
    wlogx("dialogo nao achado nos recursos, id ", (uintptr_t)name);
    return 0;
  }
  return m->base + *(const uint32_t*)e;
}

/* Laco modal do dialogo d. */
static intptr_t dialog_run(struct wwin* d) {
  struct wgui* g = gui_get();
  struct wwin* owner = d->owner ? win_top(d->owner) : 0;
  struct msg m;
  intptr_t r;
  int owner_was = owner && owner->enabled;
  if (owner_was) wEnableWindow(owner, 0);
  d->modal = 1;
  win_show(d, 1);
  if (!g->canvas_open) {
    /* fora do WIN (prompt de texto): sem tela para o dialogo */
    k_puts("wine: este programa abre janelas; rode-o pelo desktop (WIN)\n");
    wlog("dialogo sem janela do WIN: cancelado", 0);
    d->dlg_end = 1;
    d->dlg_result = IDCANCEL;
  }
  while (!d->dlg_end && !d->destroyed) {
    if (!wGetMessageW(&m, 0, 0, 0)) {
      /* WM_QUIT no meio do dialogo: devolve para o laco de fora */
      typedef void (WINAPI * pq_fn)(int);
      pq_fn pq = (pq_fn)builtin_lookup("PostQuitMessage");
      if (pq) pq((int)m.wparam);
      break;
    }
    if (!wIsDialogMessageW(d, &m)) wDispatchMessageW(&m);
  }
  r = d->dlg_result;
  if (owner_was && !owner->destroyed) wEnableWindow(owner, 1);
  win_destroy(d);
  return r;
}

static WINAPI intptr_t DialogBoxIndirectParamW(HANDLE inst, const uint8_t* tpl, HANDLE owner, dlgproc_fn proc,
                                               intptr_t lp) {
  struct wwin* d = dialog_create(pe_from_handle(inst), tpl, win_of(owner), proc, lp);
  if (!d) return -1;
  return dialog_run(d);
}
static WINAPI intptr_t DialogBoxParamW(HANDLE inst, const wchar16* name, HANDLE owner, dlgproc_fn proc, intptr_t lp) {
  struct wmod* m;
  const uint8_t* tpl = find_template(inst, name, &m);
  struct wwin* d;
  if (!tpl) return -1;
  d = dialog_create(m, tpl, win_of(owner), proc, lp);
  if (!d) return -1;
  return dialog_run(d);
}
static WINAPI HANDLE CreateDialogIndirectParamW(HANDLE inst, const uint8_t* tpl, HANDLE owner, dlgproc_fn proc,
                                                intptr_t lp) {
  struct wwin* d = dialog_create(pe_from_handle(inst), tpl, win_of(owner), proc, lp);
  return d;
}
static WINAPI HANDLE CreateDialogParamW(HANDLE inst, const wchar16* name, HANDLE owner, dlgproc_fn proc,
                                        intptr_t lp) {
  struct wmod* m;
  const uint8_t* tpl = find_template(inst, name, &m);
  if (!tpl) return 0;
  return dialog_create(m, tpl, win_of(owner), proc, lp);
}
static WINAPI BOOL EndDialog(HANDLE h, intptr_t r) {
  struct wwin* d = win_of(h);
  if (!d) return 0;
  d->dlg_end = 1;
  d->dlg_result = r;
  if (!d->modal) win_show(d, 0);
  wsignal();
  return 1;
}

/* ---------- Dlg* ---------- */
static WINAPI HANDLE GetDlgItem(HANDLE h, int id) { return dlg_item(win_of(h), (uintptr_t)(unsigned)id); }
static WINAPI BOOL SetDlgItemTextW(HANDLE h, int id, const wchar16* t) {
  struct wwin* c = dlg_item(win_of(h), (uintptr_t)(unsigned)id);
  if (!c) return 0;
  wsend(c, WM_SETTEXT, 0, (intptr_t)t);
  return 1;
}
static WINAPI DWORD GetDlgItemTextW(HANDLE h, int id, wchar16* buf, int n) {
  struct wwin* c = dlg_item(win_of(h), (uintptr_t)(unsigned)id);
  if (!c) {
    if (buf && n > 0) buf[0] = 0;
    return 0;
  }
  return (DWORD)wsend(c, WM_GETTEXT, (uintptr_t)n, (intptr_t)buf);
}
static WINAPI BOOL SetDlgItemInt(HANDLE h, int id, DWORD v, BOOL sgn) {
  wchar16 buf[16];
  char t[16];
  int i = 0, k = 0;
  long long x = sgn ? (long long)(int)v : (long long)v;
  if (x < 0) {
    t[k++] = '-';
    x = -x;
  }
  {
    char r[16];
    do {
      r[i++] = (char)('0' + x % 10);
      x /= 10;
    } while (x);
    while (i) t[k++] = r[--i];
  }
  for (i = 0; i < k; i++) buf[i] = (wchar16)t[i];
  buf[k] = 0;
  return SetDlgItemTextW(h, id, buf);
}
static WINAPI DWORD GetDlgItemInt(HANDLE h, int id, BOOL* ok, BOOL sgn) {
  wchar16 buf[32];
  int i = 0, neg = 0;
  long long v = 0;
  GetDlgItemTextW(h, id, buf, 32);
  if (sgn && buf[0] == '-') {
    neg = 1;
    i = 1;
  }
  for (; buf[i] >= '0' && buf[i] <= '9'; i++) v = v * 10 + (buf[i] - '0');
  if (ok) *ok = i > neg;
  return (DWORD)(neg ? -v : v);
}
static WINAPI intptr_t SendDlgItemMessageW(HANDLE h, int id, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wwin* c = dlg_item(win_of(h), (uintptr_t)(unsigned)id);
  return c ? wsend(c, m, wp_, lp) : 0;
}
static WINAPI BOOL CheckDlgButton(HANDLE h, int id, DWORD st) {
  return SendDlgItemMessageW(h, id, 0xF1, st, 0) == 0 || 1;
}
static WINAPI DWORD IsDlgButtonChecked(HANDLE h, int id) { return (DWORD)SendDlgItemMessageW(h, id, 0xF0, 0, 0); }
static WINAPI BOOL CheckRadioButton(HANDLE h, int first, int last, int check) {
  int i;
  for (i = first; i <= last; i++) SendDlgItemMessageW(h, i, 0xF1, i == check, 0);
  return 1;
}
static WINAPI BOOL MapDialogRect(HANDLE h, struct rect* r) {
  (void)h;
  r->left = dlg_units_x(r->left);
  r->right = dlg_units_x(r->right);
  r->top = dlg_units_y(r->top);
  r->bottom = dlg_units_y(r->bottom);
  return 1;
}
static WINAPI long GetDialogBaseUnits(void) { return (13 << 16) | 6; }
static WINAPI HANDLE GetNextDlgTabItem(HANDLE h, HANDLE ctl, BOOL prev) {
  return next_tab(win_of(h), win_of(ctl), prev);
}
static WINAPI HANDLE GetNextDlgGroupItem(HANDLE h, HANDLE ctl, BOOL prev) {
  return next_tab(win_of(h), win_of(ctl), prev);
}

/* ---------- MessageBox ---------- */
static WINAPI intptr_t msgbox_proc(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  (void)lp;
  if (m == WM_COMMAND && (wp_ >> 16) == BN_CLICKED) {
    EndDialog(h, (intptr_t)(wp_ & 0xFFFF));
    return 1;
  }
  if (m == WM_INITDIALOG) return 1;
  if (m == WM_CTLCOLORSTATIC) return (intptr_t)wGetStockObject(0); /* WHITE_BRUSH */
  return 0;
}

static int msgbox_fallback(const wchar16* text, const wchar16* cap, DWORD type) {
  char* t = wdup16to8(text);
  char* c = wdup16to8(cap);
  k_puts("[");
  k_puts(c ? c : "Mensagem");
  k_puts("] ");
  k_puts(t ? t : "");
  k_puts("\n");
  k_free(t);
  k_free(c);
  switch (type & 15) {
    case 3: case 4: return 6; /* IDYES */
    case 5: return 2;
    case 2: return 3;
  }
  return 1;
}

static WINAPI int MessageBoxW(HANDLE owner, const wchar16* text, const wchar16* cap, DWORD type) {
  static const wchar16 s_ok[] = {'O', 'K', 0}, s_cancel[] = {'C', 'a', 'n', 'c', 'e', 'l', 0},
                       s_yes[] = {'&', 'Y', 'e', 's', 0}, s_no[] = {'&', 'N', 'o', 0},
                       s_abort[] = {'&', 'A', 'b', 'o', 'r', 't', 0}, s_retry[] = {'&', 'R', 'e', 't', 'r', 'y', 0},
                       s_ignore[] = {'&', 'I', 'g', 'n', 'o', 'r', 'e', 0},
                       s_try[] = {'&', 'T', 'r', 'y', ' ', 'A', 'g', 'a', 'i', 'n', 0},
                       s_cont[] = {'&', 'C', 'o', 'n', 't', 'i', 'n', 'u', 'e', 0};
  static const wchar16 dlgcls[] = {'#', '3', '2', '7', '7', '0', 0};
  static const wchar16 stcls[] = {'S', 't', 'a', 't', 'i', 'c', 0};
  static const wchar16 btcls[] = {'B', 'u', 't', 't', 'o', 'n', 0};
  static const wchar16 error[] = {'E', 'r', 'r', 'o', 'r', 0};
  const wchar16* labels[3];
  int ids[3], nb = 0, i, tw, th, bw = 88, bh = 26, cw, ch, def;
  struct wwin* d;
  struct rect r;
  dlgproc_fn proc = msgbox_proc;
  {
    char* t8 = wdup16to8(text);
    char* c8 = wdup16to8(cap);
    wlog("MessageBox: ", c8 ? c8 : "");
    wlog("  ", t8 ? t8 : "");
    k_free(t8);
    k_free(c8);
  }
  switch (type & 15) {
    case 1: labels[0] = s_ok; ids[0] = 1; labels[1] = s_cancel; ids[1] = 2; nb = 2; break;
    case 2: labels[0] = s_abort; ids[0] = 3; labels[1] = s_retry; ids[1] = 4; labels[2] = s_ignore; ids[2] = 5; nb = 3; break;
    case 3: labels[0] = s_yes; ids[0] = 6; labels[1] = s_no; ids[1] = 7; labels[2] = s_cancel; ids[2] = 2; nb = 3; break;
    case 4: labels[0] = s_yes; ids[0] = 6; labels[1] = s_no; ids[1] = 7; nb = 2; break;
    case 5: labels[0] = s_retry; ids[0] = 4; labels[1] = s_cancel; ids[1] = 2; nb = 2; break;
    case 6: labels[0] = s_cancel; ids[0] = 2; labels[1] = s_try; ids[1] = 10; labels[2] = s_cont; ids[2] = 11; nb = 3; break;
    default: labels[0] = s_ok; ids[0] = 1; nb = 1; break;
  }
  def = (int)((type >> 8) & 3);
  if (def >= nb) def = 0;
  if (!text) text = (const wchar16*)u"";
  if (!text_ready()) return msgbox_fallback(text, cap, type);
  tw = text_width(text, -1);
  if (tw > 420) tw = 420;
  if (tw < 160) tw = 160;
  r.left = 0;
  r.top = 0;
  r.right = tw;
  r.bottom = 10000;
  th = text_box(0, 0, &r, text, -1, 0, 0, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
  cw = tw + 48;
  if (cw < nb * (bw + 8) + 24) cw = nb * (bw + 8) + 24;
  ch = th + 30 + 24 + bh + 20;
  d = win_create(0, dlgcls, cap ? cap : error, WS_POPUP | WS_CAPTION, 0, 0, cw, ch, win_of(owner), 0, 0);
  if (!d) return msgbox_fallback(text, cap, type);
  memcpy(d->extra + DWLP_DLGPROC, &proc, sizeof(proc));
  /* fundo branco em cima e cinza na faixa dos botoes, como no Windows */
  {
    static const wchar16 empty[] = {0};
    struct wwin* white = win_create(0, stcls, empty, WS_CHILD | WS_VISIBLE | 6, 0, 0, cw, ch - bh - 22, d, 0xFFFF, 0);
    (void)white;
  }
  win_create(0, stcls, text, WS_CHILD | WS_VISIBLE | 0x80, 24, 24, tw + 4, th + 4, d, 0xFFFE, 0);
  for (i = 0; i < nb; i++) {
    int x = cw - (nb - i) * (bw + 8) - 4;
    struct wwin* b = win_create(0, btcls, labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == def ? 1u : 0u), x,
                                ch - bh - 11, bw, bh, d, (uintptr_t)ids[i], 0);
    if (b && i == def) win_set_focus(b);
  }
  return (int)dialog_run(d);
}
static WINAPI int MessageBoxA(HANDLE w, const char* text, const char* cap, DWORD type) {
  wchar16* t = wdup8to16(text ? text : "");
  wchar16* c = cap ? wdup8to16(cap) : 0;
  int r = MessageBoxW(w, t, c, type);
  k_free(t);
  k_free(c);
  return r;
}
static WINAPI int MessageBoxExW(HANDLE w, const wchar16* t, const wchar16* c, DWORD type, uint16_t lang) {
  (void)lang;
  return MessageBoxW(w, t, c, type);
}

/* ---------- LoadString ---------- */
static WINAPI int LoadStringW(HANDLE inst, DWORD id, wchar16* buf, int n) {
  struct wmod* m = pe_from_handle(inst);
  uint32_t size = 0;
  const uint8_t* e = m ? pe_find_resource(m, 6, (uintptr_t)(id / 16 + 1), &size) : 0; /* RT_STRING */
  const uint16_t* p;
  int i, len;
  if (!e) {
    if (buf && n > 0) buf[0] = 0;
    return 0;
  }
  p = (const uint16_t*)(m->base + *(const uint32_t*)e);
  for (i = 0; i < (int)(id % 16); i++) p += 1 + p[0];
  len = p[0];
  if (n == 0) { /* devolve o ponteiro para o recurso */
    if (buf) *(const uint16_t**)buf = p + 1;
    return len;
  }
  if (!buf) return 0;
  if (len >= n) len = n - 1;
  memcpy(buf, p + 1, (size_t)len * 2);
  buf[len] = 0;
  return len;
}
static WINAPI int LoadStringA(HANDLE inst, DWORD id, char* buf, int n) {
  wchar16 tmp[1024];
  int len = LoadStringW(inst, id, tmp, 1024), k;
  if (!buf || n <= 0) return 0;
  k = wutf16_to_8(tmp, len, buf, n - 1);
  buf[k] = 0;
  return k;
}

#define S(n) {#n, (void*)n}
const struct wine_sym dlg_syms[] = {
    {"DefDlgProcW", (void*)wDefDlgProcW}, {"IsDialogMessageW", (void*)wIsDialogMessageW},
    {"IsDialogMessage", (void*)wIsDialogMessageW}, S(DialogBoxIndirectParamW), S(DialogBoxParamW),
    S(CreateDialogIndirectParamW), S(CreateDialogParamW), S(EndDialog), S(GetDlgItem), S(SetDlgItemTextW),
    S(GetDlgItemTextW), S(SetDlgItemInt), S(GetDlgItemInt), S(SendDlgItemMessageW), S(CheckDlgButton),
    S(IsDlgButtonChecked), S(CheckRadioButton), S(MapDialogRect), S(GetDialogBaseUnits), S(GetNextDlgTabItem),
    S(GetNextDlgGroupItem), S(MessageBoxW), S(MessageBoxA), S(MessageBoxExW), S(LoadStringW), S(LoadStringA),
    {0, 0}};
