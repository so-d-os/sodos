/*
 * wine.h - EXEs do Windows (PE32+) pelo WINE.DLL (wine/README.md).
 *
 * Opcional: SET WINE=1 liga. Sem a variavel (ou SET WINE=0) nada e carregado:
 * um .EXE do Windows nao roda e o shell diz para ligar o WINE. Com WINE=1 o
 * WINE.DLL so e carregado quando o primeiro EXE do Windows roda (de
 * PATHSODOS\DRV, depois D:\SODOS\DRV e Z:\SODOS\DRV) e fica carregado para os
 * proximos. SET WINE=0 descarrega na hora, ou quando o ultimo EXE do Windows
 * que esta rodando terminar.
 *
 * O WINE.DLL e uma DLL SOX2 que importa o KERNEL como qualquer programa de 64
 * bits. O mod_exec cria o processo (pid, pilha, argv) como para um .EXE SOX e
 * chama wine_ops.start(argc, argv) nele: argv[0] e o caminho do .EXE do
 * Windows. A imagem PE e tudo que o WINE.DLL aloca durante o programa sao do
 * pid dele e somem no fim, como num programa SOX.
 *
 * gs:0x10..0x67 e a copia do TEB da thread Windows corrente (kthread.c,
 * k_win_teb): o codigo do Windows le gs:0x30/0x58/0x60.
 */
#pragma once
#include <stdint.h>

#define WINE_ABI 1
#define WINE_SYM "wine_driver"
#define WINE_STACK (1024u * 1024u)    /* pilha da thread principal do EXE */
#define WINE_PATH_MAX 128

struct wine_ops {
  uint32_t abi;                       /* WINE_ABI */
  /* Entrada do processo (SysV, na pilha do programa). argv[0] = .EXE. */
  int (*start)(int argc, char** argv);
};

typedef const struct wine_ops* (*wine_entry_fn)(void);

/* ---- lado do kernel (kernel/wine.c) ---- */
/* 1 se o arquivo e um executavel PE (MZ + "PE\0\0"). */
int wine_is_pe(const char* path);
/* Entrada do WINE.DLL para um novo EXE do Windows (carrega se preciso).
 * 0 se WINE nao esta ligado ou o DLL nao foi achado (mod_last_error diz). */
void* wine_entry(void);
/* O EXE do Windows terminou (normal ou a forca). */
void wine_exec_done(void);
/* env.c chama quando WINE ou PATHSODOS mudam. */
void wine_env_changed(void);
/* Para o comando WINE/diagnostico: 1 se o DLL esta na memoria. */
int wine_loaded(void);
