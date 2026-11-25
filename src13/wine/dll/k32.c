/*
 * k32.c - KERNEL32/KERNELBASE, NTDLL e USER32 embutidos (W0: console).
 *
 * O minimo que o CRT do MinGW/zig e programas de console pedem. O que nao
 * esta aqui vira stub (pe.c) e so derruba o programa se for chamado.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"


#define H_STDIN ((HANDLE)(uintptr_t)0x10)
#define H_STDOUT ((HANDLE)(uintptr_t)0x14)
#define H_STDERR ((HANDLE)(uintptr_t)0x18)
#define H_PROCESS ((HANDLE)(intptr_t)-1)
#define H_THREAD ((HANDLE)(intptr_t)-2)
#define H_HEAP ((HANDLE)(uintptr_t)1)


/* ---------- processo / modulos ---------- */
static WINAPI __attribute__((noreturn)) void ExitProcess(DWORD code) {
  if (wine_debug >= 2) {
    uint64_t here;
    wwait_note("ExitProcess", __builtin_return_address(0), code);
    wstack_note("ExitProcess", &here);
  }
  wexit((int)code);
}
static WINAPI BOOL TerminateProcess(HANDLE h, DWORD code) {
  if (wh_is_self(h)) {
    if (wine_debug >= 2 || code >= 0xC0000000u) {
      uint64_t here;
      wwait_note("TerminateProcess", __builtin_return_address(0), code);
      wstack_note("TerminateProcess", &here);
    }
    wterminate((int)code);
  }
  return 0;
}
static WINAPI HANDLE GetCurrentProcess(void) { return H_PROCESS; }
static WINAPI HANDLE GetCurrentThread(void) { return H_THREAD; }
static WINAPI DWORD GetCurrentProcessId(void) { return (DWORD) * (uint64_t*)(wteb() + TEB_PID); }
static WINAPI DWORD GetCurrentThreadId(void) { return (DWORD) * (uint64_t*)(wteb() + TEB_TID); }
static WINAPI DWORD GetLastError(void) { return wget_lasterr(); }
static WINAPI void SetLastError(DWORD e) { wset_lasterr(e); }
static WINAPI char* GetCommandLineA(void) { return wp()->cmdline; }
static WINAPI wchar16* GetCommandLineW(void) { return wp()->wcmdline; }

static WINAPI HANDLE GetModuleHandleA(const char* name) {
  struct wmod* m;
  if (!name) return wp()->exe ? wp()->exe->base : 0;
  m = pe_find(name);
  if (!m) {
    /* kernel32, ntdll... "sempre carregadas" no Windows: as embutidas sim */
    char n[WMOD_NAME];
    char* dot;
    wbase(n, name, (int)sizeof(n));
    dot = strrchr(n, '.');
    if (dot && wstreq(dot, ".dll")) *dot = 0;
    if (builtin_known(n)) m = pe_load(name, 0);
  }
  if (!m) wset_lasterr(ERROR_MOD_NOT_FOUND);
  return m ? pe_handle(m) : 0;
}
static WINAPI HANDLE GetModuleHandleW(const wchar16* name) {
  char* n;
  HANDLE h;
  if (!name) return GetModuleHandleA(0);
  n = wdup16to8(name);
  h = GetModuleHandleA(n);
  k_free(n);
  return h;
}
static WINAPI BOOL GetModuleHandleExW(DWORD flags, const wchar16* name, HANDLE* out) {
  if (flags & 4) { /* FROM_ADDRESS */
    struct wmod* m;
    uintptr_t a = (uintptr_t)name;
    for (m = wp()->mods; m; m = m->next)
      if (m->base && a >= (uintptr_t)m->base && a < (uintptr_t)m->base + m->size) {
        *out = m->base;
        return 1;
      }
    *out = 0;
    return 0;
  }
  *out = GetModuleHandleW(name);
  return *out != 0;
}
/* caller: endereco de retorno no programa. DLL que nao existe, pedida pelo
 * auxiliar de carga atrasada de quem chamou (o nome passado e o da tabela
 * atrasada dele, o mesmo ponteiro; so no LoadLibrary(Ex)A): modulo vazio em
 * vez de NULL, porque o Chrome derruba o processo quando a carga atrasada
 * falha (pdh.dll um minuto depois de abrir). Quem testa uma DLL opcional por
 * conta propria (mf.dll no GPU) continua recebendo NULL. */
static HANDLE load_lib(const char* name, void* caller, int ansi) {
  struct wmod* m = pe_load(name, 0);
  if (!m && name && ansi) {
    struct wmod* who = pe_from_addr((uint64_t)(uintptr_t)caller);
    if (who && !who->builtin && pe_delay_name_is(who, name)) {
      m = pe_load_phantom(name);
      if (m) wlog("carga atrasada: DLL que o WINE nao tem, modulo vazio: ", name);
    }
  }
  if (!m) {
    wlog("LoadLibrary: DLL nao encontrada: ", name ? name : "(nulo)");
    wset_lasterr(ERROR_MOD_NOT_FOUND);
    return 0;
  }
  return pe_handle(m);
}
static HANDLE load_lib16(const wchar16* name, void* caller) {
  char* n = wdup16to8(name);
  HANDLE h = n ? load_lib(n, caller, 0) : 0;
  k_free(n);
  return h;
}
static WINAPI HANDLE LoadLibraryA(const char* name) { return load_lib(name, __builtin_return_address(0), 1); }
static WINAPI HANDLE LoadLibraryW(const wchar16* name) { return load_lib16(name, __builtin_return_address(0)); }
static WINAPI HANDLE LoadLibraryExA(const char* name, HANDLE f, DWORD fl) {
  (void)f;
  (void)fl;
  return load_lib(name, __builtin_return_address(0), 1);
}
static WINAPI HANDLE LoadLibraryExW(const wchar16* name, HANDLE f, DWORD fl) {
  (void)f;
  (void)fl;
  return load_lib16(name, __builtin_return_address(0));
}
static WINAPI BOOL FreeLibrary(HANDLE h) {
  (void)h;
  return 1; /* descarrega tudo no fim do processo */
}
/* Nt e Zw que a ntdll daqui nao tem: o programa recebe uma funcao que responde
 * STATUS_NOT_IMPLEMENTED (como uma ntdll mais antiga), em vez de NULL. O
 * Chromium confere dezenas delas no inicio e para no primeiro NULL. */
static WINAPI long nt_notimpl(const char* name) {
  wlog("ntdll sem implementacao (STATUS_NOT_IMPLEMENTED): ", name);
  return (long)0xC0000002;
}
/* kernel32/user32/advapi32/gdi32: o que falta devolve 0 (FALSE/NULL) com
 * ERROR_CALL_NOT_IMPLEMENTED, como uma versao antiga do Windows sem a funcao */
