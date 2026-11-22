/*
 * user.c - USER32: janelas, classes, mensagens, foco, pintura.
 *
 * O WIN da uma janela por programa (o canvas, gui_int.h). As janelas de topo
 * do processo sao compostas nele: a primeira visivel sem moldura, centrada;
 * as outras (MessageBox, dialogos por cima) com moldura e titulo desenhados
 * aqui. Os eventos do WIN (rato, teclado) sao convertidos em mensagens para
 * a janela certa: a de topo debaixo do rato e, dentro dela, a filha mais
 * funda; o teclado vai para a janela com o foco.
 *
 * Abrir e fechar a janela do WIN so a tarefa principal do processo pode
 * (video/visual.c): as threads pedem por wgui_world_service(), chamado pelo
 * laco de espera do main.c.
 */
#include <so.h>
#include <string.h>
#include "gui_int.h"

#define FRAME_TITLE 26
#define FRAME_BORDER 1

/* ---------- estado ---------- */
struct wgui* gui_get(void) {
  struct wproc* p = wp();
  struct wgui* g = (struct wgui*)p->user;
  int i;
  static const DWORD brush_col[5] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
  if (g) return g;
  wlock();
  g = (struct wgui*)p->user;
  if (!g) {
    g = (struct wgui*)wzalloc(sizeof(*g));
    if (g) {
      for (i = 0; i < 20; i++) {
        g->stock[i].magic = GDI_MAGIC;
        g->stock[i].stock = 1;
        g->stock[i].kind = i <= 5 ? G_BRUSH : i <= 8 ? G_PEN : G_FONT;
      }
      for (i = 0; i < 5; i++) g->stock[i].color = brush_col[i];
      g->stock[5].null = 1;          /* NULL_BRUSH */
      g->stock[6].color = 0xFFFFFF;  /* WHITE_PEN */
      g->stock[7].color = 0x000000;  /* BLACK_PEN */
      g->stock[8].null = 1;          /* NULL_PEN */
      g->stock[18].kind = G_BRUSH;   /* DC_BRUSH */
      g->stock[18].color = 0xFFFFFF;
      g->stock[19].kind = G_BITMAP;  /* bitmap padrao do DC de memoria */
      p->user = g;
    }
  }
  wunlock();
  return g;
}

struct wwin* win_of(HANDLE h) {
  struct wwin* w = (struct wwin*)h;
  struct wgui* g = (struct wgui*)wp()->user;
  struct wwin* i;
  if (!g || !w || (uintptr_t)w < 0x10000) return 0;
  for (i = g->wins; i; i = i->next)
    if (i == w) return w->magic == WIN_MAGIC && !w->destroyed ? w : 0;
  return 0;
}

/* ---------- janelas de outros processos ----------
 * O processo GPU do Chromium desenha na janela do browser: recebe o HWND pela
 * IPC e faz GetDC + BitBlt (SoftwareOutputDeviceWin). O WINE.DLL e um so para
 * todos os processos e a memoria e um espaco so, entao o ponteiro vale em
 * qualquer um; este registro diz quais HWNDs existem e de que processo (gui)
 * sao. Antes o GetDC de um HWND alheio dava o DC da tela e a janela do Claude
 * ficava branca. Mexe-se nele com wlock(). */
#define XWIN_MAX 512
static struct {
  struct wwin* w;
  struct wgui* g;
} xwins[XWIN_MAX];
static int nxwins;
static void xwin_add_locked(struct wwin* w, struct wgui* g) {
  int i;
  for (i = 0; i < nxwins; i++)
    if (!xwins[i].w) break;
  if (i == nxwins) {
    if (nxwins >= XWIN_MAX) return;
    nxwins++;
  }
  xwins[i].w = w;
  xwins[i].g = g;
}
/* processo saindo: as janelas dele (memoria do processo) deixam de valer */
void wgui_proc_exit(void) {
  struct wgui* g = (struct wgui*)wp()->user;
  int i;
  if (!g) return;
  wlock();
  for (i = 0; i < nxwins; i++)
    if (xwins[i].g == g) xwins[i].w = 0;
  wunlock();
}
/* HWND de qualquer processo; *gp = gui do dono */
struct wwin* win_any(HANDLE h, struct wgui** gp) {
  struct wwin* w = win_of(h);
  int i;
  if (w) {
    if (gp) *gp = (struct wgui*)wp()->user;
    return w;
  }
  if (!h || (uintptr_t)h < 0x10000) return 0;
  wlock();
  for (i = 0; i < nxwins; i++)
    if (xwins[i].w == (struct wwin*)h) {
      w = xwins[i].w;
      if (w->magic != WIN_MAGIC || w->destroyed) w = 0;
      if (w && gp) *gp = xwins[i].g;
      break;
    }
  wunlock();
  return w;
}

struct wwin* win_top(struct wwin* w) {
  while (w && w->parent) w = w->parent;
  return w;
}

/* Origem do cliente de w na superficie da janela de topo dela. */
void win_abs_client(struct wwin* w, int* x, int* y) {
  int ax = 0, ay = 0;
  while (w && w->parent) {
    ax += w->x;
    ay += w->y;
    w = w->parent;
  }
  *x = ax;
  *y = ay;
}

static uintptr_t me(void) { return (uintptr_t)wteb(); }

/* ---------- classes ---------- */
static int wcs_ieq(const wchar16* a, const wchar16* b) {
  for (;; a++, b++) {
    wchar16 x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (wchar16)(x - 32);
    if (y >= 'a' && y <= 'z') y = (wchar16)(y - 32);
    if (x != y) return 0;
    if (!x) return 1;
  }
}
static int wcs_ieq8(const wchar16* a, const char* b) {
  for (;; a++, b++) {
    wchar16 x = *a;
    char y = *b;
    if (x >= 'a' && x <= 'z') x = (wchar16)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != (wchar16)(uint8_t)y) return 0;
    if (!x) return 1;
  }
}
static wchar16* wcs_dup(const wchar16* s) {
  size_t n = (wstrlen16(s) + 1) * 2;
  wchar16* d = (wchar16*)k_malloc((unsigned)n);
  if (d) memcpy(d, s, n);
  return d;
}

/* Nome (ou atomo 0x80..0x85 dos dialogos) -> controle embutido. */
int wndclass_kind(const wchar16* name) {
  uintptr_t a = (uintptr_t)name;
  if (a < 0x10000) {
    switch (a) {
      case 0x80: return CK_BUTTON;
      case 0x81: return CK_EDIT;
      case 0x82: return CK_STATIC;
      case 0x83: return CK_LISTBOX;
      case 0x85: return CK_COMBO;
      case 0x8002: return CK_DIALOG; /* WC_DIALOG */
    }
    return CK_OTHER;
  }
  if (wcs_ieq8(name, "Button")) return CK_BUTTON;
  if (wcs_ieq8(name, "Static")) return CK_STATIC;
  if (wcs_ieq8(name, "Edit") || wcs_ieq8(name, "RichEdit20W") || wcs_ieq8(name, "RichEdit20A") ||
      wcs_ieq8(name, "RICHEDIT50W") || wcs_ieq8(name, "RichEdit"))
    return CK_EDIT;
  if (wcs_ieq8(name, "ListBox")) return CK_LISTBOX;
  if (wcs_ieq8(name, "ComboBox") || wcs_ieq8(name, "ComboBoxEx32")) return CK_COMBO;
  if (wcs_ieq8(name, "msctls_progress32")) return CK_PROGRESS;
  if (wcs_ieq8(name, "SysLink")) return CK_LINK;
  if (wcs_ieq8(name, "SysListView32")) return CK_LISTVIEW;
  if (wcs_ieq8(name, "ToolbarWindow32")) return CK_TOOLBAR;
  if (wcs_ieq8(name, "msctls_statusbar32")) return CK_STATUS;
  if (wcs_ieq8(name, "ReBarWindow32")) return CK_REBAR;
  if (wcs_ieq8(name, "#32770")) return CK_DIALOG;
  return CK_APP;
}

static struct wclass* class_find(struct wgui* g, const wchar16* name) {
  struct wclass* c;
  if ((uintptr_t)name < 0x10000) {
    for (c = g->classes; c; c = c->next)
      if ((uintptr_t)c->name == (uintptr_t)name || (c->atom && c->atom == (uintptr_t)name)) return c;
    return 0;
  }
  for (c = g->classes; c; c = c->next)
    if ((uintptr_t)c->name >= 0x10000 && wcs_ieq(c->name, name)) return c;
  return 0;
}

/* Classe embutida, criada na primeira vez. */
static struct wclass* class_builtin(struct wgui* g, const wchar16* name) {
  struct wclass* c;
  int kind = wndclass_kind(name);
  if (kind == CK_APP) {
    /* classe comum que o programa nao registrou (SysListView32...): generica */
    static const char* const known[] = {"SysListView32", "SysTreeView32", "msctls_trackbar32", "SysTabControl32",
                                        "msctls_updown32", "SysAnimate32", "ScrollBar", "SysHeader32",
                                        "tooltips_class32", "SysPager", "SysIPAddress32", "SysDateTimePick32",
                                        "SysMonthCal32", "msctls_hotkey32", "NativeFontCtl", "DirectUIHWND", 0};
    int i, hit = 0;
    if ((uintptr_t)name < 0x10000) return 0;
    for (i = 0; known[i]; i++)
      if (wcs_ieq8(name, known[i])) hit = 1;
    if (!hit) return 0;
    kind = CK_OTHER;
  }
  c = (struct wclass*)wzalloc(sizeof(*c));
  if (!c) return 0;
  c->name = (uintptr_t)name < 0x10000 ? (wchar16*)name : wcs_dup(name);
  c->kind = kind;
  c->proc = kind == CK_DIALOG ? wDefDlgProcW : ctl_proc;
  c->bg_sys = kind == CK_DIALOG ? 16 : 0; /* COLOR_BTNFACE + 1 */
  wlock();
  c->next = g->classes;
  g->classes = c;
  wunlock();
  return c;
}

