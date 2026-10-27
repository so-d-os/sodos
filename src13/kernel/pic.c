/* pic.c - 8259 PIC (freestanding) */
#include "pic.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

#define PIC1         0x20
#define PIC2         0xA0
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1+1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2+1)

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

void pic_mask_all(void) {
  outb(PIC1_DATA, 0xFF);
  outb(PIC2_DATA, 0xFF);
}

void pic_unmask(uint8_t irq) {
  uint16_t port;
  uint8_t val;
  if (irq < 8) port = PIC1_DATA;
  else { port = PIC2_DATA; irq -= 8; }
  val = inb(port) & ~(1 << irq);
  outb(port, val);
}

void pic_mask_irq(uint8_t irq) {
  uint16_t port;
  uint8_t val;
  if (irq < 8) port = PIC1_DATA;
  else { port = PIC2_DATA; irq -= 8; }
  val = (uint8_t)(inb(port) | (uint8_t)(1u << irq));
  outb(port, val);
}

void pic_send_eoi(uint8_t irq) {
  if (irq >= 8) outb(PIC2_COMMAND, 0x20);
  outb(PIC1_COMMAND, 0x20);
}

static void pic_io_wait(void) {
  outb(0x80, 0);
}

void pic_remap(uint8_t offset1, uint8_t offset2) {
  /* Mascara tudo antes dos ICW. Sem isto o IRQ0 do BIOS continua no vetor 8,
   * que no IDT e #DF (com error code). O iret sai para lixo e da #UD. */
  outb(PIC1_DATA, 0xFF);
  outb(PIC2_DATA, 0xFF);
  pic_io_wait();

  outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
  pic_io_wait();
  outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
  pic_io_wait();

  outb(PIC1_DATA, offset1);
  pic_io_wait();
  outb(PIC2_DATA, offset2);
  pic_io_wait();

  outb(PIC1_DATA, 4);
  pic_io_wait();
  outb(PIC2_DATA, 2);
  pic_io_wait();

  outb(PIC1_DATA, ICW4_8086);
  pic_io_wait();
  outb(PIC2_DATA, ICW4_8086);
  pic_io_wait();

  outb(PIC1_DATA, 0xFF);
  outb(PIC2_DATA, 0xFF);
}
