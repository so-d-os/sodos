/* idt.h - Interrupt Descriptor Table (long mode, gate de 16 bytes) */
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct idt_entry {
  uint16_t off_lo;
  uint16_t sel;
  uint8_t  ist;
  uint8_t  type_attr;
  uint16_t off_mid;
  uint32_t off_hi;
  uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

void idt_init(void);

#endif
