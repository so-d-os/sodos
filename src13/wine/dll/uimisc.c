/*
 * uimisc.c - O resto do USER32/COMCTL32/SHELL32/UXTHEME/GDIPLUS que um
 * programa grafico "de verdade" (WinRAR.exe, por exemplo) importa.
 *
 * O que da para fazer certo sem desenho fica certo: mensagens registradas,
 * propriedades de janela, subclasses do comctl32, menus (guardados, mas ainda
 * sem barra desenhada), area de transferencia dentro do processo, ImageList
 * como lista de icones. O resto responde "nao ha" do jeito que o Windows
 * responde quando a funcao falha, para o programa seguir pelo caminho normal
 * de erro em vez de cair no stub (que encerra o processo).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "gui_int.h"

#define E_FAIL_ 0x80004005L
#define E_NOTIMPL_ 0x80004001L
#define E_NOINTERFACE_ 0x80004002L

static int ieq16(const wchar16* a, const wchar16* b) {
  for (;; a++, b++) {
    wchar16 x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return 0;
    if (!x) return 1;
  }
}

/* ---------- mensagens e formatos registrados (0xC000..0xFFFF) ---------- */
static wchar16* reg_names[256];
static int reg_n;
static WINAPI DWORD RegisterWindowMessageW(const wchar16* name) {
  int i;
  DWORD r = 0;
  if (!name || !*name) return 0;
  wlock();
  for (i = 0; i < reg_n; i++)
    if (ieq16(reg_names[i], name)) r = 0xC000u + (DWORD)i;
  if (!r && reg_n < 256) {
    size_t n = (wstrlen16(name) + 1) * 2;
    wchar16* d = (wchar16*)k_malloc((unsigned)n);
    if (d) {
      memcpy(d, name, n);
      reg_names[reg_n] = d;
      r = 0xC000u + (DWORD)reg_n++;
    }
  }
  wunlock();
  return r;
}
static WINAPI DWORD RegisterWindowMessageA(const char* name) {
  wchar16* w = wdup8to16(name);
  DWORD r = w ? RegisterWindowMessageW(w) : 0;
  k_free(w);
  return r;
}
static WINAPI DWORD RegisterClipboardFormatW(const wchar16* name) { return RegisterWindowMessageW(name); }

/* ---------- propriedades de janela ---------- */
struct prop {
  struct prop* next;
  HANDLE hwnd;
  uintptr_t atom;  /* nome curto (ATOM) */
  wchar16* name;
  HANDLE val;
};
static struct prop* props;
static int prop_match(struct prop* p, HANDLE h, const wchar16* name) {
  if (p->hwnd != h) return 0;
  if ((uintptr_t)name < 0x10000) return p->atom == (uintptr_t)name;
  return p->name && ieq16(p->name, name);
}
static WINAPI BOOL SetPropW(HANDLE h, const wchar16* name, HANDLE v) {
  struct prop* p;
  wlock();
  for (p = props; p; p = p->next)
    if (prop_match(p, h, name)) break;
  if (!p) {
    p = (struct prop*)wzalloc(sizeof(*p));
    if (!p) {
      wunlock();
      return 0;
    }
    p->hwnd = h;
    if ((uintptr_t)name < 0x10000) {
      p->atom = (uintptr_t)name;
    } else {
      size_t n = (wstrlen16(name) + 1) * 2;
      p->name = (wchar16*)k_malloc((unsigned)n);
      if (p->name) memcpy(p->name, name, n);
    }
    p->next = props;
    props = p;
  }
  p->val = v;
  wunlock();
  return 1;
}
static WINAPI HANDLE GetPropW(HANDLE h, const wchar16* name) {
  struct prop* p;
  HANDLE v = 0;
  wlock();
  for (p = props; p; p = p->next)
    if (prop_match(p, h, name)) {
      v = p->val;
      break;
    }
  wunlock();
  return v;
}
static WINAPI HANDLE RemovePropW(HANDLE h, const wchar16* name) {
  struct prop** pp;
  HANDLE v = 0;
  wlock();
  for (pp = &props; *pp; pp = &(*pp)->next)
    if (prop_match(*pp, h, name)) {
      struct prop* p = *pp;
      *pp = p->next;
      v = p->val;
      k_free(p->name);
      k_free(p);
      break;
    }
  wunlock();
  return v;
}

/* ---------- subclasses do comctl32 (SetWindowSubclass e cia.) ---------- */
typedef intptr_t(WINAPI* subproc_fn)(HANDLE, DWORD, uintptr_t, intptr_t, uintptr_t, uintptr_t);
#define MAX_SUB 8
struct subcls {
  struct subcls* next;
  HANDLE hwnd;
  wndproc_fn orig;
  int n;
  int depth;  /* indice da subclasse sendo chamada agora (-1 = nenhuma) */
  subproc_fn fn[MAX_SUB];
  uintptr_t id[MAX_SUB], ref[MAX_SUB];
};
static struct subcls* subs;
static struct subcls* sub_find(HANDLE h) {
  struct subcls* s;
  for (s = subs; s; s = s->next)
    if (s->hwnd == h) return s;
  return 0;
}
static intptr_t sub_call(struct subcls* s, int i, HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  int save = s->depth;
  intptr_t r;
  s->depth = i;
  if (i < 0) r = s->orig ? s->orig(h, m, wp_, lp) : 0;
  else r = s->fn[i](h, m, wp_, lp, s->id[i], s->ref[i]);
  s->depth = save;
  return r;
}
static WINAPI intptr_t sub_dispatch(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct subcls* s = sub_find(h);
  if (!s) return 0;
  return sub_call(s, s->n - 1, h, m, wp_, lp);
}
static WINAPI intptr_t DefSubclassProc(HANDLE h, DWORD m, uintptr_t wp_, intptr_t lp) {
  struct subcls* s = sub_find(h);
  if (!s) return 0;
  return sub_call(s, s->depth - 1, h, m, wp_, lp);
}
static WINAPI BOOL SetWindowSubclass(HANDLE h, subproc_fn fn, uintptr_t id, uintptr_t ref) {
  struct wwin* w = win_of(h);
  struct subcls* s;
  int i;
  if (!w || !fn) return 0;
  s = sub_find(h);
  if (!s) {
    s = (struct subcls*)wzalloc(sizeof(*s));
    if (!s) return 0;
    s->hwnd = h;
    s->orig = w->proc;
    s->depth = -1;
    s->next = subs;
    subs = s;
    w->proc = sub_dispatch;
  }
  for (i = 0; i < s->n; i++)
    if (s->fn[i] == fn && s->id[i] == id) {
      s->ref[i] = ref;
      return 1;
    }
  if (s->n >= MAX_SUB) return 0;
  s->fn[s->n] = fn;
  s->id[s->n] = id;
  s->ref[s->n] = ref;
  s->n++;
  return 1;
}
static WINAPI BOOL RemoveWindowSubclass(HANDLE h, subproc_fn fn, uintptr_t id) {
  struct subcls* s = sub_find(h);
  int i;
  if (!s) return 0;
  for (i = 0; i < s->n; i++)
    if (s->fn[i] == fn && s->id[i] == id) {
      for (; i + 1 < s->n; i++) {
        s->fn[i] = s->fn[i + 1];
        s->id[i] = s->id[i + 1];
        s->ref[i] = s->ref[i + 1];
      }
      s->n--;
      if (s->n == 0) {
        struct wwin* w = win_of(h);
        if (w && w->proc == sub_dispatch) w->proc = s->orig;
        /* o registro fica (pode estar em uso na pilha); so fica vazio */
      }
      return 1;
    }
  return 0;
}
static WINAPI BOOL GetWindowSubclass(HANDLE h, subproc_fn fn, uintptr_t id, uintptr_t* ref) {
  struct subcls* s = sub_find(h);
  int i;
  if (s)
    for (i = 0; i < s->n; i++)
      if (s->fn[i] == fn && s->id[i] == id) {
        if (ref) *ref = s->ref[i];
        return 1;
      }
  return 0;
}

/* ---------- menus (guardados; a barra ainda nao e desenhada) ---------- */
#define MENU_MAGIC 0x554E454Du
struct mitem {
  DWORD flags, id, state;
  struct wmenu* sub;
  wchar16* text;
  uintptr_t data;
};
struct wmenu {
  DWORD magic;
  int n, cap;
  struct mitem* it;
};
struct menuitem_info {
  DWORD size, mask, type, state, id;
  HANDLE sub, checked, unchecked;
  uintptr_t data;
  wchar16* type_data;
  DWORD cch;
  HANDLE bmp;
};
#define MIIM_STATE 0x01
#define MIIM_ID 0x02
#define MIIM_SUBMENU 0x04
#define MIIM_DATA 0x20
#define MIIM_TYPE 0x10
#define MIIM_STRING 0x40
#define MIIM_FTYPE 0x100
#define MF_BYPOSITION 0x400
#define MF_POPUP 0x10
#define MF_SEPARATOR 0x800
#define MF_STRING 0
#define MF_OWNERDRAW 0x100
#define MF_BITMAP 4