/* Funcoes que nao temos cujo "falhou" nao e 0: HRESULT (0 = S_OK), codigos de
 * erro (0 = ERROR_SUCCESS), NTSTATUS e handles (INVALID_HANDLE_VALUE). Com 0 o
 * programa entendia sucesso e usava a saida nao preenchida: o Chrome caia
 * logo depois de SHCreateAssociationRegistration. O resto (BOOL, ponteiros,
 * contagens, void) devolve 0. Nomes tirados das importacoes do Chrome. */
#define RV_ENOTIMPL 0x80004001u    /* E_NOTIMPL */
#define RV_ERR 120u                /* ERROR_CALL_NOT_IMPLEMENTED */
#define RV_NTSTATUS 0xC0000002u    /* STATUS_NOT_IMPLEMENTED */
#define RV_SEC 0x80090302u         /* SEC_E_UNSUPPORTED_FUNCTION */
#define RV_INVALID ((uintptr_t)-1) /* INVALID_HANDLE_VALUE, e -1 dos que devolvem int */
static const struct {
  const char* name;
  uintptr_t rv;
} notimpl_rv[] = {
    /* HRESULT: shell32, propsys, uxtheme, comdlg32, ndfapi, ordinais */
    {"SHCreateAssociationRegistration", RV_ENOTIMPL}, {"SHOpenFolderAndSelectItems", RV_ENOTIMPL},
    {"SHOpenWithDialog", RV_ENOTIMPL}, {"SHParseDisplayName", RV_ENOTIMPL},
    {"SHQueryUserNotificationState", RV_ENOTIMPL}, {"InitPropVariantFromStringVector", RV_ENOTIMPL},
    {"PSGetPropertyKeyFromName", RV_ENOTIMPL}, {"GetThemeBackgroundContentRect", RV_ENOTIMPL},
    {"PrintDlgExW", RV_ENOTIMPL}, {"NdfCreateWebIncident", RV_ENOTIMPL}, {"NdfExecuteDiagnosis", RV_ENOTIMPL},
    {"NdfCloseIncident", RV_ENOTIMPL}, {"shlwapi.dll#219", 0x80004002u /* QISearch: E_NOINTERFACE */},
    {"mmdevapi.dll#17", RV_ENOTIMPL}, {"d3d12.dll#101", RV_ENOTIMPL},
    /* HRESULT: ole32 */
    {"CoAllowSetForegroundWindow", RV_ENOTIMPL}, {"CoCreateFreeThreadedMarshaler", RV_ENOTIMPL},
    {"CoGetClassObject", RV_ENOTIMPL},
    {"CoGetInterfaceAndReleaseStream", RV_ENOTIMPL}, {"CoMarshalInterThreadInterfaceInStream", RV_ENOTIMPL},
    {"CoRegisterMessageFilter", RV_ENOTIMPL}, {"CreateILockBytesOnHGlobal", RV_ENOTIMPL},
    {"GetHGlobalFromILockBytes", RV_ENOTIMPL}, {"GetHGlobalFromStream", RV_ENOTIMPL},
    {"StgCreateDocfileOnILockBytes", RV_ENOTIMPL}, {"StringFromCLSID", RV_ENOTIMPL},
    /* codigos de erro do Win32 */
    {"RegQueryValueExA", RV_ERR}, {"GetAdaptersInfo", RV_ERR}, {"GetIfTable2", RV_ERR},
    {"IpReleaseAddress", RV_ERR}, {"IpRenewAddress", RV_ERR}, {"WinHttpCreateProxyResolver", RV_ERR},
    {"WinHttpGetProxyForUrlEx", RV_ERR}, {"WinHttpGetProxyResult", RV_ERR}, {"PowerReadACValue", RV_ERR},
    {"PowerReadDCValue", RV_ERR}, {"BluetoothGetRadioInfo", RV_ERR}, {"BluetoothSdpGetAttributeValue", RV_ERR},
    {"BluetoothSdpGetContainerElementData", RV_ERR}, {"CredUIPromptForWindowsCredentialsW", 1223u /* cancelado */},
    /* CONFIGRET: CR_FAILURE */
    {"CM_Get_DevNode_PropertyW", 0x13u}, {"CM_Get_Device_IDW", 0x13u}, {"CM_Get_Parent", 0x13u},
    {"CM_Locate_DevNodeW", 0x13u},
    /* NTSTATUS e SECURITY_STATUS */
    {"BCryptOpenAlgorithmProvider", RV_NTSTATUS}, {"LsaConnectUntrusted", RV_NTSTATUS}, {"LsaLogonUser", RV_NTSTATUS},
    {"AcquireCredentialsHandleW", RV_SEC}, {"InitializeSecurityContextW", RV_SEC},
    {"QueryContextAttributesW", RV_SEC}, {"QuerySecurityPackageInfoW", RV_SEC},
    /* handles e int: falha = -1 */
    {"SetupDiGetClassDevsW", RV_INVALID}, {"SetupDiCreateDeviceInfoList", RV_INVALID},
    {"SetupDiOpenDevRegKey", RV_INVALID}, {"FindFirstFileExA", RV_INVALID}, {"FindFirstVolumeW", RV_INVALID},
    {"DocumentPropertiesW", RV_INVALID}, {"DeviceCapabilitiesW", RV_INVALID},
    {0, 0}};
static uintptr_t notimpl_value(const char* n) {
  int i;
  for (i = 0; notimpl_rv[i].name; i++)
    if (strcmp(n, notimpl_rv[i].name) == 0) return notimpl_rv[i].rv;
  /* familias: Media Foundation (HRESULT), ESENT (JET_ERR < 0), HidP (NTSTATUS), NCrypt */
  if (n[0] == 'M' && n[1] == 'F' && strcmp(n, "MFGetSystemTime") != 0) return RV_ENOTIMPL;
  if (strncmp(n, "Jet", 3) == 0) return RV_INVALID;
  if (strncmp(n, "HidP_", 5) == 0) return 0xC0110020u; /* HIDP_STATUS_NOT_IMPLEMENTED */
  if (strncmp(n, "NCrypt", 6) == 0) return 0x80090029u; /* NTE_NOT_SUPPORTED */
  return 0;
}
static WINAPI uintptr_t api_notimpl(const char* name) {
  uintptr_t rv = notimpl_value(name);
  wlog(rv ? "API sem implementacao (devolve erro): " : "API sem implementacao (devolve 0): ", name);
  wset_lasterr(120); /* ERROR_CALL_NOT_IMPLEMENTED */
  return rv;
}
static void* gen_stub(const char* name, void* target) {
  /* 48 B9 imm64  mov rcx, nome ; 48 B8 imm64  mov rax, alvo ; FF E0  jmp rax */
  size_t n = strlen(name);
  uint8_t* p = (uint8_t*)k_malloc((unsigned)(24 + n + 1));
  uint64_t v;
  if (!p) return 0;
  memcpy(p + 24, name, n + 1);
  p[0] = 0x48;
  p[1] = 0xB9;
  v = (uint64_t)(uintptr_t)(p + 24);
  memcpy(p + 2, &v, 8);
  p[10] = 0x48;
  p[11] = 0xB8;
  v = (uint64_t)(uintptr_t)target;
  memcpy(p + 12, &v, 8);
  p[20] = 0xFF;
  p[21] = 0xE0;
  return p;
}
/* funcoes que o programa usa "se existir" e cuja ausencia ele sabe tratar:
 * ficam ausentes (NULL), senao o "devolve 0" vira um ponteiro NULL usado */
