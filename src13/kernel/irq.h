/* irq.h - Tratamento de IRQ (32..47), EOI */
#pragma once
#include <stdint.h>
#include "isr.h"

void irq_handle(isr_frame_t* f);
/* Ticks do IRQ0 (PIT), contados desde o boot. O IRQ0 e desmascarado no kmain. */
uint64_t irq_ticks(void);
/* Ticks do BSP trabalhando e ocioso (marcado por cpu_idle). */
uint64_t irq_busy(void);
uint64_t irq_idle(void);
/* 1 = BSP esperando (tecla, tarefa em outra CPU); o tick conta como ocioso. */
void cpu_idle(int on);
/* Gancho do tick (IRQ0), chamado depois do EOI com o RIP interrompido e
 * IF=0. Pode trocar de tarefa (preempcao das janelas do WIN). */
void irq_set_tick_hook(void (*fn)(uint64_t rip));

/* Relogio pelo TSC (irq.c): com TSC invariante e sem hypervisor, o IRQ0
 * acerta irq_ticks() pelo TSC e os ticks perdidos (IRQ0 parado > 1 ms) sao
 * recuperados. irq_clock_init: depois de o PIT ir para 1 kHz (net.c). */
struct irq_clock_stall {
  uint64_t at;          /* irq_ticks() quando o IRQ0 voltou */
  uint64_t rip;         /* RIP interrompido por esse IRQ0 */
  uint32_t ms;          /* quanto o IRQ0 ficou parado */
};
struct irq_clock {
  int tsc;              /* 1 = acertando pelo TSC */
  int eligible;         /* 1 = TSC invariante sem hypervisor; 0 = nao; -1 = nao visto */
  uint64_t hz;          /* TSC por segundo (0 = nao calibrado) */
  uint32_t spread_ppm;  /* desvio entre as medidas da calibracao */
  uint32_t stalls;      /* vezes que o IRQ0 ficou parado >= 2 ms */
  uint32_t max_ms;      /* maior recuperacao de uma vez */
  uint64_t lost;        /* ticks recuperados no total */
  uint64_t max_at;      /* irq_ticks() da maior */
  uint64_t last_at;     /* irq_ticks() da ultima parada */
  uint64_t held;        /* ticks segurados (PIT um pouco adiantado) */
  uint64_t now;         /* irq_ticks() agora */
  const char* why;      /* por que esta assim */
  uint64_t max_rip;     /* RIP interrompido no fim da maior parada */
  uint32_t nlog;        /* paradas >= 5 ms em log[] (mais nova primeiro) */
  struct irq_clock_stall log[8];
};
void irq_clock_init(void);
/* 1 liga (calibra se preciso; forca mesmo numa VM), 0 volta a so contar. */
int irq_clock_set(int on);
void irq_clock_info(struct irq_clock* out);
/* TSC por segundo; 0 = nao calibrado (VM ou sem TSC invariante). */
uint64_t irq_clock_hz(void);
/* Segura o IRQ0 por ms (cli) e devolve quantos ticks andaram. */
int irq_clock_test(uint32_t ms, uint64_t* ticks);
