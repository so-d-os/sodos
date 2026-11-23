/* ps2in.h - filas de entrada do controlador PS/2 (8042): teclado e mouse.
 *
 * A IRQ1 (teclado) e a IRQ12 (mouse) tiram o byte do controlador na hora e
 * o poem numa fila; quem le (shell, WIN) pega da fila quando puder. Assim
 * nenhum byte se perde nem fica esperando o laco do WIN dar a volta.
 * Bytes do teclado ja passaram por kbd_watch (Ctrl+C, Ctrl+Alt+Del, Shift).
 *
 * Toda leitura tambem esvazia o controlador por polling (com o mesmo lock da
 * IRQ): se a IRQ nao vier (controlador emulado, IRQ mascarada), funciona
 * como antes. */
#ifndef PS2IN_H
#define PS2IN_H
#include <stdint.h>

void ps2in_init(void);              /* liga IRQ1/IRQ12 no 8042 e no PIC */
void ps2in_isr(void);               /* chamada pela IRQ1 e pela IRQ12 */
void ps2in_pump(void);              /* esvazia o controlador nas filas */
void ps2in_inject(uint8_t b);       /* byte de teclado vindo do USB (xhci.c) */

/* 1 com o byte em *b (e o tick em que chegou em *t, se t != 0); 0 se vazia. */
int ps2in_kbd(uint8_t* b, uint32_t* t);
int ps2in_aux(uint8_t* b, uint32_t* t);
int ps2in_pending(void);
int ps2in_aux_pop(uint8_t* b, uint32_t* t);   /* RIT: so a fila do mouse */
int ps2in_kbd_has(void);                      /* RIT: tecla na fila? */
extern void (*volatile ps2in_kick)(void);     /* RIT: chamada na IRQ1/IRQ12 */            /* 1 se ha byte de teclado ou de mouse */
int ps2in_kbd_pending(void);
void ps2in_flush(void);             /* descarta tudo (entrada no WIN etc.) */

/* Para quem conversa direto com o controlador (comandos ao mouse, LOAD de
 * programas antigos que leem a porta): 1 mascara IRQ1/IRQ12, 0 devolve.
 * Aninha. */
void ps2in_raw(int on);

/* Contadores para o diagnostico (serial). */
void ps2in_stats(uint32_t* irqs, uint32_t* drops);

#endif