static int keep_absent(const char* n) {
  static const char* const absent[] = {"VirtualAlloc2", "VirtualAlloc2FromApp", "MapViewOfFile3",
                                       "MapViewOfFile3FromApp", "UnmapViewOfFile2", "MapViewOfFileNuma2", 0};
  int i;
  for (i = 0; absent[i]; i++)
    if (strcmp(n, absent[i]) == 0) return 1;
  return 0;
}
static int api_dll(const char* m) {
  return strncmp(m, "kernel32", 8) == 0 || strncmp(m, "kernelbase", 10) == 0 || strncmp(m, "user32", 6) == 0 ||
         strncmp(m, "advapi32", 8) == 0 || strncmp(m, "gdi32", 5) == 0;
}
/* pe.c: import estatico que falta numa DLL de sistema vira "devolve 0" (como
 * no GetProcAddress) em vez do stub que encerra o processo; o Claude Code
 * (Bun) importa dezenas que nao usa na partida. 0 = nao e DLL de sistema. */
void* wine_api_stub(const char* dll, const char* name) {
  char low[24];
  wlower(low, dll, (int)sizeof(low));
  if (!api_dll(low) || (uintptr_t)name <= 0xFFFF) return 0;
  return gen_stub(name, (void*)api_notimpl);
}

/* Nt* opcionais: quem pede trata NULL (o Go 1.25 so usa os "wait completion
 * packets" se existirem; com o stub ele parava em "NtCreateWaitCompletionPacket
 * failed") */
static int nt_absent(const char* n) {
  static const char* const absent[] = {"NtCreateWaitCompletionPacket", "NtAssociateWaitCompletionPacket",
                                       "NtCancelWaitCompletionPacket", 0};
  int i;
  for (i = 0; absent[i]; i++)
    if (strcmp(n, absent[i]) == 0) return 1;
  return 0;
}

static void* nt_stub(const char* name) {
  /* 48 B9 imm64  mov rcx, nome ; 48 B8 imm64  mov rax, nt_notimpl ; FF E0  jmp rax */
  size_t n = strlen(name);
  uint8_t* p = (uint8_t*)k_malloc((unsigned)(24 + n + 1));
  uint64_t v;
  if (!p) return 0;
  memcpy(p + 24, name, n + 1);
  p[0] = 0x48;
  p[1] = 0xB9;
  v = (uint64_t)(uintptr_t)(p + 24);
  memcpy(p + 2, &v, 8);
  p[10] = 0x48;
  p[11] = 0xB8;
  v = (uint64_t)(uintptr_t)nt_notimpl;
  memcpy(p + 12, &v, 8);
  p[20] = 0xFF;
  p[21] = 0xE0;
  return p;
}

static WINAPI void* GetProcAddress(HANDLE h, const char* name) {
  struct wmod* m = pe_from_handle(h);
  void* f = m ? pe_getproc(m, name) : 0;
  if (!f && m && (uintptr_t)name > 0xFFFF && strncmp(m->name, "ntdll", 5) == 0 &&
      ((name[0] == 'N' && name[1] == 't') || (name[0] == 'Z' && name[1] == 'w')) && name[2] >= 'A' &&
      name[2] <= 'Z' && !nt_absent(name)) {
    f = nt_stub(name);
    if (f && wine_debug) wlog("GetProcAddress: Nt* sem implementacao (responde NOT_IMPLEMENTED): ", name);
  }
  if (!f && m && (uintptr_t)name > 0xFFFF && api_dll(m->name) && !keep_absent(name)) {
    f = gen_stub(name, (void*)api_notimpl);
    if (f && wine_debug) wlog("GetProcAddress: sem implementacao (devolve 0): ", name);
  }
  /* Carga atrasada: o __delayLoadHelper2 do programa pede a uma DLL embutida
   * uma funcao que nao temos. Com NULL o auxiliar lanca excecao, e o Chrome
   * derruba o processo de proposito ("Failed to delay load": int3 no
   * chrome.dll, depois de NetGetJoinInformation e uxtheme #47). So para o que
   * esta na tabela atrasada de quem chamou: o stub devolve 0 e registra. */
  if (!f && m && m->builtin) {
    struct wmod* who = pe_from_addr((uint64_t)(uintptr_t)__builtin_return_address(0));
    if (who && !who->builtin && pe_delay_has(who, m->name, name)) {
      char nm[WMOD_NAME + 12];
      const char* sn = name;
      if ((uintptr_t)name <= 0xFFFF) { /* ordinal: "uxtheme.dll#47" */
        unsigned o = (unsigned)(uintptr_t)name;
        char num[8];
        int k = 0, n;
        strcpy(nm, m->name);
        strcat(nm, "#");
        n = (int)strlen(nm);
        do {
          num[k++] = (char)('0' + o % 10);
          o /= 10;
        } while (o);
        while (k) nm[n++] = num[--k];
        nm[n] = 0;
        sn = nm;
      }
      f = gen_stub(sn, (void*)api_notimpl);
      if (f) wlog("carga atrasada sem implementacao (stub devolve 0): ", sn);
    }
  }
  if (!f) {
    if (wine_debug && (uintptr_t)name > 0xFFFF) wlog("GetProcAddress: falta ", name);
    if ((uintptr_t)name <= 0xFFFF) {
      wlog("GetProcAddress por ordinal falhou, modulo ", m ? m->name : "(handle desconhecido)");
      wlogx("  ordinal ", (uint64_t)(uintptr_t)name);
    } else if (!m) {
      wlog("GetProcAddress em handle desconhecido: ", name);
    }
    wset_lasterr(ERROR_PROC_NOT_FOUND);
  }
  return f;
}
static WINAPI DWORD GetModuleFileNameA(HANDLE h, char* buf, DWORD n) {
  struct wmod* m = pe_from_handle(h);
  const char* s = m ? (m->path[0] ? m->path : m->name) : "";
  DWORD len = (DWORD)strlen(s);
  if (!n) return 0;
  if (len >= n) {
    memcpy(buf, s, n - 1);
    buf[n - 1] = 0;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return n;
  }
  memcpy(buf, s, len + 1);
  return len;
}
static WINAPI DWORD GetModuleFileNameW(HANDLE h, wchar16* buf, DWORD n) {
  char tmp[260];
  DWORD len = GetModuleFileNameA(h, tmp, sizeof(tmp));
  int w;
  if (!n) return 0;
  w = wutf8_to_16(tmp, (int)len, buf, (int)n - 1);
  buf[w] = 0;
  return (DWORD)w;
}

