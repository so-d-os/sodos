/*
 * fill.c - Complementos que o Claude Desktop (Chromium) chama no caminho
 * normal e que antes caiam no stub fatal (STATUS_ENTRYPOINT_NOT_FOUND).
 * Cada um faz o que o Windows faria ou falha com o codigo que o Chromium
 * espera e trata (sem servico, sem suporte).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define LOOKF(t, n) ((t)builtin_lookup(n))

/* ---------- SList (pilha atomica do ntdll) ----------
 * SLIST_HEADER: 16 bytes; aqui [0] = primeiro item, [1] = profundidade. A
 * trava global basta (o Windows usa cmpxchg16b). */
struct slist_entry {
  struct slist_entry* next;
};
static volatile int sl_lock;
static void sl_take(void) {
  while (__sync_lock_test_and_set(&sl_lock, 1)) k_thread_yield();
}
static void sl_give(void) { __sync_lock_release(&sl_lock); }

static WINAPI struct slist_entry* InterlockedPushEntrySList(uint64_t* head, struct slist_entry* e) {
  struct slist_entry* old;
  sl_take();
  old = (struct slist_entry*)(uintptr_t)head[0];
  e->next = old;
  head[0] = (uint64_t)(uintptr_t)e;
  head[1] = (head[1] & ~0xFFFFull) | ((head[1] + 1) & 0xFFFF);
  sl_give();
  return old;
}
static WINAPI struct slist_entry* InterlockedPopEntrySList(uint64_t* head) {
  struct slist_entry* e;
  sl_take();
  e = (struct slist_entry*)(uintptr_t)head[0];
  if (e) {
    head[0] = (uint64_t)(uintptr_t)e->next;
    head[1] = (head[1] & ~0xFFFFull) | ((head[1] - 1) & 0xFFFF);
  }
  sl_give();
  return e;
}
static WINAPI struct slist_entry* InterlockedFlushSList(uint64_t* head) {
  struct slist_entry* e;
  sl_take();
  e = (struct slist_entry*)(uintptr_t)head[0];
  head[0] = 0;
  head[1] &= ~0xFFFFull;
  sl_give();
  return e;
}
static WINAPI uint16_t QueryDepthSList(uint64_t* head) { return (uint16_t)(head[1] & 0xFFFF); }
static WINAPI struct slist_entry* InterlockedPushListSListEx(uint64_t* head, struct slist_entry* first,
                                                            struct slist_entry* last, DWORD count) {
  struct slist_entry* old;
  sl_take();
  old = (struct slist_entry*)(uintptr_t)head[0];
  last->next = old;
  head[0] = (uint64_t)(uintptr_t)first;
  head[1] = (head[1] & ~0xFFFFull) | ((head[1] + count) & 0xFFFF);
  sl_give();
  return old;
}

/* ---------- Winsock: servicos de nome (NLA) ----------
 * O Chromium usa para saber se a rede mudou; sem servico ele cai para outro
 * metodo. */
static WINAPI int WSALookupServiceBeginW(void* restr, DWORD fl, HANDLE* h) {
  (void)restr;
  (void)fl;
  if (h) *h = 0;
  wset_lasterr(10108); /* WSASERVICE_NOT_FOUND */
  return -1;
}
static WINAPI int WSALookupServiceNextW(HANDLE h, DWORD fl, DWORD* len, void* res) {
  (void)h; (void)fl; (void)len; (void)res;
  wset_lasterr(10110); /* WSA_E_NO_MORE */
  return -1;
}
static WINAPI int WSALookupServiceEnd(HANDLE h) {
  (void)h;
  return 0;
}
static WINAPI int WSANSPIoctl(HANDLE h, DWORD code, void* in, DWORD nin, void* out, DWORD nout, DWORD* got,
                              void* comp) {
  (void)h; (void)code; (void)in; (void)nin; (void)out; (void)nout; (void)comp;
  if (got) *got = 0;
  wset_lasterr(10045); /* WSAEOPNOTSUPP */
  return -1;
}

/* ---------- arquivos ---------- */
/* Vigia de pasta (fs.watch do Node, FilePathWatcher do Chromium). Ainda sem
 * aviso de mudanca: o pedido com OVERLAPPED fica pendente para sempre (como
 * numa pasta que nada muda) e so termina cancelado, no CancelIo ou no
 * CloseHandle, com STATUS_CANCELLED na porta de conclusao. Antes devolvia
 * ERROR_INVALID_FUNCTION e o caminho de erro do libuv derrubava o Claude
 * Desktop ao vigiar .git\refs de um repositorio (escrita no endereco 8).
 * Sem OVERLAPPED (esperaria para sempre) continua recusando. */
struct ov_rdc {
  uintptr_t internal, internal_high;
  DWORD off, off_high;
  HANDLE event;
};
int wiocp_post(struct wobj* port, DWORD bytes, uintptr_t key, void* ov);
int wdir_watch_cancel(struct wobj* f) {
  struct ov_rdc* ov;
  uintptr_t evh;
  if (!f) return 0;
  wlock();
  ov = (struct ov_rdc*)f->watch_ov;
  f->watch_ov = 0;
  wunlock();
  if (!ov) return 0;
  ov->internal = 0xC0000120u; /* STATUS_CANCELLED */
  ov->internal_high = 0;
  evh = (uintptr_t)ov->event;
  if (f->p1 && !(evh & 1)) wiocp_post((struct wobj*)f->p1, 0, (uintptr_t)f->u1, ov);
  if (evh & ~(uintptr_t)1) {
    typedef BOOL(WINAPI * se_fn)(HANDLE);
    se_fn se = (se_fn)builtin_lookup("SetEvent");
    if (se) se((HANDLE)(evh & ~(uintptr_t)1));
  }
  wsignal_global();
  return 1;
}
static WINAPI BOOL ReadDirectoryChangesW(HANDLE h, void* buf, DWORD len, BOOL sub, DWORD filter, DWORD* got,
                                         void* ovp, void* comp) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  struct ov_rdc* ov = (struct ov_rdc*)ovp;
  (void)buf; (void)len; (void)sub; (void)filter;
  if (got) *got = 0;
  if (!f || !f->is_dir) {
    wset_lasterr(6); /* ERROR_INVALID_HANDLE */
    return 0;
  }
  if (!ov || comp) {
    wset_lasterr(1); /* ERROR_INVALID_FUNCTION: sem aviso sincrono */
    return 0;
  }
  ov->internal = 0x103; /* STATUS_PENDING */
  ov->internal_high = 0;
  wlock();
  f->watch_ov = ov;
  wunlock();
  if (wine_debug >= 2) wlog("ReadDirectoryChangesW: vigia pendente em ", f->path ? f->path : "?");
  wset_lasterr(997); /* ERROR_IO_PENDING */
  return 1;
}
static WINAPI BOOL CancelSynchronousIo(HANDLE t) {
  (void)t;
  wset_lasterr(1168); /* ERROR_NOT_FOUND: nenhuma E/S sincrona pendente */
  return 0;
}
/* um usuario, sem compartilhamento entre maquinas: travas sempre concedidas */
static WINAPI BOOL LockFile(HANDLE h, DWORD lo, DWORD hi, DWORD nlo, DWORD nhi) {
  (void)h; (void)lo; (void)hi; (void)nlo; (void)nhi;
  return 1;
}
static WINAPI BOOL UnlockFile(HANDLE h, DWORD lo, DWORD hi, DWORD nlo, DWORD nhi) {
  (void)h; (void)lo; (void)hi; (void)nlo; (void)nhi;
  return 1;
}

long wfile_setinfo(HANDLE h, int cls, const void* buf, DWORD len); /* file.c */
typedef BOOL(WINAPI* sfpe_fn)(HANDLE, int64_t, int64_t*, DWORD);
typedef BOOL(WINAPI* seof_fn)(HANDLE);
static WINAPI BOOL SetFileInformationByHandle(HANDLE h, int cls, void* buf, DWORD len) {
  switch (cls) {
    case 0: /* FileBasicInfo: o FAT daqui nao guarda; so a data das travas */
      wfile_setinfo(h, 4, buf, len);
      return 1;
    case 3: /* FileRenameInfo */
    case 4: /* FileDispositionInfo */
    case 21: /* FileDispositionInfoEx */
    case 22: { /* FileRenameInfoEx */
      long st = wfile_setinfo(h, cls == 3 ? 10 : cls == 4 ? 13 : cls == 21 ? 64 : 65, buf, len);
      if (st) {
        wset_lasterr(st == (long)0xC0000035 ? 183u : 5u);
        return 0;
      }
      return 1;
    }
    case 5: /* FileAllocationInfo: reserva, nada a fazer */
      return 1;
    case 6: { /* FileEndOfFileInfo */
      sfpe_fn sp = LOOKF(sfpe_fn, "SetFilePointerEx");
      seof_fn se = LOOKF(seof_fn, "SetEndOfFile");
      int64_t cur = 0;
      if (len < 8 || !sp || !se) break;
      if (!sp(h, 0, &cur, 1)) return 0;
      if (!sp(h, *(int64_t*)buf, 0, 0) || !se(h)) return 0;
      sp(h, cur, 0, 0);
      return 1;
    }
  }
  wset_lasterr(50); /* ERROR_NOT_SUPPORTED */
  return 0;
}
/* "D:\pasta\arq" -> "D:\" */
static WINAPI BOOL GetVolumePathNameW(const wchar16* path, wchar16* out, DWORD n) {
  if (!path || !out || n < 4) {
    wset_lasterr(87);
    return 0;
  }
  if (path[0] && path[1] == ':') {
    out[0] = path[0];
  } else {
    out[0] = 'D';
  }
  out[1] = ':';
  out[2] = '\\';
  out[3] = 0;
  return 1;
}
typedef BOOL(WINAPI* mfe_fn)(const wchar16*, const wchar16*, DWORD);
static WINAPI BOOL ReplaceFileW(const wchar16* replaced, const wchar16* repl, const wchar16* backup, DWORD fl,
                                void* x, void* y) {
  mfe_fn mv = LOOKF(mfe_fn, "MoveFileExW");
  (void)fl; (void)x; (void)y;
  if (!mv) {
    wset_lasterr(50);
    return 0;
  }
  if (backup) mv(replaced, backup, 1);
  return mv(repl, replaced, 1); /* MOVEFILE_REPLACE_EXISTING */
}


