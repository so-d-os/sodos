/*
 * gui_int.h - Janelas, GDI e controles do WINE.DLL (user.c, gdi.c, ctl.c,
 * dlg.c, text.c).
 *
 * Cada processo Windows tem UMA janela no WIN (o "canvas"). Dentro dela o
 * WINE.DLL compoe as janelas de topo do programa: a primeira que aparece
 * ocupa o canvas sem moldura (a moldura e o titulo sao os do WIN); as outras
 * (MessageBox, um segundo dialogo) sao desenhadas por cima, centralizadas,
 * com moldura e barra de titulo proprias. Cada janela de topo tem uma
 * superficie RGBA do tamanho da area cliente; as filhas (controles) desenham
 * nela, no proprio retangulo.
 */
#pragma once
#include "wine_int.h"

/* ---------- mensagens ---------- */
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_NEXTDLGCTL 0x0028
#define WM_DRAWITEM 0x002B
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NOTIFY 0x004E
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_GETDLGCODE 0x0087
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORDLG 0x0136
#define WM_CTLCOLORSTATIC 0x0138
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MOUSEWHEEL 0x020A
#define WM_USER 0x0400

#define BN_CLICKED 0
#define EN_CHANGE 0x0300
#define LBN_SELCHANGE 1
#define CBN_SELCHANGE 1

/* estilos */
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define WS_VSCROLL 0x00200000u
#define WS_GROUP 0x00020000u
#define WS_TABSTOP 0x00010000u

typedef intptr_t (WINAPI * wndproc_fn)(HANDLE, DWORD, uintptr_t, intptr_t);

struct rect {
  int32_t left, top, right, bottom;
};
struct point {
  int32_t x, y;
};
struct msg {
  HANDLE hwnd;
  DWORD message;
  uintptr_t wparam;
  intptr_t lparam;
  DWORD time;
  struct point pt;
  DWORD priv;
};

/* ---------- superficies e GDI ---------- */
/* Pixels de 32 bits: rgba = R,G,B,A (janelas); senao B,G,R,A (DIB do Windows). */
struct surf {
  uint8_t* px;
  int w, h;
  int rgba;
  int bottom_up;
  int foreign;   /* px e de uma secao (CreateDIBSection com hSection): nao solta */
};

#define DC_MAGIC 0x43445757u  /* "WWDC" */
#define GDI_MAGIC 0x44475757u /* "WWGD" */
enum { G_BRUSH = 1, G_PEN, G_BITMAP, G_FONT, G_RGN };
struct wgdi {
  DWORD magic;
  int kind;
  DWORD color;      /* COLORREF 0x00BBGGRR */
  int null;         /* NULL_BRUSH / NULL_PEN */
  int stock;
  int bold;         /* fonte */
  int height;       /* fonte: altura pedida (px) */
  struct surf s;    /* bitmap */
  int rl, rt, rr, rb; /* regiao (so retangulo: o que o Chromium/Skia usa) */
};

struct wwin;
struct wdc {
  DWORD magic;
  struct wwin* win;       /* DC de janela (0 = de memoria) */
  struct wgdi* bitmap;    /* DC de memoria */
  struct wgdi* brush;
  struct wgdi* pen;
  struct wgdi* font;
  DWORD text_color, bk_color;
  int bk_mode;            /* 1 TRANSPARENT, 2 OPAQUE */
  int painting;
  int ox, oy;             /* origem (0,0 do cliente) na superficie */
  struct rect clip;       /* em coordenadas da superficie */
  int cur_x, cur_y;       /* MoveToEx */
  DWORD align;
  struct wgui* xgui;      /* GetDC de janela de outro processo: gui do dono */
};

/* ---------- janelas ---------- */
#define WIN_MAGIC 0x4E495757u /* "WWIN" */

/* classes embutidas (controles do Windows) */
enum { CK_APP = 0, CK_BUTTON, CK_STATIC, CK_EDIT, CK_LISTBOX, CK_COMBO, CK_PROGRESS, CK_DIALOG, CK_LINK,
       CK_LISTVIEW, CK_TOOLBAR, CK_STATUS, CK_REBAR, CK_OTHER };