/* ---------- console / arquivos (W0: so console) ---------- */
static WINAPI HANDLE GetStdHandle(DWORD which) {
  struct wproc* p = wp();
  if (which >= (DWORD)-12 && p->std[(DWORD)-10 - which]) return p->std[(DWORD)-10 - which];
  switch (which) {
    case (DWORD)-10: return H_STDIN;
    case (DWORD)-11: return H_STDOUT;
    case (DWORD)-12: return H_STDERR;
  }
  wset_lasterr(ERROR_INVALID_HANDLE);
  return (HANDLE)(intptr_t)-1;
}
static WINAPI BOOL GetConsoleMode(HANDLE h, DWORD* mode) {
  if (h != H_STDIN && h != H_STDOUT && h != H_STDERR) return 0;
  if (mode) *mode = 3;
  return 1;
}
static WINAPI BOOL SetConsoleMode(HANDLE h, DWORD mode) {
  (void)h;
  (void)mode;
  return 1;
}
static WINAPI DWORD GetConsoleCP(void) { return 65001; }
static WINAPI DWORD GetConsoleOutputCP(void) { return 65001; }
static WINAPI BOOL SetConsoleOutputCP(DWORD cp) {
  (void)cp;
  return 1;
}
static WINAPI BOOL SetConsoleCtrlHandler(void* fn, BOOL add) {
  (void)fn;
  (void)add;
  return 1;
}
/* ---------- memoria ---------- */
static WINAPI HANDLE GetProcessHeap(void) { return H_HEAP; }
static WINAPI HANDLE HeapCreate(DWORD o, size_t a, size_t b) {
  (void)o;
  (void)a;
  (void)b;
  return H_HEAP;
}
static WINAPI BOOL HeapDestroy(HANDLE h) {
  (void)h;
  return 1;
}
static WINAPI void* HeapAlloc(HANDLE h, DWORD fl, size_t n) {
  void* p;
  (void)h;
  p = wheap_alloc(n);
  if (p && (fl & 8)) memset(p, 0, n ? n : 1);
  if (!p) wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
  return p;
}
static WINAPI BOOL HeapFree(HANDLE h, DWORD fl, void* p) {
  (void)h;
  (void)fl;
  wheap_free(p);
  return 1;
}
static WINAPI void* HeapReAlloc(HANDLE h, DWORD fl, void* p, size_t n) {
  size_t old = p ? wheap_size(p) : 0;
  void* q;
  (void)h;
  q = wheap_realloc(p, n);
  if (q && (fl & 8) && n > old) memset((char*)q + old, 0, n - old);
  if (!q) wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
  return q;
}
static WINAPI size_t HeapSize(HANDLE h, DWORD fl, const void* p) {
  (void)h;
  (void)fl;
  return wheap_size((void*)p);
}
static WINAPI void* LocalAlloc(DWORD fl, size_t n) { return HeapAlloc(H_HEAP, (fl & 0x40) ? 8 : 0, n); }
static WINAPI void* LocalFree(void* p) {
  wheap_free(p);
  return 0;
}

/* ---------- sincronizacao ---------- */
struct critsec {
  void* debug;
  volatile int lock;      /* LockCount: palavra do futex */
  int recursion;
  volatile uintptr_t owner;
  HANDLE sem;
  uintptr_t spin;
};
static WINAPI void InitializeCriticalSection(struct critsec* c) { memset(c, 0, sizeof(*c)); }
static WINAPI BOOL InitializeCriticalSectionAndSpinCount(struct critsec* c, DWORD s) {
  InitializeCriticalSection(c);
  c->spin = s;
  return 1;
}
static WINAPI BOOL InitializeCriticalSectionEx(struct critsec* c, DWORD s, DWORD f) {
  (void)f;
  return InitializeCriticalSectionAndSpinCount(c, s);
}
static WINAPI void DeleteCriticalSection(struct critsec* c) { (void)c; }
static WINAPI BOOL TryEnterCriticalSection(struct critsec* c) {
  uintptr_t me = (uintptr_t)wteb();
  if (c->owner == me) {
    c->recursion++;
    return 1;
  }
  if (__sync_val_compare_and_swap(&c->lock, 0, 1) != 0) return 0;
  c->owner = me;
  c->recursion = 1;
  return 1;
}
static WINAPI void EnterCriticalSection(struct critsec* c) {
  uintptr_t me = (uintptr_t)wteb();
  if (c->owner == me) {
    c->recursion++;
    return;
  }
  /* mutex de futex: 0 livre, 1 travado, 2 travado com alguem esperando.
   * Quem acorda pega com 2 (pode haver outro esperando), senao o Leave nao
   * acorda ninguem e a outra thread dorme para sempre. */
  {
    int v = __sync_val_compare_and_swap(&c->lock, 0, 1);
    if (v != 0) {
      if (v != 2) v = __sync_lock_test_and_set(&c->lock, 2);
      uint64_t t0 = wnow_ms();
      int noted = 0;
      if (v != 0) wwait_begin("secao critica", c, (uint64_t)c->owner, 0, __builtin_return_address(0));
      while (v != 0) {
        if (wine_debug >= 2) {
          k_futex_wait(&c->lock, 2, 5000);
          if (!noted && wnow_ms() - t0 > 5000) {
            noted = 1;
            wwait_note("secao critica", __builtin_return_address(0), wnow_ms() - t0);
          }
        } else {
          k_futex_wait(&c->lock, 2, -1);
        }
        v = __sync_lock_test_and_set(&c->lock, 2);
      }
      wwait_end();
    }
  }
  c->owner = me;
  c->recursion = 1;
}
static WINAPI void LeaveCriticalSection(struct critsec* c) {
  if (--c->recursion > 0) return;
  c->owner = 0;
  if (__sync_lock_test_and_set(&c->lock, 0) == 2) k_futex_wake(&c->lock, 1);
}
static WINAPI void InitializeSRWLock(void** l) { *l = 0; }
/* SRW: -1 = escritor, n > 0 = n leitores, 0 = livre. Espera girando com
 * yield (as secoes sao curtas). */
