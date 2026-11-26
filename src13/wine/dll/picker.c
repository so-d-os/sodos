/* picker.c - dialogos de escolher pasta/arquivo do Windows pelo Explorer do
 * SODOS.
 *
 * O Claude Desktop (Electron) abre "Choose folder" com IFileOpenDialog
 * (CoCreateInstance de CLSID_FileOpenDialog, FOS_PICKFOLDERS) numa thread;
 * programas antigos usam SHBrowseForFolder. Antes os dois devolviam
 * "cancelado" (ou "classe nao registrada") e o botao nao fazia nada.
 *
 * Agora: a thread deixa o pedido aqui, a tarefa principal do processo (laco
 * do main.c, wpick_world_service: so ela pode abrir programa no WIN) abre
 *   D:\EXPLORER.EXE "/PICK|Z:\PICKn.TXT|pasta inicial|titulo"
 * e a thread espera o arquivo de resposta: "OK <pasta>\n" ou "CANCEL\n".
 *
 * Objetos COM (comobj generico de winrt.c, a vtable diz o metodo):
 *   FileOpenDialog  IUnknown 0-2, IModalWindow 3 Show, IFileDialog 4-26,
 *                   IFileOpenDialog 27 GetResults, 28 GetSelectedItems
 *   ShellItem       3 BindToHandler, 4 GetParent, 5 GetDisplayName,
 *                   6 GetAttributes, 7 Compare
 *   ShellItemArray  7 GetCount, 8 GetItemAt
 */
#include <so.h>
#include <string.h>
#include <stdio.h>
#include "wine_int.h"
#include "winrt.h"

int k_win_launch_arg(const char* exe, const char* arg);

#define ERROR_CANCELLED_HR ((long)0x800704C7)
#define FOS_PICKFOLDERS 0x20u

/* ---------- pedido para a tarefa principal ---------- */
static volatile int pk_lock;
static struct wproc* pk_owner;   /* processo que tem o pedido (0 = livre) */
static char pk_arg[700];
static volatile int pk_state;    /* 0 livre, 1 pedido, 2 aberto, 3 falhou */
static volatile int pk_seq;

static void pk_take(void) {
  wglock(&pk_lock);
}
static void pk_give(void) { wgunlock(&pk_lock); }

/* Na tarefa principal (main.c). */
void wpick_world_service(void) {
  char a[700];
  if (pk_owner != wp() || pk_state != 1) return;
  pk_take();
  strcpy(a, pk_arg);
  pk_give();
  if (k_win_launch_arg("D:\\EXPLORER.EXE", a) == 0) {
    pk_state = 2;
    wlog("Escolher pasta: Explorer aberto ", a);
  } else {
    /* o WIN tem um pedido na frente: tenta de novo na proxima volta */
    static int tries;
    if (++tries > 150) {
      tries = 0;
      pk_state = 3;
      wlog("Escolher pasta: o WIN nao abriu o Explorer", 0);
    }
  }
  k_futex_wake(&pk_state, -1);
}

static void san(char* d, const char* s, int max) {
  int i = 0;
  for (; s && *s && i < max - 1; s++) d[i++] = *s == '|' ? ' ' : *s;
  d[i] = 0;
}