static struct wmenu* menu_of(HANDLE h) {
  struct wmenu* m = (struct wmenu*)h;
  return (wptr_ok(m) && m->magic == MENU_MAGIC) ? m : 0;
}
static struct wmenu* menu_new(void) {
  struct wmenu* m = (struct wmenu*)wzalloc(sizeof(*m));
  if (m) m->magic = MENU_MAGIC;
  return m;
}
static wchar16* dup16(const wchar16* s) {
  size_t n;
  wchar16* d;
  if (!s || (uintptr_t)s < 0x10000) return 0;
  n = (wstrlen16(s) + 1) * 2;
  d = (wchar16*)k_malloc((unsigned)n);
  if (d) memcpy(d, s, n);
  return d;
}
static struct mitem* menu_insert(struct wmenu* m, int pos) {
  if (m->n >= m->cap) {
    int nc = m->cap ? m->cap * 2 : 8;
    struct mitem* ni = (struct mitem*)wzalloc(sizeof(struct mitem) * (size_t)nc);
    if (!ni) return 0;
    if (m->it) {
      memcpy(ni, m->it, sizeof(struct mitem) * (size_t)m->n);
      k_free(m->it);
    }
    m->it = ni;
    m->cap = nc;
  }
  if (pos < 0 || pos > m->n) pos = m->n;
  memmove(&m->it[pos + 1], &m->it[pos], sizeof(struct mitem) * (size_t)(m->n - pos));
  memset(&m->it[pos], 0, sizeof(struct mitem));
  m->n++;
  return &m->it[pos];
}
/* procura por posicao ou por id (tambem nos submenus) */
static struct mitem* menu_item(struct wmenu* m, DWORD what, int bypos, struct wmenu** owner) {
  int i;
  if (!m) return 0;
  if (bypos) {
    if (owner) *owner = m;
    return what < (DWORD)m->n ? &m->it[what] : 0;
  }
  for (i = 0; i < m->n; i++) {
    if (!(m->it[i].flags & MF_POPUP) && m->it[i].id == what) {
      if (owner) *owner = m;
      return &m->it[i];
    }
    if (m->it[i].sub) {
      struct mitem* r = menu_item(m->it[i].sub, what, 0, owner);
      if (r) return r;
    }
  }
  return 0;
}
static void menu_free(struct wmenu* m) {
  int i;
  if (!m) return;
  for (i = 0; i < m->n; i++) {
    k_free(m->it[i].text);
    menu_free(m->it[i].sub);
  }
  k_free(m->it);
  m->magic = 0;
  k_free(m);
}
static WINAPI HANDLE CreateMenu(void) { return (HANDLE)menu_new(); }
static WINAPI HANDLE CreatePopupMenu(void) { return (HANDLE)menu_new(); }
static WINAPI BOOL DestroyMenu(HANDLE h) {
  struct wmenu* m = menu_of(h);
  if (!m) return 0;
  menu_free(m);
  return 1;
}
static void item_set(struct mitem* it, DWORD fl, uintptr_t id, const wchar16* text) {
  it->flags = fl & ~MF_BYPOSITION;
  if (fl & MF_POPUP) {
    it->sub = menu_of((HANDLE)id);
    it->id = 0xFFFFFFFFu;
  } else {
    it->id = (DWORD)id;
  }
  k_free(it->text);
  it->text = (fl & (MF_SEPARATOR | MF_OWNERDRAW | MF_BITMAP)) ? 0 : dup16(text);
  if (fl & MF_OWNERDRAW) it->data = (uintptr_t)text;
  it->state = fl & 0x0B; /* MF_GRAYED|MF_DISABLED|MF_CHECKED */
}
static WINAPI BOOL AppendMenuW(HANDLE h, DWORD fl, uintptr_t id, const wchar16* text) {
  struct wmenu* m = menu_of(h);
  struct mitem* it = m ? menu_insert(m, -1) : 0;
  if (!it) return 0;
  item_set(it, fl, id, text);
  return 1;
}
static WINAPI BOOL InsertMenuW(HANDLE h, DWORD pos, DWORD fl, uintptr_t id, const wchar16* text) {
  struct wmenu* m = menu_of(h);
  int at = -1;
  struct mitem* it;
  if (!m) return 0;
  if (fl & MF_BYPOSITION) {
    at = pos == 0xFFFFFFFFu ? -1 : (int)pos;
  } else {
    int i;
    for (i = 0; i < m->n; i++)
      if (m->it[i].id == pos) at = i;
  }
  it = menu_insert(m, at);
  if (!it) return 0;
  item_set(it, fl, id, text);
  return 1;
}
static void info_apply(struct mitem* it, const struct menuitem_info* mi) {
  if (mi->mask & (MIIM_TYPE | MIIM_FTYPE)) it->flags = mi->type;
  if (mi->mask & MIIM_STATE) it->state = mi->state;
  if (mi->mask & MIIM_ID) it->id = mi->id;
  if (mi->mask & MIIM_SUBMENU) {
    it->sub = menu_of(mi->sub);
    if (it->sub) it->flags |= MF_POPUP;
  }
  if (mi->mask & MIIM_DATA) it->data = mi->data;
  if ((mi->mask & MIIM_STRING) || ((mi->mask & MIIM_TYPE) && !(mi->type & (MF_SEPARATOR | MF_BITMAP)))) {
    k_free(it->text);
    it->text = dup16(mi->type_data);
  }
}
static WINAPI BOOL InsertMenuItemW(HANDLE h, DWORD pos, BOOL bypos, const struct menuitem_info* mi) {
  struct wmenu* m = menu_of(h);
  struct mitem* it;
  int at = -1;
  if (!m || !mi) return 0;
  if (bypos) {
    at = pos == 0xFFFFFFFFu ? -1 : (int)pos;
  } else {
    int i;
    for (i = 0; i < m->n; i++)
      if (m->it[i].id == pos) at = i;
  }
  it = menu_insert(m, at);
  if (!it) return 0;
  it->id = 0;
  info_apply(it, mi);
  return 1;
}
static WINAPI BOOL SetMenuItemInfoW(HANDLE h, DWORD what, BOOL bypos, const struct menuitem_info* mi) {
  struct mitem* it = menu_item(menu_of(h), what, bypos, 0);
  if (!it || !mi) return 0;
  info_apply(it, mi);
  return 1;
}
static WINAPI BOOL GetMenuItemInfoW(HANDLE h, DWORD what, BOOL bypos, struct menuitem_info* mi) {
  struct mitem* it = menu_item(menu_of(h), what, bypos, 0);
  if (!it || !mi) return 0;
  if (mi->mask & (MIIM_TYPE | MIIM_FTYPE)) mi->type = it->flags & ~(DWORD)MF_POPUP;
  if (mi->mask & MIIM_STATE) mi->state = it->state;
  if (mi->mask & MIIM_ID) mi->id = it->id;
  if (mi->mask & MIIM_SUBMENU) mi->sub = (HANDLE)it->sub;
  if (mi->mask & MIIM_DATA) mi->data = it->data;
  if (mi->mask & (MIIM_STRING | MIIM_TYPE)) {
    DWORD len = it->text ? (DWORD)wstrlen16(it->text) : 0;
    if (mi->type_data && mi->cch) {
      DWORD k = len < mi->cch - 1 ? len : mi->cch - 1;
      if (k) memcpy(mi->type_data, it->text, k * 2);
      mi->type_data[k] = 0;
    }
    mi->cch = len;
  }
  return 1;
}
static WINAPI BOOL DeleteMenu(HANDLE h, DWORD what, DWORD fl) {
  struct wmenu* owner = 0;
  struct mitem* it = menu_item(menu_of(h), what, (fl & MF_BYPOSITION) != 0, &owner);
  int i;
  if (!it) return 0;
  k_free(it->text);
  menu_free(it->sub);
  i = (int)(it - owner->it);
  memmove(&owner->it[i], &owner->it[i + 1], sizeof(struct mitem) * (size_t)(owner->n - i - 1));
  owner->n--;
  return 1;
}
static WINAPI BOOL RemoveMenu(HANDLE h, DWORD what, DWORD fl) {
  struct wmenu* owner = 0;
  struct mitem* it = menu_item(menu_of(h), what, (fl & MF_BYPOSITION) != 0, &owner);
  int i;
  if (!it) return 0;
  k_free(it->text);
  i = (int)(it - owner->it);
  memmove(&owner->it[i], &owner->it[i + 1], sizeof(struct mitem) * (size_t)(owner->n - i - 1));
  owner->n--;
  return 1;
}
static WINAPI DWORD CheckMenuItem(HANDLE h, DWORD what, DWORD fl) {
  struct mitem* it = menu_item(menu_of(h), what, (fl & MF_BYPOSITION) != 0, 0);
  DWORD old;
  if (!it) return 0xFFFFFFFFu;
  old = it->state & 8;
  it->state = (it->state & ~8u) | (fl & 8);
  return old;
}
static WINAPI BOOL EnableMenuItem(HANDLE h, DWORD what, DWORD fl) {
  struct mitem* it = menu_item(menu_of(h), what, (fl & MF_BYPOSITION) != 0, 0);
  DWORD old;
  if (!it) return -1;
  old = it->state & 3;
  it->state = (it->state & ~3u) | (fl & 3);
  return (BOOL)old;
}
static WINAPI BOOL CheckMenuRadioItem(HANDLE h, DWORD first, DWORD last, DWORD check, DWORD fl) {
  DWORD i;
  for (i = first; i <= last; i++) CheckMenuItem(h, i, (fl & MF_BYPOSITION) | (i == check ? 8u : 0u));
  return 1;
}
static WINAPI int GetMenuItemCount(HANDLE h) {
  struct wmenu* m = menu_of(h);
  return m ? m->n : -1;
}
static WINAPI DWORD GetMenuItemID(HANDLE h, int pos) {
  struct wmenu* m = menu_of(h);
  if (!m || pos < 0 || pos >= m->n || (m->it[pos].flags & MF_POPUP)) return 0xFFFFFFFFu;
  return m->it[pos].id;
}
static WINAPI HANDLE GetSubMenu(HANDLE h, int pos) {
  struct wmenu* m = menu_of(h);
  if (!m || pos < 0 || pos >= m->n) return 0;
  return (HANDLE)m->it[pos].sub;
}
static WINAPI DWORD GetMenuState(HANDLE h, DWORD what, DWORD fl) {
  struct mitem* it = menu_item(menu_of(h), what, (fl & MF_BYPOSITION) != 0, 0);
  if (!it) return 0xFFFFFFFFu;
  if (it->sub) return ((DWORD)it->sub->n << 8) | MF_POPUP | it->state;
  return it->state | (it->flags & MF_SEPARATOR);
}
static WINAPI int GetMenuStringW(HANDLE h, DWORD what, wchar16* buf, int n, DWORD fl) {
  struct mitem* it = menu_item(menu_of(h), what, (fl & MF_BYPOSITION) != 0, 0);
  int len;
  if (!it) return 0;
  len = it->text ? (int)wstrlen16(it->text) : 0;
  if (buf && n > 0) {
    int k = len < n - 1 ? len : n - 1;
    if (k) memcpy(buf, it->text, (size_t)k * 2);
    buf[k] = 0;
    return k;
  }
  return len;
}
static WINAPI BOOL SetMenuInfo(HANDLE h, const void* mi) {
  (void)mi;
  return menu_of(h) != 0;
}
static WINAPI BOOL GetMenuInfo(HANDLE h, void* mi) {
  (void)mi;
  return menu_of(h) != 0;
}
static WINAPI BOOL SetMenuDefaultItem(HANDLE h, DWORD a, DWORD b) {
  (void)a;
  (void)b;
  return menu_of(h) != 0;
}
static WINAPI BOOL ModifyMenuW(HANDLE h, DWORD what, DWORD fl, uintptr_t id, const wchar16* text) {
  struct mitem* it = menu_item(menu_of(h), what, (fl & MF_BYPOSITION) != 0, 0);
  if (!it) return 0;
  item_set(it, fl, id, text);
  return 1;
}