/* WINEDEBUG=2: quem tem cada SRW exclusivo (thread e endereco de retorno),
 * para achar o dono quando alguem gira mais de 5 s. Tabela pequena por
 * hash do endereco; colisao so perde a informacao. */
#define SRW_TRACK 512
static struct {
  void* lock;
  DWORD tid;
  void* caller;
} srw_owner[SRW_TRACK];
static unsigned srw_slot(void* l) { return (unsigned)(((uintptr_t)l >> 3) * 2654435761u) % SRW_TRACK; }
void wsrw_note(void* l, void* caller) {
  struct wobj* t;
  unsigned i;
  if (wine_debug < 2) return;
  t = wthread();
  i = srw_slot(l);
  srw_owner[i].lock = l;
  srw_owner[i].tid = t ? t->tid : 0;
  srw_owner[i].caller = caller;
}
void wsrw_report(void* l, void* caller) {
  unsigned i = srw_slot(l);
  wwait_note("SRW exclusivo", caller, 5000);
  wstack_note("SRW exclusivo, pilha de quem espera:", (const uint64_t*)__builtin_frame_address(0));
  wlogx("  trava ", (uint64_t)(uintptr_t)l);
  wlogx("  valor ", (uint64_t)*(volatile intptr_t*)l);
  if (srw_owner[i].lock == l) {
    wlogx("  dono: thread ", srw_owner[i].tid);
    wwait_note("  dono pegou em", srw_owner[i].caller, 0);
  }
}
/* Quem espera dorme no futex da propria trava (os 32 bits baixos) em vez de
 * girar: com varias threads disputando a trava do PartitionAlloc, girar com
 * yield deixava o dono sem CPU e a thread principal esperava minutos.
 * srw_waiters conta quem dorme, para a liberacao so acordar se preciso. */
volatile int srw_waiters;
void wsrw_wait(volatile intptr_t* v, intptr_t seen) {
  __sync_fetch_and_add(&srw_waiters, 1);
  if (*v == seen) k_futex_wait((volatile int*)v, (int)seen, 2); /* 2 ms: sem perder acordar */
  __sync_fetch_and_sub(&srw_waiters, 1);
}
void wsrw_wake(volatile intptr_t* v) {
  if (srw_waiters) k_futex_wake((volatile int*)v, -1);
}
static WINAPI void AcquireSRWLockExclusive(void** l) {
  volatile intptr_t* v = (volatile intptr_t*)l;
  if (__sync_val_compare_and_swap(v, 0, -1) != 0) {
    uint64_t t0 = wnow_ms();
    int noted = 0, spins = 0;
    wwait_begin("SRW exclusivo", l, 0, 0, __builtin_return_address(0));
    for (;;) {
      intptr_t cur = *v;
      if (cur == 0 && __sync_val_compare_and_swap(v, 0, -1) == 0) break;
      if (wine_debug >= 2 && !noted && wnow_ms() - t0 > 5000) {
        noted = 1;
        wsrw_report(l, __builtin_return_address(0));
      }
      if (++spins < 64) {
        __builtin_ia32_pause();
        continue;
      }
      wsrw_wait(v, cur);
    }
    wwait_end();
  }
  wsrw_note(l, __builtin_return_address(0));
}
static WINAPI void ReleaseSRWLockExclusive(void** l) {
  __sync_lock_release((volatile intptr_t*)l);
  wsrw_wake((volatile intptr_t*)l);
}

/* TLS do Windows: TEB->TlsSlots[64]. */
static WINAPI DWORD TlsAlloc(void) {
  struct wproc* p = wp();
  DWORD i;
  wlock();
  for (i = 0; i < TLS_SLOTS; i++)
    if (!p->tls_used[i]) {
      p->tls_used[i] = 1;
      wunlock();
      ((void**)(wteb() + TEB_TLSSLOTS))[i] = 0;
      return i;
    }
  wunlock();
  return 0xFFFFFFFFu;
}
static WINAPI BOOL TlsFree(DWORD i) {
  if (i >= TLS_SLOTS) return 0;
  wp()->tls_used[i] = 0;
  return 1;
}
static WINAPI void* TlsGetValue(DWORD i) {
  wset_lasterr(0);
  return i < TLS_SLOTS ? ((void**)(wteb() + TEB_TLSSLOTS))[i] : 0;
}
static WINAPI BOOL TlsSetValue(DWORD i, void* v) {
  if (i >= TLS_SLOTS) return 0;
  ((void**)(wteb() + TEB_TLSSLOTS))[i] = v;
  return 1;
}

static WINAPI DWORD GetTickCount(void) { return k_ms(); }
static WINAPI uint64_t GetTickCount64(void) { return k_ms(); }
static WINAPI BOOL QueryPerformanceFrequency(int64_t* f) {
  *f = 1000;
  return 1;
}
static WINAPI BOOL QueryPerformanceCounter(int64_t* c) {
  *c = k_ms();
  return 1;
}
/* FILETIME: 100 ns desde 1601-01-01 (UTC). */
static uint64_t days_from_civil(int y, unsigned m, unsigned d) {
  int era;
  unsigned yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint64_t)(era * 146097 + (int)doe - 719468); /* dias desde 1970 */
}
uint64_t wfiletime_now(void);
static WINAPI void GetSystemTimeAsFileTime(uint64_t* ft) { *ft = wfiletime_now(); }
uint64_t wfiletime_now(void) {
  uint64_t v;
  uint64_t* ft = &v;
  int d = k_date(), t = k_time();
  uint64_t secs;
  if (d <= 0) d = 20260101;
  if (t < 0) t = 0;
  secs = days_from_civil(d / 10000, (unsigned)(d / 100 % 100), (unsigned)(d % 100)) * 86400u +
         (uint64_t)(t / 10000) * 3600u + (uint64_t)(t / 100 % 100) * 60u + (uint64_t)(t % 100);
  *ft = (secs + 11644473600ull) * 10000000ull + (uint64_t)(k_ms() % 1000) * 10000u;
  return v;
}
static WINAPI void GetSystemTimePreciseAsFileTime(uint64_t* ft) { GetSystemTimeAsFileTime(ft); }

/* ---------- ambiente / sistema ---------- */
/* Ambiente do processo Windows: um conjunto fixo (pastas dentro de D:\WINE)
 * e, antes dele, as variaveis do SODOS (SET), para GODEBUG, TEMP proprio... */
