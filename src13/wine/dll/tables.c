/*
 * tables.c - DLLs do sistema embutidas no WINE.DLL.
 *
 * Um nome desta lista nunca e procurado em disco: LoadLibrary devolve um
 * modulo embutido e as funcoes saem das tabelas *_syms (uma por arquivo). Os
 * nomes nao se repetem entre as DLLs do Windows que importam aqui, entao a
 * busca e uma so, em todas as tabelas. Funcao que falta: GetProcAddress
 * devolve 0 e um import vira stub que diz o nome se for chamado.
 */
#include <string.h>
#include "wine_int.h"

static const char* const known[] = {
    "kernel32", "kernelbase", "ntdll", "ucrtbase", "msvcrt", "user32", "gdi32", "advapi32",
    "shell32", "ole32", "combase", "oleaut32", "ws2_32", "mswsock", "winmm", "bcryptprimitives",
    "bcrypt", "crypt32", "version", "userenv", "psapi", "iphlpapi", "dnsapi", "secur32", "sechost",
    "netapi32", "winhttp", "wintrust", "wtsapi32", "setupapi", "cfgmgr32", "dwmapi", "comctl32",
    "shlwapi", "uxtheme", "imm32", "powrprof", "shcore", "rpcrt4", "msimg32", "gdiplus",
    "vcruntime140", "vcruntime140_1", "msvcp140", 0};

static const struct wine_sym* const tables[] = {k32_syms, vm_syms,  obj_syms, exc_syms, file_syms,
                                                crt_syms, sys_syms, reg_syms, net_syms, gui_syms, misc_syms, crypt_syms, winrt_syms, proc_syms, sig_syms, compat_syms, gdi_syms, dlg_syms, shell_syms, uimisc_syms, 0};

static int starts(const char* s, const char* p) {
  while (*p && *s == *p) {
    s++;
    p++;
  }
  return *p == 0;
}

int builtin_known(const char* n) {
  int i;
  if (starts(n, "api-ms-win-") || starts(n, "ext-ms-win-")) return 1;
  for (i = 0; known[i]; i++)
    if (wstreq(n, known[i])) return 1;
  return 0;
}

void* builtin_lookup(const char* name) {
  int t;
  for (t = 0; tables[t]; t++) {
    const struct wine_sym* s;
    for (s = tables[t]; s->name; s++)
      if (s->name[0] == name[0] && strcmp(s->name, name) == 0) return s->fn;
  }
  return 0;
}