struct wndclassexw {
  uint32_t size, style;
  wndproc_fn proc;
  int32_t cls_extra, wnd_extra;
  HANDLE inst, icon, cursor, bg;
  const wchar16* menu;
  const wchar16* name;
  HANDLE icon_sm;
};
static uint16_t atom_seq = 0xC100;
static uint16_t class_register(const wchar16* name, wndproc_fn proc, HANDLE bg, int extra) {
  struct wgui* g = gui_get();
  struct wclass* c;
  if (!g || !name) return 0;
  c = (struct wclass*)wzalloc(sizeof(*c));
  if (!c) return 0;
  c->name = (uintptr_t)name < 0x10000 ? (wchar16*)name : wcs_dup(name);
  c->proc = proc;
  c->bg = gdi_of(bg);
  c->bg_sys = (uintptr_t)bg <= 31 ? (uintptr_t)bg : 0;
  c->extra = extra;
  c->kind = CK_APP;
  wlock();
  c->atom = ++atom_seq;
  c->next = g->classes;
  g->classes = c;
  wunlock();
  return c->atom;
}
static WINAPI uint16_t RegisterClassExW(const struct wndclassexw* wc) {
  return class_register(wc->name, wc->proc, wc->bg, wc->wnd_extra);
}
struct wndclassw {
  uint32_t style;
  wndproc_fn proc;
  int32_t cls_extra, wnd_extra;
  HANDLE inst, icon, cursor, bg;
  const wchar16* menu;
  const wchar16* name;
};
static WINAPI uint16_t RegisterClassW(const struct wndclassw* wc) {
  return class_register(wc->name, wc->proc, wc->bg, wc->wnd_extra);
}
static WINAPI BOOL UnregisterClassW(const wchar16* name, HANDLE inst) {
  (void)name;
  (void)inst;
  return 1;
}
static WINAPI BOOL GetClassInfoExW(HANDLE inst, const wchar16* name, struct wndclassexw* out) {
  struct wgui* g = gui_get();
  struct wclass* c = g ? class_find(g, name) : 0;
  (void)inst;
  if (!c && g) c = class_builtin(g, name);
  if (!c) {
    wset_lasterr(1411); /* ERROR_CLASS_DOES_NOT_EXIST */
    return 0;
  }
  memset((uint8_t*)out + 4, 0, sizeof(*out) - 4);
  out->proc = c->proc;
  out->name = name;
  out->wnd_extra = c->extra;
  out->bg = c->bg ? (HANDLE)c->bg : (HANDLE)c->bg_sys;
  return 1;
}
static WINAPI BOOL GetClassInfoW(HANDLE inst, const wchar16* name, void* out) {
  struct wndclassexw ex;
  ex.size = sizeof(ex);
  if (!GetClassInfoExW(inst, name, &ex)) return 0;
  memcpy(out, (uint8_t*)&ex + 4, sizeof(struct wndclassw));
  return 1;
}
static WINAPI int GetClassNameW(HANDLE h, wchar16* buf, int n) {
  struct wwin* w = win_of(h);
  const wchar16* name;
  static const wchar16 names[][20] = {{0}, {'B', 'u', 't', 't', 'o', 'n', 0}, {'S', 't', 'a', 't', 'i', 'c', 0},
                                      {'E', 'd', 'i', 't', 0}, {'L', 'i', 's', 't', 'B', 'o', 'x', 0},
                                      {'C', 'o', 'm', 'b', 'o', 'B', 'o', 'x', 0},
                                      {'m', 's', 'c', 't', 'l', 's', '_', 'p', 'r', 'o', 'g', 'r', 'e', 's', 's', '3', '2', 0},
                                      {'#', '3', '2', '7', '7', '0', 0}, {'S', 'y', 's', 'L', 'i', 'n', 'k', 0}};
  int len;
  if (!w || n <= 0) return 0;
  name = (uintptr_t)w->cls->name < 0x10000 ? (w->cls->kind < CK_OTHER ? names[w->cls->kind] : names[0])
                                           : w->cls->name;
  len = (int)wstrlen16(name);
  if (len >= n) len = n - 1;
  memcpy(buf, name, (size_t)len * 2);
  buf[len] = 0;
  return len;
}
static WINAPI uintptr_t GetClassLongPtrW(HANDLE h, int idx) {
  struct wwin* w = win_of(h);
  if (!w) return 0;
  if (idx == -10) return (uintptr_t)(w->cls->bg ? (HANDLE)w->cls->bg : (HANDLE)w->cls->bg_sys); /* GCLP_HBRBACKGROUND */
  if (idx == -24) return (uintptr_t)w->cls->proc;                                            /* GCLP_WNDPROC */
  return 0;
}
static WINAPI uintptr_t SetClassLongPtrW(HANDLE h, int idx, uintptr_t v) {
  struct wwin* w = win_of(h);
  (void)v;
  (void)idx;
  return w ? 0 : 0;
}

/* ---------- mensagens enviadas ---------- */
intptr_t wsend(struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wgui* g;
  struct sentmsg s;
  struct wproc* p;
  if (!w || w->destroyed || !w->proc) return 0;
  /* mesma thread, ou controle embutido (sem estado de thread): chama direto */
  if (w->thread == me() || w->cls->kind != CK_APP || !w->thread) return w->proc(w, m, wp_, lp);
  /* outra thread: entra na fila dela e espera ela atender (no GetMessage) */
  g = gui_get();
  p = wp();
  memset(&s, 0, sizeof(s));
  s.w = w;
  s.m = m;
  s.wp = wp_;
  s.lp = lp;
  wlock();
  s.next = g->sent;
  g->sent = &s;
  wunlock();
  wsignal_global();
  wwait_begin("SendMessage (espera a thread da janela)", w, 0, 0, __builtin_return_address(0));
  while (!s.done && !p->exit_req && !w->destroyed) k_futex_wait(&s.done, 0, 20);
  wwait_end();
  if (!s.done) { /* a janela morreu: tira da fila */
    struct sentmsg** pp;
    wlock();
    for (pp = &g->sent; *pp; pp = &(*pp)->next)
      if (*pp == &s) {
        *pp = s.next;
        break;
      }
    wunlock();
  }
  return s.result;
}

/* Atende as mensagens enviadas a janelas desta thread. */
static void serve_sent(struct wgui* g) {
  for (;;) {
    struct sentmsg* s = 0;
    struct sentmsg** pp;
    wlock();
    for (pp = &g->sent; *pp; pp = &(*pp)->next)
      if ((*pp)->w->thread == me()) {
        s = *pp;
        *pp = s->next;
        break;
      }
    wunlock();
    if (!s) return;
    s->result = s->w->destroyed ? 0 : s->w->proc(s->w, s->m, s->wp, s->lp);
    s->done = 1;
    k_futex_wake(&s->done, -1);
  }
}

/* ---------- canvas (a janela do WIN) ---------- */
static int world_request(struct wgui* g, int req) {
  struct wproc* p = wp();
  g->req_done = 0;
  g->req = req;
  __sync_fetch_and_add(&wine_gen, 1);
  k_futex_wake(&p->exit_req, -1);
  wwait_begin("pedido a tarefa do WINE (janela)", (void*)(uintptr_t)req, 0, 0, __builtin_return_address(0));
  while (!g->req_done && !p->exit_req) k_futex_wait(&g->req_done, 0, 20);
  wwait_end();
  return g->req_rc;
}

/* Na tarefa principal do processo (main.c). */
void wgui_world_service(void) {
  struct wgui* g = (struct wgui*)wp()->user;
  if (!g || !g->req) return;
  if (g->req == 1) {
    int sw = 0, sh = 0, rc, wpc, hpc;
    if (k_win_primary(&sw, &sh) != 0) k_win_size(&sw, &sh); /* % da tela principal (o WIN abre nela) */
    if (sw <= 0 || sh <= 0) {
      sw = 1024;
      sh = 768;
    }
    /* % da tela, arredondado para cima. A moldura do WIN varia com o tema:
     * se a area cliente sair menor que o pedido, reabre maior. */
    int mw = 16, mh = 48, tries;
    rc = -1;
    for (tries = 0; tries < 5; tries++) {
      int cw = 0, ch = 0;
      wpc = ((g->req_w + mw) * 100 + sw - 1) / sw;
      hpc = ((g->req_h + mh) * 100 + sh - 1) / sh;
      if (wpc < 15) wpc = 15;
      if (hpc < 15) hpc = 15;
      if (wpc > 100) wpc = 100;
      if (hpc > 100) hpc = 100;
      rc = k_gui_open_pct(g->req_title, wpc, hpc);
      if (rc != 0) break;
      k_gui_size(&cw, &ch);
      if ((cw >= g->req_w || wpc == 100) && (ch >= g->req_h || hpc == 100)) break;
      k_gui_close();
      if (cw < g->req_w) mw += g->req_w - cw + 8;
      if (ch < g->req_h) mh += g->req_h - ch + 8;
    }
    g->req_rc = rc;
    if (rc != 0) wlogx("k_gui_open falhou (o EXE foi aberto pelo WIN?): ", (uint64_t)(int64_t)rc);
    /* Ctrl+V chega como tecla (22) e o programa cola pelo clipboard do
     * Windows, que le o do SODOS (uimisc.c); sem isso o WIN digitava o texto */
    if (rc == 0) k_gui_clip(1);
  } else if (g->req == 2) {
    k_gui_close();
    g->req_rc = 0;
  }
  g->req = 0;
  g->req_done = 1;
  k_futex_wake(&g->req_done, -1);
}

static int visible_tops(struct wgui* g, struct wwin** out, int max) {
  struct wwin* w;
  int n = 0, i, j;
  for (w = g->wins; w; w = w->next)
    if (!w->parent && w->visible && !w->destroyed && w->s.px && n < max) out[n++] = w;
  /* por z, de tras para frente */
  for (i = 1; i < n; i++)
    for (j = i; j > 0 && out[j - 1]->z > out[j]->z; j--) {
      struct wwin* t = out[j];
      out[j] = out[j - 1];
      out[j - 1] = t;
    }
  return n;
}

static void layout(struct wgui* g) {
  struct wwin* v[32];
  int n = visible_tops(g, v, 32), i;
  for (i = 0; i < n; i++) {
    struct wwin* w = v[i];
    int fw = w->s.w, fh = w->s.h;
    w->framed = w != g->canvas_main;
    if (w->framed) {
      fw += 2 * FRAME_BORDER;
      fh += FRAME_TITLE + 2 * FRAME_BORDER;
    }
    w->cx = (g->canvas.w - fw) / 2;
    w->cy = (g->canvas.h - fh) / 2;
    if (w->cx < 0) w->cx = 0;
    if (w->cy < 0) w->cy = 0;
    if (w->framed) {
      w->cx += FRAME_BORDER;
      w->cy += FRAME_TITLE + FRAME_BORDER;
    }
  }
}

/* Janela de topo com tamanho novo (a janela do WIN mudou de tamanho, ou
 * SetWindowPos/MoveWindow do programa): superficie nova com o que cabia da
 * antiga e os avisos que o programa espera do Windows (WM_WINDOWPOSCHANGED e
 * WM_SIZE). Antes so a tela do WIN mudava: o Claude Desktop (Chromium)
 * continuava desenhando no tamanho antigo, encostado no canto. */
struct winpos {
  HANDLE hwnd, after;
  int x, y, cx, cy;
  uint32_t flags;
};
static void top_resize(struct wwin* w, int cw, int ch, int maxed) {
  struct surf ns;
  struct winpos wp;
  if (cw < 1 || ch < 1 || (cw == w->w && ch == w->h && cw == w->s.w && ch == w->s.h)) return;
  if (!surf_alloc(&ns, cw, ch, 1)) return;
  surf_fill(&ns, 0, 0, cw, ch, sys_color(15));
  if (w->s.px) {
    surf_blit(&ns, 0, 0, w->s.w < cw ? w->s.w : cw, w->s.h < ch ? w->s.h : ch, &w->s, 0, 0,
              w->s.w < cw ? w->s.w : cw, w->s.h < ch ? w->s.h : ch, 0);
    if (!w->s.foreign) k_free(w->s.px);
  }
  w->s = ns;
  w->w = cw;
  w->h = ch;
  memset(&wp, 0, sizeof(wp));
  wp.hwnd = (HANDLE)w;
  wp.cx = cw;
  wp.cy = ch;
  wp.flags = 0x0002 | 0x0004 | 0x0010; /* SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE */
  wsend(w, WM_WINDOWPOSCHANGED, 0, (intptr_t)(uintptr_t)&wp);
  wsend(w, WM_SIZE, maxed ? 2 : 0, ((intptr_t)ch << 16) | (cw & 0xFFFF)); /* SIZE_MAXIMIZED / RESTORED */
  w->dirty = 1;
  win_invalidate(w);
}