const char* wine_dir(const char* rel);
struct env_fix {
  const char* name;
  const char* val;
  int dir;   /* 1 = val e uma pasta relativa a D:\WINE */
};
static const struct env_fix env_fixed[] = {
    {"SystemDrive", "D:", 0},
    {"SystemRoot", "", 1},
    {"windir", "", 1},
    {"TEMP", "TEMP", 1},
    {"TMP", "TEMP", 1},
    {"USERPROFILE", "USERS\\SODOS", 1},
    {"HOMEDRIVE", "D:", 0},
    {"HOMEPATH", "\\WINE\\USERS\\SODOS", 0},
    {"APPDATA", "USERS\\SODOS\\APPDATA\\ROAMING", 1},
    {"LOCALAPPDATA", "USERS\\SODOS\\APPDATA\\LOCAL", 1},
    {"ProgramData", "PROGRAMDATA", 1},
    {"ProgramFiles", "PROGRAM FILES", 1},
    {"USERNAME", "sodos", 0},
    {"COMPUTERNAME", "SODOS", 0},
    {"OS", "Windows_NT", 0},
    {"PROCESSOR_ARCHITECTURE", "AMD64", 0},
    {"NUMBER_OF_PROCESSORS", "1", 0},
    {"PATHEXT", ".COM;.EXE;.BAT;.CMD", 0},
    {"PATH", "SYSTEM32", 1},
    {0, 0, 0}};

/* ---- ambiente do processo (wp()->envb) ---- */
static int envname_eq(const char* e, const char* name) { /* "NOME=..." x nome, sem caixa */
  while (*name && *e && *e != '=') {
    char a = *e, b = *name;
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    if (a != b) return 0;
    e++;
    name++;
  }
  return !*name && *e == '=';
}
/* entrada "NOME=valor" no bloco (o '=' inicial de "=C:=C:\" faz parte do nome) */
static const char* envb_find(const char* name) {
  struct wproc* p = wp();
  const char* e;
  if (!p->envb) return 0;
  for (e = p->envb; e < p->envb + p->envlen && *e; e += strlen(e) + 1) {
    const char* s = e + (*e == '=' ? 1 : 0);
    if (envname_eq(s, name)) return strchr(s, '=') + 1;
  }
  return 0;
}
static void envb_set(const char* name, const char* val) {
  struct wproc* p = wp();
  size_t nl = strlen(name), vl = val ? strlen(val) : 0, cap = p->envlen + nl + vl + 4;
  char* nb = (char*)k_malloc((unsigned)cap);
  size_t o = 0;
  const char* e;
  if (!nb) return;
  for (e = p->envb; e && e < p->envb + p->envlen && *e; e += strlen(e) + 1) {
    const char* s = e + (*e == '=' ? 1 : 0);
    size_t l = strlen(e);
    if (envname_eq(s, name)) continue;
    memcpy(nb + o, e, l + 1);
    o += l + 1;
  }
  if (val) {
    memcpy(nb + o, name, nl);
    o += nl;
    nb[o++] = '=';
    memcpy(nb + o, val, vl + 1);
    o += vl + 1;
  }
  nb[o++] = 0;
  k_free(p->envb);
  p->envb = nb;
  p->envlen = (uint32_t)o;
}

static int env_get(const char* name, char* out, int max) {
  int i, len;
  {
    const char* v = envb_find(name);
    if (v) {
      len = (int)strlen(v);
      if (len >= max) len = max - 1;
      memcpy(out, v, (size_t)len);
      out[len] = 0;
      return len;
    }
    /* com bloco proprio, so o conjunto fixo completa (como o Windows) */
    if (wp()->envb) goto fixed;
  }
  /* PATH do SODOS tem outro formato: fica o do conjunto fixo */
  if (!wstreq(name, "PATH")) {
    len = k_getenv(name, out, max);
    if (len >= 0) return len;
  }
fixed:
  for (i = 0; env_fixed[i].name; i++) {
    const char* v;
    if (!wstreq(name, env_fixed[i].name)) continue;
    v = env_fixed[i].dir ? wine_dir(env_fixed[i].val) : env_fixed[i].val;
    len = (int)strlen(v);
    if (len >= max) len = max - 1;
    memcpy(out, v, (size_t)len);
    out[len] = 0;
    return len;
  }
  return -1;
}
static WINAPI DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD n) {
  char tmp[300];
  int len = env_get(name, tmp, (int)sizeof(tmp));
  if (len < 0) {
    wset_lasterr(ERROR_ENVVAR_NOT_FOUND);
    return 0;
  }
  if ((DWORD)len + 1 > n) return (DWORD)len + 1;
  memcpy(buf, tmp, (size_t)len + 1);
  return (DWORD)len;
}
static WINAPI DWORD GetEnvironmentVariableW(const wchar16* name, wchar16* buf, DWORD n) {
  char* nm = wdup16to8(name);
  char tmp[300];
  int len = nm ? env_get(nm, tmp, (int)sizeof(tmp)) : -1;
  int w;
  k_free(nm);
  if (len < 0) {
    wset_lasterr(ERROR_ENVVAR_NOT_FOUND);
    return 0;
  }
  w = wutf8_to_16(tmp, len, 0, 0);
  if ((DWORD)w + 1 > n) return (DWORD)w + 1;
  wutf8_to_16(tmp, len, buf, w);
  buf[w] = 0;
  return (DWORD)w;
}
static WINAPI BOOL SetEnvironmentVariableA(const char* name, const char* v) {
  if (!name) return 0;
  if (wp()->envb) {
    envb_set(name, v);
    wenv_publish();
    return 1;
  }
  return k_setenv(name, v ? v : "") == 0;
}
static WINAPI BOOL SetEnvironmentVariableW(const wchar16* name, const wchar16* v) {
  char* n8 = wdup16to8(name);
  char* v8 = v ? wdup16to8(v) : 0;
  BOOL r;
  if (n8 && wp()->envb) {
    envb_set(n8, v8);
    wenv_publish();
    r = 1;
  } else {
    r = n8 ? k_setenv(n8, v8 ? v8 : "") == 0 : 0;
  }
  k_free(n8);
  k_free(v8);
  return r;
}
/* Bloco "NOME=valor\0...\0\0": o conjunto fixo e as variaveis do Go do SODOS. */
static size_t env_add(char* b, size_t o, const char* name) {
  char tmp[300];
  int len = env_get(name, tmp, (int)sizeof(tmp));
  size_t nl = strlen(name);
  if (len < 0 || o + nl + (size_t)len + 4 > 4096) return o;
  memcpy(b + o, name, nl);
  o += nl;
  b[o++] = '=';
  memcpy(b + o, tmp, (size_t)len + 1);
  return o + (size_t)len + 1;
}
static char* env_block(size_t* size) {
  char* b;
  size_t o = 0;
  int i;
  static const char* const extra[] = {"GODEBUG", "GOMAXPROCS", "GOTRACEBACK", 0};
  struct wproc* p = wp();
  if (p->envb) {
    b = (char*)wzalloc(p->envlen + 2);
    if (!b) return 0;
    memcpy(b, p->envb, p->envlen);
    *size = p->envlen;
    return b;
  }
  b = (char*)wzalloc(4096);
  if (!b) return 0;
  for (i = 0; env_fixed[i].name; i++) o = env_add(b, o, env_fixed[i].name);
  for (i = 0; extra[i]; i++) o = env_add(b, o, extra[i]);
  /* SET WINEENV=NOME1;NOME2: variaveis do SODOS que entram no ambiente dos
   * programas do Windows (ex.: CLAUDE_CODE_GIT_BASH_PATH) */
  {
    char list[300], nm[64];
    int n = k_getenv("WINEENV", list, (int)sizeof(list)), k = 0;
    for (i = 0; n > 0 && i <= n; i++) {
      char c = i < n ? list[i] : 0;
      if (c == ';' || c == ',' || c == ' ' || c == 0) {
        nm[k] = 0;
        if (k) o = env_add(b, o, nm);
        k = 0;
      } else if (k < 63) {
        nm[k++] = c;
      }
    }
  }
  b[o++] = 0;
  *size = o;
  return b;
}
/* proc.c (CreateProcess sem lpEnvironment): o filho herda este ambiente */
char* wenv_snapshot(uint32_t* len) {
  size_t n = 0;
  char* b;
  char* s;
  if (!wp()->envb) {
    *len = 0;
    return 0; /* o filho usa o padrao, como hoje */
  }
  b = env_block(&n);
  if (!b) return 0;
  s = (char*)k_malloc_shared((uint32_t)n + 2);
  if (s) memcpy(s, b, n);
  k_free(b);
  *len = (uint32_t)n;
  return s;
}
static WINAPI char* GetEnvironmentStringsA(void) {
  size_t n;
  return env_block(&n);
}
static WINAPI wchar16* GetEnvironmentStringsW(void) {
  size_t n = 0, i;
  char* b = env_block(&n);
  wchar16* w = (wchar16*)wzalloc((n + 2) * 2);
  if (b && w) {
    int m = wutf8_to_16(b, (int)n, w, (int)n + 1);
    if (m < 0)
      for (i = 0; i < n; i++) w[i] = (uint8_t)b[i];
  }
  k_free(b);
  return w;
}
/* RTL_USER_PROCESS_PARAMETERS.Environment (+0x80) e EnvironmentSize (+0x3F0):
 * o Bun (Zig) le o ambiente direto do PEB, sem GetEnvironmentStringsW */