/* menu de recurso (RT_MENU = 4): MENUHEADER normal ou MENUEX */
static const uint16_t* parse_menu(struct wmenu* m, const uint16_t* p, const uint16_t* end) {
  while (p < end) {
    uint16_t fl = *p++, id = 0;
    struct mitem* it;
    if (!(fl & MF_POPUP)) id = *p++;
    it = menu_insert(m, -1);
    if (!it) return end;
    it->flags = fl & ~0x80u & ~(DWORD)MF_POPUP;
    it->state = fl & 0x0B;
    it->id = id;
    if (*p) it->text = dup16((const wchar16*)p);
    else if (!(fl & MF_POPUP) && !id) it->flags |= MF_SEPARATOR;
    p += wstrlen16((const wchar16*)p) + 1;
    if (fl & MF_POPUP) {
      it->flags |= MF_POPUP;
      it->id = 0xFFFFFFFFu;
      it->sub = menu_new();
      if (it->sub) p = parse_menu(it->sub, p, end);
    }
    if (fl & 0x80) break; /* MF_END */
  }
  return p;
}
static const uint8_t* parse_menuex(struct wmenu* m, const uint8_t* p, const uint8_t* end) {
  while (p + 14 <= end) {
    DWORD type, state, id;
    uint16_t res;
    struct mitem* it;
    const wchar16* txt;
    memcpy(&type, p, 4);
    memcpy(&state, p + 4, 4);
    memcpy(&id, p + 8, 4);
    memcpy(&res, p + 12, 2);
    p += 14;
    txt = (const wchar16*)p;
    p += (wstrlen16(txt) + 1) * 2;
    p = (const uint8_t*)(((uintptr_t)p + 3) & ~(uintptr_t)3);
    it = menu_insert(m, -1);
    if (!it) return end;
    it->flags = type;
    it->state = state;
    it->id = id;
    if (*txt) it->text = dup16(txt);
    if (res & 1) {
      p += 4; /* dwHelpId */
      it->flags |= MF_POPUP;
      it->sub = menu_new();
      if (it->sub) p = parse_menuex(it->sub, p, end);
    }
    if (res & 0x80) break;
  }
  return p;
}
static WINAPI HANDLE LoadMenuW(HANDLE inst, const wchar16* name) {
  struct wmod* mod = pe_from_handle(inst);
  uint32_t sz = 0;
  const uint8_t* r = mod ? pe_find_resource(mod, 4, (uintptr_t)name, &sz) : 0;
  struct wmenu* m;
  uint16_t ver, off;
  if (!r || sz < 4) return 0;
  m = menu_new();
  if (!m) return 0;
  memcpy(&ver, r, 2);
  memcpy(&off, r + 2, 2);
  if (ver == 1) parse_menuex(m, r + 4 + off, r + sz);
  else parse_menu(m, (const uint16_t*)(r + 4 + off), (const uint16_t*)(r + sz));
  return (HANDLE)m;
}
static WINAPI HANDLE LoadMenuIndirectW(const void* tmpl) {
  (void)tmpl;
  return (HANDLE)menu_new();
}

/* menu de cada janela de topo */
struct winmenu {
  struct winmenu* next;
  HANDLE hwnd, menu;
};
static struct winmenu* wmenus;
static WINAPI HANDLE GetMenu(HANDLE h) {
  struct winmenu* x;
  for (x = wmenus; x; x = x->next)
    if (x->hwnd == h) return x->menu;
  return 0;
}
static WINAPI BOOL SetMenu(HANDLE h, HANDLE menu) {
  struct winmenu* x;
  if (!win_of(h)) return 0;
  wlock();
  for (x = wmenus; x; x = x->next)
    if (x->hwnd == h) break;
  if (!x) {
    x = (struct winmenu*)wzalloc(sizeof(*x));
    if (x) {
      x->hwnd = h;
      x->next = wmenus;
      wmenus = x;
    }
  }
  if (x) x->menu = menu;
  wunlock();
  return x != 0;
}
static WINAPI BOOL DrawMenuBar(HANDLE h) { return win_of(h) != 0; }
static WINAPI HANDLE GetSystemMenu(HANDLE h, BOOL revert) {
  static struct wmenu* sys;
  (void)h;
  (void)revert;
  if (!sys) sys = menu_new();
  return (HANDLE)sys;
}
static WINAPI BOOL TrackPopupMenu(HANDLE m, DWORD fl, int x, int y, int r, HANDLE h, const void* rc) {
  (void)m; (void)fl; (void)x; (void)y; (void)r; (void)h; (void)rc;
  return 0; /* menu fechado sem escolha */
}
static WINAPI BOOL TrackPopupMenuEx(HANDLE m, DWORD fl, int x, int y, HANDLE h, const void* tpm) {
  (void)m; (void)fl; (void)x; (void)y; (void)h; (void)tpm;
  return 0;
}
static WINAPI HANDLE LoadAcceleratorsW(HANDLE inst, const wchar16* name) {
  struct wmod* mod = pe_from_handle(inst);
  uint32_t sz = 0;
  const uint8_t* r = mod ? pe_find_resource(mod, 9, (uintptr_t)name, &sz) : 0;
  return (HANDLE)(uintptr_t)r;
}
static WINAPI int TranslateAcceleratorW(HANDLE h, HANDLE acc, const void* msg) {
  (void)h;
  (void)acc;
  (void)msg;
  return 0;
}
static WINAPI BOOL DestroyAcceleratorTable(HANDLE h) {
  (void)h;
  return 1;
}

/* ---------- ganchos e eventos: aceitos, nunca chamados ---------- */
static uintptr_t hook_seq = 0x7000;
static WINAPI HANDLE SetWindowsHookExW(int id, void* fn, HANDLE mod, DWORD tid) {
  (void)id; (void)fn; (void)mod; (void)tid;
  return (HANDLE)(hook_seq += 4);
}
static WINAPI BOOL UnhookWindowsHookEx(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI intptr_t CallNextHookEx(HANDLE h, int code, uintptr_t wp_, intptr_t lp) {
  (void)h; (void)code; (void)wp_; (void)lp;
  return 0;
}
static WINAPI HANDLE SetWinEventHook(DWORD a, DWORD b, HANDLE mod, void* fn, DWORD pid, DWORD tid, DWORD fl) {
  (void)a; (void)b; (void)mod; (void)fn; (void)pid; (void)tid; (void)fl;
  return (HANDLE)(hook_seq += 4);
}
static WINAPI BOOL UnhookWinEvent(HANDLE h) {
  (void)h;
  return 1;
}

/* ---------- area de transferencia ----------
 * Texto vai e vem do clipboard do SODOS (k_clip_get/k_clip_set, o mesmo do
 * DOS, Command e Bloco de notas): Ctrl+V no Claude cola o que foi copiado no
 * SODOS e copiar no Claude vale fora dele. O texto do SODOS e lido como UTF-8
 * (Latin-1 se nao for UTF-8 valido). Outros formatos ficam so no processo. */
#define CF_TEXT_ 1
#define CF_OEMTEXT_ 7
#define CF_UNICODETEXT_ 13
int k_clip_get(char* out, int max);
void k_clip_set(const char* s, int n);
static DWORD clip_fmt;
static void* clip_data;
static int clip_open;
static void* clip_sodos;  /* copia entregue por GetClipboardData (liberada na proxima) */
static int is_text(DWORD f) { return f == CF_TEXT_ || f == CF_OEMTEXT_ || f == CF_UNICODETEXT_; }
static int sodos_text(char* buf, int max) {
  int n = k_clip_get(buf, max);
  if (n < 0) n = 0;
  if (n >= max) n = max - 1;
  buf[n] = 0;
  return n;
}
static int clip_len_sodos(void) {
  char b[8];
  int n = k_clip_get(b, (int)sizeof(b));
  return n < 0 ? 0 : n;
}
static int utf8_ok(const char* s) {
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
    if (k < 0) return 0;
    p++;
    while (k--) {
      if ((*p & 0xC0) != 0x80) return 0;
      p++;
    }
  }
  return 1;
}
/* Rastro das primeiras chamadas (SERIAL.LOG, "[wine] clip ..."): o Ctrl+V do
 * Claude nao colava e nada aparecia no log. */
static int clip_trace_n;
static void clip_trace(const char* what, uint64_t v) {
  if (clip_trace_n >= 96 && !wine_debug) return;
  clip_trace_n++;
  wlogx(what, v);
}
/* O WINE.DLL e um so para todos os processos: quem abriu e guardado (TEB da
 * thread). Como no Windows, a mesma thread pode abrir de novo; aberta por
 * outra ha mais de 300 ms = esquecida aberta (processo que morreu entre o
 * Open e o Close), e a vez passa. Antes um Open sem Close travava a colagem
 * de todos para sempre. */