/* Abre o Explorer no modo selecionar pasta e espera. 1 = escolheu (out). */
int wpick_folder(const char* title, const char* init, char* out, int max) {
  struct wproc* p = wp();
  char res[64], t[80], in[300];
  int seq, n, got = 0;
  uint64_t t0;
  out[0] = 0;
  /* um dialogo por vez no sistema todo (o WIN so guarda um pedido) */
  for (;;) {
    pk_take();
    if (!pk_owner) break;
    pk_give();
    k_futex_wait(&pk_state, pk_state, 100);
  }
  seq = __sync_add_and_fetch(&pk_seq, 1);
  snprintf(res, sizeof(res), "Z:\\PICK%u.TXT", (unsigned)(seq + (int)(k_ms() % 100000u) * 7));
  k_fdelete(res);
  san(t, title && title[0] ? title : "Selecionar pasta", (int)sizeof(t));
  san(in, init ? init : "", (int)sizeof(in));
  snprintf(pk_arg, sizeof(pk_arg), "/PICK|%s|%s|%s", res, in, t);
  pk_owner = p;
  pk_state = 1;
  pk_give();
  /* a tarefa principal abre; depois o usuario escolhe (sem limite curto:
   * e um dialogo; 30 min para nao prender a thread para sempre se o
   * Explorer morrer) */
  t0 = wnow_ms();
  while (pk_state == 1 && wnow_ms() - t0 < 10000) k_futex_wait(&pk_state, 1, 50);
  if (pk_state == 2) {
    for (;;) {
      n = k_fsize(res);
      if (n > 0) {
        char b[400];
        if (n >= (int)sizeof(b)) n = (int)sizeof(b) - 1;
        if (k_fread(res, 0, b, n) == n) {
          b[n] = 0;
          if (n > 0 && b[n - 1] == '\n') {
            b[n - 1] = 0;
            if (b[0] == 'O' && b[1] == 'K' && b[2] == ' ') {
              strncpy(out, b + 3, (size_t)max - 1);
              out[max - 1] = 0;
              got = out[0] != 0;
            }
            break;
          }
        }
      }
      if (wnow_ms() - t0 > 30ull * 60 * 1000 || p->exit_req) break;
      k_futex_wait(&p->exit_req, 0, 150);
    }
  }
  k_fdelete(res);
  pk_take();
  pk_owner = 0;
  pk_state = 0;
  pk_give();
  k_futex_wake(&pk_state, -1);
  wlog(got ? "Escolher pasta: escolhida " : "Escolher pasta: cancelado", got ? out : 0);
  return got;
}

/* ---------- IShellItem / IShellItemArray ---------- */
static long item_handler(struct comobj* self, int slot, uintptr_t* a);
static struct comobj* plain_new(const char* name, com_handler h, void* data) {
  struct comobj* o = com_new(name, h, data);
  if (o) o->plain = 1;
  return o;
}
static struct comobj* item_new8(const char* path) {
  wchar16* w = wdup8to16(path ? path : "");
  return plain_new("ShellItem", item_handler, w);
}

static long item_handler(struct comobj* self, int slot, uintptr_t* a) {
  const wchar16* path = (const wchar16*)self->data;
  switch (slot) {
    case 5: { /* GetDisplayName(sigdn, LPWSTR*): o caminho em todos os modos */
      wchar16** out = (wchar16**)a[1];
      size_t n = path ? wstrlen16(path) : 0;
      wchar16* d;
      uint32_t sigdn = (uint32_t)a[0];
      const wchar16* src = path;
      if (!out) return E_POINTER;
      if (sigdn == 0 || sigdn == 0x8000100Cu) { /* NORMALDISPLAY, PARENTRELATIVE...: o nome */
        size_t i;
        for (i = n; i > 0; i--)
          if (path[i - 1] == '\\' && i < n) {
            src = path + i;
            break;
          }
        n = wstrlen16(src);
      }
      d = (wchar16*)wheap_alloc((n + 1) * 2);
      if (!d) return E_OUTOFMEMORY;
      memcpy(d, src, n * 2);
      d[n] = 0;
      *out = d;
      return S_OK;
    }
    case 6: { /* GetAttributes(mask, *attrs): pasta do sistema de arquivos */
      uint32_t at = 0x40000000u | 0x20000000u | 0x00400000u; /* FILESYSTEM|FOLDER|... */
      if (a[1]) *(uint32_t*)a[1] = at & (uint32_t)a[0];
      return S_OK;
    }
    case 4: { /* GetParent */
      char* s = wdup16to8(path);
      int l;
      if (!s) return E_OUTOFMEMORY;
      l = (int)strlen(s);
      while (l > 3 && s[l - 1] != '\\') l--;
      if (l > 3) l--;
      s[l] = 0;
      *(void**)a[0] = item_new8(s);
      k_free(s);
      return S_OK;
    }
    case 7: { /* Compare(other, hint, *order) */
      struct comobj* o = (struct comobj*)a[0];
      const wchar16* q = o ? (const wchar16*)o->data : 0;
      int same = q && path && wstrlen16(q) == wstrlen16(path) && !memcmp(q, path, wstrlen16(q) * 2);
      if (a[2]) *(int*)a[2] = same ? 0 : 1;
      return same ? S_OK : 1;
    }
    case 3: /* BindToHandler: sem pastas virtuais */
      if (a[3]) *(void**)a[3] = 0;
      return (long)0x80004002;
  }
  return E_NOTIMPL;
}

