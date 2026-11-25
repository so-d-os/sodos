/* vt.h - Telas virtuais: Ctrl+F1..F6 e Ctrl+F8..F12 sao prompts do DOS em
 * modo texto, cada um com a sua tela, cursor, unidade e programa. Ctrl+F7 e o
 * WIN. As doze combinacoes sao do SO: nenhum programa as recebe.
 *
 * Cada tela e uma tarefa cooperativa com pilha propria (ctx_switch). So a
 * tela visivel le o teclado. As outras andam quando a visivel espera tecla:
 * um programa ao fundo escreve na copia da tela dele e para de novo quando
 * pede tecla, ou a cada 256 caracteres escritos. O WIN fica parado enquanto
 * nao esta na tela. */
#pragma once
#include <stdint.h>

#define VT_TEXT 11           /* Ctrl+F1..F6 e F8..F12 (F7 e o WIN) */
#define VT_WIN  VT_TEXT      /* indice da tarefa do WIN (Ctrl+F7) */
#define VT_TASKS (VT_TEXT + 1)

/* Estado do console do kernel.c que cada tela guarda. */
struct con_state {
  int row, col;
  void (*out)(char c);
  int (*exec)(const char* path, int argc, char** argv);
  void (*flush)(void);  /* shell_set_flush */
  int pipe_on;
};
void con_state_save(struct con_state* s);
void con_state_load(const struct con_state* s);

/* Liga as telas; a tarefa do boot vira a tela 1. */
void vt_init(void);
/* 1 se quem esta rodando nao e a tela visivel (nao le teclado nem mexe no
 * hardware da tela). */
int vt_bg(void);
/* Scancode (make, sem E0) com Ctrl apertado: indice da tela, ou -1. */
int vt_hotkey(uint8_t sc);
/* k_kbd_raw: o teclado cru (Ctrl+C vai ao programa) e de cada tela e vale
 * so enquanto ela esta visivel (o WSL ao fundo no Ctrl+F10 nao prende o
 * Ctrl+C das outras telas). */
void vt_kbd_raw(int on);
/* SET WSL=1: a VM do WSL (WSL.EXE run --service) sobe na tela Ctrl+F10. */
void vt_wsl_check(void);
/* SET SYSTEMLOG=1: o log do Ctrl+F12 vai para \LOG\CONSOLE.LOG a cada 5 min. */
void syslog_service_check(void);
void syslog_save_now(void);   /* REBOOT: grava ja se SYSTEMLOG=1 */
/* Mostra a tela i e passa a ela o teclado. Volta quando esta tela for
 * mostrada de novo (ou, numa tela de texto, quando ela ganhar a vez ao fundo). */
void vt_select(int i);
/* A tela ao fundo cede a vez. wait=1: espera tecla e so volta na tela. */
void vt_yield(int wait);
void vt_yield_poll(void);   /* ao fundo, k_trygetc sem tecla: volta no proximo tick */
/* A tela visivel, sem nada a fazer, da uma vez a uma tela ao fundo. */
void vt_idle(void);
/* Saida de caractere: ao fundo, cede a vez de tempos em tempos. */
void vt_out_tick(void);
/* Comando WIN: abre o WIN na tarefa propria ou volta a ele.
 * Devolve 1 se o WIN continua aberto quando esta tela volta. */
int vt_win(void);
/* SET DISKSYNC=1: abre DISKSYNC.EXE numa tela ao fundo (Ctrl+F12) se ainda
 * nao esta rodando. Sem a variavel nada acontece. env.c chama a cada SET. */
void vt_service_check(void);

/* Log do sistema, mostrado na tela Ctrl+F12 (anel de 64 KB desde o boot).
 * Pode ser chamado de qualquer lugar do kernel, inclusive das tarefas do WIN. */
void syslog_putc(char c);
void syslog_puts(const char* s);
void syslog_num(int v);
void syslog_stamp(void);      /* "hh:mm:ss " comecando uma linha */
/* SET PRODUCTION=1 (ou ON/SIM/YES/TRUE/PRODUCTION): o WIN nao abre janela de
 * saida para os programas; o texto deles vai para o log (Ctrl+F12). */
int syslog_production(void);

int vt_cur_index(void);   /* tela que esta rodando (VT_WIN = o WIN) */
int vt_state(int i);
/* LIMPA (shell/cmd_limpa.c): telas de texto em uso (indice e pid do programa,
 * 0 = prompt); recomecar todas, com a tela 1 em entry (nao volta, a nao ser
 * sem memoria); e, ja na tela 1 nova, encerrar os programas largados. */
int vt_limpa_list(int* vt, uint32_t* pid, int max);
void vt_limpa_finish(void (*entry)(void));
void vt_limpa_after(void);
