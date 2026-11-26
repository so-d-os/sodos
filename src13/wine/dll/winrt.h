/* winrt.h - objeto COM generico e HSTRING do WINE.DLL (winrt.c). */
#pragma once
#include "wine_int.h"

#define S_OK 0
#define E_NOTIMPL ((long)0x80004001)
#define E_NOINTERFACE ((long)0x80004002)
#define E_POINTER ((long)0x80004003)
#define E_FAIL ((long)0x80004005)
#define E_OUTOFMEMORY ((long)0x8007000E)

#define COM_SLOTS 48 /* IMFSample vai ate o slot 46 (mf.c) */
struct comobj;
/* slot = indice na vtable; a = ate 6 argumentos depois do this. Devolver
 * E_NOTIMPL deixa o tratamento padrao (QI devolve o proprio objeto; metodo
 * desconhecido e registrado na serial). */
typedef long (*com_handler)(struct comobj* self, int slot, uintptr_t* a);
struct comobj {
  void** vtbl;
  int refs;
  char name[80];
  com_handler handler;
  void* data;
  uintptr_t u[4];
  int plain;   /* 1 = COM classico (sem IInspectable): slots 3-5 vao ao handler */
};

struct comobj* com_new(const char* name, com_handler h, void* data);
long WINAPI wWindowsCreateString(const wchar16* src, DWORD len, void** out);
const wchar16* WINAPI wWindowsGetStringRawBuffer(void* h, DWORD* len);
char* whstring8(void* h);                 /* copia em UTF-8 (k_free) */
long whstring_from8(const char* s, void** out);
int whstring_is(void* h);                 /* 1 se e um HSTRING nosso */