void wenv_publish(void) {
  struct wproc* p = wp();
  uint8_t* pp = (uint8_t*)(uintptr_t) * (uint64_t*)(p->peb + PEB_PARAMS);
  wchar16* w;
  size_t n = 0;
  if (!pp) return;
  w = GetEnvironmentStringsW();
  if (!w) return;
  while (w[n] || w[n + 1]) n++;
  *(uint64_t*)(pp + 0x80) = (uint64_t)(uintptr_t)w;
  *(uint64_t*)(pp + 0x3F0) = (uint64_t)(n + 2) * 2;
}
/* %NOME% -> valor */
static WINAPI DWORD ExpandEnvironmentStringsW(const wchar16* src, wchar16* dst, DWORD n) {
  char* s8 = wdup16to8(src);
  char out[900];
  size_t o = 0;
  const char* s;
  int w;
  if (!s8) return 0;
  for (s = s8; *s && o < sizeof(out) - 301;) {
    const char* e = *s == '%' ? strchr(s + 1, '%') : 0;
    if (e && e - s < 64) {
      char name[64], val[300];
      int len;
      memcpy(name, s + 1, (size_t)(e - s - 1));
      name[e - s - 1] = 0;
      len = env_get(name, val, (int)sizeof(val));
      if (len >= 0) {
        memcpy(out + o, val, (size_t)len);
        o += (size_t)len;
        s = e + 1;
        continue;
      }
    }
    out[o++] = *s++;
  }
  out[o] = 0;
  k_free(s8);
  w = wutf8_to_16(out, -1, 0, 0);
  if ((DWORD)w + 1 <= n) {
    wutf8_to_16(out, -1, dst, w);
    dst[w] = 0;
  }
  return (DWORD)w + 1;
}
static WINAPI BOOL FreeEnvironmentStringsA(char* p) {
  k_free(p);
  return 1;
}
static WINAPI BOOL FreeEnvironmentStringsW(wchar16* p) {
  k_free(p);
  return 1;
}
struct startupinfo {
  DWORD cb;
  uint8_t rest[100];
};
static WINAPI void GetStartupInfoA(struct startupinfo* s) {
  memset(s, 0, sizeof(*s));
  s->cb = 104;
}
static WINAPI void GetStartupInfoW(struct startupinfo* s) { GetStartupInfoA(s); }
struct sysinfo {
  uint16_t arch, res;
  DWORD page;
  void* minaddr;
  void* maxaddr;
  uintptr_t mask;
  DWORD ncpu, type, gran;
  uint16_t level, rev;
};
static WINAPI void GetSystemInfo(struct sysinfo* s) {
  memset(s, 0, sizeof(*s));
  s->arch = 9; /* AMD64 */
  s->page = 0x1000;
  s->minaddr = (void*)0x10000;
  s->maxaddr = (void*)0x7FFFFFFEFFFFull;
  s->mask = 1;
  s->ncpu = 1;
  s->type = 8664;
  s->gran = 0x10000;
  s->level = 6;
}
static WINAPI void GetNativeSystemInfo(struct sysinfo* s) { GetSystemInfo(s); }
static WINAPI BOOL IsProcessorFeaturePresent(DWORD f) {
  /* 10 SSE2, 6 SSE, 17 NX, 23 FASTFAIL... diz sim so para SSE/SSE2/SSE3 */
  return f == 6 || f == 10 || f == 13;
}
static WINAPI BOOL IsDebuggerPresent(void) { return 0; }
static WINAPI DWORD GetVersion(void) { return 0x23F00206u; } /* 6.2 build 9200 */
static WINAPI uint16_t GetACP(void) { return 65001; }
static WINAPI uint16_t GetOEMCP(void) { return 437; }
static WINAPI BOOL IsValidCodePage(DWORD cp) { return cp == 65001 || cp == 437 || cp == 1252 || cp == 0; }