static uint8_t* volatile clip_owner;
static uint64_t clip_since;
static WINAPI BOOL OpenClipboard(HANDLE h) {
  uint8_t* t = wteb();
  uint64_t now = wnow_ms();
  (void)h;
  if (clip_open && clip_owner != t && now - clip_since < 300) {
    clip_trace("clip: OpenClipboard ocupado, dono teb ", (uint64_t)(uintptr_t)clip_owner);
    wset_lasterr(5); /* ERROR_ACCESS_DENIED */
    return 0;
  }
  if (clip_open && clip_owner != t) clip_trace("clip: OpenClipboard tomou de teb esquecido ", (uint64_t)(uintptr_t)clip_owner);
  clip_open = 1;
  clip_owner = t;
  clip_since = now;
  {
    /* o tamanho sai sempre que muda (o limite do rastro escondia se a copia
     * do Command tinha chegado) */
    static int last_len = -1;
    int n = clip_len_sodos();
    if (n != last_len) wlogx("clip: OpenClipboard, texto SODOS bytes (mudou) ", (uint64_t)n);
    last_len = n;
  }
  return 1;
}
static WINAPI BOOL CloseClipboard(void) {
  clip_open = 0;
  clip_owner = 0;
  return 1;
}
static WINAPI BOOL EmptyClipboard(void) {
  if (clip_data) k_free(clip_data);
  clip_data = 0;
  clip_fmt = 0;
  return 1;
}
static WINAPI HANDLE SetClipboardData(DWORD fmt, HANDLE mem) {
  /* sempre no log: gravar texto aqui troca o clipboard do SODOS inteiro */
  wlogx("clip: SetClipboardData fmt ", fmt);
  if (mem && fmt == CF_UNICODETEXT_) wlogx("clip:   caracteres ", (uint64_t)(wheap_size(mem) / 2));
  if (clip_data) k_free(clip_data);
  clip_fmt = fmt;
  clip_data = mem; /* passa a ser do sistema */
  if (mem && fmt == CF_UNICODETEXT_) {
    char* u = wdup16to8((const wchar16*)mem);
    if (u) {
      k_clip_set(u, -1);
      k_free(u);
    }
  } else if (mem && (fmt == CF_TEXT_ || fmt == CF_OEMTEXT_)) {
    k_clip_set((const char*)mem, -1);
  }
  return mem;
}
static WINAPI HANDLE GetClipboardData(DWORD fmt) {
  static char buf[65536];
  int n;
  clip_trace("clip: GetClipboardData fmt ", fmt);
  if (!is_text(fmt)) return fmt == clip_fmt ? (HANDLE)clip_data : 0;
  n = sodos_text(buf, (int)sizeof(buf));
  clip_trace("clip: GetClipboardData texto bytes ", (uint64_t)n);
  if (!n) return fmt == clip_fmt ? (HANDLE)clip_data : 0;
  if (clip_sodos) k_free(clip_sodos);
  clip_sodos = 0;
  if (fmt == CF_UNICODETEXT_) {
    wchar16* w;
    int i, k;
    if (utf8_ok(buf)) {
      k = wutf8_to_16(buf, -1, 0, 0);
      w = (wchar16*)k_malloc((unsigned)(k + 1) * 2u);
      if (!w) return 0;
      wutf8_to_16(buf, -1, w, k);
      w[k] = 0;
    } else {
      w = (wchar16*)k_malloc((unsigned)(n + 1) * 2u);
      if (!w) return 0;
      for (i = 0; i < n; i++) w[i] = (uint8_t)buf[i];
      w[n] = 0;
    }
    clip_sodos = w;
  } else {
    char* t = (char*)k_malloc((unsigned)n + 1u);
    if (!t) return 0;
    memcpy(t, buf, (size_t)n + 1);
    clip_sodos = t;
  }
  return clip_sodos;
}
static WINAPI BOOL IsClipboardFormatAvailable(DWORD fmt) {
  char b[8];
  clip_trace("clip: IsClipboardFormatAvailable fmt ", fmt);
  if (is_text(fmt) && k_clip_get(b, (int)sizeof(b)) > 0) return 1;
  return clip_data && fmt == clip_fmt;
}
static WINAPI DWORD EnumClipboardFormats(DWORD fmt) {
  char b[8];
  int has = k_clip_get(b, (int)sizeof(b)) > 0;
  if (has) {
    if (!fmt) return CF_UNICODETEXT_;
    if (fmt == CF_UNICODETEXT_) return CF_TEXT_;
    if (fmt == CF_TEXT_) return clip_data && !is_text(clip_fmt) ? clip_fmt : 0;
    return 0;
  }
  return (!fmt && clip_data) ? clip_fmt : 0;
}
/* muda quando o texto do SODOS muda (o Chromium usa para saber se a area de
 * transferencia mudou) */
static WINAPI DWORD GetClipboardSequenceNumber(void) {
  static char buf[65536];
  int n = sodos_text(buf, (int)sizeof(buf)), i;
  DWORD hsh = 2166136261u;
  for (i = 0; i < n; i++) hsh = (hsh ^ (uint8_t)buf[i]) * 16777619u;
  return (hsh ^ (DWORD)n) | 1u;
}
static WINAPI long OleSetClipboard(void* obj) {
  (void)obj;
  return 0;
}
static WINAPI long OleGetClipboard(void** obj) {
  clip_trace("clip: OleGetClipboard (sem suporte) ", 0);
  if (obj) *obj = 0;
  return E_FAIL_;
}
static WINAPI long OleFlushClipboard(void) { return 0; }