/* Abre, troca ou fecha a janela do WIN conforme as janelas de topo visiveis. */
static void canvas_update(struct wgui* g) {
  struct wwin* v[32];
  int n = visible_tops(g, v, 32);
  struct wwin* main = 0;
  int i;
  if (g->canvas_main && (!g->canvas_main->visible || g->canvas_main->destroyed)) g->canvas_main = 0;
  if (!n) {
    if (g->canvas_open) {
      world_request(g, 2);
      g->canvas_open = 0;
    }
    g->canvas_main = 0;
    return;
  }
  main = g->canvas_main ? g->canvas_main : v[0];
  if (g->canvas_open && main != g->canvas_main &&
      (main->s.w > g->canvas.w || main->s.h > g->canvas.h || main->s.w < g->canvas.w / 2)) {
    world_request(g, 2); /* a nova principal nao cabe (ou sobra demais): reabre */
    g->canvas_open = 0;
  }
  g->canvas_main = main;
  if (!g->canvas_open) {
    int cw = 0, ch = 0;
    int i2, n8 = 0;
    g->req_w = main->s.w;
    g->req_h = main->s.h;
    if (main->text) n8 = wutf16_to_8(main->text, -1, g->req_title, (int)sizeof(g->req_title) - 1);
    g->req_title[n8] = 0;
    for (i2 = 0; i2 < n8; i2++)
      if (g->req_title[i2] == '&') g->req_title[i2] = ' ';
    if (world_request(g, 1) != 0) return;
    k_gui_size(&cw, &ch);
    if (cw < main->s.w) cw = main->s.w;
    if (ch < main->s.h) ch = main->s.h;
    if (g->canvas.px) k_free(g->canvas.px);
    if (!surf_alloc(&g->canvas, cw, ch, 1)) return;
    g->canvas_open = 1;
    /* a janela do WIN pode abrir maior que a pedida: a principal ocupa a
     * area toda (antes ficava no meio, com margem embaixo e dos lados) */
    if (main->s.w != cw || main->s.h != ch) top_resize(main, cw, ch, 0);
  } else if (main->text) {
    char t[96];
    int k = wutf16_to_8(main->text, -1, t, (int)sizeof(t) - 1);
    t[k] = 0;
    k_gui_title(t);
  }
  layout(g);
  for (i = 0; i < n; i++) v[i]->dirty = 1;
  g->canvas_dirty = 1;
}