static long array_handler(struct comobj* self, int slot, uintptr_t* a) {
  struct comobj* it = (struct comobj*)self->data;
  switch (slot) {
    case 7: /* GetCount */
      *(DWORD*)a[0] = it ? 1 : 0;
      return S_OK;
    case 8: /* GetItemAt(i, IShellItem**) */
      if (a[0] != 0 || !it) return E_FAIL;
      __sync_fetch_and_add(&it->refs, 1);
      *(void**)a[1] = it;
      return S_OK;
    case 6: /* GetAttributes(flags, mask, *attrs) */
      if (a[2]) *(uint32_t*)a[2] = (uint32_t)a[1] & 0x60400000u;
      return S_OK;
  }
  return E_NOTIMPL;
}

/* ---------- IFileOpenDialog ---------- */
struct fdlg {
  uint32_t opts;
  char folder[300];   /* SetFolder / SetDefaultFolder */
  char title[80];
  char result[300];
  int done;
};

static void item_path8(uintptr_t v, char* out, int max) {
  struct comobj* o = (struct comobj*)v;
  out[0] = 0;
  if (!o || strcmp(o->name, "ShellItem") != 0 || !o->data) return;
  {
    char* s = wdup16to8((const wchar16*)o->data);
    if (s) {
      strncpy(out, s, (size_t)max - 1);
      out[max - 1] = 0;
      k_free(s);
    }
  }
}

static long dlg_handler(struct comobj* self, int slot, uintptr_t* a) {
  struct fdlg* d = (struct fdlg*)self->data;
  if (!d) return E_FAIL;
  switch (slot) {
    case 0: { /* QueryInterface: sem IFileDialogCustomize {E6FDD21A-...} nem eventos */
      uint32_t g = a[0] ? *(const uint32_t*)a[0] : 0;
      if (g == 0xE6FDD21Au || g == 0x8016B7B3u) {
        *(void**)a[1] = 0;
        return E_NOINTERFACE;
      }
      return E_NOTIMPL; /* o resto: o proprio objeto */
    }
    case 3: { /* Show(hwnd) */
      char out[300];
      const char* init = d->folder[0] ? d->folder : "D:\\";
      if (!(d->opts & FOS_PICKFOLDERS))
        wlog("Escolher arquivo: so pastas por enquanto; abrindo o seletor de pastas", 0);
      if (!wpick_folder(d->title, init, out, (int)sizeof(out))) return ERROR_CANCELLED_HR;
      strcpy(d->result, out);
      d->done = 1;
      return S_OK;
    }
    case 9: /* SetOptions */
      d->opts = (uint32_t)a[0];
      return S_OK;
    case 10: /* GetOptions */
      if (a[0]) *(uint32_t*)a[0] = d->opts;
      return S_OK;
    case 11: /* SetDefaultFolder */
      if (!d->folder[0]) item_path8(a[0], d->folder, (int)sizeof(d->folder));
      return S_OK;
    case 12: /* SetFolder */
      item_path8(a[0], d->folder, (int)sizeof(d->folder));
      return S_OK;
    case 13: /* GetFolder */
    case 14: /* GetCurrentSelection */
      if (!a[0]) return E_POINTER;
      *(void**)a[0] = item_new8(d->done ? d->result : (d->folder[0] ? d->folder : "D:\\"));
      return S_OK;
    case 16: { /* GetFileName(LPWSTR*) */
      wchar16* z = (wchar16*)wheap_alloc(2);
      if (!z) return E_OUTOFMEMORY;
      z[0] = 0;
      *(wchar16**)a[0] = z;
      return S_OK;
    }
    case 17: { /* SetTitle */
      char* s = a[0] ? wdup16to8((const wchar16*)a[0]) : 0;
      if (s) {
        strncpy(d->title, s, sizeof(d->title) - 1);
        k_free(s);
      }
      return S_OK;
    }
    case 20: /* GetResult(IShellItem**) */
      if (!a[0]) return E_POINTER;
      if (!d->done) return E_FAIL;
      *(void**)a[0] = item_new8(d->result);
      return S_OK;
    case 27: /* GetResults(IShellItemArray**) */
    case 28: /* GetSelectedItems */
      if (!a[0]) return E_POINTER;
      if (!d->done) return E_FAIL;
      *(void**)a[0] = plain_new("ShellItemArray", array_handler, item_new8(d->result));
      return S_OK;
    case 7: /* Advise(sink, *cookie): sem eventos */
      if (a[1]) *(DWORD*)a[1] = 1;
      return S_OK;
    case 6: /* GetFileTypeIndex */
      if (a[0]) *(uint32_t*)a[0] = 1;
      return S_OK;
    case 4: case 5: case 8: case 15: case 18: case 19: case 21: case 22: case 23: case 24: case 25:
    case 26:
      return S_OK; /* tipos, nomes, rotulos, lugares, Close, guid: nada a guardar */
  }
  return E_NOTIMPL;
}

