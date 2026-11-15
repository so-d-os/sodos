/*
 * linkext.h - Associacao de extensoes (qual programa abre .TXT, .PDF...).
 *
 * Opcional: SET LINKEXT=1 (AUTOEXEC.BAT ou prompt) carrega o LINKEXT.DLL de
 * PATHSODOS\DRV (depois D:\SODOS\DRV e Z:\SODOS\DRV). SET LINKEXT=0 (ou sem
 * a variavel) descarrega: o ponteiro linkext_fn fica 0 e o launcher do WIN
 * (desktop, Explorer, gerenciador) nem chama nada; nenhum arquivo e lido e
 * nenhuma memoria fica com o DLL.
 *
 * O DLL roda no contexto do kernel (como FAT32.DLL: -m64, sem SSE, sem libc,
 * sem importar KERNEL); tudo vem em linkext_host. Ele le
 * PATHSODOS\LINKEXT.CFG (uma linha por extensao: TXT=D:\SODOS\EDIT.EXE),
 * acha o programa da extensao e pede ao host que o abra com o arquivo.
 * Devolve 0 se abriu (achou o programa) e 1 se nao: o launcher padrao segue.
 *
 * Programas especiais no CFG: @NOTEPAD, @IMAGEM e @VIDEO (visores do WIN).
 */
#pragma once
#include <stdint.h>

#define LINKEXT_ABI 1
#define LINKEXT_SYM "linkext_driver"
#define LINKEXT_CFG "LINKEXT.CFG"   /* em PATHSODOS */
#define LINKEXT_PATH_MAX 128

struct linkext_host {
  uint32_t abi;
  /* Le ate max-1 bytes (poe NUL). Quantos leu; <0 se nao existe. */
  int (*fload)(const char* path, char* buf, int max);
  /* Tamanho do arquivo ou <0 se nao existe. */
  int (*fsize)(const char* path);
  /* PATHSODOS + "\" + rel. */
  void (*sodos_path)(const char* rel, char* out, int max);
  /* Abre exe (ou @NOTEPAD/@IMAGEM/@VIDEO) com file como argv[1]. 0 ok. */
  int (*launch)(const char* exe, const char* file);
  void (*log)(const char* s);       /* serial */
};

struct linkext_ops {
  uint32_t abi;                     /* LINKEXT_ABI */
  /* 0 = achou o programa e pediu para abrir; 1 = nao tem associacao. */
  int (*open)(const struct linkext_host* h, const char* file);
};

typedef const struct linkext_ops* (*linkext_entry_fn)(void);

/* ---- lado do kernel (kernel/linkext.c) ---- */
/* 0 quando desligado: quem chama testa antes (if (linkext_fn) ...). */
extern int (*linkext_fn)(const char* file);
/* env.c chama quando LINKEXT ou PATHSODOS mudam: carrega ou descarrega. */
void linkext_env_changed(void);
/* Pedido de abrir do WIN (visual.c): exe com argumento. */
int win_launch_arg(const char* exe, const char* arg);