/* ---------- IPHLPAPI: avisos de mudanca de rede ----------
 * O endereco do SODOS nao muda durante a sessao: o aviso fica pendente para
 * sempre (como no Windows sem mudanca) e o cancelamento sempre da certo. */
#define NO_ERROR 0
static WINAPI DWORD NotifyAddrChange(HANDLE* h, void* ov) {
  if (h) *h = 0;
  return ov ? 997u : 50u; /* ERROR_IO_PENDING; sincrono: ERROR_NOT_SUPPORTED */
}
static WINAPI BOOL CancelIPChangeNotify(void* ov) {
  (void)ov;
  return 1;
}
static WINAPI DWORD NotifyIpInterfaceChange(int fam, void* cb, void* ctx, BOOL initial, HANDLE* h) {
  (void)fam; (void)cb; (void)ctx; (void)initial;
  if (h) *h = (HANDLE)(uintptr_t)0x1F1F0001u;
  return NO_ERROR;
}
static WINAPI DWORD CancelMibChangeNotify2(HANDLE h) {
  (void)h;
  return NO_ERROR;
}
static WINAPI DWORD ConvertInterfaceIndexToLuid(DWORD idx, uint64_t* luid) {
  if (!luid) return 87;
  *luid = ((uint64_t)6 << 48) | idx; /* IF_TYPE_ETHERNET_CSMACD */
  return NO_ERROR;
}
static WINAPI DWORD ConvertInterfaceLuidToNameW(const uint64_t* luid, wchar16* out, size_t n) {
  static const char nm[] = "ethernet_0";
  size_t i;
  (void)luid;
  if (!out || n < sizeof(nm)) return 122; /* ERROR_INSUFFICIENT_BUFFER */
  for (i = 0; i < sizeof(nm); i++) out[i] = (wchar16)nm[i];
  return NO_ERROR;
}
static WINAPI DWORD GetBestRoute2(void* a, DWORD b, void* c, void* d, DWORD e, void* f, void* g) {
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
  return 1168; /* ERROR_NOT_FOUND */
}
static WINAPI DWORD GetInterfaceInfo(void* t, DWORD* len) {
  (void)t;
  (void)len;
  return 232; /* ERROR_NO_DATA */
}
static WINAPI char* if_indextoname(DWORD idx, char* out) {
  static const char nm[] = "ethernet_0";
  (void)idx;
  if (!out) return 0;
  memcpy(out, nm, sizeof(nm));
  return out;
}
static WINAPI DWORD if_nametoindex(const char* n) {
  (void)n;
  return 1;
}

