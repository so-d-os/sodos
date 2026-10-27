/* irq.c - Envia EOI para IRQ 32..47 */
#include "irq.h"
#include "pic.h"

static inline unsigned char irq_inb(unsigned short port) {
  unsigned char data;
  __asm__ volatile ("inb %1, %0" : "=a"(data) : "Nd"(port));
  return data;
}

void irq_handle(isr_frame_t* f) {
  uint32_t intno = f->int_no;
  if (intno >= 32 && intno <= 47) {
    uint8_t irq = (uint8_t)(intno - 32);
    if (irq == 1 && (irq_inb(0x64) & 1))
      (void)irq_inb(0x60);
    pic_send_eoi(irq);
  }
}
