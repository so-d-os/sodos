/* idt.c - Interrupt Descriptor Table setup */
#include "idt.h"

#define IDT_SEL_CODE 0x08
#define IDT_FLAGS    0x8E  /* present, ring0, interrupt gate (long mode) */

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern void isr0_stub(void),  isr1_stub(void),  isr2_stub(void),  isr3_stub(void);
extern void isr4_stub(void),  isr5_stub(void),  isr6_stub(void),  isr7_stub(void);
extern void isr8_stub(void),  isr9_stub(void),  isr10_stub(void), isr11_stub(void);
extern void isr12_stub(void), isr13_stub(void), isr14_stub(void), isr15_stub(void);
extern void isr16_stub(void), isr17_stub(void), isr18_stub(void), isr19_stub(void);
extern void isr20_stub(void), isr21_stub(void), isr22_stub(void), isr23_stub(void);
extern void isr24_stub(void), isr25_stub(void), isr26_stub(void), isr27_stub(void);
extern void isr28_stub(void), isr29_stub(void), isr30_stub(void), isr31_stub(void);

extern void irq0_stub(void),  irq1_stub(void),  irq2_stub(void),  irq3_stub(void);
extern void irq4_stub(void),  irq5_stub(void),  irq6_stub(void),  irq7_stub(void);
extern void irq8_stub(void),  irq9_stub(void),  irq10_stub(void), irq11_stub(void);
extern void irq12_stub(void), irq13_stub(void), irq14_stub(void), irq15_stub(void);

static void set_gate(int n, uint64_t handler, uint16_t sel, uint8_t flags) {
  idt[n].off_lo    = (uint16_t)(handler & 0xFFFFu);
  idt[n].off_mid   = (uint16_t)((handler >> 16) & 0xFFFFu);
  idt[n].off_hi    = (uint32_t)((handler >> 32) & 0xFFFFFFFFu);
  idt[n].sel       = sel;
  idt[n].ist       = 0;
  idt[n].type_attr = flags;
  idt[n].reserved  = 0;
}

void idt_init(void) {
  uint32_t i;
  for (i = 0; i < 256; i++) {
    idt[i].off_lo    = 0;
    idt[i].sel       = 0;
    idt[i].ist       = 0;
    idt[i].type_attr = 0;
    idt[i].off_mid   = 0;
    idt[i].off_hi    = 0;
    idt[i].reserved  = 0;
  }

  set_gate(0,  (uint64_t)isr0_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(1,  (uint64_t)isr1_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(2,  (uint64_t)isr2_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(3,  (uint64_t)isr3_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(4,  (uint64_t)isr4_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(5,  (uint64_t)isr5_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(6,  (uint64_t)isr6_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(7,  (uint64_t)isr7_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(8,  (uint64_t)isr8_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(9,  (uint64_t)isr9_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(10, (uint64_t)isr10_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(11, (uint64_t)isr11_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(12, (uint64_t)isr12_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(13, (uint64_t)isr13_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(14, (uint64_t)isr14_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(15, (uint64_t)isr15_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(16, (uint64_t)isr16_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(17, (uint64_t)isr17_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(18, (uint64_t)isr18_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(19, (uint64_t)isr19_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(20, (uint64_t)isr20_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(21, (uint64_t)isr21_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(22, (uint64_t)isr22_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(23, (uint64_t)isr23_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(24, (uint64_t)isr24_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(25, (uint64_t)isr25_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(26, (uint64_t)isr26_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(27, (uint64_t)isr27_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(28, (uint64_t)isr28_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(29, (uint64_t)isr29_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(30, (uint64_t)isr30_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(31, (uint64_t)isr31_stub, IDT_SEL_CODE, IDT_FLAGS);

  /* IRQ 0..15 -> vetores 32..47 */
  set_gate(32, (uint64_t)irq0_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(33, (uint64_t)irq1_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(34, (uint64_t)irq2_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(35, (uint64_t)irq3_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(36, (uint64_t)irq4_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(37, (uint64_t)irq5_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(38, (uint64_t)irq6_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(39, (uint64_t)irq7_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(40, (uint64_t)irq8_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(41, (uint64_t)irq9_stub,  IDT_SEL_CODE, IDT_FLAGS);
  set_gate(42, (uint64_t)irq10_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(43, (uint64_t)irq11_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(44, (uint64_t)irq12_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(45, (uint64_t)irq13_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(46, (uint64_t)irq14_stub, IDT_SEL_CODE, IDT_FLAGS);
  set_gate(47, (uint64_t)irq15_stub, IDT_SEL_CODE, IDT_FLAGS);

  idtp.limit = (uint16_t)(sizeof(idt) - 1);
  idtp.base  = (uint64_t)&idt[0];

  __asm__ volatile ("lidt %0" : : "m"(idtp));
  /* Nao dar sti aqui */
}
