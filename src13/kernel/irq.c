/* irq.c - Envia EOI para IRQ 32..47 */
#include "irq.h"
#include "pic.h"
#include "reboot.h"

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

static inline unsigned char irq_inb(unsigned short port) {
  unsigned char data;
  __asm__ volatile ("inb %1, %0" : "=a"(data) : "Nd"(port));
  return data;
}

void irq_handle(isr_frame_t* f) {
  uint32_t intno = f->int_no;
  if (intno >= 32 && intno <= 47) {
    uint8_t irq = (uint8_t)(intno - 32);
    if (irq == 0) {
      /* So conta: nenhum outro estado do kernel e tocado aqui. */
      irq_tick_count++;
      if (bsp_idle_flag) bsp_idle_ticks++;
      else bsp_busy_ticks++;
    } else if (irq == 1) {
      uint8_t st = irq_inb(0x64);
      if (st & 1) {
        uint8_t sc = irq_inb(0x60);
        if (!(st & 0x20))
          kbd_watch(sc);
      }
    }
    pic_send_eoi(irq);
    /* Depois do EOI: o gancho pode trocar de tarefa e so voltar bem depois. */
    if (irq == 0 && tick_hook) tick_hook(f->eip);
  }
}
