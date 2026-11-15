/* irq.c - Envia EOI para IRQ 32..47 */
#include "irq.h"
#include "pic.h"
#include "reboot.h"
#include "kthread.h"
#include "ps2in.h"

/* B1: contador do IRQ0. volatile: quem le nao pode cachear o valor. */
static volatile uint64_t irq_tick_count;
static volatile uint64_t bsp_busy_ticks;
static volatile uint64_t bsp_idle_ticks;
static volatile int bsp_idle_flag;

uint64_t irq_ticks(void) { return irq_tick_count; }
uint64_t irq_busy(void) { return bsp_busy_ticks; }
uint64_t irq_idle(void) { return bsp_idle_ticks; }

/* Marca o BSP como ocioso (esperando tecla/tarefa) para o TOP. */
void cpu_idle(int on) { bsp_idle_flag = on ? 1 : 0; }

/* Preempcao do WIN: chamado no IRQ0, depois do EOI, com o RIP interrompido. */
static void (*tick_hook)(uint64_t rip);
void irq_set_tick_hook(void (*fn)(uint64_t rip)) { tick_hook = fn; }

void irq_handle(isr_frame_t* f) {
  uint32_t intno = f->int_no;
  if (intno >= 32 && intno <= 47) {
    uint8_t irq = (uint8_t)(intno - 32);
    if (irq == 0) {
      /* So conta: nenhum outro estado do kernel e tocado aqui. */
      irq_tick_count++;
      if (bsp_idle_flag || kt_cpu_idle(0)) bsp_idle_ticks++;
      else bsp_busy_ticks++;
    } else if (irq == 1 || irq == 12) {
      /* Teclado/mouse: so tira o byte do 8042 e poe na fila (ps2in.c). */
      ps2in_isr();
    }
    pic_send_eoi(irq);
    /* Depois do EOI: o gancho pode trocar de tarefa e so voltar bem depois.
     * So com o mundo (WIN) nesta CPU, e so se o BKL estiver livre ou com
     * ele: com uma thread de outra CPU no kernel, fica para o proximo tick. */
    if (irq == 0) {
      if (tick_hook && kt_world_here() && kt_bkl_try_world()) {
        tick_hook(f->eip);
        kt_bkl_drop_world();
      }
      kt_tick_pit();
    }
  }
}