/* ---------- arrastar e soltar ---------- */
static WINAPI void DragAcceptFiles(HANDLE h, BOOL on) {
  (void)h;
  (void)on;
}
static WINAPI DWORD DragQueryFileW(HANDLE d, DWORD i, wchar16* buf, DWORD n) {
  (void)d; (void)i; (void)buf; (void)n;
  return 0;
}
static WINAPI void DragFinish(HANDLE d) { (void)d; }
static WINAPI long RegisterDragDrop(HANDLE h, void* t) {
  (void)h;
  (void)t;
  return 0;
}
static WINAPI long RevokeDragDrop(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI long DoDragDrop(void* a, void* b, DWORD ok, DWORD* eff) {
  (void)a; (void)b; (void)ok;
  if (eff) *eff = 0;
  return 0x00040101L; /* DRAGDROP_S_CANCEL */
}

/* ---------- barras de rolagem: guardadas por janela ---------- */
struct scrollinfo {
  DWORD size, mask;
  int min, max;
  DWORD page;
  int pos, track;
};
struct wscroll {
  struct wscroll* next;
  HANDLE hwnd;
  int bar;
  struct scrollinfo si;
};
static struct wscroll* scrolls;
static struct scrollinfo* scroll_of(HANDLE h, int bar) {
  struct wscroll* s;
  for (s = scrolls; s; s = s->next)
    if (s->hwnd == h && s->bar == bar) return &s->si;
  s = (struct wscroll*)wzalloc(sizeof(*s));
  if (!s) return 0;
  s->hwnd = h;
  s->bar = bar;
  s->si.max = 100;
  s->next = scrolls;
  scrolls = s;
  return &s->si;
}
static WINAPI int SetScrollInfo(HANDLE h, int bar, const struct scrollinfo* in, BOOL redraw) {
  struct scrollinfo* s = scroll_of(h, bar);
  (void)redraw;
  if (!s || !in) return 0;
  if (in->mask & 1) {
    s->min = in->min;
    s->max = in->max;
  }
  if (in->mask & 2) s->page = in->page;
  if (in->mask & 4) s->pos = in->pos;
  return s->pos;
}
static WINAPI BOOL GetScrollInfo(HANDLE h, int bar, struct scrollinfo* out) {
  struct scrollinfo* s = scroll_of(h, bar);
  if (!s || !out) return 0;
  if (out->mask & 1) {
    out->min = s->min;
    out->max = s->max;
  }
  if (out->mask & 2) out->page = s->page;
  if (out->mask & 4) out->pos = s->pos;
  if (out->mask & 0x10) out->track = s->pos;
  return 1;
}
static WINAPI int SetScrollPos(HANDLE h, int bar, int pos, BOOL redraw) {
  struct scrollinfo* s = scroll_of(h, bar);
  int old;
  (void)redraw;
  if (!s) return 0;
  old = s->pos;
  s->pos = pos;
  return old;
}
static WINAPI int GetScrollPos(HANDLE h, int bar) {
  struct scrollinfo* s = scroll_of(h, bar);
  return s ? s->pos : 0;
}
static WINAPI BOOL SetScrollRange(HANDLE h, int bar, int lo, int hi, BOOL redraw) {
  struct scrollinfo* s = scroll_of(h, bar);
  (void)redraw;
  if (!s) return 0;
  s->min = lo;
  s->max = hi;
  return 1;
}
static WINAPI BOOL GetScrollRange(HANDLE h, int bar, int* lo, int* hi) {
  struct scrollinfo* s = scroll_of(h, bar);
  if (!s) return 0;
  if (lo) *lo = s->min;
  if (hi) *hi = s->max;
  return 1;
}
static WINAPI BOOL ShowScrollBar(HANDLE h, int bar, BOOL show) {
  (void)h; (void)bar; (void)show;
  return 1;
}
static WINAPI BOOL EnableScrollBar(HANDLE h, DWORD bar, DWORD fl) {
  (void)h; (void)bar; (void)fl;
  return 1;
}
static WINAPI BOOL GetScrollBarInfo(HANDLE h, long obj, void* info) {
  (void)h; (void)obj; (void)info;
  return 0;
}
static WINAPI int ScrollWindowEx(HANDLE h, int dx, int dy, const void* a, const void* b, HANDLE rgn, void* upd,
                                 DWORD fl) {
  (void)dx; (void)dy; (void)a; (void)b; (void)rgn; (void)upd; (void)fl;
  { /* repinta tudo: mais simples que mover os pixels */
    typedef BOOL(WINAPI * inv_fn)(HANDLE, const void*, BOOL);
    inv_fn inv = (inv_fn)builtin_lookup("InvalidateRect");
    if (inv) inv(h, 0, 1);
  }
  return 2; /* SIMPLEREGION */
}
static WINAPI BOOL ScrollWindow(HANDLE h, int dx, int dy, const void* a, const void* b) {
  return ScrollWindowEx(h, dx, dy, a, b, 0, 0, 0) != 0;
}

/* ---------- janelas: o que faltava ---------- */
struct wplacement {
  DWORD len, flags, show;
  struct point min, max;
  struct rect normal;
};
static WINAPI BOOL GetWindowPlacement(HANDLE h, struct wplacement* p) {
  typedef BOOL(WINAPI * gwr)(HANDLE, struct rect*);
  gwr f = (gwr)builtin_lookup("GetWindowRect");
  struct wwin* w = win_of(h);
  if (!w || !p) return 0;
  memset(p, 0, sizeof(*p));
  p->len = sizeof(*p);
  p->show = w->visible ? 1 : 0;
  if (f) f(h, &p->normal);
  return 1;
}
static WINAPI BOOL SetWindowPlacement(HANDLE h, const struct wplacement* p) {
  typedef BOOL(WINAPI * swp)(HANDLE, HANDLE, int, int, int, int, DWORD);
  swp f = (swp)builtin_lookup("SetWindowPos");
  if (!win_of(h) || !p) return 0;
  if (f)
    f(h, 0, p->normal.left, p->normal.top, p->normal.right - p->normal.left, p->normal.bottom - p->normal.top,
      0x14); /* SWP_NOZORDER|SWP_NOACTIVATE */
  return 1;
}
/* WindowFromPoint: em user.c (precisa do hit_top) */
static WINAPI HANDLE ChildWindowFromPointEx(HANDLE h, struct point pt, DWORD fl) {
  (void)pt;
  (void)fl;
  return h;
}
static WINAPI HANDLE GetLastActivePopup(HANDLE h) { return h; }
static WINAPI BOOL FlashWindow(HANDLE h, BOOL inv) {
  (void)h;
  (void)inv;
  return 0;
}
static WINAPI BOOL FlashWindowEx(const void* fi) {
  (void)fi;
  return 0;
}
static WINAPI void keybd_event(uint8_t vk, uint8_t sc, DWORD fl, uintptr_t x) {
  (void)vk; (void)sc; (void)fl; (void)x;
}
static WINAPI DWORD SendInput(DWORD n, void* in, int sz) {
  (void)n; (void)in; (void)sz;
  return 0;
}
static WINAPI BOOL GetComboBoxInfo(HANDLE h, void* info) {
  (void)h;
  (void)info;
  return 0;
}
typedef BOOL(WINAPI* monenum_fn)(HANDLE, HANDLE, struct rect*, intptr_t);
static WINAPI BOOL EnumDisplayMonitors(HANDLE dc, const struct rect* clip, monenum_fn fn, intptr_t lp) {
  typedef int(WINAPI * gsm)(int);
  gsm m = (gsm)builtin_lookup("GetSystemMetrics");
  struct rect r;
  (void)dc;
  (void)clip;
  r.left = r.top = 0;
  r.right = m ? m(0) : 1024;
  r.bottom = m ? m(1) : 768;
  if (fn) fn((HANDLE)(uintptr_t)0x10030, 0, &r, lp);
  return 1;
}
static WINAPI BOOL MessageBeep(DWORD t) {
  (void)t;
  return 1;
}
static WINAPI BOOL Beep(DWORD f, DWORD ms) {
  (void)f;
  (void)ms;
  return 1;
}
static WINAPI BOOL IsCharAlphaW(wchar16 c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
}
static WINAPI BOOL IsCharAlphaNumericW(wchar16 c) { return IsCharAlphaW(c) || (c >= '0' && c <= '9'); }

/* icones: um handle simbolico por pedido */
static WINAPI HANDLE CreateIconIndirect(const void* ii) {
  (void)ii;
  return (HANDLE)(uintptr_t)0x10024;
}
static WINAPI HANDLE CreateIcon(HANDLE inst, int w, int h, uint8_t pl, uint8_t bpp, const void* a, const void* x) {
  (void)inst; (void)w; (void)h; (void)pl; (void)bpp; (void)a; (void)x;
  return (HANDLE)(uintptr_t)0x10024;
}
static WINAPI BOOL GetIconInfo(HANDLE ic, void* info) {
  (void)ic;
  if (info) memset(info, 0, 32);
  return 0;
}
static WINAPI long LoadIconMetric(HANDLE inst, const wchar16* name, int metric, HANDLE* out) {
  typedef HANDLE(WINAPI * li)(HANDLE, const wchar16*);
  li f = (li)builtin_lookup("LoadIconW");
  (void)metric;
  if (!out) return E_FAIL_;
  *out = f ? f(inst, name) : 0;
  return *out ? 0 : E_FAIL_;
}
static WINAPI long LoadIconWithScaleDown(HANDLE inst, const wchar16* name, int cx, int cy, HANDLE* out) {
  (void)cx;
  (void)cy;
  return LoadIconMetric(inst, name, 0, out);
}

/* ---------- comctl32: ImageList e PropertySheet ---------- */
#define IL_MAGIC 0x4C4D4957u
struct imagelist {
  DWORD magic;
  int cx, cy, n;
};
static struct imagelist* il_of(HANDLE h) {
  struct imagelist* l = (struct imagelist*)h;
  return (wptr_ok(l) && l->magic == IL_MAGIC) ? l : 0;
}
static WINAPI HANDLE ImageList_Create(int cx, int cy, DWORD fl, int init, int grow) {
  struct imagelist* l = (struct imagelist*)wzalloc(sizeof(*l));
  (void)fl; (void)init; (void)grow;
  if (!l) return 0;
  l->magic = IL_MAGIC;
  l->cx = cx;
  l->cy = cy;
  return (HANDLE)l;
}
static WINAPI BOOL ImageList_Destroy(HANDLE h) {
  struct imagelist* l = il_of(h);
  if (!l) return 0;
  l->magic = 0;
  k_free(l);
  return 1;
}
static WINAPI int ImageList_ReplaceIcon(HANDLE h, int i, HANDLE ic) {
  struct imagelist* l = il_of(h);
  (void)ic;
  if (!l) return -1;
  if (i < 0 || i >= l->n) return l->n++;
  return i;
}
static WINAPI int ImageList_Add(HANDLE h, HANDLE bmp, HANDLE mask) {
  struct imagelist* l = il_of(h);
  (void)bmp;
  (void)mask;
  return l ? l->n++ : -1;
}
static WINAPI int ImageList_AddMasked(HANDLE h, HANDLE bmp, DWORD key) {
  (void)key;
  return ImageList_Add(h, bmp, 0);
}
static WINAPI BOOL ImageList_Remove(HANDLE h, int i) {
  struct imagelist* l = il_of(h);
  if (!l) return 0;
  if (i < 0) l->n = 0;
  else if (i < l->n) l->n--;
  return 1;
}
static WINAPI int ImageList_GetImageCount(HANDLE h) {
  struct imagelist* l = il_of(h);
  return l ? l->n : 0;
}
static WINAPI BOOL ImageList_SetImageCount(HANDLE h, DWORD n) {
  struct imagelist* l = il_of(h);
  if (!l) return 0;
  l->n = (int)n;
  return 1;
}
static WINAPI BOOL ImageList_GetIconSize(HANDLE h, int* cx, int* cy) {
  struct imagelist* l = il_of(h);
  if (!l) return 0;
  if (cx) *cx = l->cx;
  if (cy) *cy = l->cy;
  return 1;
}
static WINAPI BOOL ImageList_SetIconSize(HANDLE h, int cx, int cy) {
  struct imagelist* l = il_of(h);
  if (!l) return 0;
  l->cx = cx;
  l->cy = cy;
  l->n = 0;
  return 1;
}
static WINAPI BOOL ImageList_Draw(HANDLE h, int i, HANDLE dc, int x, int y, DWORD st) {
  (void)i; (void)dc; (void)x; (void)y; (void)st;
  return il_of(h) != 0;
}
static WINAPI BOOL ImageList_DrawEx(HANDLE h, int i, HANDLE dc, int x, int y, int dx, int dy, DWORD bg, DWORD fg,
                                    DWORD st) {
  (void)dx; (void)dy; (void)bg; (void)fg;
  return ImageList_Draw(h, i, dc, x, y, st);
}
static WINAPI DWORD ImageList_SetBkColor(HANDLE h, DWORD c) {
  (void)h;
  (void)c;
  return 0xFFFFFFFFu;
}
static WINAPI HANDLE ImageList_GetIcon(HANDLE h, int i, DWORD fl) {
  (void)i;
  (void)fl;
  return il_of(h) ? (HANDLE)(uintptr_t)0x10024 : 0;
}
static WINAPI HANDLE ImageList_Duplicate(HANDLE h) {
  struct imagelist* l = il_of(h);
  struct imagelist* d;
  if (!l) return 0;
  d = (struct imagelist*)ImageList_Create(l->cx, l->cy, 0, 0, 0);
  if (d) d->n = l->n;
  return (HANDLE)d;
}
static WINAPI intptr_t PropertySheetW(const void* h) {
  (void)h;
  wsay("PropertySheet ainda nao suportado", "");
  return -1;
}
static WINAPI HANDLE CreateStatusWindowW(long style, const wchar16* text, HANDLE parent, DWORD id) {
  typedef HANDLE(WINAPI * cw)(DWORD, const wchar16*, const wchar16*, DWORD, int, int, int, int, HANDLE, HANDLE,
                              HANDLE, void*);
  static const wchar16 cls[] = {'m', 's', 'c', 't', 'l', 's', '_', 's', 't', 'a', 't', 'u', 's', 'b',
                                'a', 'r', '3', '2', 0};
  cw f = (cw)builtin_lookup("CreateWindowExW");
  return f ? f(0, cls, text, (DWORD)style, 0, 0, 0, 0, parent, (HANDLE)(uintptr_t)id, 0, 0) : 0;
}

/* ---------- comdlg32: ainda sem dialogos de arquivo/fonte ---------- */
static WINAPI BOOL GetOpenFileNameW(void* ofn) {
  (void)ofn;
  return 0; /* cancelado */
}
static WINAPI BOOL GetSaveFileNameW(void* ofn) {
  (void)ofn;
  return 0;
}
static WINAPI BOOL ChooseFontW(void* cf) {
  (void)cf;
  return 0;
}
static WINAPI BOOL ChooseColorW(void* cc) {
  (void)cc;
  return 0;
}
static WINAPI DWORD CommDlgExtendedError(void) { return 0; }

/* ---------- uxtheme: sem temas (desenho classico) ---------- */
static WINAPI HANDLE OpenThemeData(HANDLE h, const wchar16* cls) {
  (void)h;
  (void)cls;
  return 0;
}
static WINAPI long CloseThemeData(HANDLE t) {
  (void)t;
  return 0;
}
static WINAPI BOOL IsThemeActive(void) { return 0; }
static WINAPI BOOL IsAppThemed(void) { return 0; }
static WINAPI long SetWindowTheme(HANDLE h, const wchar16* a, const wchar16* b) {
  (void)h; (void)a; (void)b;
  return 0;
}
static WINAPI long GetThemePartSize(HANDLE t, HANDLE dc, int part, int st, const void* r, int ts, void* sz) {
  (void)t; (void)dc; (void)part; (void)st; (void)r; (void)ts; (void)sz;
  return E_FAIL_;
}
static WINAPI long DrawThemeBackground(HANDLE t, HANDLE dc, int part, int st, const void* r, const void* clip) {
  (void)t; (void)dc; (void)part; (void)st; (void)r; (void)clip;
  return E_FAIL_;
}
static WINAPI long EnableThemeDialogTexture(HANDLE h, DWORD fl) {
  (void)h;
  (void)fl;
  return 0;
}

/* ---------- shell32 ---------- */
static WINAPI BOOL Shell_NotifyIconW(DWORD msg, void* data) {
  (void)msg;
  (void)data;
  return 1;
}
static WINAPI void SHAddToRecentDocs(DWORD fl, const void* pv) {
  (void)fl;
  (void)pv;
}
static WINAPI long SHGetDesktopFolder(void** out) {
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI long SHGetFolderLocation(HANDLE w, int csidl, HANDLE tok, DWORD r, void** out) {
  typedef long(WINAPI * sfl)(HANDLE, int, void**);
  sfl f = (sfl)builtin_lookup("SHGetSpecialFolderLocation");
  (void)tok;
  (void)r;
  return f ? f(w, csidl, out) : E_FAIL_;
}
static WINAPI long SHGetStockIconInfo(int id, DWORD fl, void* info) {
  (void)id; (void)fl; (void)info;
  return E_NOTIMPL_;
}
static WINAPI DWORD SHRestricted(DWORD what) {
  (void)what;
  return 0;
}
static WINAPI long AssocQueryStringW(DWORD fl, DWORD str, const wchar16* a, const wchar16* x, wchar16* out,
                                     DWORD* n) {
  (void)fl; (void)str; (void)a; (void)x; (void)out; (void)n;
  return (long)0x80070483; /* HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) */
}
/* comparacao "natural": numeros pelo valor, o resto sem caixa */
static WINAPI int StrCmpLogicalW(const wchar16* a, const wchar16* b) {
  while (*a && *b) {
    if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
      uint64_t x = 0, y = 0;
      while (*a >= '0' && *a <= '9') x = x * 10 + (uint64_t)(*a++ - '0');
      while (*b >= '0' && *b <= '9') y = y * 10 + (uint64_t)(*b++ - '0');
      if (x != y) return x < y ? -1 : 1;
    } else {
      wchar16 x = *a++, y = *b++;
      if (x >= 'a' && x <= 'z') x -= 32;
      if (y >= 'a' && y <= 'z') y -= 32;
      if (x != y) return x < y ? -1 : 1;
    }
  }
  return *a ? 1 : *b ? -1 : 0;
}
/* QITAB: { const IID* iid; DWORD offset; } terminada em iid 0 */
struct qitab {
  const uint8_t* iid;
  DWORD off;
};
static WINAPI long QISearch(void* self, const struct qitab* tab, const uint8_t* riid, void** out) {
  static const uint8_t unk[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
  int i;
  if (!out) return E_FAIL_;
  *out = 0;
  for (i = 0; tab && tab[i].iid; i++)
    if (memcmp(tab[i].iid, riid, 16) == 0 || (memcmp(riid, unk, 16) == 0 && i == 0)) {
      void** obj = (void**)((uint8_t*)self + tab[i].off);
      typedef DWORD(WINAPI * addref_fn)(void*);
      ((addref_fn)((void**)*obj)[1])(obj);
      *out = obj;
      return 0;
    }
  return E_NOINTERFACE_;
}

/* ---------- gdi32: o que faltava ---------- */
static WINAPI int GetMapMode(HANDLE dc) {
  (void)dc;
  return 1; /* MM_TEXT */
}
static WINAPI int SetMapMode(HANDLE dc, int m) {
  (void)dc;
  (void)m;
  return 1;
}
static WINAPI BOOL DPtoLP(HANDLE dc, void* pts, int n) {
  (void)dc; (void)pts; (void)n;
  return 1; /* MM_TEXT: iguais */
}
static WINAPI BOOL LPtoDP(HANDLE dc, void* pts, int n) {
  (void)dc; (void)pts; (void)n;
  return 1;
}
static WINAPI int ExcludeClipRect(HANDLE dc, int l, int t, int r, int b) {
  (void)dc; (void)l; (void)t; (void)r; (void)b;
  return 2; /* SIMPLEREGION (o recorte fica como estava) */
}
static WINAPI int IntersectClipRect(HANDLE dc, int l, int t, int r, int b) {
  (void)dc; (void)l; (void)t; (void)r; (void)b;
  return 2;
}
static WINAPI DWORD GetPixel(HANDLE dc, int x, int y) {
  (void)dc; (void)x; (void)y;
  return 0xFFFFFFFFu; /* CLR_INVALID */
}
static WINAPI BOOL GradientFill(HANDLE dc, void* v, DWORD nv, void* m, DWORD nm, DWORD mode) {
  (void)dc; (void)v; (void)nv; (void)m; (void)nm; (void)mode;
  return 1;
}

/* ---------- gdiplus: GdiplusStartup ja falha; isto so evita o stub ---------- */
#define GP_FAIL 1 /* GenericError */
#define GP0(n) \
  static WINAPI int n(void* a, void* b, void* c, void* d, void* e, void* f) { \
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return GP_FAIL; }
/* as que devolvem um objeto: zera a saida (ultimo ponteiro) */
#define GPOUT(n, k) \
  static WINAPI int n(void* a, void* b, void* c, void* d, void* e, void* f) { \
    void* v[6] = {a, b, c, d, e, f}; if (v[k]) *(void**)v[k] = 0; return GP_FAIL; }
GP0(GdipAddPathArc) GP0(GdipAddPathLine) GP0(GdipBitmapGetPixel) GP0(GdipBitmapSetPixel)
GPOUT(GdipCloneBrush, 1) GP0(GdipClosePathFigure) GPOUT(GdipCreateBitmapFromFile, 1)
GPOUT(GdipCreateBitmapFromHBITMAP, 2) GPOUT(GdipCreateFromHDC, 1) GPOUT(GdipCreatePath, 1)
GPOUT(GdipCreatePen1, 3) GPOUT(GdipCreateSolidFill, 1) GP0(GdipDeleteBrush) GP0(GdipDeleteGraphics)
GP0(GdipDeletePath) GP0(GdipDeletePen) GP0(GdipDrawEllipse) GP0(GdipDrawLine) GP0(GdipDrawLineI)
GP0(GdipDrawPath) GP0(GdipDrawRectangle) GP0(GdipFillEllipse) GP0(GdipFillPath) GP0(GdipFillPolygon)
GP0(GdipFillRectangle) GP0(GdipSetPenEndCap) GP0(GdipSetPenLineJoin) GP0(GdipSetPenStartCap)
GP0(GdipSetSmoothingMode)

/* ---------- kernel32/ole32: o que o WinRAR.exe ainda pedia ---------- */
static WINAPI int MulDiv(int a, int b, int c) {
  int64_t r;
  if (!c) return -1;
  r = (int64_t)a * b;
  r = (r >= 0) == (c > 0) ? (r + (c > 0 ? c : -c) / 2) / c : (r - (c > 0 ? c : -c) / 2) / c;
  if (r > 0x7FFFFFFF || r < -0x7FFFFFFF - 1) return -1;
  return (int)r;
}
static WINAPI size_t GlobalSize(void* p) { return wheap_size(p); }
static WINAPI DWORD GetCompressedFileSizeW(const wchar16* name, DWORD* hi) {
  char* n = wdup16to8(name);
  char path[300];
  long s = -1;
  if (n) {
    wpath(n, path, (int)sizeof(path));
    s = k_fsize(path);
    k_free(n);
  }
  if (hi) *hi = 0;
  if (s < 0) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return 0xFFFFFFFFu;
  }
  return (DWORD)s;
}
static WINAPI BOOL GetPhysicallyInstalledSystemMemory(uint64_t* kb) {
  if (!kb) return 0;
  *kb = 4ull * 1024 * 1024; /* 4 GB, como o GlobalMemoryStatusEx informa */
  return 1;
}
struct cpinfoex {
  DWORD max_char;
  uint8_t def_char[2], lead[12];
  wchar16 uni_def;
  DWORD cp;
  wchar16 name[260];
};
static WINAPI BOOL GetCPInfoExW(DWORD cp, DWORD fl, struct cpinfoex* i) {
  static const char nm[] = "65001 (UTF-8)";
  int k;
  (void)fl;
  if (!i) return 0;
  memset(i, 0, sizeof(*i));
  i->max_char = (cp == 65001 || cp == 0 || cp == 3) ? 4 : 1;
  i->def_char[0] = '?';
  i->uni_def = '?';
  i->cp = cp ? cp : 65001;
  for (k = 0; nm[k]; k++) i->name[k] = (wchar16)nm[k];
  return 1;
}
static WINAPI DWORD GetActiveProcessorGroupCount(void) { return 1; }
static WINAPI DWORD GetActiveProcessorCount(uint16_t group) {
  typedef void(WINAPI * gsi)(void*);
  uint8_t si[64];
  gsi f = (gsi)builtin_lookup("GetSystemInfo");
  (void)group;
  if (!f) return 1;
  f(si);
  return *(DWORD*)(si + 32); /* dwNumberOfProcessors */
}
static WINAPI DWORD GetMaximumProcessorGroupCount(void) { return 1; }
static WINAPI long AppPolicyGetProcessTerminationMethod(HANDLE tok, int* m) {
  (void)tok;
  if (m) *m = 0; /* ExitProcess */
  return 0;
}
static WINAPI HANDLE OpenSemaphoreW(DWORD acc, BOOL inh, const wchar16* name) {
  (void)acc; (void)inh; (void)name;
  wset_lasterr(ERROR_FILE_NOT_FOUND);
  return 0;
}
static WINAPI HANDLE CreateNamedPipeW(const wchar16* n, DWORD om, DWORD pm, DWORD mx, DWORD ob, DWORD ib, DWORD to,
                                      void* sa) {
  (void)n; (void)om; (void)pm; (void)mx; (void)ob; (void)ib; (void)to; (void)sa;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return (HANDLE)(intptr_t)-1;
}
static WINAPI BOOL ConnectNamedPipe(HANDLE h, void* ov) {
  (void)h;
  (void)ov;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI BOOL DisconnectNamedPipe(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI BOOL WaitNamedPipeW(const wchar16* n, DWORD ms) {
  (void)n;
  (void)ms;
  wset_lasterr(ERROR_FILE_NOT_FOUND);
  return 0;
}
static WINAPI BOOL PeekNamedPipe(HANDLE h, void* b, DWORD n, DWORD* rd, DWORD* av, DWORD* left) {
  (void)h; (void)b; (void)n;
  if (rd) *rd = 0;
  if (av) *av = 0;
  if (left) *left = 0;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI HANDLE FindFirstChangeNotificationW(const wchar16* p, BOOL sub, DWORD fl) {
  (void)p; (void)sub; (void)fl;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return (HANDLE)(intptr_t)-1;
}
static WINAPI BOOL FindNextChangeNotification(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI BOOL FindCloseChangeNotification(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI HANDLE BeginUpdateResourceW(const wchar16* f, BOOL del) {
  (void)f;
  (void)del;
  wset_lasterr(ERROR_NOT_SUPPORTED);
  return 0;
}
static WINAPI BOOL UpdateResourceW(HANDLE h, const void* t, const void* n, uint16_t l, void* d, DWORD sz) {
  (void)h; (void)t; (void)n; (void)l; (void)d; (void)sz;
  return 0;
}
static WINAPI BOOL EndUpdateResourceW(HANDLE h, BOOL discard) {
  (void)h;
  (void)discard;
  return 0;
}
static WINAPI BOOL EnumResourceNamesW(HANDLE m, const void* t, void* fn, intptr_t lp) {
  (void)m; (void)t; (void)fn; (void)lp;
  wset_lasterr(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
  return 0;
}
static WINAPI BOOL EnumResourceLanguagesW(HANDLE m, const void* t, const void* n, void* fn, intptr_t lp) {
  typedef BOOL(WINAPI * cb)(HANDLE, const void*, const void*, uint16_t, intptr_t);
  struct wmod* mod = pe_from_handle(m);
  uint32_t sz = 0;
  if (!mod || !pe_find_resource(mod, (uintptr_t)t, (uintptr_t)n, &sz)) {
    wset_lasterr(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
    return 0;
  }
  if (fn) ((cb)fn)(m, t, n, 0x409, lp);
  return 1;
}
static WINAPI void DebugBreak(void) { wsay("DebugBreak ignorado", ""); }
static WINAPI void FreeLibraryAndExitThread(HANDLE m, DWORD code) {
  typedef void(WINAPI * et_fn)(DWORD);
  et_fn et = (et_fn)builtin_lookup("ExitThread");
  (void)m;
  if (et) et(code);
}
static WINAPI long CoSetProxyBlanket(void* p, DWORD a, DWORD b, void* c, DWORD d, DWORD e, void* f, DWORD g) {
  (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
  return 0;
}


/* ---------- o que o Claude.exe (Chromium) pediu depois do W4 ---------- */
static WINAPI DWORD FormatMessageA(DWORD fl, const void* src, DWORD id, DWORD lang, char* buf, DWORD n, void* args) {
  char txt[64];
  int i = 0, k = 0;
  char num[12];
  DWORD v = id, len;
  const char* pre = "Erro do Windows ";
  (void)src; (void)lang; (void)args;
  wlogx("FormatMessageA do erro ", id);
  for (; pre[k]; k++) txt[i++] = pre[k];
  k = 0;
  do {
    num[k++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (k) txt[i++] = num[--k];
  txt[i++] = '.';
  txt[i] = 0;
  len = (DWORD)i;
  if (fl & 0x100) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER (LocalFree solta) */
    typedef void*(WINAPI * la_fn)(DWORD, size_t);
    la_fn la = (la_fn)builtin_lookup("LocalAlloc");
    char* out = la ? (char*)la(0, len + 1) : 0;
    if (!out) return 0;
    memcpy(out, txt, len + 1);
    *(char**)buf = out;
    return len;
  }
  if (!buf || len + 1 > n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(buf, txt, len + 1);
  return len;
}
static int wname_is(const wchar16* a, const char* b) {
  if (!a) return 0;
  for (; *a && *b; a++, b++) {
    wchar16 x = *a, y = (wchar16)*b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return 0;
  }
  return !*a && !*b;
}
static WINAPI DWORD LocaleNameToLCID(const wchar16* name, DWORD fl) {
  (void)fl;
  if (!name || !*name || wname_is(name, "en-US") || wname_is(name, "en")) return 0x409;
  if (wname_is(name, "pt-BR")) return 0x416;
  return 0x409;
}
static WINAPI int LCIDToLocaleName(DWORD lcid, wchar16* out, int n, DWORD fl) {
  static const wchar16 en[] = {'e', 'n', '-', 'U', 'S', 0};
  static const wchar16 pt[] = {'p', 't', '-', 'B', 'R', 0};
  const wchar16* s = lcid == 0x416 ? pt : en;
  (void)fl;
  if (!out || n <= 0) return 6;
  if (n < 6) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(out, s, 12);
  return 6;
}
static WINAPI BOOL IsValidLocaleName(const wchar16* name) {
  return wname_is(name, "en-US") || wname_is(name, "en") || wname_is(name, "pt-BR") || (name && !*name);
}
typedef BOOL(WINAPI* locenum_fn)(wchar16*, DWORD, intptr_t);
static WINAPI BOOL EnumSystemLocalesEx(locenum_fn fn, DWORD fl, intptr_t lp, void* res) {
  wchar16 en[] = {'e', 'n', '-', 'U', 'S', 0};
  (void)fl;
  (void)res;
  if (fn) fn(en, 2 /* LOCALE_SPECIFICDATA */, lp);
  return 1;
}
/* uma thread por item (o pool do Windows reaproveita threads; o efeito e o mesmo) */
static WINAPI BOOL QueueUserWorkItem(void* fn, void* ctx, DWORD fl) {
  typedef HANDLE(WINAPI * ct_fn)(void*, size_t, void*, void*, DWORD, DWORD*);
  typedef BOOL(WINAPI * ch_fn)(HANDLE);
  ct_fn ct = (ct_fn)builtin_lookup("CreateThread");
  ch_fn ch = (ch_fn)builtin_lookup("CloseHandle");
  HANDLE h;
  (void)fl;
  if (!ct || !fn) return 0;
  h = ct(0, 0, fn, ctx, 0, 0);
  if (!h) return 0;
  if (ch) ch(h);
  return 1;
}
static WINAPI long NtQueryInformationFile(HANDLE h, void* iosb, void* buf, DWORD len, int cls) {
  (void)h; (void)iosb; (void)buf; (void)len;
  wlogx("NtQueryInformationFile: classe ainda nao suportada ", (uint32_t)cls);
  return (long)0xC0000003; /* STATUS_INVALID_INFO_CLASS */
}
static WINAPI long NtDeviceIoControlFile(HANDLE h, HANDLE ev, void* apc, void* ctx, void* iosb, DWORD code,
                                         void* in, DWORD nin, void* out, DWORD nout) {
  (void)h; (void)ev; (void)apc; (void)ctx; (void)iosb; (void)in; (void)nin; (void)out; (void)nout;
  { static int n_; if (wine_debug >= 2 && n_ < 12) { n_++; wlogx("NtDeviceIoControlFile (nao suportado), codigo ", code); wlogx("  handle ", (uint64_t)(uintptr_t)h); } }
  return (long)0xC00000BB; /* STATUS_NOT_SUPPORTED */
}

static WINAPI long LoadRegTypeLib(const void* guid, uint16_t maj, uint16_t min, DWORD lcid, void** out);
static WINAPI long LoadTypeLib(const wchar16* file, void** out);
static WINAPI long LoadTypeLibEx(const wchar16* file, int kind, void** out);
static WINAPI long RegisterTypeLib(void* tl, const wchar16* path, const wchar16* help);
#define S(n) {#n, (void*)n}
const struct wine_sym uimisc_syms[] = {
    S(LoadRegTypeLib), S(LoadTypeLib), S(LoadTypeLibEx), S(RegisterTypeLib),
    S(FormatMessageA), S(LocaleNameToLCID), S(LCIDToLocaleName), S(IsValidLocaleName),
    S(EnumSystemLocalesEx), S(NtDeviceIoControlFile), S(NtQueryInformationFile), S(QueueUserWorkItem),
    S(RegisterWindowMessageW), S(RegisterWindowMessageA), S(RegisterClipboardFormatW), S(SetPropW), S(GetPropW),
    S(RemovePropW), S(SetWindowSubclass), S(RemoveWindowSubclass), S(GetWindowSubclass), S(DefSubclassProc),
    S(CreateMenu), S(CreatePopupMenu), S(DestroyMenu), S(AppendMenuW), S(InsertMenuW), S(InsertMenuItemW),
    S(SetMenuItemInfoW), S(GetMenuItemInfoW), S(DeleteMenu), S(RemoveMenu), S(CheckMenuItem), S(EnableMenuItem),
    S(CheckMenuRadioItem), S(GetMenuItemCount), S(GetMenuItemID), S(GetSubMenu), S(GetMenuState),
    S(GetMenuStringW), S(SetMenuInfo), S(GetMenuInfo), S(SetMenuDefaultItem), S(ModifyMenuW), S(LoadMenuW),
    S(LoadMenuIndirectW), S(GetMenu), S(SetMenu), S(DrawMenuBar), S(GetSystemMenu), S(TrackPopupMenu),
    S(TrackPopupMenuEx), S(LoadAcceleratorsW), S(TranslateAcceleratorW), S(DestroyAcceleratorTable),
    S(SetWindowsHookExW), S(UnhookWindowsHookEx), S(CallNextHookEx), S(SetWinEventHook), S(UnhookWinEvent),
    S(OpenClipboard), S(CloseClipboard), S(EmptyClipboard), S(SetClipboardData), S(GetClipboardData),
    S(IsClipboardFormatAvailable), S(EnumClipboardFormats), S(GetClipboardSequenceNumber), S(OleSetClipboard),
    S(OleGetClipboard), S(OleFlushClipboard), S(DragAcceptFiles), S(DragQueryFileW), S(DragFinish),
    S(RegisterDragDrop), S(RevokeDragDrop), S(DoDragDrop), S(SetScrollInfo), S(GetScrollInfo), S(SetScrollPos),
    S(GetScrollPos), S(SetScrollRange), S(GetScrollRange), S(ShowScrollBar), S(EnableScrollBar),
    S(GetScrollBarInfo), S(ScrollWindowEx), S(ScrollWindow), S(GetWindowPlacement), S(SetWindowPlacement),
    S(ChildWindowFromPointEx), S(GetLastActivePopup), S(FlashWindow), S(FlashWindowEx),
    S(keybd_event), S(SendInput), S(GetComboBoxInfo), S(EnumDisplayMonitors), S(MessageBeep), S(Beep),
    S(IsCharAlphaW), S(IsCharAlphaNumericW), S(CreateIconIndirect), S(CreateIcon), S(GetIconInfo),
    S(LoadIconMetric), S(LoadIconWithScaleDown), S(ImageList_Create), S(ImageList_Destroy),
    S(ImageList_ReplaceIcon), S(ImageList_Add), S(ImageList_AddMasked), S(ImageList_Remove),
    S(ImageList_GetImageCount), S(ImageList_SetImageCount), S(ImageList_GetIconSize), S(ImageList_SetIconSize),
    S(ImageList_Draw), S(ImageList_DrawEx), S(ImageList_SetBkColor), S(ImageList_GetIcon), S(ImageList_Duplicate),
    S(PropertySheetW), S(CreateStatusWindowW), S(GetOpenFileNameW), S(GetSaveFileNameW), S(ChooseFontW),
    S(ChooseColorW), S(CommDlgExtendedError), S(OpenThemeData), S(CloseThemeData), S(IsThemeActive),
    S(IsAppThemed), S(SetWindowTheme), S(GetThemePartSize), S(DrawThemeBackground), S(EnableThemeDialogTexture),
    S(Shell_NotifyIconW), S(SHAddToRecentDocs), S(SHGetDesktopFolder), S(SHGetFolderLocation),
    S(SHGetStockIconInfo), S(SHRestricted), S(AssocQueryStringW), S(StrCmpLogicalW), S(QISearch), S(GetMapMode),
    S(SetMapMode), S(DPtoLP), S(LPtoDP), S(ExcludeClipRect), S(IntersectClipRect), S(GetPixel), S(GradientFill),
    S(GdipAddPathArc), S(GdipAddPathLine), S(GdipBitmapGetPixel), S(GdipBitmapSetPixel), S(GdipCloneBrush),
    S(GdipClosePathFigure), S(GdipCreateBitmapFromFile), S(GdipCreateBitmapFromHBITMAP), S(GdipCreateFromHDC),
    S(GdipCreatePath), S(GdipCreatePen1), S(GdipCreateSolidFill), S(GdipDeleteBrush), S(GdipDeleteGraphics),
    S(GdipDeletePath), S(GdipDeletePen), S(GdipDrawEllipse), S(GdipDrawLine), S(GdipDrawLineI), S(GdipDrawPath),
    S(GdipDrawRectangle), S(GdipFillEllipse), S(GdipFillPath), S(GdipFillPolygon), S(GdipFillRectangle),
    S(GdipSetPenEndCap), S(GdipSetPenLineJoin), S(GdipSetPenStartCap), S(GdipSetSmoothingMode), S(MulDiv),
    S(GlobalSize), S(GetCompressedFileSizeW), S(GetPhysicallyInstalledSystemMemory), S(GetCPInfoExW),
    S(GetActiveProcessorGroupCount), S(GetActiveProcessorCount), S(GetMaximumProcessorGroupCount),
    S(AppPolicyGetProcessTerminationMethod), S(OpenSemaphoreW), S(CreateNamedPipeW), S(ConnectNamedPipe),
    S(DisconnectNamedPipe), S(WaitNamedPipeW), S(PeekNamedPipe), S(FindFirstChangeNotificationW),
    S(FindNextChangeNotification), S(FindCloseChangeNotification), S(BeginUpdateResourceW), S(UpdateResourceW),
    S(EndUpdateResourceW), S(EnumResourceNamesW), S(EnumResourceLanguagesW), S(DebugBreak),
    S(FreeLibraryAndExitThread), S(CoSetProxyBlanket), {0, 0}};


/* ---------- uxtheme por ordinal: modo escuro (sem nome publico) ----------
 * 104 RefreshImmersiveColorPolicyState, 132 ShouldAppsUseDarkMode,
 * 133 AllowDarkModeForWindow, 135 SetPreferredAppMode, 136 FlushMenuThemes,
 * 137 IsDarkModeAllowedForWindow, 138 ShouldSystemUseDarkMode,
 * 139 IsDarkModeAllowedForApp. O SODOS desenha claro: tudo "nao". */
static WINAPI void ux_void(void) {}
static WINAPI BOOL ux_false(void) { return 0; }
static WINAPI BOOL ux_allow_dark(HANDLE w, BOOL on) {
  (void)w;
  (void)on;
  return 0;
}
static WINAPI int ux_preferred_mode(int mode) {
  (void)mode;
  return 0; /* modo anterior: Default */
}

/* ---------- oleaut32: bibliotecas de tipos nao registradas ---------- */
#define TYPE_E_LIBNOTREGISTERED ((long)0x8002801D)
static WINAPI long LoadRegTypeLib(const void* guid, uint16_t maj, uint16_t min, DWORD lcid, void** out) {
  (void)guid; (void)maj; (void)min; (void)lcid;
  if (out) *out = 0;
  return TYPE_E_LIBNOTREGISTERED;
}
static WINAPI long LoadTypeLib(const wchar16* file, void** out) {
  (void)file;
  if (out) *out = 0;
  return TYPE_E_LIBNOTREGISTERED;
}
static WINAPI long LoadTypeLibEx(const wchar16* file, int kind, void** out) {
  (void)kind;
  return LoadTypeLib(file, out);
}
static WINAPI long RegisterTypeLib(void* tl, const wchar16* path, const wchar16* help) {
  (void)tl; (void)path; (void)help;
  return 0;
}

/* ordinais: comctl32 e shell32 */
void* uimisc_ordinal(const char* dll, unsigned ord) {
  if (strncmp(dll, "comctl32", 8) == 0) switch (ord) {
      case 380: return (void*)LoadIconMetric;
      case 381: return (void*)LoadIconWithScaleDown;
      case 410: return (void*)SetWindowSubclass;
      case 411: return (void*)GetWindowSubclass;
      case 412: return (void*)RemoveWindowSubclass;
      case 413: return (void*)DefSubclassProc;
      case 17: return 0; /* InitCommonControls: ja por nome */
    }
  if (strncmp(dll, "shell32", 7) == 0 && ord == 100) return (void*)SHRestricted;
  if (strncmp(dll, "uxtheme", 7) == 0) switch (ord) {
      case 104: case 136: return (void*)ux_void;
      case 132: case 137: case 138: case 139: return (void*)ux_false;
      case 133: return (void*)ux_allow_dark;
      case 135: return (void*)ux_preferred_mode;
    }
  if (strncmp(dll, "oleaut32", 8) == 0) switch (ord) {
      case 161: return (void*)LoadTypeLib;
      case 162: return (void*)LoadRegTypeLib;
      case 163: return (void*)RegisterTypeLib;
      case 183: return (void*)LoadTypeLibEx;
    }
  if (strncmp(dll, "shlwapi", 7) == 0 && ord == 219) return (void*)QISearch;
  return 0;
}