/* {DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7} CLSID_FileOpenDialog */
static const uint8_t clsid_open[16] = {0x9C, 0x5A, 0x1C, 0xDC, 0x8A, 0xE8, 0xDE, 0x4D,
                                       0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7};

/* CoCreateInstance (sys.c) pergunta aqui primeiro. 1 = criado. */
int wpicker_create(const void* clsid, void** out) {
  struct fdlg* d;
  if (!clsid || memcmp(clsid, clsid_open, 16) != 0) return 0;
  d = (struct fdlg*)wzalloc(sizeof(*d));
  if (!d) return 0;
  *out = plain_new("FileOpenDialog", dlg_handler, d);
  wlog("CoCreateInstance: FileOpenDialog (seletor do Explorer)", 0);
  return *out != 0;
}

/* SHCreateItemFromParsingName(path, bindctx, riid, ppv) */
static WINAPI long SHCreateItemFromParsingName(const wchar16* path, void* bc, const void* iid, void** out) {
  char p[300];
  (void)bc;
  (void)iid;
  if (!out) return E_POINTER;
  *out = 0;
  if (!path) return (long)0x80070057;
  wpath16(path, p, (int)sizeof(p));
  *out = item_new8(p);
  return *out ? S_OK : E_OUTOFMEMORY;
}

/* ---------- SHBrowseForFolderW ---------- */
struct browseinfo {
  HANDLE owner;
  void* root;
  wchar16* display;   /* MAX_PATH: o nome da pasta escolhida */
  const wchar16* title;
  uint32_t flags;
  void* fn;
  uintptr_t lparam;
  int image;
};
#define PIDL_MAGIC 0x4C444950u /* o mesmo de shell.c (SHGetPathFromIDList) */
static WINAPI void* SHBrowseForFolderW(struct browseinfo* bi) {
  char out[300], t[80];
  uint32_t* pidl;
  wchar16* w;
  size_t n;
  t[0] = 0;
  if (bi && bi->title) {
    char* s = wdup16to8(bi->title);
    if (s) {
      strncpy(t, s, sizeof(t) - 1);
      t[sizeof(t) - 1] = 0;
      k_free(s);
    }
  }
  if (!wpick_folder(t, "D:\\", out, (int)sizeof(out))) return 0;
  w = wdup8to16(out);
  if (!w) return 0;
  n = wstrlen16(w);
  pidl = (uint32_t*)k_malloc((unsigned)(4 + (n + 1) * 2));
  if (pidl) {
    pidl[0] = PIDL_MAGIC;
    memcpy(pidl + 1, w, (n + 1) * 2);
  }
  if (bi && bi->display) {
    const wchar16* nm = w + n;
    while (nm > w && nm[-1] != '\\') nm--;
    memcpy(bi->display, nm, (wstrlen16(nm) + 1) * 2);
  }
  k_free(w);
  return pidl;
}
static WINAPI void* SHBrowseForFolderA(struct browseinfo* bi) {
  (void)bi;
  return SHBrowseForFolderW(0);
}

/* SHCreateItemFromIDList(pidl, riid, ppv): o PIDL nosso tem o caminho */
static WINAPI long SHCreateItemFromIDList(const void* pidl, const void* iid, void** out) {
  const uint32_t* p = (const uint32_t*)pidl;
  char* s;
  (void)iid;
  if (!out) return E_POINTER;
  *out = 0;
  if (!p || p[0] != PIDL_MAGIC) return E_FAIL;
  s = wdup16to8((const wchar16*)(p + 1));
  *out = item_new8(s ? s : "");
  k_free(s);
  return S_OK;
}

#define S(n) {#n, (void*)n}
const struct wine_sym picker_syms[] = {S(SHCreateItemFromParsingName), S(SHBrowseForFolderW),
                                       S(SHBrowseForFolderA), S(SHCreateItemFromIDList), {0, 0}};