struct wclass {
  struct wclass* next;
  wchar16* name;
  wndproc_fn proc;
  struct wgdi* bg;        /* hbrBackground (0 = nada) */
  uintptr_t bg_sys;       /* (HBRUSH)(COLOR_x + 1) */
  int extra;              /* cbWndExtra */
  int kind;
  uint16_t atom;          /* devolvido pelo RegisterClass (MAKEINTATOM) */
};

#define WEXTRA 128

struct wwin {
  DWORD magic;
  struct wwin* next;      /* lista de todas */
  struct wwin* parent;    /* 0 = de topo */
  struct wwin* owner;     /* de topo: dona (dialogo modal, MessageBox) */
  struct wwin* child;     /* primeira filha (ordem de criacao = de tras para frente) */
  struct wwin* sibling;
  struct wclass* cls;
  wndproc_fn proc;
  DWORD style, exstyle;
  uintptr_t id;
  int x, y, w, h;         /* filha: no cliente da mae; de topo: tamanho do cliente */
  int visible, enabled, destroyed;
  wchar16* text;
  intptr_t userdata;
  uint8_t extra[WEXTRA];  /* cbWndExtra; nos dialogos DWLP_* */
  uintptr_t thread;       /* TEB de quem criou */
  struct wdc dc;
  struct wgdi* font;
  /* de topo */
  struct surf s;
  int dirty;              /* precisa repintar a arvore */
  int z;                  /* ordem no canvas */
  int cx, cy;             /* posicao do cliente no canvas */
  int framed;             /* desenhada com moldura propria */
  struct wwin* focus;     /* filha com o foco */
  /* dialogo */
  int dlg_end;
  intptr_t dlg_result;
  int modal;
  /* controles */
  int check, pressed, hot;
  int64_t pmin, pmax, ppos;     /* progresso */
  int sel_a, sel_b;             /* edit */
  int scroll;                   /* edit/list: primeira linha */
  wchar16** items;              /* list/combo */
  intptr_t* item_data;
  int nitems, cap_items, cur;
  struct wgdi* image;           /* static SS_BITMAP */
  uintptr_t res_id;             /* titulo dado como ordinal (id de recurso) */
  int limit;
};

/* ---------- estado por processo ---------- */
struct wtimer {
  struct wtimer* next;
  struct wwin* win;
  uintptr_t id;
  DWORD period;
  uint64_t due;
  void* proc;
  uintptr_t thread;
};

struct sentmsg {
  struct sentmsg* next;
  struct wwin* w;
  DWORD m;
  uintptr_t wp;
  intptr_t lp;
  intptr_t result;
  volatile int done;
};

#define MSGQ 512
struct qmsg {
  struct msg m;
  uintptr_t thread;       /* mensagem de thread (hwnd 0) */
};

struct wgui {
  struct wclass* classes;
  struct wwin* wins;
  struct wtimer* timers;
  struct sentmsg* sent;
  struct qmsg q[MSGQ];
  int qhead, qn;
  int quit_code;
  uintptr_t quit_thread;
  int quit_posted;
  int zseq;
  /* canvas: a janela do WIN */
  struct surf canvas;
  int canvas_open;
  struct wwin* canvas_main;   /* desenhada sem moldura em (0,0) */
  int canvas_dirty;
  uint64_t present_at;
  uint32_t present_cost;
  int present_pending;
  volatile int presenting;    /* present_now em andamento (uma thread por vez) */
  volatile int req, req_done;
  int req_rc, req_w, req_h;
  char req_title[96];
  /* entrada */
  struct wwin* capture;
  struct wwin* active;
  int mouse_down;
  struct wgdi stock[20];
  struct wgdi sysbrush[31];
};

struct wgui* gui_get(void);
struct wwin* win_of(HANDLE h);
struct wwin* win_any(HANDLE h, struct wgui** gp); /* HWND de qualquer processo */
void wgui_proc_exit(void);
struct wwin* win_top(struct wwin* w);
intptr_t wsend(struct wwin* w, DWORD m, uintptr_t wp, intptr_t lp);
void win_invalidate(struct wwin* w);
struct wwin* win_create(DWORD ex, const wchar16* cls, const wchar16* title, DWORD style, int x, int y, int cw,
                        int ch, struct wwin* parent, uintptr_t id, void* param);