static void compose(struct wgui* g) {
  struct wwin* v[32];
  int n, i;
  if (!g->canvas_open || !g->canvas.px) return;
  n = visible_tops(g, v, 32);
  layout(g);
  surf_fill(&g->canvas, 0, 0, g->canvas.w, g->canvas.h, sys_color(15));
  for (i = 0; i < n; i++) {
    struct wwin* w = v[i];
    if (w->framed) {
      int l = w->cx - FRAME_BORDER, t = w->cy - FRAME_TITLE - FRAME_BORDER;
      int r = w->cx + w->s.w + FRAME_BORDER, b = w->cy + w->s.h + FRAME_BORDER;
      struct rect tr = {l + 10, t + 1, r - 34, t + FRAME_TITLE};
      wchar16 x[2] = {'x', 0};
      struct rect xr = {r - 34, t + 1, r - 1, t + FRAME_TITLE};
      surf_fill(&g->canvas, l + 4, t + 4, r + 4, b + 4, 0x909090); /* sombra */
      surf_fill(&g->canvas, l, t, r, b, 0xFFFFFF);
      surf_fill(&g->canvas, l, t, r, t + 1, 0x707070);
      surf_fill(&g->canvas, l, b - 1, r, b, 0x707070);
      surf_fill(&g->canvas, l, t, l + 1, b, 0x707070);
      surf_fill(&g->canvas, r - 1, t, r, b, 0x707070);
      if (w->text) text_box(&g->canvas, 0, &tr, w->text, -1, 0, 0, DT_SINGLELINE | DT_VCENTER);
      text_box(&g->canvas, 0, &xr, x, 1, 0x404040, 0, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    surf_blit(&g->canvas, w->cx, w->cy, w->s.w, w->s.h, &w->s, 0, 0, w->s.w, w->s.h, 0);
  }
  g->canvas_dirty = 0;
}

/* Mandar para a tela recompoe o desktop com a trava do kernel: limitado a
 * ~1/4 do tempo (se custou 40 ms, a proxima so sai 120 ms depois); o que
 * chegar antes sai pelo laco de mensagens. */
static void present_now(struct wgui* g) {
  uint64_t t0 = wnow_ms();
  compose(g);
  k_gui_pixels(0, 0, g->canvas.w, g->canvas.h, g->canvas.px, g->canvas.w * 4);
  k_gui_present();
  g->present_cost = (uint32_t)(wnow_ms() - t0);
  g->present_at = wnow_ms();
  g->present_pending = 0;
}
static uint64_t present_next(struct wgui* g) {
  uint32_t gap = g->present_cost * 3u;
  if (gap < 30) gap = 30;
  if (gap > 1000) gap = 1000;
  return g->present_at + gap;
}
static void present(struct wgui* g) {
  if (!g->canvas_open) return;
  if (wnow_ms() < present_next(g)) {
    g->present_pending = 1;
    return;
  }
  present_now(g);
}

/* w = 0: so recompor/apresentar (algo foi desenhado direto num DC). */
void win_invalidate(struct wwin* w) {
  struct wgui* g = gui_get();
  struct wwin* t = win_top(w);
  if (!g) return;
  if (t) t->dirty = 1;
  else g->present_pending = 1;
  wsignal_global();
}

/* ---------- pintura ---------- */
static void paint_tree(struct wwin* w) {
  struct wwin* c;
  if (!w->visible || w->destroyed) return;
  wsend(w, WM_PAINT, 0, 0);
  for (c = w->child; c; c = c->sibling) paint_tree(c);
}
static void paint_dirty(struct wgui* g) {
  struct wwin* v[32];
  int n = visible_tops(g, v, 32), i, any = 0;
  for (i = 0; i < n; i++)
    if (v[i]->dirty && v[i]->thread == me()) {
      v[i]->dirty = 0;
      paint_tree(v[i]);
      any = 1;
    }
  if (any || g->present_pending || g->canvas_dirty) present(g);
}

/* ---------- criacao ---------- */
struct wwin* win_create(DWORD ex, const wchar16* cls, const wchar16* title, DWORD style, int x, int y, int cw,
                        int ch, struct wwin* parent, uintptr_t id, void* param) {
  struct wgui* g = gui_get();
  struct wclass* c;
  struct wwin* w;
  struct {
    void* param;
    HANDLE inst, menu, parent;
    int cy, cx, y, x;
    int32_t style;
    const wchar16* name;
    const wchar16* cls;
    DWORD ex;
  } cs;
  if (!g || !cls) {
    wlogx(!g ? "CreateWindow: sem GUI nesta thread, classe " : "CreateWindow: sem classe ", (uintptr_t)cls);
    return 0;
  }
  c = class_find(g, cls);
  if (!c) c = class_builtin(g, cls);
  if (!c) {
    if (wine_debug && (uintptr_t)cls >= 0x10000) {
      char n8[64];
      int k = wutf16_to_8(cls, -1, n8, 63);
      n8[k] = 0;
      wlog("CreateWindow: classe desconhecida ", n8);
    }
    if (wine_debug && (uintptr_t)cls < 0x10000) wlogx("CreateWindow: atomo de classe desconhecido ", (uintptr_t)cls);
    wset_lasterr(1407); /* ERROR_CANNOT_FIND_WND_CLASS */
    return 0;
  }
  w = (struct wwin*)wzalloc(sizeof(*w));
  if (!w) return 0;
  w->magic = WIN_MAGIC;
  w->cls = c;
  w->proc = c->proc;
  w->style = style;
  w->exstyle = ex;
  w->id = id;
  w->enabled = !(style & WS_DISABLED);
  w->thread = me();
  w->cur = -1;
  w->pmax = 100;
  if (cw == (int)0x80000000 || cw <= 0) cw = parent ? 0 : 640;
  if (ch == (int)0x80000000 || ch <= 0) ch = parent ? 0 : 480;
  if (x == (int)0x80000000) x = 0;
  if (y == (int)0x80000000) y = 0;
  w->x = x;
  w->y = y;
  w->w = cw;
  w->h = ch;
  if (title && (uintptr_t)title >= 0x10000) w->text = wcs_dup(title);
  else if (title) w->res_id = (uintptr_t)title; /* ordinal: recurso (icone/bitmap do static) */
  w->dc.magic = DC_MAGIC;
  w->dc.win = w;
  w->dc.brush = &g->stock[0];
  w->dc.pen = &g->stock[7];
  w->dc.font = &g->stock[17];
  w->dc.bk_color = 0xFFFFFF;
  w->dc.bk_mode = 2;
  if (parent && (style & WS_CHILD)) {
    struct wwin** pp;
    w->parent = parent;
    wlock();
    for (pp = &parent->child; *pp; pp = &(*pp)->sibling) {
    }
    *pp = w;
    w->next = g->wins;
    g->wins = w;
    xwin_add_locked(w, g);
    wunlock();
  } else {
    /* de topo: janelas normais de aplicativo tem moldura do Windows no tamanho */
    if (c->kind == CK_APP && (style & WS_CAPTION) == WS_CAPTION) {
      w->w = cw - 16 > 80 ? cw - 16 : cw;
      w->h = ch - 39 > 60 ? ch - 39 : ch;
    }
    w->owner = parent;
    w->z = ++g->zseq;
    if (!surf_alloc(&w->s, w->w, w->h, 1)) {
      wlogx("CreateWindow: sem memoria para a superficie, largura ", (uint64_t)(uint32_t)w->w);
      wlogx("  altura ", (uint64_t)(uint32_t)w->h);
      k_free(w);
      return 0;
    }
    surf_fill(&w->s, 0, 0, w->s.w, w->s.h, sys_color(15));
    wlock();
    w->next = g->wins;
    g->wins = w;
    xwin_add_locked(w, g);
    wunlock();
  }
  memset(&cs, 0, sizeof(cs));
  cs.param = param;
  cs.parent = parent;
  cs.cx = w->w;
  cs.cy = w->h;
  cs.x = x;
  cs.y = y;
  cs.style = (int32_t)style;
  cs.name = title;
  cs.cls = cls;
  cs.ex = ex;
  if (wine_debug && !parent && w->text) {
    char t8[96];
    int k = wutf16_to_8(w->text, -1, t8, 95);
    t8[k] = 0;
    wlog("CreateWindow ", t8);
  }
  if (!wsend(w, WM_NCCREATE, 0, (intptr_t)&cs) || wsend(w, WM_CREATE, 0, (intptr_t)&cs) == -1) {
    if (wine_debug) wlogx("CreateWindow: o programa recusou no WM_NCCREATE/WM_CREATE, estilo ", style);
    win_destroy(w);
    return 0;
  }
  if (style & WS_VISIBLE) {
    w->visible = 0;
    win_show(w, 1);
  }
  return w;
}

void win_destroy(struct wwin* w) {
  struct wgui* g = gui_get();
  struct wtimer** tp;
  struct wwin* c;
  if (!w || w->destroyed) return;
  /* filhas primeiro */
  for (c = w->child; c; c = c->sibling) win_destroy(c);
  wsend(w, WM_DESTROY, 0, 0);
  wsend(w, WM_NCDESTROY, 0, 0);
  w->destroyed = 1;
  w->visible = 0;
  wlock();
  {
    int i;
    for (i = 0; i < nxwins; i++)
      if (xwins[i].w == w) xwins[i].w = 0;
  }
  for (tp = &g->timers; *tp;) {
    if ((*tp)->win == w) {
      struct wtimer* t = *tp;
      *tp = t->next;
      k_free(t);
    } else {
      tp = &(*tp)->next;
    }
  }
  if (w->parent) {
    struct wwin** pp;
    for (pp = &w->parent->child; *pp; pp = &(*pp)->sibling)
      if (*pp == w) {
        *pp = w->sibling;
        break;
      }
    if (win_top(w)->focus == w) win_top(w)->focus = 0;
  }
  if (g->capture == w) g->capture = 0;
  if (g->active == w) g->active = 0;
  wunlock();
  if (!w->parent) {
    /* a dona volta a valer (modal) */
    canvas_update(g);
    win_invalidate(0);
  } else {
    win_invalidate(w->parent);
  }
}

void win_show(struct wwin* w, int show) {
  struct wgui* g = gui_get();
  int was = w->visible;
  w->visible = show != 0;
  if (w->visible == was) return;
  wsend(w, WM_SHOWWINDOW, (uintptr_t)w->visible, 0);
  if (!w->parent) {
    if (w->visible) {
      w->z = ++g->zseq;
      g->active = w;
      wsend(w, WM_SIZE, 0, ((intptr_t)w->h << 16) | (w->w & 0xFFFF));
    }
    canvas_update(g);
  }
  win_invalidate(w->parent ? w->parent : w);
}

void win_set_text(struct wwin* w, const wchar16* t) {
  wchar16* old = w->text;
  w->text = t ? wcs_dup(t) : 0;
  if (old) k_free(old);
  if (!w->parent) {
    struct wgui* g = gui_get();
    if (g->canvas_main == w && g->canvas_open && w->text) {
      char t8[96];
      int k = wutf16_to_8(w->text, -1, t8, 95);
      t8[k] = 0;
      k_gui_title(t8);
    }
    g->canvas_dirty = 1;
  }
  win_invalidate(w);
}

int win_get_text(struct wwin* w, wchar16* buf, int n) {
  int len = w->text ? (int)wstrlen16(w->text) : 0;
  if (!buf || n <= 0) return 0;
  if (len >= n) len = n - 1;
  if (len) memcpy(buf, w->text, (size_t)len * 2);
  buf[len] = 0;
  return len;
}

void win_set_focus(struct wwin* w) {
  struct wwin* t = win_top(w);
  struct wwin* old;
  if (!t) return;
  old = t->focus;
  if (old == w) return;
  t->focus = w == t ? 0 : w;
  if (old && !old->destroyed) {
    wsend(old, WM_KILLFOCUS, (uintptr_t)w, 0);
    win_invalidate(old);
  }
  if (w) {
    wsend(w, WM_SETFOCUS, (uintptr_t)old, 0);
    win_invalidate(w);
  }
}

/* ---------- API de janelas ---------- */
static WINAPI HANDLE CreateWindowExW(DWORD ex, const wchar16* cls, const wchar16* title, DWORD style, int x, int y,
                                     int cw, int ch, HANDLE parent, HANDLE menu, HANDLE inst, void* param) {
  struct wwin* pw = win_of(parent);
  (void)inst;
  return win_create(ex, cls, title, style, x, y, cw, ch, pw, (style & WS_CHILD) ? (uintptr_t)menu : 0, param);
}
static WINAPI HANDLE CreateWindowExA(DWORD ex, const char* cls, const char* title, DWORD style, int x, int y, int cw,
                                     int ch, HANDLE parent, HANDLE menu, HANDLE inst, void* param) {
  wchar16* c = (uintptr_t)cls < 0x10000 ? (wchar16*)cls : wdup8to16(cls);
  wchar16* t = title ? wdup8to16(title) : 0;
  HANDLE h = CreateWindowExW(ex, c, t, style, x, y, cw, ch, parent, menu, inst, param);
  if ((uintptr_t)cls >= 0x10000) k_free(c);
  k_free(t);
  return h;
}
static WINAPI BOOL DestroyWindow(HANDLE h) {
  struct wwin* w = win_of(h);
  if (!w) return 0;
  win_destroy(w);
  return 1;
}
static WINAPI BOOL ShowWindow(HANDLE h, int cmd) {
  struct wwin* w = win_of(h);
  int was;
  if (!w) return 0;
  was = w->visible;
  win_show(w, cmd != 0); /* SW_HIDE = 0 */
  return was;
}
static WINAPI BOOL UpdateWindow(HANDLE h) {
  struct wwin* w = win_of(h);
  struct wgui* g = gui_get();
  if (!w || !g) return 0;
  serve_sent(g);
  paint_dirty(g);
  return 1;
}
static WINAPI BOOL InvalidateRect(HANDLE h, const struct rect* r, BOOL erase) {
  struct wwin* w = win_of(h);
  (void)r;
  (void)erase;
  if (!h) {
    struct wgui* g = gui_get();
    struct wwin* i;
    for (i = g ? g->wins : 0; i; i = i->next)
      if (!i->parent) i->dirty = 1;
    wsignal_global();
    return 1;
  }
  if (!w) return 0;
  win_invalidate(w);
  return 1;
}
static WINAPI BOOL ValidateRect(HANDLE h, const struct rect* r) {
  (void)h;
  (void)r;
  return 1;
}
static WINAPI BOOL RedrawWindow(HANDLE h, const struct rect* r, HANDLE rgn, DWORD fl) {
  (void)rgn;
  InvalidateRect(h, r, 1);
  if (fl & 0x100) UpdateWindow(h); /* RDW_UPDATENOW */
  return 1;
}
static WINAPI BOOL IsWindow(HANDLE h) { return win_any(h, 0) != 0; }
static WINAPI BOOL IsWindowVisible(HANDLE h) {
  struct wwin* w = win_of(h);
  for (; w; w = w->parent)
    if (!w->visible) return 0;
  return win_of(h) != 0;
}
static WINAPI BOOL IsWindowEnabled(HANDLE h) {
  struct wwin* w = win_of(h);
  return w && w->enabled;
}
BOOL WINAPI wEnableWindow(HANDLE h, BOOL en) {
  struct wwin* w = win_of(h);
  int was;
  if (!w) return 0;
  was = !w->enabled;
  if (w->enabled != (en != 0)) {
    w->enabled = en != 0;
    if (en) w->style &= ~WS_DISABLED;
    else w->style |= WS_DISABLED;
    wsend(w, WM_ENABLE, (uintptr_t)en, 0);
    win_invalidate(w);
  }
  return was;
}
static WINAPI BOOL GetClientRect(HANDLE h, struct rect* r) {
  struct wwin* w = win_any(h, 0); /* o processo GPU pergunta o da janela do browser */
  if (!w) return 0;
  r->left = r->top = 0;
  r->right = w->w;
  r->bottom = w->h;
  return 1;
}
/* Coordenadas de "tela" = do canvas. */
static void to_screen(struct wwin* w, int* x, int* y) {
  int ax = 0, ay = 0;
  struct wwin* t = win_top(w);
  win_abs_client(w, &ax, &ay);
  *x += ax + (t ? t->cx : 0);
  *y += ay + (t ? t->cy : 0);
}
static WINAPI BOOL GetWindowRect(HANDLE h, struct rect* r) {
  struct wwin* w = win_of(h);
  int x = 0, y = 0;
  if (!w) return 0;
  to_screen(w, &x, &y);
  r->left = x;
  r->top = y;
  r->right = x + w->w;
  r->bottom = y + w->h;
  return 1;
}
static WINAPI BOOL ClientToScreen(HANDLE h, struct point* p) {
  struct wwin* w = win_of(h);
  int x = p->x, y = p->y;
  if (!w) return 0;
  to_screen(w, &x, &y);
  p->x = x;
  p->y = y;
  return 1;
}
static WINAPI BOOL ScreenToClient(HANDLE h, struct point* p) {
  struct wwin* w = win_of(h);
  int x = 0, y = 0;
  if (!w) return 0;
  to_screen(w, &x, &y);
  p->x -= x;
  p->y -= y;
  return 1;
}
static WINAPI int MapWindowPoints(HANDLE from, HANDLE to, struct point* pts, DWORD n) {
  int fx = 0, fy = 0, tx = 0, ty = 0;
  DWORD i;
  struct wwin* a = win_of(from);
  struct wwin* b = win_of(to);
  if (a) to_screen(a, &fx, &fy);
  if (b) to_screen(b, &tx, &ty);
  for (i = 0; i < n; i++) {
    pts[i].x += fx - tx;
    pts[i].y += fy - ty;
  }
  return ((fy - ty) << 16) | ((fx - tx) & 0xFFFF);
}
static WINAPI BOOL SetWindowPos(HANDLE h, HANDLE after, int x, int y, int cx, int cy, DWORD fl) {
  struct wwin* w = win_of(h);
  (void)after;
  if (!w) return 0;
  if (w->parent) {
    if (!(fl & 2)) { /* SWP_NOMOVE */
      w->x = x;
      w->y = y;
    }
    if (!(fl & 1) && cx >= 0 && cy >= 0) { /* SWP_NOSIZE */
      w->w = cx;
      w->h = cy;
      wsend(w, WM_SIZE, 0, ((intptr_t)cy << 16) | (cx & 0xFFFF));
    }
  } else if (!(fl & 1) && cx > 0 && cy > 0) {
    /* de topo: o tamanho vale (a principal fica no maximo do tamanho da
     * janela do WIN; as outras, como pedido) */
    struct wgui* g = gui_get();
    /* cx/cy do Windows incluem a moldura; aqui a de topo e so o cliente
     * (como no CreateWindow) */
    if (w->cls && w->cls->kind == CK_APP && (w->style & WS_CAPTION) == WS_CAPTION) {
      if (cx - 16 > 80) cx -= 16;
      if (cy - 39 > 60) cy -= 39;
    }
    /* a principal ocupa sempre a janela do WIN inteira (quem muda o tamanho
     * dela e o usuario, pela janela do WIN): o pedido do programa so vale
     * para as outras. O Chromium tira a moldura no WM_NCCALCSIZE e pede o
     * tamanho de fora: sobrava uma faixa vazia embaixo e a direita. */
    if (w == g->canvas_main && g->canvas_open) {
      cx = g->canvas.w;
      cy = g->canvas.h;
    }
    top_resize(w, cx, cy, 0);
    if (!w->parent) g->canvas_dirty = 1;
  }
  if (fl & 0x40) win_show(w, 1);  /* SWP_SHOWWINDOW */
  if (fl & 0x80) win_show(w, 0);  /* SWP_HIDEWINDOW */
  win_invalidate(w->parent ? w->parent : w);
  return 1;
}
static WINAPI BOOL MoveWindow(HANDLE h, int x, int y, int cx, int cy, BOOL rep) {
  (void)rep;
  return SetWindowPos(h, 0, x, y, cx, cy, 0);
}
static WINAPI HANDLE GetParent(HANDLE h) {
  struct wwin* w = win_of(h);
  if (!w) return 0;
  return w->parent ? (HANDLE)w->parent : (HANDLE)w->owner;
}
static WINAPI HANDLE GetAncestor(HANDLE h, DWORD how) {
  struct wwin* w = win_of(h);
  if (!w) return 0;
  if (how == 1) return w->parent; /* GA_PARENT */
  return win_top(w);
}
static WINAPI HANDLE SetParent(HANDLE h, HANDLE p) {
  struct wwin* w = win_of(h);
  (void)p;
  return w ? (HANDLE)w->parent : 0;
}
static WINAPI BOOL IsChild(HANDLE p, HANDLE h) {
  struct wwin* w = win_of(h);
  for (w = w ? w->parent : 0; w; w = w->parent)
    if (w == (struct wwin*)p) return 1;
  return 0;
}
static WINAPI HANDLE GetWindow(HANDLE h, DWORD cmd) {
  struct wwin* w = win_of(h);
  struct wwin* c;
  if (!w) return 0;
  switch (cmd) {
    case 5: return w->child;          /* GW_CHILD */
    case 2: return w->sibling;        /* GW_HWNDNEXT */
    case 4: return w->owner;          /* GW_OWNER */
    case 3:                           /* GW_HWNDPREV */
      if (!w->parent) return 0;
      for (c = w->parent->child; c; c = c->sibling)
        if (c->sibling == w) return c;
      return 0;
    case 0:                           /* GW_HWNDFIRST */
      return w->parent ? (HANDLE)w->parent->child : (HANDLE)w;
    case 1:                           /* GW_HWNDLAST */
      if (!w->parent) return w;
      for (c = w->parent->child; c && c->sibling; c = c->sibling) {
      }
      return c;
  }
  return 0;
}
static WINAPI HANDLE GetTopWindow(HANDLE h) {
  struct wwin* w = win_of(h);
  return w ? (HANDLE)w->child : 0;
}
static WINAPI int GetDlgCtrlID(HANDLE h) {
  struct wwin* w = win_of(h);
  return w ? (int)w->id : 0;
}
static WINAPI BOOL SetWindowTextW(HANDLE h, const wchar16* t) {
  struct wwin* w = win_of(h);
  if (!w) return 0;
  wsend(w, WM_SETTEXT, 0, (intptr_t)t);
  return 1;
}
static WINAPI BOOL SetWindowTextA(HANDLE h, const char* t) {
  wchar16* w = wdup8to16(t ? t : "");
  BOOL r = SetWindowTextW(h, w);
  k_free(w);
  return r;
}
static WINAPI int GetWindowTextW(HANDLE h, wchar16* buf, int n) {
  struct wwin* w = win_of(h);
  if (!w || !buf || n <= 0) return 0;
  return (int)wsend(w, WM_GETTEXT, (uintptr_t)n, (intptr_t)buf);
}
static WINAPI int GetWindowTextLengthW(HANDLE h) {
  struct wwin* w = win_of(h);
  return w ? (int)wsend(w, WM_GETTEXTLENGTH, 0, 0) : 0;
}
static WINAPI intptr_t SetWindowLongPtrW(HANDLE h, int idx, intptr_t v) {
  struct wwin* w = win_of(h);
  intptr_t old = 0;
  if (!w) return 0;
  switch (idx) {
    case -21: old = w->userdata; w->userdata = v; break;      /* GWLP_USERDATA */
    case -4: old = (intptr_t)w->proc; w->proc = (wndproc_fn)v; break; /* GWLP_WNDPROC */
    case -16: old = w->style; w->style = (DWORD)v; win_invalidate(w); break;
    case -20: old = w->exstyle; w->exstyle = (DWORD)v; break;
    case -12: old = (intptr_t)w->id; w->id = (uintptr_t)v; break; /* GWLP_ID */
    default:
      if (idx >= 0 && idx + 8 <= WEXTRA - 8) {
        memcpy(&old, w->extra + idx, 8);
        memcpy(w->extra + idx, &v, 8);
      }
  }
  return old;
}
static WINAPI intptr_t GetWindowLongPtrW(HANDLE h, int idx) {
  struct wwin* w = win_of(h);
  intptr_t v = 0;
  if (!w) return 0;
  switch (idx) {
    case -21: return w->userdata;
    case -4: return (intptr_t)w->proc;
    case -16: return (intptr_t)(w->style | (w->visible ? WS_VISIBLE : 0) | (w->enabled ? 0 : WS_DISABLED));
    case -20: return w->exstyle;
    case -12: return (intptr_t)w->id;
    case -8: return (intptr_t)w->parent;
    case -6: return 0x400000; /* GWLP_HINSTANCE */
  }
  if (idx >= 0 && idx + 8 <= WEXTRA - 8) memcpy(&v, w->extra + idx, 8);
  return v;
}
static WINAPI long SetWindowLongW(HANDLE h, int idx, long v) {
  struct wwin* w = win_of(h);
  if (w && idx >= 0 && idx + 4 <= WEXTRA - 8) {
    long old;
    memcpy(&old, w->extra + idx, 4);
    memcpy(w->extra + idx, &v, 4);
    return old;
  }
  return (long)SetWindowLongPtrW(h, idx, v);
}
static WINAPI long GetWindowLongW(HANDLE h, int idx) {
  struct wwin* w = win_of(h);
  if (w && idx >= 0 && idx + 4 <= WEXTRA - 8) {
    long v;
    memcpy(&v, w->extra + idx, 4);
    return v;
  }
  return (long)GetWindowLongPtrW(h, idx);
}
static WINAPI DWORD GetWindowThreadProcessId(HANDLE h, DWORD* pid) {
  (void)h;
  if (pid) *pid = 0x100;
  return (DWORD) * (uint64_t*)(wteb() + TEB_TID);
}
static WINAPI HANDLE SetFocus(HANDLE h) {
  struct wwin* w = win_of(h);
  struct wwin* t;
  HANDLE old;
  if (!w) return 0;
  t = win_top(w);
  old = t->focus;
  win_set_focus(w);
  return old;
}
static WINAPI HANDLE GetFocus(void) {
  struct wgui* g = gui_get();
  struct wwin* a = g ? g->active : 0;
  if (!a) return 0;
  return a->focus ? (HANDLE)a->focus : (HANDLE)a;
}
static WINAPI HANDLE GetActiveWindow(void) {
  struct wgui* g = gui_get();
  return g ? g->active : 0;
}
static WINAPI HANDLE SetActiveWindow(HANDLE h) {
  struct wgui* g = gui_get();
  struct wwin* w = win_of(h);
  HANDLE old = g ? g->active : 0;
  if (w && g) {
    g->active = win_top(w);
    g->active->z = ++g->zseq;
    g->canvas_dirty = 1;
  }
  return old;
}
static WINAPI BOOL SetForegroundWindow(HANDLE h) {
  SetActiveWindow(h);
  return 1;
}
static WINAPI BOOL BringWindowToTop(HANDLE h) {
  SetActiveWindow(h);
  return 1;
}
static WINAPI HANDLE GetForegroundWindow(void) { return GetActiveWindow(); }
static WINAPI HANDLE GetDesktopWindow(void) { return (HANDLE)(uintptr_t)0x10010; }
static WINAPI HANDLE GetShellWindow(void) { return (HANDLE)(uintptr_t)0x10014; }
static WINAPI BOOL IsIconic(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI BOOL IsZoomed(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI HANDLE FindWindowExW(HANDLE parent, HANDLE after, const wchar16* cls, const wchar16* title) {
  struct wgui* g = gui_get();
  struct wwin* w;
  (void)parent;
  (void)after;
  for (w = g ? g->wins : 0; w; w = w->next) {
    if (w->destroyed || w->parent) continue;
    if (title && (!w->text || !wcs_ieq(w->text, title))) continue;
    if (cls && (uintptr_t)cls >= 0x10000 && (uintptr_t)w->cls->name >= 0x10000 && !wcs_ieq(w->cls->name, cls)) continue;
    return w;
  }
  return 0;
}
static WINAPI HANDLE FindWindowW(const wchar16* cls, const wchar16* title) { return FindWindowExW(0, 0, cls, title); }
typedef BOOL (WINAPI * enum_fn)(HANDLE, intptr_t);
static WINAPI BOOL EnumChildWindows(HANDLE p, enum_fn fn, intptr_t lp) {
  struct wwin* w = win_of(p);
  struct wwin* c;
  if (!w) return 0;
  for (c = w->child; c; c = c->sibling)
    if (!fn(c, lp)) break;
  return 1;
}
static WINAPI BOOL EnumWindows(enum_fn fn, intptr_t lp) {
  struct wgui* g = gui_get();
  struct wwin* w;
  for (w = g ? g->wins : 0; w; w = w->next)
    if (!w->parent && !w->destroyed && !fn(w, lp)) break;
  return 1;
}
static WINAPI HANDLE GetCapture(void) {
  struct wgui* g = gui_get();
  return g ? g->capture : 0;
}
static WINAPI HANDLE SetCapture(HANDLE h) {
  struct wgui* g = gui_get();
  HANDLE old = g ? g->capture : 0;
  if (g) g->capture = win_of(h);
  return old;
}
static WINAPI BOOL ReleaseCapture(void) {
  struct wgui* g = gui_get();
  if (g) g->capture = 0;
  return 1;
}

/* ---------- entrada: eventos do WIN -> mensagens ---------- */
static int q_push(struct wgui* g, struct wwin* w, DWORD m, uintptr_t wp_, intptr_t lp, uintptr_t thread) {
  struct qmsg* e;
  wlock();
  if (g->qn == MSGQ) {
    wunlock();
    return 0;
  }
  e = &g->q[(g->qhead + g->qn) % MSGQ];
  memset(e, 0, sizeof(*e));
  e->m.hwnd = w;
  e->m.message = m;
  e->m.wparam = wp_;
  e->m.lparam = lp;
  e->m.time = k_ms();
  e->thread = w ? w->thread : thread;
  g->qn++;
  wunlock();
  wsignal_global();
  return 1;
}

/* Filha visivel e habilitada mais funda em (x, y) do cliente de w. */
static struct wwin* hit_child(struct wwin* w, int x, int y, int* ox, int* oy) {
  struct wwin* c;
  struct wwin* best = 0;
  for (c = w->child; c; c = c->sibling) {
    if (!c->visible || c->destroyed) continue;
    if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) best = c; /* a ultima ganha (por cima) */
  }
  if (!best) {
    *ox = x;
    *oy = y;
    return w;
  }
  if ((best->style & 0xF) == 7 && best->cls->kind == CK_BUTTON) { /* BS_GROUPBOX nao pega o rato */
    *ox = x;
    *oy = y;
    return w;
  }
  return hit_child(best, x - best->x, y - best->y, ox, oy);
}

/* Janela de topo em (x, y) do canvas; *title = 1 na barra de titulo, 2 no X. */
static struct wwin* hit_top(struct wgui* g, int x, int y, int* title) {
  struct wwin* v[32];
  int n = visible_tops(g, v, 32), i;
  *title = 0;
  for (i = n - 1; i >= 0; i--) {
    struct wwin* w = v[i];
    if (x >= w->cx && x < w->cx + w->s.w && y >= w->cy && y < w->cy + w->s.h) return w;
    if (w->framed && x >= w->cx - FRAME_BORDER && x < w->cx + w->s.w + FRAME_BORDER &&
        y >= w->cy - FRAME_TITLE - FRAME_BORDER && y < w->cy) {
      *title = x >= w->cx + w->s.w - 34 ? 2 : 1;
      return w;
    }
    if (w->framed) return 0; /* fora da caixa de cima: nada (modal) */
  }
  return 0;
}

static struct wwin* top_active(struct wgui* g) {
  struct wwin* v[32];
  int n = visible_tops(g, v, 32);
  return n ? v[n - 1] : 0;
}

/* Estado das teclas (GetKeyState): o Windows atualiza quando a mensagem de
 * teclado sai da fila; o Chromium pergunta o Ctrl ao tratar o WM_KEYDOWN do
 * V (Ctrl+V colar). Um por WINE.DLL: o teclado e um so. */
static volatile uint8_t key_down[256];
static void keystate_track(const struct msg* m) {
  if (m->message == WM_KEYDOWN && m->wparam < 256) key_down[m->wparam] = 1;
  else if (m->message == WM_KEYUP && m->wparam < 256) key_down[m->wparam] = 0;
}

/* O WIN manda as teclas em CP850 (a fonte dele): ç = 0x87, é = 0x82, ã =
 * 0xC6. Os programas do Windows esperam Unicode no WM_CHAR. */
static const unsigned short cp850_uni_tab[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
  0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
  0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
  0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
  0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
  0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
  0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
  0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
  0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
  0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};
static int cp850_uni(int k) {
  return (k >= 0x80 && k <= 0xFF) ? cp850_uni_tab[k - 0x80] : k;
}

static int vk_of(int key, int* ch) {
  *ch = 0;
  switch (key) {
    case 0x101: return 0x26;
    case 0x102: return 0x28;
    case 0x103: return 0x25;
    case 0x104: return 0x27;
    case 0x105: return 0x21;
    case 0x106: return 0x22;
    case 0x107: return 0x24;
    case 0x108: return 0x23;
    case 0x109: return 0x2E;
    case '\r': *ch = '\r'; return 0x0D;
    case '\n': *ch = '\r'; return 0x0D;
    case 27: *ch = 27; return 0x1B;
    case 8: *ch = 8; return 0x08;
    case 9: *ch = 9; return 0x09;
  }
  if (key >= 32 && key < 256) {
    /* Codigo virtual da tecla que produz o caractere (teclado US). Antes o
     * ASCII ia como VK: '.' (0x2E) e VK_DELETE, '!' e VK_PRIOR, '-' e
     * VK_INSERT... o Chromium tratava como tecla de edicao e o ponto nao
     * entrava no campo de e-mail do login. Acentuados: so o WM_CHAR. */
    static const char shifted_digits[] = ")!@#$%^&*(";
    const char* d;
    *ch = key;
    if (key >= 'a' && key <= 'z') return key - 32;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == ' ') return key;
    if (key < 128 && (d = strchr(shifted_digits, key)) != 0) return '0' + (int)(d - shifted_digits);
    switch (key) {
      case ';': case ':': return 0xBA;  /* VK_OEM_1 */
      case '=': case '+': return 0xBB;  /* VK_OEM_PLUS */
      case ',': case '<': return 0xBC;  /* VK_OEM_COMMA */
      case '-': case '_': return 0xBD;  /* VK_OEM_MINUS */
      case '.': case '>': return 0xBE;  /* VK_OEM_PERIOD */
      case '/': case '?': return 0xBF;  /* VK_OEM_2 */
      case '`': case '~': return 0xC0;  /* VK_OEM_3 */
      case '[': case '{': return 0xDB;  /* VK_OEM_4 */
      case '\\': case '|': return 0xDC; /* VK_OEM_5 */
      case ']': case '}': return 0xDD;  /* VK_OEM_6 */
      case '\'': case '"': return 0xDE; /* VK_OEM_7 */
    }
    return 0; /* acentuados e outros: so o caractere */
  }
  return 0;
}

static void mouse_msg(struct wgui* g, DWORD m, int key, int cxp, int cyp) {
  int title, x, y;
  struct wwin* t;
  struct wwin* target;
  if (g->capture && !g->capture->destroyed) {
    int ax = 0, ay = 0;
    struct wwin* ct = win_top(g->capture);
    win_abs_client(g->capture, &ax, &ay);
    target = g->capture;
    x = cxp - ct->cx - ax;
    y = cyp - ct->cy - ay;
  } else {
    t = hit_top(g, cxp, cyp, &title);
    if (!t) return;
    if (title) {
      if (title == 2 && m == WM_LBUTTONUP) q_push(g, t, WM_SYSCOMMAND, 0xF060, 0, 0); /* SC_CLOSE */
      return;
    }
    if (!t->enabled) return; /* dona de um dialogo modal */
    target = hit_child(t, cxp - t->cx, cyp - t->cy, &x, &y);
    if (!target->enabled) return;
    if (m == WM_LBUTTONDOWN && g->active != t) {
      g->active = t;
      t->z = ++g->zseq;
      g->canvas_dirty = 1;
    }
  }
  if (m == WM_LBUTTONDOWN) {
    static struct wwin* last;
    static uint64_t last_ms;
    static int lx, ly;
    uint64_t now = wnow_ms();
    if (target == last && now - last_ms < 500 && x - lx < 5 && lx - x < 5 && y - ly < 5 && ly - y < 5 &&
        target->cls && target->cls->kind != CK_APP && target->cls->kind != CK_DIALOG) {
      m = WM_LBUTTONDBLCLK;
      last = 0;
    } else {
      last = target;
      last_ms = now;
      lx = x;
      ly = y;
    }
  }
  if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) g->capture = target;
  if (m == WM_LBUTTONUP) g->capture = 0;
  q_push(g, target, m, (uintptr_t)key, ((intptr_t)(y & 0xFFFF) << 16) | (x & 0xFFFF), 0);
}

static void pump_events(struct wgui* g) {
  int ev[4], n;
  if (!g->canvas_open) return;
  for (n = 0; n < 32; n++) {
    int t = k_gui_event(ev, 0);
    if (t <= 0) break;
    switch (t) {
      case 1: /* DOWN */
        g->mouse_down = 1;
        mouse_msg(g, ev[3] == 2 ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, ev[3] == 2 ? 2 : 1, ev[1], ev[2]);
        break;
      case 4: /* UP */
        g->mouse_down = 0;
        mouse_msg(g, ev[3] == 2 ? WM_RBUTTONUP : WM_LBUTTONUP, 0, ev[1], ev[2]);
        break;
      case 5: /* DRAG */
      case 8: /* MOVE */
        mouse_msg(g, WM_MOUSEMOVE, g->mouse_down ? 1 : 0, ev[1], ev[2]);
        break;
      case 6: { /* WHEEL */
        struct wwin* a = top_active(g);
        struct wwin* f = a && a->focus ? a->focus : a;
        if (f) q_push(g, f, WM_MOUSEWHEEL, (uintptr_t)((-ev[3] * 120) << 16), 0, 0);
        break;
      }
      case 2: { /* KEY */
        struct wwin* a = top_active(g);
        struct wwin* f;
        int ch, vk = vk_of(cp850_uni(ev[3]), &ch);
        if (!a || !a->enabled) break;
        f = a->focus && !a->focus->destroyed ? a->focus : a;
        /* Ctrl+letra (o WIN manda 1..26; com k_gui_clip(1) o Ctrl+V tambem):
         * Ctrl, letra, solta letra, solta Ctrl. 8/9/10/13 sao Backspace/Tab/
         * Enter (vk_of). */
        if (ev[3] >= 1 && ev[3] <= 26 && ev[3] != 8 && ev[3] != 9 && ev[3] != 10 && ev[3] != 13) {
          int L = 'A' + ev[3] - 1;
          q_push(g, f, WM_KEYDOWN, 0x11, 1, 0);
          q_push(g, f, WM_KEYDOWN, (uintptr_t)L, 1, 0);
          q_push(g, f, WM_KEYUP, (uintptr_t)L, (intptr_t)0xC0000001, 0);
          q_push(g, f, WM_KEYUP, 0x11, (intptr_t)0xC0000001, 0);
          break;
        }
        if (vk) q_push(g, f, WM_KEYDOWN, (uintptr_t)vk, 1, 0);
        if (ch) q_push(g, f, WM_CHAR, (uintptr_t)ch, 1, 0);
        if (vk) q_push(g, f, WM_KEYUP, (uintptr_t)vk, (intptr_t)0xC0000001, 0);
        break;
      }
      case 3: { /* CLOSE */
        struct wwin* a = top_active(g);
        if (a && a->enabled) q_push(g, a, WM_SYSCOMMAND, 0xF060, 0, 0);
        else if (g->canvas_main) q_push(g, g->canvas_main, WM_SYSCOMMAND, 0xF060, 0, 0);
        break;
      }
      case 9: /* RESIZE */
        if (ev[1] > 0 && ev[2] > 0 && (ev[1] != g->canvas.w || ev[2] != g->canvas.h)) {
          struct surf ns;
          if (surf_alloc(&ns, ev[1], ev[2], 1)) {
            int big = ev[1] > g->canvas.w || ev[2] > g->canvas.h;
            k_free(g->canvas.px);
            g->canvas = ns;
            /* a janela principal ocupa a tela toda do WIN: acompanha o tamanho */
            if (g->canvas_main && !g->canvas_main->destroyed)
              top_resize(g->canvas_main, ev[1], ev[2], big);
            layout(g);
            g->canvas_dirty = 1;
          }
        }
        break;
    }
  }
}

/* ---------- laco de mensagens ---------- */
struct wtimer;
static int next_message(struct wgui* g, struct msg* m, HANDLE filter, int remove, uint64_t* wait_until) {
  struct wtimer* t;
  uint64_t now = wnow_ms();
  int i;
  (void)filter;
  *wait_until = now + 15; /* reconfere os eventos do WIN */
  serve_sent(g);
  pump_events(g);
  wlock();
  for (i = 0; i < g->qn; i++) {
    struct qmsg* e = &g->q[(g->qhead + i) % MSGQ];
    if (e->thread != me() && e->thread) continue;
    *m = e->m;
    if (remove) {
      int j;
      keystate_track(m);
      for (j = i; j > 0; j--) g->q[(g->qhead + j) % MSGQ] = g->q[(g->qhead + j - 1) % MSGQ];
      g->qhead = (g->qhead + 1) % MSGQ;
      g->qn--;
    }
    wunlock();
    return 1;
  }
  wunlock();
  if (g->quit_posted && (!g->quit_thread || g->quit_thread == me())) {
    memset(m, 0, sizeof(*m));
    m->message = WM_QUIT;
    m->wparam = (uintptr_t)g->quit_code;
    if (remove) g->quit_posted = 0;
    return 1;
  }
  paint_dirty(g);
  for (t = g->timers; t; t = t->next) {
    if (t->thread != me()) continue;
    if (now >= t->due) {
      memset(m, 0, sizeof(*m));
      m->hwnd = t->win;
      m->message = WM_TIMER;
      m->wparam = t->id;
      m->lparam = (intptr_t)t->proc;
      if (remove) t->due = now + (t->period ? t->period : 1);
      return 1;
    }
    if (t->due < *wait_until) *wait_until = t->due;
  }
  if (g->present_pending && g->canvas_open) {
    uint64_t nx = present_next(g);
    if (now >= nx) present_now(g);
    else if (nx < *wait_until) *wait_until = nx;
  }
  return 0;
}

BOOL WINAPI wGetMessageW(struct msg* m, HANDLE h, DWORD lo, DWORD hi) {
  struct wgui* g = gui_get();
  struct wproc* p = wp();
  (void)lo;
  (void)hi;
  int marked = 0;
  if (!g) return -1;
  for (;;) {
    uint64_t until, now;
    int gen = wine_gen;
    if (next_message(g, m, h, 1, &until)) {
      if (marked) wwait_end();
      return m->message != WM_QUIT;
    }
    if (!marked) {
      marked = 1;
      wwait_begin("GetMessage (fila de mensagens)", h, 0, 0, __builtin_return_address(0));
    }
    now = wnow_ms();
    if (p->exit_req) k_thread_exit(0);
    k_futex_wait(&wine_gen, gen, until > now ? (int)(until - now) : 1);
  }
}
static WINAPI BOOL PeekMessageW(struct msg* m, HANDLE h, DWORD lo, DWORD hi, DWORD remove) {
  struct wgui* g = gui_get();
  uint64_t until;
  (void)lo;
  (void)hi;
  if (!g) return 0;
  return next_message(g, m, h, remove & 1, &until);
}
static WINAPI BOOL WaitMessage(void) {
  struct wgui* g = gui_get();
  struct msg m;
  uint64_t until, now;
  int gen = wine_gen;
  if (!g || next_message(g, &m, 0, 0, &until)) return 1;
  now = wnow_ms();
  wwait_begin("WaitMessage", 0, 0, 0, __builtin_return_address(0));
  k_futex_wait(&wine_gen, gen, until > now ? (int)(until - now) : 1);
  wwait_end();
  return 1;
}
static WINAPI BOOL TranslateMessage(const struct msg* m) {
  (void)m;
  return 0; /* o WM_CHAR ja vem junto do WM_KEYDOWN */
}
intptr_t WINAPI wDispatchMessageW(const struct msg* m) {
  struct wwin* w = win_of(m->hwnd);
  if (m->message == WM_TIMER && m->lparam) {
    typedef void (WINAPI * timer_fn)(HANDLE, DWORD, uintptr_t, DWORD);
    ((timer_fn)m->lparam)(m->hwnd, WM_TIMER, m->wparam, k_ms());
    return 0;
  }
  if (!w) return 0;
  return w->proc ? w->proc(w, m->message, m->wparam, m->lparam) : 0;
}
static WINAPI BOOL PostMessageW(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wgui* g = gui_get();
  if (!g) return 0;
  if (h == (HANDLE)(intptr_t)0xFFFF) h = g->canvas_main; /* HWND_BROADCAST: so as nossas */
  if (h && !win_of(h)) {
    wset_lasterr(1400); /* ERROR_INVALID_WINDOW_HANDLE */
    return 0;
  }
  return q_push(g, (struct wwin*)h, m, wp_, lp, me());
}
static WINAPI BOOL PostThreadMessageW(DWORD tid, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wgui* g = gui_get();
  (void)tid;
  return g ? q_push(g, 0, m, wp_, lp, 0) : 0;
}
static WINAPI void PostQuitMessage(int code) {
  struct wgui* g = gui_get();
  if (!g) return;
  g->quit_code = code;
  g->quit_thread = me();
  g->quit_posted = 1;
  wsignal_global();
}
static WINAPI intptr_t SendMessageW(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wwin* w = win_of(h);
  return w ? wsend(w, m, wp_, lp) : 0;
}
static WINAPI intptr_t SendMessageTimeoutW(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp, DWORD fl, DWORD ms,
                                           uintptr_t* res) {
  intptr_t r;
  (void)fl;
  (void)ms;
  r = SendMessageW(h, m, wp_, lp);
  if (res) *res = (uintptr_t)r;
  return 1;
}
static WINAPI BOOL SendNotifyMessageW(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  return PostMessageW(h, m, wp_, lp);
}
static WINAPI intptr_t CallWindowProcW(wndproc_fn f, HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  return f ? f(h, m, wp_, lp) : 0;
}
static DWORD msgwait_in(DWORD n, const HANDLE* hs, DWORD ms, DWORD mask, DWORD fl);
static WINAPI DWORD MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE* hs, DWORD ms, DWORD mask, DWORD fl) {
  DWORD r;
  if (!ms) return msgwait_in(n, hs, ms, mask, fl);
  wwait_begin(n ? "MsgWaitForMultipleObjects (objetos + mensagens)" : "MsgWaitForMultipleObjects (mensagens)",
              n ? wh_get(hs[0], 0) : 0, 0, 0, __builtin_return_address(0));
  r = msgwait_in(n, hs, ms, mask, fl);
  wwait_end();
  return r;
}
static DWORD msgwait_in(DWORD n, const HANDLE* hs, DWORD ms, DWORD mask, DWORD fl) {
  struct wgui* g = gui_get();
  struct wproc* p = wp();
  uint64_t t0 = wnow_ms();
  (void)mask;
  (void)fl;
  for (;;) {
    struct msg m;
    uint64_t until;
    int gen = wine_gen;
    DWORD i;
    for (i = 0; i < n; i++) {
      struct wobj* o = wh_get(hs[i], 0);
      if (o && wwait(1, &o, 0, 0) == 0) return i;
    }
    if (g && next_message(g, &m, 0, 0, &until)) return n;
    if (ms != INFINITE && wnow_ms() - t0 >= ms) return WAIT_TIMEOUT;
    if (p->exit_req) k_thread_exit(0);
    k_futex_wait(&wine_gen, gen, 10);
  }
}
static WINAPI DWORD MsgWaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, DWORD mask) {
  (void)all;
  return MsgWaitForMultipleObjectsEx(n, hs, ms, mask, 0);
}
static WINAPI DWORD GetQueueStatus(DWORD fl) {
  struct wgui* g = gui_get();
  (void)fl;
  return g && g->qn ? 0x00040004u : 0;
}