/* ---------- user32 ---------- */
typedef int(WINAPI* gsm_fn)(int);
typedef BOOL(WINAPI* gcp_fn)(int32_t*);
typedef DWORD(WINAPI* gtc_fn)(void);
static int screen(int i) {
  gsm_fn f = LOOKF(gsm_fn, "GetSystemMetrics");
  return f ? f(i) : (i ? 768 : 1024);
}
static DWORD ticks(void) { return (DWORD)wnow_ms(); }
static WINAPI DWORD GetDoubleClickTime(void) { return 500; }
static WINAPI DWORD GetMessagePos(void) {
  gcp_fn f = LOOKF(gcp_fn, "GetCursorPos");
  int32_t pt[2] = {0, 0};
  if (f) f(pt);
  return ((DWORD)(pt[1] & 0xFFFF) << 16) | (DWORD)(pt[0] & 0xFFFF);
}
static WINAPI DWORD GetMessageTime(void) { return ticks(); }
static WINAPI intptr_t GetMessageExtraInfo(void) { return 0; }
#define HKL_US ((HANDLE)(uintptr_t)0x04090409u)
static WINAPI HANDLE GetKeyboardLayout(DWORD tid) {
  (void)tid;
  return HKL_US;
}
static WINAPI int GetKeyboardLayoutList(int n, HANDLE* list) {
  if (n > 0 && list) list[0] = HKL_US;
  return 1;
}
static WINAPI BOOL GetKeyboardLayoutNameW(wchar16* out) {
  static const char nm[] = "00000409";
  int i;
  if (!out) return 0;
  for (i = 0; i < 9; i++) out[i] = (wchar16)nm[i];
  return 1;
}
static WINAPI BOOL GetKeyboardState(uint8_t* st) {
  typedef short(WINAPI * gks_fn)(int);
  gks_fn f = LOOKF(gks_fn, "GetKeyState");
  int i;
  if (!st) return 0;
  for (i = 0; i < 256; i++) st[i] = f ? (uint8_t)(((f(i) & 0x8000) ? 0x80 : 0) | (f(i) & 1)) : 0;
  return 1;
}
static WINAPI BOOL SetKeyboardState(uint8_t* st) {
  (void)st;
  return 1;
}
static WINAPI BOOL GetLastInputInfo(DWORD* lii) {
  if (!lii) return 0;
  lii[1] = ticks();
  return 1;
}
static WINAPI BOOL TrackMouseEvent(void* t) {
  (void)t;
  return 1;
}
static WINAPI BOOL RegisterRawInputDevices(void* d, DWORD n, DWORD sz) {
  (void)d; (void)n; (void)sz;
  return 1;
}
static WINAPI DWORD GetRawInputDeviceList(void* l, DWORD* n, DWORD sz) {
  (void)l;
  (void)sz;
  if (n) *n = 0;
  return 0;
}
static WINAPI DWORD GetRawInputData(HANDLE h, DWORD cmd, void* d, DWORD* sz, DWORD hs) {
  (void)h; (void)cmd; (void)d; (void)sz; (void)hs;
  return 0xFFFFFFFFu;
}
static WINAPI DWORD GetRawInputDeviceInfoW(HANDLE h, DWORD cmd, void* d, DWORD* sz) {
  (void)h; (void)cmd; (void)d; (void)sz;
  return 0xFFFFFFFFu;
}
static WINAPI intptr_t DefRawInputProc(void* r, int n, DWORD h) {
  (void)r; (void)n; (void)h;
  return 0;
}
static WINAPI HANDLE RegisterPowerSettingNotification(HANDLE h, const void* g, DWORD fl) {
  (void)h; (void)g; (void)fl;
  return (HANDLE)(uintptr_t)0x1F1F0002u;
}
static WINAPI BOOL UnregisterPowerSettingNotification(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI HANDLE RegisterDeviceNotificationW(HANDLE h, void* f, DWORD fl) {
  (void)h; (void)f; (void)fl;
  return (HANDLE)(uintptr_t)0x1F1F0003u;
}
static WINAPI BOOL UnregisterDeviceNotification(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI void NotifyWinEvent(DWORD ev, HANDLE w, long o, long c) {
  (void)ev; (void)w; (void)o; (void)c;
}
static WINAPI BOOL AddClipboardFormatListener(HANDLE w) {
  (void)w;
  return 1;
}
static WINAPI BOOL RemoveClipboardFormatListener(HANDLE w) {
  (void)w;
  return 1;
}
static WINAPI BOOL AllowSetForegroundWindow(DWORD pid) {
  (void)pid;
  return 1;
}
static WINAPI BOOL SetLayeredWindowAttributes(HANDLE w, DWORD key, uint8_t a, DWORD fl) {
  (void)w; (void)key; (void)a; (void)fl;
  return 1;
}
static WINAPI BOOL GetLayeredWindowAttributes(HANDLE w, DWORD* key, uint8_t* a, DWORD* fl) {
  (void)w;
  if (key) *key = 0;
  if (a) *a = 255;
  if (fl) *fl = 0;
  return 1;
}
static WINAPI BOOL UpdateLayeredWindow(HANDLE w, HANDLE dd, void* pt, void* sz, HANDLE ds, void* sp, DWORD key,
                                       void* bl, DWORD fl) {
  static int said;
  (void)w; (void)dd; (void)pt; (void)sz; (void)ds; (void)sp; (void)key; (void)bl; (void)fl;
  if (!said && wine_debug) { /* para saber se o Chromium desenha por aqui */
    said = 1;
    wlog("UpdateLayeredWindow chamado (ainda sem desenho)", 0);
  }
  return 1;
}
/* janela sem moldura do Chromium: a regiao recorta cantos; aqui o retangulo todo */
static WINAPI int SetWindowRgn(HANDLE w, HANDLE rgn, BOOL redraw) {
  (void)w; (void)rgn; (void)redraw;
  return 1;
}
static WINAPI int GetWindowRgn(HANDLE w, HANDLE rgn) {
  (void)w;
  (void)rgn;
  return 0; /* ERROR: sem regiao */
}
static WINAPI BOOL SetWindowDisplayAffinity(HANDLE w, DWORD a) {
  (void)w;
  (void)a;
  return 1;
}
static WINAPI BOOL GetWindowDisplayAffinity(HANDLE w, DWORD* a) {
  (void)w;
  if (a) *a = 0;
  return 1;
}
static WINAPI BOOL IsWindowArranged(HANDLE w) {
  (void)w;
  return 0;
}
static WINAPI DWORD MapVirtualKeyExW(DWORD code, DWORD type, HANDLE hkl) {
  (void)hkl;
  if (type == 2) { /* MAPVK_VK_TO_CHAR */
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == ' ') return code;
    return 0;
  }
  return 0;
}
static WINAPI DWORD MapVirtualKeyW(DWORD code, DWORD type) { return MapVirtualKeyExW(code, type, 0); }
static WINAPI int ToUnicodeEx(DWORD vk, DWORD sc, const uint8_t* st, wchar16* out, int n, DWORD fl, HANDLE hkl) {
  (void)vk; (void)sc; (void)st; (void)out; (void)n; (void)fl; (void)hkl;
  return 0;
}
static WINAPI BOOL GetPointerType(DWORD id, DWORD* t) {
  (void)id;
  if (t) *t = 4; /* PT_MOUSE */
  return 1;
}
static WINAPI BOOL GetPointerDevices(DWORD* n, void* d) {
  (void)d;
  if (n) *n = 0;
  return 1;
}
static WINAPI BOOL GetPointerDevice(HANDLE h, void* d) {
  (void)h;
  (void)d;
  return 0;
}
static WINAPI BOOL GetPointerPenInfo(DWORD id, void* d) {
  (void)id;
  (void)d;
  return 0;
}
static WINAPI BOOL RegisterPointerDeviceNotifications(HANDLE w, BOOL r) {
  (void)w;
  (void)r;
  return 1;
}
static WINAPI BOOL EnumThreadWindows(DWORD tid, void* fn, intptr_t lp) {
  (void)tid; (void)fn; (void)lp;
  return 1;
}
static WINAPI HANDLE CopyIcon(HANDLE h) { return h; }
static WINAPI BOOL GetCursorInfo(DWORD* ci) {
  gcp_fn f = LOOKF(gcp_fn, "GetCursorPos");
  if (!ci) return 0;
  ci[1] = 1; /* CURSOR_SHOWING */
  *(HANDLE*)(ci + 2) = 0;
  if (f) f((int32_t*)(ci + 4));
  return 1;
}
static WINAPI DWORD GetGuiResources(HANDLE p, DWORD fl) {
  (void)p;
  (void)fl;
  return 0;
}
static WINAPI long GetDisplayConfigBufferSizes(DWORD fl, DWORD* np, DWORD* nm) {
  (void)fl;
  if (np) *np = 0;
  if (nm) *nm = 0;
  return 50; /* ERROR_NOT_SUPPORTED */
}
static WINAPI long QueryDisplayConfig(DWORD fl, DWORD* np, void* p, DWORD* nm, void* m, void* t) {
  (void)fl; (void)np; (void)p; (void)nm; (void)m; (void)t;
  return 50;
}
static WINAPI long DisplayConfigGetDeviceInfo(void* r) {
  (void)r;
  return 50;
}
static void put16(wchar16* d, const char* s, int max) {
  int i;
  for (i = 0; s[i] && i < max - 1; i++) d[i] = (wchar16)s[i];
  d[i] = 0;
}
/* um monitor so: \\.\DISPLAY1 no tamanho da tela do WIN */
static WINAPI BOOL EnumDisplayDevicesW(const wchar16* dev, DWORD idx, uint8_t* dd, DWORD fl) {
  (void)fl;
  if (!dd || idx > 0) return 0;
  memset(dd + 4, 0, 840 - 4);
  if (!dev) {
    put16((wchar16*)(dd + 4), "\\\\.\\DISPLAY1", 32);
    put16((wchar16*)(dd + 68), "SODOS Display", 128);
    *(DWORD*)(dd + 324) = 1 | 4; /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
  } else {
    put16((wchar16*)(dd + 4), "\\\\.\\DISPLAY1\\Monitor0", 32);
    put16((wchar16*)(dd + 68), "Monitor SODOS", 128);
    *(DWORD*)(dd + 324) = 1 | 2; /* ACTIVE | ATTACHED */
  }
  return 1;
}
static WINAPI BOOL EnumDisplaySettingsExW(const wchar16* dev, DWORD mode, uint8_t* dm, DWORD fl) {
  (void)dev;
  (void)fl;
  if (!dm || (mode != 0xFFFFFFFFu && mode != 0xFFFFFFFEu && mode != 0)) return 0;
  memset(dm + 64, 0, 220 - 64);
  *(uint16_t*)(dm + 68) = 220; /* dmSize */
  *(DWORD*)(dm + 72) = 0x40000 | 0x80000 | 0x100000 | 0x400000 | 0x20; /* BITSPERPEL|PELSWIDTH|PELSHEIGHT|FREQ|POSITION */
  *(DWORD*)(dm + 168) = 32;
  *(DWORD*)(dm + 172) = (DWORD)screen(0);
  *(DWORD*)(dm + 176) = (DWORD)screen(1);
  *(DWORD*)(dm + 184) = 60;
  return 1;
}
static WINAPI BOOL EnumDisplaySettingsW(const wchar16* dev, DWORD mode, uint8_t* dm) {
  return EnumDisplaySettingsExW(dev, mode, dm, 0);
}
typedef HANDLE(WINAPI* mfp_fn)(int64_t, DWORD);
static WINAPI HANDLE MonitorFromRect(const int32_t* r, DWORD fl) {
  mfp_fn f = LOOKF(mfp_fn, "MonitorFromPoint");
  int64_t pt = r ? ((int64_t)(uint32_t)r[1] << 32) | (uint32_t)r[0] : 0;
  return f ? f(pt, fl | 2) : 0; /* MONITOR_DEFAULTTONEAREST */
}
typedef BOOL(WINAPI* gmi_fn)(HANDLE, void*);
static WINAPI BOOL GetMonitorInfoA(HANDLE m, uint8_t* mi) {
  gmi_fn f = LOOKF(gmi_fn, "GetMonitorInfoW");
  uint8_t w[104];
  DWORD cb;
  int i;
  if (!mi || !f) return 0;
  cb = *(DWORD*)mi;
  memset(w, 0, sizeof(w));
  *(DWORD*)w = 104;
  if (!f(m, w)) return 0;
  memcpy(mi + 4, w + 4, 36);
  if (cb >= 72)
    for (i = 0; i < 32; i++) mi[40 + i] = (uint8_t)((wchar16*)(w + 40))[i];
  return 1;
}
static WINAPI DWORD CharUpperA_(char* s) {
  if ((uintptr_t)s < 0x10000) {
    DWORD c = (DWORD)(uintptr_t)s;
    return c >= 'a' && c <= 'z' ? c - 32 : c;
  }
  {
    char* p;
    for (p = s; *p; p++)
      if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  return (DWORD)(uintptr_t)s;
}
static WINAPI BOOL RegisterHotKey(HANDLE w, int id, DWORD m, DWORD vk) {
  (void)w; (void)id; (void)m; (void)vk;
  return 1;
}
static WINAPI BOOL UnregisterHotKey(HANDLE w, int id) {
  (void)w;
  (void)id;
  return 1;
}
static WINAPI BOOL SetCursorPos(int x, int y) {
  (void)x;
  (void)y;
  return 1;
}
static WINAPI BOOL ClipCursor(const void* r) {
  (void)r;
  return 1;
}
static WINAPI BOOL CloseTouchInputHandle(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI HANDLE WindowFromDC(HANDLE dc) {
  (void)dc;
  return 0;
}
static WINAPI BOOL InvertRect(HANDLE dc, const void* r) {
  (void)dc;
  (void)r;
  return 1;
}
static WINAPI BOOL DrawFrameControl(HANDLE dc, void* r, DWORD t, DWORD st) {
  (void)dc; (void)r; (void)t; (void)st;
  return 1;
}
static WINAPI BOOL PrintWindow(HANDLE w, HANDLE dc, DWORD fl) {
  (void)w; (void)dc; (void)fl;
  return 0;
}
static WINAPI int GetClipboardFormatNameW(DWORD f, wchar16* out, int n) {
  (void)f;
  if (out && n > 0) out[0] = 0;
  return 0;
}
static WINAPI BOOL UnregisterClassA(const char* n, HANDLE i) {
  (void)n;
  (void)i;
  return 1;
}

/* ---------- shcore / dwmapi ---------- */
static WINAPI long GetDpiForMonitor(HANDLE m, int type, DWORD* x, DWORD* y) {
  (void)m;
  (void)type;
  if (x) *x = 96;
  if (y) *y = 96;
  return 0;
}
static WINAPI long GetScaleFactorForMonitor(HANDLE m, int* sc) {
  (void)m;
  if (sc) *sc = 100;
  return 0;
}
static WINAPI BOOL DwmDefWindowProc(HANDLE w, DWORD m, uintptr_t wp, intptr_t lp, intptr_t* r) {
  (void)w; (void)m; (void)wp; (void)lp;
  if (r) *r = 0;
  return 0;
}
static WINAPI long DwmExtendFrameIntoClientArea(HANDLE w, const void* m) {
  (void)w;
  (void)m;
  return 0;
}
static WINAPI long DwmGetCompositionTimingInfo(HANDLE w, void* t) {
  (void)w;
  (void)t;
  return (long)0x80263001; /* DWM_E_COMPOSITIONDISABLED */
}

/* ---------- kernel32 / energia / sessao ---------- */
static WINAPI BOOL QueryUnbiasedInterruptTimePrecise(uint64_t* t) {
  if (!t) return 0;
  *t = wnow_ms() * 10000ull;
  return 1;
}
static WINAPI BOOL CompareObjectHandles(HANDLE a, HANDLE b) {
  return a == b || (wh_get(a, 0) && wh_get(a, 0) == wh_get(b, 0));
}
static WINAPI long CallNtPowerInformation(int lvl, void* in, DWORD nin, void* out, DWORD nout) {
  (void)lvl; (void)in; (void)nin; (void)out; (void)nout;
  return (long)0xC0000002; /* STATUS_NOT_IMPLEMENTED */
}
static WINAPI int PowerDeterminePlatformRoleEx(DWORD v) {
  (void)v;
  return 1; /* PlatformRoleDesktop */
}
static WINAPI BOOL WTSRegisterSessionNotification(HANDLE w, DWORD fl) {
  (void)w;
  (void)fl;
  return 1;
}
static WINAPI BOOL WTSUnRegisterSessionNotification(HANDLE w) {
  (void)w;
  return 1;
}
static WINAPI BOOL WTSQuerySessionInformationW(HANDLE s, DWORD id, int cls, wchar16** buf, DWORD* n) {
  (void)s; (void)id; (void)cls;
  if (buf) *buf = 0;
  if (n) *n = 0;
  wset_lasterr(50);
  return 0;
}
static WINAPI void WTSFreeMemory(void* p) { (void)p; }

/* ---------- ole32 / shell32 / shlwapi ---------- */
/* shlwapi #437 (o Chrome pede por ordinal, delay-load: sem ele, int3 na
 * checagem de dominio/gerencia). Windows 10 de estacao, fora de dominio, 64 bits
 * nativo: verdadeiro so para as familias "NT" e "ou mais novo". */
static WINAPI BOOL IsOS(DWORD os) {
  switch (os) {
    case 0:  /* OS_WIN32SORGREATER */
    case 1:  /* OS_NT */
    case 2:  /* OS_WIN95ORGREATER */
    case 3:  /* OS_NT4ORGREATER */
    case 4:  /* OS_WIN2000ORGREATER_ALT */
    case 5:  /* OS_WIN98ORGREATER */
    case 7:  /* OS_WIN2000ORGREATER */
    case 8:  /* OS_WIN2000PRO */
    case 18: /* OS_XPORGREATER */
    case 20: /* OS_PROFESSIONAL */
      return 1;
    default: /* OS_DOMAINMEMBER 28, OS_ANYSERVER 29, OS_WOW6432 30, servidor... */
      return 0;
  }
}
static WINAPI long PropVariantClear(void* pv) {
  if (pv) memset(pv, 0, 24);
  return 0;
}
static WINAPI long PropVariantCopy(void* d, const void* s) {
  if (!d || !s) return (long)0x80070057;
  memcpy(d, s, 24);
  return 0;
}
static WINAPI long CoRegisterInitializeSpy(void* spy, uint64_t* cookie) {
  (void)spy;
  if (cookie) *cookie = 1;
  return 0;
}
static WINAPI long CoRevokeInitializeSpy(uint64_t cookie) {
  (void)cookie;
  return 0;
}
static WINAPI long RoGetAgileReference(DWORD o, const void* iid, void* unk, void** out) {
  (void)o; (void)iid; (void)unk;
  if (out) *out = 0;
  return (long)0x80004001; /* E_NOTIMPL */
}
static WINAPI long SetCurrentProcessExplicitAppUserModelID(const wchar16* id) {
  (void)id;
  return 0;
}
static WINAPI long GetCurrentProcessExplicitAppUserModelID(wchar16** id) {
  if (id) *id = 0;
  return (long)0x80004005; /* E_FAIL: nenhum definido */
}
static WINAPI long SHGetPropertyStoreForWindow(HANDLE w, const void* iid, void** out) {
  (void)w;
  (void)iid;
  if (out) *out = 0;
  return (long)0x80004001;
}
static WINAPI wchar16* PathFindFileNameW(const wchar16* p) {
  const wchar16* b = p;
  if (!p) return 0;
  for (; *p; p++)
    if ((*p == '\\' || *p == '/' || *p == ':') && p[1]) b = p + 1;
  return (wchar16*)b;
}
static WINAPI wchar16* PathFindExtensionW(const wchar16* p) {
  const wchar16* dot = 0;
  if (!p) return 0;
  for (; *p; p++) {
    if (*p == '.') dot = p;
    else if (*p == '\\' || *p == '/' || *p == ' ') dot = 0;
  }
  return (wchar16*)(dot ? dot : p);
}
static WINAPI void PathRemoveExtensionW(wchar16* p) {
  wchar16* e = PathFindExtensionW(p);
  if (e && *e) *e = 0;
}
static int spec_match(const wchar16* s, const wchar16* m) {
  for (; *m; m++, s++) {
    wchar16 a = *s, b = *m;
    if (b == '*') {
      for (;; s++) {
        if (spec_match(s, m + 1)) return 1;
        if (!*s) return 0;
      }
    }
    if (!a) return 0;
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (b != '?' && a != b) return 0;
  }
  return !*s;
}
static WINAPI BOOL PathMatchSpecW(const wchar16* f, const wchar16* spec) {
  wchar16 one[128];
  const wchar16* name = PathFindFileNameW(f);
  if (!f || !spec) return 0;
  while (*spec) { /* "*.txt;*.md" */
    int n = 0;
    while (*spec == ' ' || *spec == ';') spec++;
    while (*spec && *spec != ';' && n < 127) one[n++] = *spec++;
    one[n] = 0;
    if (n && spec_match(name, one)) return 1;
  }
  return 0;
}
typedef wchar16*(WINAPI* ges_fn)(void);
static WINAPI BOOL CreateEnvironmentBlock(void** env, HANDLE tok, BOOL inh) {
  ges_fn f = LOOKF(ges_fn, "GetEnvironmentStringsW");
  (void)tok;
  (void)inh;
  if (!env) return 0;
  *env = f ? f() : 0;
  return *env != 0;
}
static WINAPI BOOL DestroyEnvironmentBlock(void* env) {
  (void)env;
  return 1;
}

/* ---------- DXGI / D3D11 / DirectComposition ----------
 * Sem GPU no WINE: existem (o Chromium carrega por delay-load e cai se a DLL
 * faltar) e respondem "sem suporte"; o Chromium passa ao desenho por software. */
#define DXGI_ERROR_UNSUPPORTED ((long)0x887A0004)
static WINAPI long CreateDXGIFactory(const void* iid, void** out) {
  (void)iid;
  if (out) *out = 0;
  return DXGI_ERROR_UNSUPPORTED;
}
static WINAPI long CreateDXGIFactory1(const void* iid, void** out) { return CreateDXGIFactory(iid, out); }
static WINAPI long CreateDXGIFactory2(DWORD fl, const void* iid, void** out) {
  (void)fl;
  return CreateDXGIFactory(iid, out);
}
static WINAPI long DXGIGetDebugInterface1(DWORD fl, const void* iid, void** out) {
  (void)fl;
  return CreateDXGIFactory(iid, out);
}
static WINAPI long D3D11CreateDevice(void* ad, int type, HANDLE sw, DWORD fl, const void* lv, DWORD nlv, DWORD sdk,
                                     void** dev, int* got, void** ctx) {
  (void)ad; (void)type; (void)sw; (void)fl; (void)lv; (void)nlv; (void)sdk;
  if (dev) *dev = 0;
  if (got) *got = 0;
  if (ctx) *ctx = 0;
  return DXGI_ERROR_UNSUPPORTED;
}
static WINAPI long DCompositionCreateDevice(void* dx, const void* iid, void** out) {
  (void)dx;
  (void)iid;
  if (out) *out = 0;
  return (long)0x80004001; /* E_NOTIMPL */
}
static WINAPI long DCompositionCreateDevice2(void* dx, const void* iid, void** out) {
  return DCompositionCreateDevice(dx, iid, out);
}
static WINAPI long DCompositionCreateDevice3(void* dx, const void* iid, void** out) {
  return DCompositionCreateDevice(dx, iid, out);
}
static WINAPI long DCompositionCreateSurfaceHandle(DWORD acc, void* sa, HANDLE* h) {
  (void)acc;
  (void)sa;
  if (h) *h = 0;
  return (long)0x80004001;
}

/* ---------- miudezas de DLLs do app (d3dcompiler, ffmpeg, swiftshader) ---------- */
static WINAPI BOOL DisableThreadLibraryCalls(HANDLE mod) {
  (void)mod;
  return 1; /* o WINE nao chama DllMain por thread mesmo */
}
static WINAPI BOOL HeapQueryInformation(HANDLE h, int cls, void* out, size_t n, size_t* got) {
  (void)h;
  if (got) *got = 4;
  if (cls != 0 || !out || n < 4) { /* so HeapCompatibilityInformation */
    wset_lasterr(cls != 0 ? 87u : 122u);
    return 0;
  }
  *(DWORD*)out = 2; /* LFH */
  return 1;
}
static WINAPI long UuidCreate(uint8_t* u) {
  if (!u) return 1702; /* RPC_S_INVALID_ARG */
  k_random(u, 16);
  u[6] = (uint8_t)((u[6] & 0x0F) | 0x40); /* versao 4 */
  u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);
  return 0;
}

/* ---------- NtQueryObject ----------
 * O Chromium confere o tipo dos handles que passa aos filhos (e o nome em
 * alguns casos). Basico (56 bytes), tipo (OBJECT_TYPE_INFORMATION, 104 bytes
 * + o nome logo depois) e nome (vazio). */
static const char* obj_type_name(HANDLE h) {
  struct wobj* o;
  if (h == (HANDLE)(intptr_t)-1) return "Process";
  if (h == (HANDLE)(intptr_t)-2) return "Thread";
  o = wh_get(h, 0);
  if (!o) return 0;
  switch (o->type) {
    case OBJ_EVENT: return "Event";
    case OBJ_THREAD: return "Thread";
    case OBJ_MUTEX: return "Mutant";
    case OBJ_SEM: return "Semaphore";
    case OBJ_TIMER: return "Timer";
    case OBJ_IOCP: return "IoCompletion";
    case OBJ_KEY: return "Key";
    case OBJ_PROCESS: return "Process";
    case OBJ_SECTION: return "Section";
    case OBJ_FILE:
    case OBJ_FIND:
    case OBJ_SOCKET:
    case OBJ_PIPE: return "File";
  }
  return "Event";
}
static WINAPI long NtQueryObject(HANDLE h, int cls, void* buf, DWORD len, DWORD* ret) {
  const char* tn = obj_type_name(h);
  uint8_t* b = (uint8_t*)buf;
  DWORD need, i, n;
  if (!tn) return (long)0xC0000008; /* STATUS_INVALID_HANDLE */
  switch (cls) {
    case 0: { /* ObjectBasicInformation */
      struct wobj* o = wh_get(h, 0);
      need = 56;
      if (ret) *ret = need;
      if (len < need || !b) return (long)0xC0000004; /* STATUS_INFO_LENGTH_MISMATCH */
      memset(b, 0, need);
      {
        /* GrantedAccess real: o Chromium confere que arquivo/secao recebidos
         * como "somente leitura" nao tem direito de escrita (CHECK) */
        DWORD acc = 0x001F0FFFu;
        if (o && o->type == OBJ_FILE) acc = o->writable ? 0x0012019Fu : 0x00120089u; /* FILE_GENERIC_* */
        else if (o && o->type == OBJ_SECTION) acc = (o->u1 & 1) ? 0x00020005u : 0x000F001Fu;
        *(DWORD*)(b + 4) = acc;
      }
      *(DWORD*)(b + 8) = o ? (DWORD)o->refs : 1; /* HandleCount */
      *(DWORD*)(b + 12) = o ? (DWORD)o->refs + 1 : 2;
      *(DWORD*)(b + 44) = 104 + 32;            /* TypeInfoSize */
      return 0;
    }
    case 1: /* ObjectNameInformation: UNICODE_STRING vazio */
      need = 16;
      if (ret) *ret = need;
      if (len < need || !b) return (long)0xC0000004;
      memset(b, 0, need);
      return 0;
    case 2: /* ObjectTypeInformation */
      for (n = 0; tn[n]; n++) {
      }
      need = 104 + (n + 1) * 2;
      if (ret) *ret = need;
      if (len < need || !b) return (long)0xC0000004;
      memset(b, 0, 104);
      *(uint16_t*)b = (uint16_t)(n * 2);           /* Length */
      *(uint16_t*)(b + 2) = (uint16_t)((n + 1) * 2); /* MaximumLength */
      *(uint8_t**)(b + 8) = b + 104;               /* Buffer */
      for (i = 0; i <= n; i++) ((uint16_t*)(b + 104))[i] = (uint16_t)tn[i];
      *(DWORD*)(b + 16) = 1;                       /* TotalNumberOfObjects */
      *(DWORD*)(b + 20) = 1;                       /* TotalNumberOfHandles */
      *(DWORD*)(b + 84) = 0x001F0FFFu;             /* ValidAccessMask */
      return 0;
  }
  return (long)0xC0000003; /* STATUS_INVALID_INFO_CLASS */
}

/* ---------- crypt32: o que o Chromium chama ao montar os repositorios do
 * sistema. Sem certificados do Windows: ele usa a propria Chrome Root Store. */
#define CRYPT_E_NOT_FOUND 0x80092004u
static WINAPI BOOL CertAddStoreToCollection(HANDLE col, HANDLE sib, DWORD fl, DWORD pri) {
  (void)col; (void)sib; (void)fl; (void)pri;
  return 1;
}
static WINAPI BOOL CertControlStore(HANDLE st, DWORD fl, DWORD ctl, const void* p) {
  (void)st; (void)fl; (void)ctl; (void)p;
  return 1;
}
static WINAPI BOOL CertAddEncodedCertificateToStore(HANDLE st, DWORD enc, const uint8_t* b, DWORD n, DWORD disp,
                                                     void** out) {
  (void)st; (void)enc; (void)b; (void)n; (void)disp;
  if (out) *out = 0;
  wset_lasterr(CRYPT_E_NOT_FOUND);
  return 0;
}
static WINAPI BOOL CertCompareCertificateName(DWORD enc, const void* a, const void* b) {
  const DWORD* x = (const DWORD*)a;
  const DWORD* y = (const DWORD*)b;
  (void)enc;
  if (!x || !y || x[0] != y[0]) return 0; /* CERT_NAME_BLOB {cbData, pbData} */
  return memcmp(*(void* const*)(x + 2), *(void* const*)(y + 2), x[0]) == 0;
}
static WINAPI void* CertFindChainInStore(HANDLE st, DWORD enc, DWORD fl, DWORD type, const void* para, void* prev) {
  (void)st; (void)enc; (void)fl; (void)type; (void)para; (void)prev;
  wset_lasterr(CRYPT_E_NOT_FOUND);
  return 0;
}
static WINAPI BOOL CertGetCertificateContextProperty(const void* c, DWORD id, void* d, DWORD* n) {
  (void)c; (void)id; (void)d;
  if (n) *n = 0;
  wset_lasterr(CRYPT_E_NOT_FOUND);
  return 0;
}
static WINAPI BOOL CertGetEnhancedKeyUsage(const void* c, DWORD fl, void* u, DWORD* n) {
  (void)c; (void)fl; (void)u;
  if (n) *n = 0;
  wset_lasterr(CRYPT_E_NOT_FOUND);
  return 0;
}
static WINAPI BOOL CertGetIntendedKeyUsage(DWORD enc, const void* ci, uint8_t* u, DWORD n) {
  (void)enc; (void)ci;
  if (u && n) memset(u, 0, n);
  return 0; /* sem extensao de uso */
}
static WINAPI long CertVerifyTimeValidity(const void* t, const void* ci) {
  (void)t;
  (void)ci;
  return 0;
}
static WINAPI BOOL CryptAcquireCertificatePrivateKey(const void* c, DWORD fl, void* r, void** key, DWORD* spec,
                                                     BOOL* free_) {
  (void)c; (void)fl; (void)r;
  if (key) *key = 0;
  if (spec) *spec = 0;
  if (free_) *free_ = 0;
  wset_lasterr(CRYPT_E_NOT_FOUND);
  return 0;
}
static WINAPI BOOL CryptVerifyCertificateSignatureEx(HANDLE p, DWORD enc, DWORD st, void* s, DWORD it, void* i,
                                                     DWORD fl, void* r) {
  (void)p; (void)enc; (void)st; (void)s; (void)it; (void)i; (void)fl; (void)r;
  wset_lasterr(0x80090006u); /* NTE_BAD_SIGNATURE */
  return 0;
}

/* ---------- acessibilidade (oleacc, UI Automation) e propsys ----------
 * Sem leitor de tela no SODOS: o Chromium carrega estas DLLs por delay-load
 * ao tratar WM_GETOBJECT e cai se a DLL faltar. Respondem "sem objeto". */
#define E_NOTIMPL_ ((long)0x80004001)
#define E_FAIL_ ((long)0x80004005)
static WINAPI long AccessibleObjectFromWindow(HANDLE w, DWORD id, const void* iid, void** out) {
  (void)w; (void)id; (void)iid;
  if (out) *out = 0;
  return E_FAIL_;
}
static WINAPI long AccessibleChildren(void* acc, long start, long n, void* arr, long* got) {
  (void)acc; (void)start; (void)n; (void)arr;
  if (got) *got = 0;
  return E_NOTIMPL_;
}
static WINAPI long CreateStdAccessibleObject(HANDLE w, long id, const void* iid, void** out) {
  (void)w; (void)id; (void)iid;
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI intptr_t LresultFromObject(const void* iid, uintptr_t wp, void* unk) {
  (void)iid; (void)wp; (void)unk;
  return E_NOTIMPL_; /* LRESULT de erro: o WM_GETOBJECT fica sem objeto */
}
static WINAPI long WindowFromAccessibleObject(void* acc, HANDLE* w) {
  (void)acc;
  if (w) *w = 0;
  return E_FAIL_;
}
static WINAPI intptr_t UiaReturnRawElementProvider(HANDLE w, uintptr_t wp, intptr_t lp, void* el) {
  (void)w; (void)wp; (void)lp; (void)el;
  return 0;
}
static WINAPI long UiaHostProviderFromHwnd(HANDLE w, void** out) {
  (void)w;
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI long UiaDisconnectProvider(void* p) {
  (void)p;
  return 0;
}
static WINAPI long UiaDisconnectAllProviders(void) { return 0; }
static WINAPI long UiaRaiseAutomationEvent(void* p, int id) {
  (void)p; (void)id;
  return 0;
}
static WINAPI long UiaRaiseAutomationPropertyChangedEvent(void* p, int id, uint8_t ov[24], uint8_t nv[24]) {
  (void)p; (void)id; (void)ov; (void)nv;
  return 0;
}
static WINAPI long UiaRaiseStructureChangedEvent(void* p, int t, int* rid, int n) {
  (void)p; (void)t; (void)rid; (void)n;
  return 0;
}
static WINAPI long UiaGetReservedMixedAttributeValue(void** out) {
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI long UiaGetReservedNotSupportedValue(void** out) {
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI long InitPropVariantFromCLSID(const void* clsid, uint8_t* pv) {
  (void)clsid;
  if (pv) memset(pv, 0, 24);
  return E_NOTIMPL_;
}
static WINAPI long PSCreateMemoryPropertyStore(const void* iid, void** out) {
  (void)iid;
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI int VariantCompare(const void* a, const void* b) {
  (void)a;
  (void)b;
  return 0;
}

/* ---------- memoria de outro processo ----------
 * Um espaco de enderecos so: ler/gravar "no outro processo" e copiar. O
 * crashpad-handler le a memoria do cliente ao registrar e ao gerar o dump.
 * So enderecos possiveis (canonicos, fora da pagina 0); sem garantia de que
 * a pagina exista: endereco virtual nao commitado ainda falharia. */
static int rpm_ok(const void* a, size_t n) {
  uintptr_t v = (uintptr_t)a;
  return v >= 0x10000 && v + n >= v && v + n < 0x0000800000000000ull;
}
static WINAPI BOOL ReadProcessMemory(HANDLE p, const void* a, void* buf, size_t n, size_t* got) {
  (void)p;
  if (got) *got = 0;
  if (!buf || !rpm_ok(a, n)) {
    wset_lasterr(299); /* ERROR_PARTIAL_COPY */
    return 0;
  }
  memcpy(buf, a, n);
  if (got) *got = n;
  return 1;
}
static WINAPI BOOL WriteProcessMemory(HANDLE p, void* a, const void* buf, size_t n, size_t* got) {
  (void)p;
  if (got) *got = 0;
  if (!buf || !rpm_ok(a, n)) {
    wset_lasterr(299);
    return 0;
  }
  memcpy(a, buf, n);
  if (got) *got = n;
  return 1;
}


/* ---------- o que o claude-native-binding.node importa ----------
 * mpr, ncrypt, wevtapi, energia, WTHelper...: respondem "nao suportado"
 * para o modulo carregar; NtCreateFile/NtReadFile/NtWriteFile vao para as
 * funcoes Win32 de arquivo. */
typedef HANDLE(WINAPI* cfw_fn)(const wchar16*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI* rf_fn)(HANDLE, void*, DWORD, DWORD*, void*);
typedef BOOL(WINAPI* sfp_fn)(HANDLE, int64_t, int64_t*, DWORD);
struct ntiosb {
  uintptr_t status;
  uintptr_t info;
};
static long nt_status_of(DWORD e) {
  switch (e) {
    case 2: return (long)0xC0000034;   /* OBJECT_NAME_NOT_FOUND */
    case 3: return (long)0xC000003A;   /* OBJECT_PATH_NOT_FOUND */
    case 80:
    case 183: return (long)0xC0000035; /* OBJECT_NAME_COLLISION */
    case 32: return (long)0xC0000043;  /* SHARING_VIOLATION */
    case 6: return (long)0xC0000008;   /* INVALID_HANDLE */
    case 38: return (long)0xC0000011;  /* END_OF_FILE */
  }
  return (long)0xC0000022; /* ACCESS_DENIED */
}
/* "\??\C:\x" e "\\?\C:\x" -> "C:\x" (no lugar) */
static void nt_dos_name(wchar16* n) {
  int k = 0, i;
  if (n[0] == '\\' && (n[1] == '?' || n[1] == '\\') && n[2] == '?' && n[3] == '\\') k = 4;
  if (!k) return;
  for (i = 0; n[i + k]; i++) n[i] = n[i + k];
  n[i] = 0;
}
/* nome de um OBJECT_ATTRIBUTES (UNICODE_STRING em +16) */
static int oa_name(const uint8_t* oa, wchar16* name, int cap) {
  const uint8_t* us;
  DWORD n;
  if (!oa) return -1;
  us = *(const uint8_t* const*)(oa + 16);
  if (!us) return -1;
  n = *(const uint16_t*)us / 2;
  if ((int)n >= cap) return -1;
  memcpy(name, *(const wchar16* const*)(us + 8), n * 2);
  name[n] = 0;
  nt_dos_name(name);
  return (int)n;
}
static WINAPI long NtCreateFile(HANDLE* out, DWORD access, const uint8_t* oa, struct ntiosb* io, void* alloc,
                                DWORD attrs, DWORD share, DWORD disp, DWORD opts, void* ea, DWORD ealen) {
  static const DWORD map[6] = {2, 3, 1, 4, 5, 2};
  cfw_fn cf = LOOKF(cfw_fn, "CreateFileW");
  const uint8_t* us;
  wchar16 name[400];
  DWORD n, fl = 0;
  HANDLE h;
  (void)alloc; (void)ea; (void)ealen;
  if (!out || !oa || !cf) return (long)0xC000000D;
  us = *(const uint8_t* const*)(oa + 16);
  if (!us) return (long)0xC000000D;
  n = *(const uint16_t*)us / 2;
  if (n >= 399) return (long)0xC0000106; /* NAME_TOO_LONG */
  memcpy(name, *(const wchar16* const*)(us + 8), n * 2);
  name[n] = 0;
  nt_dos_name(name);
  if (opts & 1) fl |= 0x02000000; /* FILE_DIRECTORY_FILE -> BACKUP_SEMANTICS */
  if (opts & 0x1000) fl |= 0x04000000; /* DELETE_ON_CLOSE */
  h = cf(name, access, share, 0, disp < 6 ? map[disp] : 3, fl | (attrs ? attrs : 0x80), 0);
  if (h == (HANDLE)(intptr_t)-1) {
    long st = nt_status_of(wget_lasterr());
    if (io) { io->status = (uintptr_t)(intptr_t)st; io->info = 0; }
    return st;
  }
  *out = h;
  if (io) { io->status = 0; io->info = 1; /* FILE_OPENED */ }
  return 0;
}
static long nt_rw(int wr, HANDLE h, HANDLE ev, struct ntiosb* io, void* buf, DWORD len, const int64_t* off) {
  rf_fn f = LOOKF(rf_fn, wr ? "WriteFile" : "ReadFile");
  sfp_fn sp = LOOKF(sfp_fn, "SetFilePointerEx");
  DWORD got = 0;
  long st = 0;
  if (!f) return (long)0xC0000002;
  if (off && *off >= 0 && sp) sp(h, *off, 0, 0);
  if (off && *off == -1 && wr && sp) sp(h, 0, 0, 2); /* FILE_WRITE_TO_END_OF_FILE */
  if (!f(h, buf, len, &got, 0)) st = nt_status_of(wget_lasterr());
  else if (!wr && len && !got) st = (long)0xC0000011; /* END_OF_FILE */
  if (io) { io->status = (uintptr_t)(intptr_t)st; io->info = got; }
  if (ev) {
    typedef BOOL(WINAPI * se_fn)(HANDLE);
    se_fn se = LOOKF(se_fn, "SetEvent");
    if (se) se(ev);
  }
  return st;
}
static WINAPI long NtReadFile(HANDLE h, HANDLE ev, void* apc, void* ctx, struct ntiosb* io, void* buf, DWORD len,
                              const int64_t* off, DWORD* key) {
  (void)apc; (void)ctx; (void)key;
  return nt_rw(0, h, ev, io, buf, len, off);
}
static WINAPI long NtWriteFile(HANDLE h, HANDLE ev, void* apc, void* ctx, struct ntiosb* io, const void* buf, DWORD len,
                               const int64_t* off, DWORD* key) {
  (void)apc; (void)ctx; (void)key;
  return nt_rw(1, h, ev, io, (void*)buf, len, off);
}
/* ---------- ntdll que o Claude Code (claude.exe, Bun) importa direto ----------
 * Sem elas o stub encerrava o processo (0xC0000139) na partida. */
static WINAPI long NtOpenFile(HANDLE* out, DWORD access, const uint8_t* oa, struct ntiosb* io, DWORD share,
                              DWORD opts) {
  return NtCreateFile(out, access, oa, io, 0, 0, share, 1 /* FILE_OPEN */, opts, 0, 0);
}
struct attr_data_ {
  DWORD attrs;
  uint64_t ctime, atime, mtime;
  DWORD size_hi, size_lo;
} __attribute__((packed));
typedef BOOL(WINAPI* gfae_fn)(const wchar16*, int, struct attr_data_*);
/* FILE_BASIC_INFORMATION: 4 tempos (LARGE_INTEGER) + atributos */
static WINAPI long NtQueryAttributesFile(const uint8_t* oa, uint8_t* info) {
  gfae_fn ga = LOOKF(gfae_fn, "GetFileAttributesExW");
  wchar16 name[400];
  struct attr_data_ d;
  if (oa_name(oa, name, 400) < 0 || !info || !ga) return (long)0xC000000D;
  if (!ga(name, 0, &d)) return nt_status_of(wget_lasterr());
  memcpy(info, &d.ctime, 8);
  memcpy(info + 8, &d.atime, 8);
  memcpy(info + 16, &d.mtime, 8);
  memcpy(info + 24, &d.mtime, 8);
  memcpy(info + 32, &d.attrs, 4);
  return 0;
}
static WINAPI long NtClose(HANDLE h) {
  typedef BOOL(WINAPI * ch_fn)(HANDLE);
  ch_fn ch = LOOKF(ch_fn, "CloseHandle");
  return ch && ch(h) ? 0 : (long)0xC0000008;
}
static WINAPI long NtQuerySecurityObject(HANDLE h, DWORD what, void* sd, DWORD len, DWORD* need) {
  (void)h; (void)what; (void)sd; (void)len;
  if (need) *need = 0;
  return (long)0xC00000BB; /* NOT_SUPPORTED */
}
static WINAPI long NtSetSecurityObject(HANDLE h, DWORD what, void* sd) {
  (void)h; (void)what; (void)sd;
  return 0;
}
static WINAPI long NtOpenDirectoryObject(HANDLE* out, DWORD access, const void* oa) {
  (void)access; (void)oa;
  if (out) *out = 0;
  return (long)0xC0000022;
}
static WINAPI long NtFsControlFile(HANDLE h, HANDLE ev, void* apc, void* ctx, struct ntiosb* io, DWORD code,
                                   void* in, DWORD inlen, void* out, DWORD outlen) {
  (void)h; (void)ev; (void)apc; (void)ctx; (void)in; (void)inlen; (void)out; (void)outlen;
  if (wine_debug) wlogx("NtFsControlFile sem suporte, codigo ", code);
  if (io) { io->status = (uintptr_t)(intptr_t)(long)0xC0000010; io->info = 0; }
  return (long)0xC0000010; /* INVALID_DEVICE_REQUEST */
}
static WINAPI long NtCreateNamedPipeFile(HANDLE* out, DWORD access, const uint8_t* oa, struct ntiosb* io,
                                         DWORD share, DWORD disp, DWORD opts, DWORD type, DWORD rmode,
                                         DWORD cmode, DWORD maxi, DWORD inq, DWORD outq, const int64_t* tmo) {
  typedef HANDLE(WINAPI * cnp_fn)(const wchar16*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void*);
  cnp_fn cnp = LOOKF(cnp_fn, "CreateNamedPipeW");
  wchar16 name[400], full[420];
  int n, i, k = 0;
  HANDLE h;
  DWORD om = 0;
  (void)share; (void)disp; (void)tmo; (void)cmode;
  if (!out || !cnp || (n = oa_name(oa, name, 400)) < 0) return (long)0xC000000D;
  /* "\Device\NamedPipe\x" -> "\\.\pipe\x" */
  {
    static const char dev[] = "\\Device\\NamedPipe\\";
    int m = 0;
    while (dev[m] && name[m] == (wchar16)dev[m]) m++;
    if (!dev[m]) k = m;
  }
  {
    static const char pre[] = "\\\\.\\pipe\\";
    for (i = 0; pre[i]; i++) full[i] = (wchar16)pre[i];
    for (; name[k] && i < 418; k++) full[i++] = name[k];
    full[i] = 0;
  }
  if (access & 0x80000000u) om |= 1; /* GENERIC_READ -> PIPE_ACCESS_INBOUND */
  if (access & 0x40000000u) om |= 2;
  if (access & 1) om |= 1;           /* FILE_READ_DATA */
  if (access & 2) om |= 2;           /* FILE_WRITE_DATA */
  if (!om) om = 3;
  if (!(opts & 0x30)) om |= 0x40000000u; /* sem SYNCHRONOUS_IO -> OVERLAPPED */
  h = cnp(full, om, (type ? 4 : 0) | (rmode ? 2 : 0), maxi ? maxi : 255, outq, inq, 0, 0);
  if (h == (HANDLE)(intptr_t)-1) {
    long st = nt_status_of(wget_lasterr());
    if (io) { io->status = (uintptr_t)(intptr_t)st; io->info = 0; }
    return st;
  }
  *out = h;
  if (io) { io->status = 0; io->info = 2; /* FILE_CREATED */ }
  return 0;
}
static WINAPI void RtlUnwind(void* frame, void* target, void* rec, void* ret) {
  (void)frame; (void)target; (void)rec; (void)ret;
  wsay("RtlUnwind: SEH por tabela ainda nao suportado", 0);
}
/* apiset: "IsApiSetImplemented" responde nao para tudo (o chamador cai no
 * caminho antigo) */
static WINAPI BOOL IsApiSetImplemented(const char* name) {
  (void)name;
  return 0;
}
static WINAPI BOOL QueryUnbiasedInterruptTime(uint64_t* t) {
  if (!t) return 0;
  *t = wnow_ms() * 10000ull;
  return 1;
}
static WINAPI BOOL IsProcessInJob(HANDLE p, HANDLE job, BOOL* r) {
  (void)p; (void)job;
  if (r) *r = 0;
  return 1;
}
static WINAPI long GetApplicationUserModelId(HANDLE p, DWORD* len, wchar16* id) {
  (void)p; (void)id;
  if (len) *len = 0;
  return 15703; /* APPMODEL_ERROR_NO_APPLICATION */
}
static WINAPI long UnregisterApplicationRestart(void) { return 0; }
static WINAPI HANDLE OpenFileById(HANDLE vol, void* id, DWORD acc, DWORD share, void* sa, DWORD fl) {
  (void)vol; (void)id; (void)acc; (void)share; (void)sa; (void)fl;
  wset_lasterr(50);
  return (HANDLE)(intptr_t)-1;
}
static WINAPI BOOL StartServiceW(HANDLE s, DWORD n, const wchar16** a) {
  (void)s; (void)n; (void)a;
  wset_lasterr(1060); /* SERVICE_DOES_NOT_EXIST */
  return 0;
}
static WINAPI long DwmFlush(void) { return 0; }
static WINAPI DWORD GetExtendedTcpTable(void* t, DWORD* sz, BOOL order, DWORD af, int cls, DWORD r) {
  (void)t; (void)order; (void)af; (void)cls; (void)r;
  if (sz) *sz = 0;
  return 50; /* ERROR_NOT_SUPPORTED */
}
static WINAPI DWORD WNetOpenEnumW(DWORD sc, DWORD ty, DWORD us, void* res, HANDLE* h) {
  (void)sc; (void)ty; (void)us; (void)res;
  if (h) *h = 0;
  return 1222; /* ERROR_NO_NETWORK */
}
static WINAPI DWORD WNetEnumResourceW(HANDLE h, DWORD* n, void* buf, DWORD* sz) {
  (void)h; (void)buf; (void)sz;
  if (n) *n = 0;
  return 259; /* ERROR_NO_MORE_ITEMS */
}
static WINAPI DWORD WNetCloseEnum(HANDLE h) { (void)h; return 0; }
#define NTE_NOT_SUPPORTED_ ((long)0x80090029)
static WINAPI long NCryptOpenStorageProvider(uintptr_t* p, const wchar16* name, DWORD fl) {
  (void)name; (void)fl;
  if (p) *p = 0;
  return NTE_NOT_SUPPORTED_;
}
static WINAPI long NCryptOpenKey(uintptr_t p, uintptr_t* k, const wchar16* n, DWORD spec, DWORD fl) {
  (void)p; (void)n; (void)spec; (void)fl;
  if (k) *k = 0;
  return (long)0x80090016; /* NTE_BAD_KEYSET */
}
static WINAPI long NCryptCreatePersistedKey(uintptr_t p, uintptr_t* k, const wchar16* alg, const wchar16* n, DWORD spec,
                                            DWORD fl) {
  (void)p; (void)alg; (void)n; (void)spec; (void)fl;
  if (k) *k = 0;
  return NTE_NOT_SUPPORTED_;
}
static WINAPI long NCryptFinalizeKey(uintptr_t k, DWORD fl) { (void)k; (void)fl; return NTE_NOT_SUPPORTED_; }
static WINAPI long NCryptExportKey(uintptr_t k, uintptr_t e, const wchar16* t, void* pl, void* out, DWORD n, DWORD* got,
                                   DWORD fl) {
  (void)k; (void)e; (void)t; (void)pl; (void)out; (void)n; (void)fl;
  if (got) *got = 0;
  return NTE_NOT_SUPPORTED_;
}
static WINAPI long NCryptSignHash(uintptr_t k, void* pad, const void* h, DWORD hn, void* sig, DWORD sn, DWORD* got,
                                  DWORD fl) {
  (void)k; (void)pad; (void)h; (void)hn; (void)sig; (void)sn; (void)fl;
  if (got) *got = 0;
  return NTE_NOT_SUPPORTED_;
}
static WINAPI long NCryptFreeObject(uintptr_t o) { (void)o; return 0; }
static WINAPI BOOL GetPwrCapabilities(void* c) {
  if (c) memset(c, 0, 76);
  return 1;
}
static WINAPI DWORD PowerGetActiveScheme(void* root, void** guid) {
  (void)root;
  if (guid) *guid = 0;
  return 2; /* ERROR_FILE_NOT_FOUND */
}
static WINAPI DWORD PowerReadACValueIndex(void* r, const void* s, const void* sg, const void* st, DWORD* v) {
  (void)r; (void)s; (void)sg; (void)st;
  if (v) *v = 0;
  return 2;
}
static WINAPI DWORD PowerReadDCValueIndex(void* r, const void* s, const void* sg, const void* st, DWORD* v) {
  (void)r; (void)s; (void)sg; (void)st;
  if (v) *v = 0;
  return 2;
}
#define E_NOTIMPL_ ((long)0x80004001)
static WINAPI long PropVariantToBSTR(const void* pv, void** b) { (void)pv; if (b) *b = 0; return E_NOTIMPL_; }
static WINAPI long VariantToBoolean(const void* v, BOOL* b) { (void)v; if (b) *b = 0; return E_NOTIMPL_; }
static WINAPI long VariantToPropVariant(const void* v, void* pv) { (void)v; (void)pv; return E_NOTIMPL_; }
static WINAPI long SHGetKnownFolderItem(const void* id, DWORD fl, HANDLE tok, const void* iid, void** out) {
  (void)id; (void)fl; (void)tok; (void)iid;
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI long AssocCreate(uint8_t clsid[16], const void* iid, void** out) {
  (void)clsid; (void)iid;
  if (out) *out = 0;
  return E_NOTIMPL_;
}
static WINAPI long CLSIDFromProgID(const wchar16* prog, void* clsid) {
  (void)prog;
  if (clsid) memset(clsid, 0, 16);
  return (long)0x800401F3; /* CO_E_CLASSSTRING */
}
static WINAPI long CoIncrementMTAUsage(void** cookie) {
  if (cookie) *cookie = (void*)1;
  return 0;
}
static WINAPI BOOL GetGUIThreadInfo(DWORD tid, uint8_t* info) {
  (void)tid;
  if (!info) return 0;
  {
    DWORD sz = *(DWORD*)info;
    memset(info, 0, sz >= 72 ? 72 : sz);
    *(DWORD*)info = sz;
  }
  return 1;
}
static WINAPI short VkKeyScanExW(wchar16 c, void* hkl) {
  (void)hkl;
  if (c >= 'a' && c <= 'z') return (short)(c - 32);
  if (c >= 'A' && c <= 'Z') return (short)(0x100 | c);
  if (c >= '0' && c <= '9') return (short)c;
  if (c == ' ') return 0x20;
  return -1;
}
static WINAPI HANDLE EvtQuery(HANDLE s, const wchar16* p, const wchar16* q, DWORD fl) {
  (void)s; (void)p; (void)q; (void)fl;
  wset_lasterr(15007); /* ERROR_EVT_CHANNEL_NOT_FOUND */
  return 0;
}
static WINAPI BOOL EvtNext(HANDLE r, DWORD n, HANDLE* ev, DWORD to, DWORD fl, DWORD* got) {
  (void)r; (void)n; (void)ev; (void)to; (void)fl;
  if (got) *got = 0;
  wset_lasterr(259);
  return 0;
}
static WINAPI BOOL EvtRender(HANDLE c, HANDLE f, DWORD fl, DWORD n, void* b, DWORD* used, DWORD* cnt) {
  (void)c; (void)f; (void)fl; (void)n; (void)b;
  if (used) *used = 0;
  if (cnt) *cnt = 0;
  wset_lasterr(50);
  return 0;
}
static WINAPI BOOL EvtClose(HANDLE h) { (void)h; return 1; }
static WINAPI void* WTHelperProvDataFromStateData(HANDLE s) { (void)s; return 0; }
static WINAPI void* WTHelperGetProvSignerFromChain(void* p, DWORD i, BOOL c, DWORD j) {
  (void)p; (void)i; (void)c; (void)j;
  return 0;
}
static WINAPI void* WTHelperGetProvCertFromChain(void* s, DWORD i) { (void)s; (void)i; return 0; }

/* informacao de erro COM (oleaut32 #200-202): nao ha objeto de erro */
static WINAPI long GetErrorInfo(DWORD r, void** out) {
  (void)r;
  if (out) *out = 0;
  return 1; /* S_FALSE */
}
static WINAPI long SetErrorInfo(DWORD r, void* e) {
  (void)r; (void)e;
  return 0;
}
static WINAPI long CreateErrorInfo(void** out) {
  if (out) *out = 0;
  return (long)0x80004001; /* E_NOTIMPL */
}

#define S(n) {#n, (void*)n}
const struct wine_sym fill_syms[] = {
    S(GetErrorInfo), S(SetErrorInfo), S(CreateErrorInfo),
    S(NtCreateFile), {"ZwCreateFile", (void*)NtCreateFile}, S(NtReadFile), {"ZwReadFile", (void*)NtReadFile},
    S(NtWriteFile), {"ZwWriteFile", (void*)NtWriteFile},
    S(NtOpenFile), {"ZwOpenFile", (void*)NtOpenFile}, S(NtQueryAttributesFile), S(NtClose), {"ZwClose", (void*)NtClose},
    S(NtQuerySecurityObject), S(NtSetSecurityObject), S(NtOpenDirectoryObject), S(NtFsControlFile),
    S(NtCreateNamedPipeFile), S(RtlUnwind), S(IsApiSetImplemented), S(QueryUnbiasedInterruptTime), S(IsProcessInJob),
    S(GetApplicationUserModelId), S(UnregisterApplicationRestart), S(OpenFileById), S(StartServiceW), S(DwmFlush),
    S(GetExtendedTcpTable), S(WNetOpenEnumW), S(WNetEnumResourceW), S(WNetCloseEnum), S(NCryptOpenStorageProvider),
    S(NCryptOpenKey), S(NCryptCreatePersistedKey), S(NCryptFinalizeKey), S(NCryptExportKey), S(NCryptSignHash),
    S(NCryptFreeObject), S(GetPwrCapabilities), S(PowerGetActiveScheme), S(PowerReadACValueIndex),
    S(PowerReadDCValueIndex), S(PropVariantToBSTR), S(VariantToBoolean), S(VariantToPropVariant),
    S(SHGetKnownFolderItem), S(AssocCreate), S(CLSIDFromProgID), S(CoIncrementMTAUsage), S(GetGUIThreadInfo),
    S(VkKeyScanExW), S(EvtQuery), S(EvtNext), S(EvtRender), S(EvtClose), S(WTHelperProvDataFromStateData),
    S(WTHelperGetProvSignerFromChain), S(WTHelperGetProvCertFromChain),
    S(InterlockedPushEntrySList), S(InterlockedPopEntrySList), S(InterlockedFlushSList), S(QueryDepthSList),
    S(InterlockedPushListSListEx), {"RtlInterlockedPushEntrySList", (void*)InterlockedPushEntrySList},
    {"RtlInterlockedPopEntrySList", (void*)InterlockedPopEntrySList},
    {"RtlInterlockedFlushSList", (void*)InterlockedFlushSList}, {"RtlQueryDepthSList", (void*)QueryDepthSList},
    S(WSALookupServiceBeginW), S(WSALookupServiceNextW), S(WSALookupServiceEnd), S(WSANSPIoctl),
    S(ReadDirectoryChangesW), S(CancelSynchronousIo), S(LockFile), S(UnlockFile), S(SetFileInformationByHandle),
    S(GetVolumePathNameW), S(ReplaceFileW),
    S(NotifyAddrChange), S(CancelIPChangeNotify), S(NotifyIpInterfaceChange), S(CancelMibChangeNotify2),
    S(ConvertInterfaceIndexToLuid), S(ConvertInterfaceLuidToNameW), S(GetBestRoute2), S(GetInterfaceInfo),
    S(if_indextoname), S(if_nametoindex),
    S(GetDoubleClickTime), S(GetMessagePos), S(GetMessageTime), S(GetMessageExtraInfo), S(GetKeyboardLayout),
    S(GetKeyboardLayoutList), S(GetKeyboardLayoutNameW), S(GetKeyboardState), S(SetKeyboardState),
    S(GetLastInputInfo), S(TrackMouseEvent), S(RegisterRawInputDevices), S(GetRawInputDeviceList),
    S(GetRawInputData), S(GetRawInputDeviceInfoW), S(DefRawInputProc), S(RegisterPowerSettingNotification),
    S(UnregisterPowerSettingNotification), S(RegisterDeviceNotificationW), S(UnregisterDeviceNotification),
    S(NotifyWinEvent), S(AddClipboardFormatListener), S(RemoveClipboardFormatListener),
    S(AllowSetForegroundWindow), S(SetLayeredWindowAttributes), S(GetLayeredWindowAttributes),
    S(UpdateLayeredWindow), S(SetWindowRgn), S(GetWindowRgn), S(SetWindowDisplayAffinity),
    S(GetWindowDisplayAffinity), S(IsWindowArranged), S(MapVirtualKeyExW), S(MapVirtualKeyW), S(ToUnicodeEx),
    S(GetPointerType), S(GetPointerDevices), S(GetPointerDevice), S(GetPointerPenInfo),
    S(RegisterPointerDeviceNotifications), S(EnumThreadWindows), S(CopyIcon), S(GetCursorInfo), S(GetGuiResources),
    S(GetDisplayConfigBufferSizes), S(QueryDisplayConfig), S(DisplayConfigGetDeviceInfo), S(EnumDisplayDevicesW),
    S(EnumDisplaySettingsExW), S(EnumDisplaySettingsW), S(MonitorFromRect), S(GetMonitorInfoA),
    {"CharUpperA", (void*)CharUpperA_}, S(RegisterHotKey), S(UnregisterHotKey), S(SetCursorPos), S(ClipCursor),
    S(CloseTouchInputHandle), S(WindowFromDC), S(InvertRect), S(DrawFrameControl), S(PrintWindow),
    S(GetClipboardFormatNameW), S(UnregisterClassA),
    S(GetDpiForMonitor), S(GetScaleFactorForMonitor), S(DwmDefWindowProc), S(DwmExtendFrameIntoClientArea),
    S(DwmGetCompositionTimingInfo), S(QueryUnbiasedInterruptTimePrecise), S(CompareObjectHandles),
    S(CallNtPowerInformation), S(PowerDeterminePlatformRoleEx), S(WTSRegisterSessionNotification),
    S(WTSUnRegisterSessionNotification), S(WTSQuerySessionInformationW), S(WTSFreeMemory),
    S(PropVariantClear), S(PropVariantCopy), S(CoRegisterInitializeSpy), S(CoRevokeInitializeSpy),
    S(RoGetAgileReference), S(SetCurrentProcessExplicitAppUserModelID), S(GetCurrentProcessExplicitAppUserModelID),
    S(SHGetPropertyStoreForWindow), S(PathFindFileNameW), S(PathFindExtensionW), S(PathRemoveExtensionW),
    S(PathMatchSpecW), S(IsOS), S(CreateEnvironmentBlock), S(DestroyEnvironmentBlock),
    S(CreateDXGIFactory), S(CreateDXGIFactory1), S(CreateDXGIFactory2), S(DXGIGetDebugInterface1),
    S(D3D11CreateDevice), S(DCompositionCreateDevice), S(DCompositionCreateDevice2), S(DCompositionCreateDevice3),
    S(DCompositionCreateSurfaceHandle), S(ReadProcessMemory), S(WriteProcessMemory), S(AccessibleObjectFromWindow), S(AccessibleChildren),
    S(CreateStdAccessibleObject), S(LresultFromObject), S(WindowFromAccessibleObject), S(UiaReturnRawElementProvider),
    S(UiaHostProviderFromHwnd), S(UiaDisconnectProvider), S(UiaDisconnectAllProviders), S(UiaRaiseAutomationEvent),
    S(UiaRaiseAutomationPropertyChangedEvent), S(UiaRaiseStructureChangedEvent), S(UiaGetReservedMixedAttributeValue),
    S(UiaGetReservedNotSupportedValue), S(InitPropVariantFromCLSID), S(PSCreateMemoryPropertyStore), S(VariantCompare), S(CertAddStoreToCollection), S(CertControlStore),
    S(CertAddEncodedCertificateToStore), S(CertCompareCertificateName), S(CertFindChainInStore),
    S(CertGetCertificateContextProperty), S(CertGetEnhancedKeyUsage), S(CertGetIntendedKeyUsage),
    S(CertVerifyTimeValidity), S(CryptAcquireCertificatePrivateKey), S(CryptVerifyCertificateSignatureEx), S(NtQueryObject), {"ZwQueryObject", (void*)NtQueryObject}, S(DisableThreadLibraryCalls), S(HeapQueryInformation), S(UuidCreate),
    {0, 0}};