void win_destroy(struct wwin* w);
void win_show(struct wwin* w, int show);
void win_set_text(struct wwin* w, const wchar16* t);
int win_get_text(struct wwin* w, wchar16* buf, int n);
void win_set_focus(struct wwin* w);
void win_abs_client(struct wwin* w, int* x, int* y); /* origem do cliente na superficie */
int message_pump_one(int wait);                     /* dlg.c: um ciclo do laco */
BOOL WINAPI wGetMessageW(struct msg* m, HANDLE h, DWORD lo, DWORD hi);
intptr_t WINAPI wDispatchMessageW(const struct msg* m);
BOOL WINAPI wIsDialogMessageW(HANDLE h, struct msg* m);
BOOL WINAPI wEnableWindow(HANDLE h, BOOL en);
int wndclass_kind(const wchar16* name);

/* gdi.c */
DWORD sys_color(int i);
struct wgdi* gdi_of(HANDLE h);
struct wdc* dc_of(HANDLE h);
struct surf* dc_surf(struct wdc* d);
void dc_prepare(struct wdc* d, struct wwin* w);     /* origem e recorte da janela */
DWORD brush_color(HANDLE brush, int* null);
void dc_fill(struct wdc* d, int l, int t, int r, int b, DWORD c);
void dc_frame(struct wdc* d, int l, int t, int r, int b, DWORD c);
void dc_line(struct wdc* d, int x0, int y0, int x1, int y1, DWORD c);
int surf_alloc(struct surf* s, int w, int h, int rgba);
void surf_fill(struct surf* s, int l, int t, int r, int b, DWORD c);
struct wgdi* bitmap_from_dib(const uint8_t* bih, const uint8_t* bits);
void surf_blit(struct surf* dst, int dx, int dy, int dw, int dh, const struct surf* src, int sx, int sy, int sw, int sh,
               const struct rect* clip);
HANDLE WINAPI wSelectObject(HANDLE dc, HANDLE obj);
HANDLE WINAPI wGetStockObject(int i);

/* text.c: fonte da interface (TrueType do WIN) */
int text_ready(void);
int text_height(void);       /* altura da linha */
int text_ascent(void);
int text_width(const wchar16* s, int n);
/* Desenha s (n < 0: ate o 0) com o topo da linha em y. Devolve a largura. */
int text_draw(struct surf* s, const struct rect* clip, int x, int y, const wchar16* str, int n, DWORD color, int bold);
/* Texto com quebra de linha dentro de r; DT_* do Windows. Devolve a altura. */
int text_box(struct surf* s, const struct rect* clip, const struct rect* r, const wchar16* str, int n, DWORD color,
             int bold, DWORD fmt);

#define MAKELONG_(a, b) ((intptr_t)(((DWORD)(a) & 0xFFFF) | (((DWORD)(b) & 0xFFFF) << 16)))
/* comctl.c */
intptr_t comctl_proc(struct wwin* w, DWORD m, uintptr_t wp, intptr_t lp);
int comboex_msg(struct wwin* w, DWORD m, uintptr_t wp, intptr_t lp, intptr_t* res);
/* ctl.c */
intptr_t WINAPI ctl_proc(HANDLE h, DWORD m, uintptr_t wp, intptr_t lp);
/* dlg.c */
intptr_t WINAPI wDefDlgProcW(HANDLE h, DWORD m, uintptr_t wp, intptr_t lp);
intptr_t WINAPI wDefWindowProcW(HANDLE h, DWORD m, uintptr_t wp, intptr_t lp);
int dlg_units_x(int du);
int dlg_units_y(int du);

/* DT_* */
#define DT_CENTER 0x1
#define DT_RIGHT 0x2
#define DT_VCENTER 0x4
#define DT_BOTTOM 0x8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
#define DT_END_ELLIPSIS 0x8000