/* ---------- pintura (BeginPaint) e DefWindowProc ---------- */
struct paintstruct {
  HANDLE hdc;
  BOOL erase;
  struct rect paint;
  BOOL restore, inc_update;
  uint8_t reserved[32];
};
static WINAPI HANDLE BeginPaint(HANDLE h, struct paintstruct* ps) {
  struct wwin* w = win_of(h);
  struct wgui* g = gui_get();
  if (!w) return 0;
  memset(ps, 0, sizeof(*ps));
  if (!w->dc.brush) w->dc.brush = &g->stock[0];
  if (!w->dc.pen) w->dc.pen = &g->stock[7];
  if (!w->dc.font) w->dc.font = &g->stock[17];
  dc_prepare(&w->dc, w);
  w->dc.painting = 1;
  wsend(w, WM_ERASEBKGND, (uintptr_t)&w->dc, 0);
  ps->hdc = &w->dc;
  ps->paint.right = w->w;
  ps->paint.bottom = w->h;
  return &w->dc;
}
static WINAPI BOOL EndPaint(HANDLE h, const struct paintstruct* ps) {
  struct wwin* w = win_of(h);
  (void)ps;
  if (w) w->dc.painting = 0;
  return 1;
}

intptr_t WINAPI wDefWindowProcW(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct wwin* w = win_of(h);
  if (!w) return 0;
  switch (m) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE:
      win_destroy(w);
      return 0;
    case WM_SYSCOMMAND:
      if ((wp_ & 0xFFF0) == 0xF060) wsend(w, WM_CLOSE, 0, 0);
      return 0;
    case WM_PAINT: {
      struct paintstruct ps;
      BeginPaint(w, &ps);
      EndPaint(w, &ps);
      return 0;
    }
    case WM_ERASEBKGND: {
      struct wdc* d = dc_of((HANDLE)wp_);
      if (d && (w->cls->bg || w->cls->bg_sys)) {
        int null;
        DWORD c = brush_color(w->cls->bg ? (HANDLE)w->cls->bg : (HANDLE)w->cls->bg_sys, &null);
        if (!null) dc_fill(d, 0, 0, w->w, w->h, c);
      }
      return 1;
    }
    case WM_SETTEXT:
      win_set_text(w, (const wchar16*)lp);
      return 1;
    case WM_GETTEXT:
      return win_get_text(w, (wchar16*)lp, (int)wp_);
    case WM_GETTEXTLENGTH:
      return w->text ? (intptr_t)wstrlen16(w->text) : 0;
    case WM_SETFONT:
      w->font = gdi_of((HANDLE)wp_);
      if (lp) win_invalidate(w);
      return 0;
    case WM_GETFONT:
      return (intptr_t)w->font;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
      return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      return 0;
    case WM_SETCURSOR:
      return 0;
  }
  return 0;
}