/* ---------- strings ---------- */
static WINAPI int MultiByteToWideChar(DWORD cp, DWORD fl, const char* s, int n, wchar16* d, int max) {
  int len;
  (void)cp;
  (void)fl;
  if (n < 0) n = (int)strlen(s) + 1;
  len = wutf8_to_16(s, n, 0, 0);
  if (!max) return len;
  if (len > max) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  return wutf8_to_16(s, n, d, max);
}
static WINAPI int WideCharToMultiByte(DWORD cp, DWORD fl, const wchar16* s, int n, char* d, int max,
                                      const char* defch, BOOL* used) {
  int len;
  (void)cp;
  (void)fl;
  (void)defch;
  if (used) *used = 0;
  if (n < 0) n = (int)wstrlen16(s) + 1;
  len = wutf16_to_8(s, n, 0, 0);
  if (!max) return len;
  if (len > max) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  return wutf16_to_8(s, n, d, max);
}
/* FormatMessageW: so mensagens do sistema, texto generico com o numero. */
static WINAPI DWORD FormatMessageW(DWORD fl, const void* src, DWORD id, DWORD lang, wchar16* buf, DWORD n,
                                   void* args) {
  char txt[64];
  static const char hex[] = "0123456789";
  int i = 0, k, w;
  char num[12];
  DWORD v = id;
  const char* pre = "Erro do Windows ";
  (void)src;
  (void)lang;
  (void)args;
  for (k = 0; pre[k]; k++) txt[i++] = pre[k];
  k = 0;
  do {
    num[k++] = hex[v % 10];
    v /= 10;
  } while (v);
  while (k) txt[i++] = num[--k];
  txt[i++] = '.';
  txt[i] = 0;
  w = wutf8_to_16(txt, -1, 0, 0);
  if (fl & 0x100) { /* ALLOCATE_BUFFER */
    wchar16* out = wdup8to16(txt);
    *(wchar16**)buf = out;
    return out ? (DWORD)w : 0;
  }
  if ((DWORD)w + 1 > n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  wutf8_to_16(txt, -1, buf, w);
  buf[w] = 0;
  return (DWORD)w;
}
static WINAPI wchar16* lstrcpyW(wchar16* d, const wchar16* s) {
  wchar16* r = d;
  while ((*d++ = *s++)) {
  }
  return r;
}
static WINAPI wchar16* lstrcatW(wchar16* d, const wchar16* s) {
  lstrcpyW(d + wstrlen16(d), s);
  return d;
}
static WINAPI wchar16* lstrcpynW(wchar16* d, const wchar16* s, int n) {
  int i = 0;
  if (n <= 0) return d;
  while (i < n - 1 && s[i]) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
  return d;
}
/* NULL vale como menor que qualquer texto, como no Windows (o Chromium
 * compara titulos/classes que podem ser NULL; aqui derrubava o processo) */
static WINAPI int lstrcmpiW(const wchar16* a, const wchar16* b) {
  if (!a || !b) return a == b ? 0 : (!a ? -1 : 1);
  for (;; a++, b++) {
    wchar16 x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (wchar16)(x - 32);
    if (y >= 'a' && y <= 'z') y = (wchar16)(y - 32);
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
}
static WINAPI int lstrcmpW(const wchar16* a, const wchar16* b) {
  if (!a || !b) return a == b ? 0 : (!a ? -1 : 1);
  for (;; a++, b++) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (!*a) return 0;
  }
}
static WINAPI int lstrlenA(const char* s) { return s ? (int)strlen(s) : 0; }
static WINAPI int lstrlenW(const wchar16* s) { return s ? (int)wstrlen16(s) : 0; }

static WINAPI void InitializeSListHead(void* h) { memset(h, 0, 16); }
static WINAPI DWORD FlsAlloc(void* cb) {
  (void)cb;
  return TlsAlloc();
}
static WINAPI void* FlsGetValue(DWORD i) { return TlsGetValue(i); }
static WINAPI void* FlsGetValue2(DWORD i) { return TlsGetValue(i); }
static WINAPI BOOL FlsSetValue(DWORD i, void* v) { return TlsSetValue(i, v); }
static WINAPI BOOL FlsFree(DWORD i) { return TlsFree(i); }
static WINAPI uint64_t EncodePointer(uint64_t p) { return p; }
static WINAPI uint64_t DecodePointer(uint64_t p) { return p; }

#define S(n) {#n, (void*)n}

const struct wine_sym k32_syms[] = {
    S(ExitProcess), S(TerminateProcess), S(GetCurrentProcess), S(GetCurrentThread),
    S(GetCurrentProcessId), S(GetCurrentThreadId), S(GetLastError), S(SetLastError),
    S(GetCommandLineA), S(GetCommandLineW), S(GetModuleHandleA), S(GetModuleHandleW),
    S(GetModuleHandleExW), S(LoadLibraryA), S(LoadLibraryW), S(LoadLibraryExA), S(LoadLibraryExW),
    S(FreeLibrary), S(GetProcAddress), S(GetModuleFileNameA), S(GetModuleFileNameW),
    S(GetStdHandle), 
    S(GetConsoleMode), S(SetConsoleMode), S(GetConsoleCP), S(GetConsoleOutputCP),
    S(SetConsoleOutputCP), S(SetConsoleCtrlHandler),
    S(GetProcessHeap), S(HeapCreate), S(HeapDestroy), S(HeapAlloc), S(HeapFree), S(HeapReAlloc),
    S(HeapSize), S(LocalAlloc), S(LocalFree),  S(InitializeCriticalSection), S(InitializeCriticalSectionAndSpinCount),
    S(InitializeCriticalSectionEx), S(DeleteCriticalSection), S(TryEnterCriticalSection),
    S(EnterCriticalSection), S(LeaveCriticalSection), S(InitializeSRWLock),
    S(AcquireSRWLockExclusive), S(ReleaseSRWLockExclusive), S(TlsAlloc), S(TlsFree),
    S(TlsGetValue), S(TlsSetValue),  S(GetTickCount), S(GetTickCount64),
    S(QueryPerformanceFrequency), S(QueryPerformanceCounter), S(GetSystemTimeAsFileTime),
    S(GetSystemTimePreciseAsFileTime), S(GetEnvironmentVariableA), S(GetEnvironmentVariableW),
    S(SetEnvironmentVariableA), S(SetEnvironmentVariableW), S(ExpandEnvironmentStringsW), S(GetEnvironmentStringsA), S(GetEnvironmentStringsW),
    S(FreeEnvironmentStringsA), S(FreeEnvironmentStringsW), S(GetStartupInfoA),
    S(GetStartupInfoW), S(GetSystemInfo), S(GetNativeSystemInfo), S(IsProcessorFeaturePresent),
    S(IsDebuggerPresent),  S(GetVersion), S(GetACP), S(GetOEMCP), S(IsValidCodePage),
    S(MultiByteToWideChar), S(FormatMessageW), S(lstrcpyW), S(lstrcatW), S(lstrcpynW), S(lstrcmpiW), S(lstrcmpW), S(WideCharToMultiByte), S(lstrlenA), S(lstrlenW),
     S(InitializeSListHead), S(FlsAlloc), S(FlsGetValue), S(FlsGetValue2), S(FlsSetValue),
    S(FlsFree), S(EncodePointer), S(DecodePointer),
    {0, 0}};