/* ---------- timers ---------- */
static WINAPI uintptr_t SetTimer(HANDLE h, uintptr_t id, DWORD ms, void* proc) {
  struct wgui* g = gui_get();
  struct wwin* w = win_of(h);
  struct wtimer* t;
  static uintptr_t seq = 0x7000;
  if (!g) return 0;
  wlock();
  for (t = g->timers; t; t = t->next)
    if (t->win == w && t->id == id && id) break;
  if (!t) {
    wunlock();
    t = (struct wtimer*)wzalloc(sizeof(*t));
    if (!t) return 0;
    wlock();
    t->win = w;
    t->id = id ? id : ++seq;
    t->next = g->timers;
    g->timers = t;
  }
  t->thread = w ? w->thread : me();
  t->period = ms < 1 ? 1 : ms;
  t->due = wnow_ms() + t->period;
  t->proc = proc;
  wunlock();
  wsignal_global();
  return t->id;
}
static WINAPI BOOL KillTimer(HANDLE h, uintptr_t id) {
  struct wgui* g = gui_get();
  struct wwin* w = win_of(h);
  struct wtimer** tp;
  if (!g) return 0;
  wlock();
  for (tp = &g->timers; *tp; tp = &(*tp)->next)
    if ((*tp)->win == w && (*tp)->id == id) {
      struct wtimer* t = *tp;
      *tp = t->next;
      wunlock();
      k_free(t);
      return 1;
    }
  wunlock();
  return 0;
}

/* ---------- metricas e miudezas ---------- */
int k_win_primary(int* w, int* h);
static WINAPI int GetSystemMetrics(int i) {
  int sw = 0, sh = 0, pw = 0, ph = 0;
  k_win_size(&sw, &sh);
  if (sw <= 0) {
    sw = 1024;
    sh = 768;
  }
  /* como no Windows: SCREEN = monitor principal, VIRTUALSCREEN = todos */
  if (k_win_primary(&pw, &ph) != 0 || pw <= 0) {
    pw = sw;
    ph = sh;
  }
  switch (i) {
    case 0: case 16: return pw;
    case 1: case 17: return ph;
    case 78: return sw;
    case 79: return sh;
    case 2: case 3: return 17;      /* CXVSCROLL, CYHSCROLL */
    case 4: return 23;              /* CYCAPTION */
    case 5: case 6: return 1;       /* CXBORDER */
    case 7: case 8: return 3;       /* CXFIXEDFRAME */
    case 11: case 12: return 32;    /* CXICON */
    case 13: case 14: return 32;    /* CXCURSOR */
    case 15: return 20;             /* CYMENU */
    case 32: case 33: return 4;     /* CXFRAME */
    case 49: case 50: return 16;    /* CXSMICON */
    case 80: return 1;              /* CMONITORS */
  }
  return 0;
}
static WINAPI int GetSystemMetricsForDpi(int i, DWORD dpi) {
  (void)dpi;
  return GetSystemMetrics(i);
}
static WINAPI DWORD GetDpiForSystem(void) { return 96; }
static WINAPI DWORD GetDpiForWindow(HANDLE h) {
  (void)h;
  return 96;
}
static WINAPI BOOL SetProcessDPIAware(void) { return 1; }
/* filtro de mensagens entre niveis de integridade (UIPI): nao ha niveis aqui */
static WINAPI BOOL ChangeWindowMessageFilterEx(void* w, DWORD msg, DWORD act, void* st) {
  (void)w; (void)msg; (void)act;
  if (st) ((DWORD*)st)[1] = 0; /* CHANGEFILTERSTRUCT.ExtStatus = MSGFLTINFO_NONE */
  return 1;
}
static WINAPI DWORD GetCaretBlinkTime(void) { return 530; } /* padrao do Windows, ms */
static WINAPI BOOL ChangeWindowMessageFilter(DWORD msg, DWORD act) {
  (void)msg; (void)act;
  return 1;
}
static WINAPI BOOL SetProcessDpiAwarenessContext(HANDLE c) {
  (void)c;
  return 1;
}
static WINAPI long SetProcessDpiAwareness(int v) {
  (void)v;
  return 0;
}
static WINAPI BOOL AdjustWindowRectEx(struct rect* r, DWORD st, BOOL menu, DWORD ex) {
  (void)menu;
  (void)ex;
  if ((st & WS_CAPTION) == WS_CAPTION) {
    r->left -= 8;
    r->right += 8;
    r->top -= 31;
    r->bottom += 8;
  }
  return 1;
}
static WINAPI BOOL AdjustWindowRect(struct rect* r, DWORD st, BOOL menu) { return AdjustWindowRectEx(r, st, menu, 0); }
static WINAPI BOOL SystemParametersInfoW(DWORD a, DWORD b, void* p, DWORD f) {
  (void)b;
  (void)f;
  if (a == 0x30 && p) { /* SPI_GETWORKAREA */
    struct rect* r = (struct rect*)p;
    r->left = r->top = 0;
    r->right = GetSystemMetrics(0);
    r->bottom = GetSystemMetrics(1) - 40;
    return 1;
  }
  if (a == 0x29 && p) { /* SPI_GETNONCLIENTMETRICS: fontes zeradas */
    uint32_t sz = *(uint32_t*)p;
    memset((uint8_t*)p + 4, 0, sz > 4 ? sz - 4 : 0);
    return 1;
  }
  if (p) memset(p, 0, 4);
  return 0;
}
static WINAPI HANDLE LoadCursorW(HANDLE inst, const wchar16* name) {
  (void)inst;
  (void)name;
  return (HANDLE)(uintptr_t)0x10020;
}
static WINAPI HANDLE LoadIconW(HANDLE inst, const wchar16* name) {
  (void)inst;
  (void)name;
  return (HANDLE)(uintptr_t)0x10024;
}
HANDLE WINAPI wLoadBitmapW(HANDLE inst, const wchar16* name);
static WINAPI HANDLE LoadImageW(HANDLE inst, const wchar16* name, DWORD type, int cx, int cy, DWORD fl) {
  (void)cx;
  (void)cy;
  if (type == 0 && !(fl & 0x10)) { /* IMAGE_BITMAP, sem LR_LOADFROMFILE */
    struct wmod* m = pe_from_handle(inst);
    uint32_t size = 0;
    const uint8_t* e = m ? pe_find_resource(m, 2, (uintptr_t)name, &size) : 0;
    if (e) return bitmap_from_dib(m->base + *(const uint32_t*)e, 0);
    return 0;
  }
  return (HANDLE)(uintptr_t)(type == 2 ? 0x10020 : 0x10024);
}
static WINAPI BOOL DestroyIcon(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI HANDLE CopyImage(HANDLE h, DWORD type, int cx, int cy, DWORD fl) {
  (void)type; (void)cx; (void)cy; (void)fl;
  return h;
}
static WINAPI BOOL DrawIconEx(HANDLE dc, int x, int y, HANDLE icon, int cx, int cy, DWORD step, HANDLE br, DWORD fl) {
  (void)dc; (void)x; (void)y; (void)icon; (void)cx; (void)cy; (void)step; (void)br; (void)fl;
  return 1;
}
static WINAPI BOOL DrawIcon(HANDLE dc, int x, int y, HANDLE icon) { return DrawIconEx(dc, x, y, icon, 0, 0, 0, 0, 3); }
static WINAPI HANDLE SetCursor(HANDLE c) { return c; }
static WINAPI BOOL GetCursorPos(struct point* p) {
  p->x = p->y = 0;
  return 1;
}
static WINAPI int ShowCursor(BOOL s) { return s ? 1 : 0; }
static WINAPI short GetKeyState(int vk) {
  if (vk == 0x10 || vk == 0xA0 || vk == 0xA1) vk = key_down[0x10] ? 0x10 : vk; /* Shift */
  if (vk == 0xA2 || vk == 0xA3) vk = 0x11;                                   /* Ctrl esq/dir */
  return vk >= 0 && vk < 256 && key_down[vk] ? (short)0x8000 : 0;
}
static WINAPI short GetAsyncKeyState(int vk) { return GetKeyState(vk); }
static WINAPI HANDLE MonitorFromWindow(HANDLE h, DWORD f) {
  (void)h;
  (void)f;
  return (HANDLE)(uintptr_t)0x10030;
}
static WINAPI HANDLE MonitorFromPoint(int64_t pt, DWORD f) {
  (void)pt;
  (void)f;
  return (HANDLE)(uintptr_t)0x10030;
}
static WINAPI BOOL GetMonitorInfoW(HANDLE m, DWORD* mi) {
  struct rect r;
  (void)m;
  r.left = r.top = 0;
  r.right = GetSystemMetrics(0);
  r.bottom = GetSystemMetrics(1);
  memcpy(mi + 1, &r, sizeof(r));
  r.bottom -= 40;
  memcpy(mi + 5, &r, sizeof(r));
  mi[9] = 1; /* MONITORINFOF_PRIMARY */
  return 1;
}
static WINAPI BOOL MessageBeep(DWORD t) {
  (void)t;
  return 1;
}
static WINAPI BOOL SetUserObjectInformationW(HANDLE h, int i, void* p, DWORD n) {
  (void)h; (void)i; (void)p; (void)n;
  return 1;
}
static WINAPI DWORD WaitForInputIdle(HANDLE h, DWORD ms) {
  (void)h;
  (void)ms;
  return 0;
}
static WINAPI BOOL SetProcessDefaultLayout(DWORD l) {
  (void)l;
  return 1;
}
static WINAPI HANDLE GetSystemMenu(HANDLE h, BOOL r) {
  (void)h;
  (void)r;
  return 0;
}
static WINAPI BOOL EnableMenuItem(HANDLE m, DWORD i, DWORD f) {
  (void)m; (void)i; (void)f;
  return 0;
}
static WINAPI BOOL FlashWindowEx(void* p) {
  (void)p;
  return 0;
}
static WINAPI BOOL IsWindowUnicode(HANDLE h) {
  (void)h;
  return 1;
}
/* ---------- retangulos ---------- */
static WINAPI BOOL SetRect(struct rect* r, int l, int t, int ri, int b) {
  r->left = l;
  r->top = t;
  r->right = ri;
  r->bottom = b;
  return 1;
}
static WINAPI BOOL SetRectEmpty(struct rect* r) { return SetRect(r, 0, 0, 0, 0); }
static WINAPI BOOL CopyRect(struct rect* d, const struct rect* s) {
  *d = *s;
  return 1;
}
static WINAPI BOOL OffsetRect(struct rect* r, int dx, int dy) {
  r->left += dx;
  r->right += dx;
  r->top += dy;
  r->bottom += dy;
  return 1;
}
static WINAPI BOOL InflateRect(struct rect* r, int dx, int dy) {
  r->left -= dx;
  r->right += dx;
  r->top -= dy;
  r->bottom += dy;
  return 1;
}
static WINAPI BOOL PtInRect(const struct rect* r, int64_t pt) {
  int x = (int)(int32_t)pt, y = (int)(int32_t)(pt >> 32);
  return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}
static WINAPI BOOL IsRectEmpty(const struct rect* r) { return r->right <= r->left || r->bottom <= r->top; }
static WINAPI BOOL EqualRect(const struct rect* a, const struct rect* b) {
  return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}
static WINAPI BOOL IntersectRect(struct rect* d, const struct rect* a, const struct rect* b) {
  d->left = a->left > b->left ? a->left : b->left;
  d->top = a->top > b->top ? a->top : b->top;
  d->right = a->right < b->right ? a->right : b->right;
  d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
  if (d->right <= d->left || d->bottom <= d->top) {
    SetRectEmpty(d);
    return 0;
  }
  return 1;
}
static WINAPI BOOL UnionRect(struct rect* d, const struct rect* a, const struct rect* b) {
  d->left = a->left < b->left ? a->left : b->left;
  d->top = a->top < b->top ? a->top : b->top;
  d->right = a->right > b->right ? a->right : b->right;
  d->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
  return 1;
}

#define S(n) {#n, (void*)n}
const struct wine_sym gui_syms[] = {
    S(RegisterClassExW), S(RegisterClassW), S(UnregisterClassW), S(GetClassInfoExW), S(GetClassInfoW),
    S(GetClassNameW), S(GetClassLongPtrW), S(SetClassLongPtrW), S(CreateWindowExW), S(CreateWindowExA),
    S(DestroyWindow), S(ShowWindow), S(UpdateWindow), S(InvalidateRect), S(ValidateRect), S(RedrawWindow),
    S(IsWindow), S(IsWindowVisible), S(IsWindowEnabled), {"EnableWindow", (void*)wEnableWindow}, S(GetClientRect),
    S(GetWindowRect), S(ClientToScreen), S(ScreenToClient), S(MapWindowPoints), S(SetWindowPos), S(MoveWindow),
    S(GetParent), S(GetAncestor), S(SetParent), S(IsChild), S(GetWindow), S(GetTopWindow), S(GetDlgCtrlID),
    S(SetWindowTextW), S(SetWindowTextA), S(GetWindowTextW), S(GetWindowTextLengthW), S(SetWindowLongPtrW),
    S(GetWindowLongPtrW), S(SetWindowLongW), S(GetWindowLongW), S(GetWindowThreadProcessId), S(SetFocus),
    S(GetFocus), S(GetActiveWindow), S(SetActiveWindow), S(SetForegroundWindow), S(BringWindowToTop),
    S(GetForegroundWindow), S(GetDesktopWindow), S(GetShellWindow), S(IsIconic), S(IsZoomed), S(FindWindowExW),
    S(FindWindowW), S(EnumChildWindows), S(EnumWindows), S(GetCapture), S(SetCapture), S(ReleaseCapture),
    {"GetMessageW", (void*)wGetMessageW}, S(PeekMessageW), S(WaitMessage), S(TranslateMessage),
    {"DispatchMessageW", (void*)wDispatchMessageW}, S(PostMessageW), S(PostThreadMessageW), S(PostQuitMessage),
    S(SendMessageW), S(SendMessageTimeoutW), S(SendNotifyMessageW), S(CallWindowProcW),
    S(MsgWaitForMultipleObjectsEx), S(MsgWaitForMultipleObjects), S(GetQueueStatus), S(BeginPaint), S(EndPaint),
    {"DefWindowProcW", (void*)wDefWindowProcW}, S(SetTimer), S(KillTimer), S(GetSystemMetrics),
    S(GetSystemMetricsForDpi), S(GetDpiForSystem), S(GetDpiForWindow), S(SetProcessDPIAware), S(ChangeWindowMessageFilterEx), S(ChangeWindowMessageFilter), S(GetCaretBlinkTime),
    S(SetProcessDpiAwarenessContext), S(SetProcessDpiAwareness), S(AdjustWindowRectEx), S(AdjustWindowRect),
    S(SystemParametersInfoW), S(LoadCursorW), S(LoadIconW), S(LoadImageW), S(DestroyIcon), S(CopyImage),
    S(DrawIconEx), S(DrawIcon), S(SetCursor), S(GetCursorPos), S(ShowCursor), S(GetKeyState), S(GetAsyncKeyState),
    S(MonitorFromWindow), S(MonitorFromPoint), S(GetMonitorInfoW), S(MessageBeep), S(SetUserObjectInformationW),
    S(WaitForInputIdle), S(SetProcessDefaultLayout), S(GetSystemMenu), S(EnableMenuItem), S(FlashWindowEx),
    S(IsWindowUnicode), S(SetRect), S(SetRectEmpty), S(CopyRect), S(OffsetRect), S(InflateRect), S(PtInRect),
    S(IsRectEmpty), S(EqualRect), S(IntersectRect), S(UnionRect),
    {0, 0}};
