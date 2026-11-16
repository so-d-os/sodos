/* serial.c - UART 16550 COM1; saida capturada por qemu -serial file:... */
#include <stdint.h>
#include "serial.h"

#define COM1 0x3F8u

static inline void outb(uint16_t port, uint8_t v) {
  __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

void serial_init(void) {
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x80);
  outb(COM1 + 0, 0x01);
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x03);
  outb(COM1 + 2, 0xC7);
  outb(COM1 + 4, 0x0B);
}

static int tx_ready(void) {
  return (int)(inb(COM1 + 5) & 0x20u);
}

/* Copia de tudo o que sai na COM1, para o \LOG\SERIAL.LOG: o PC real nao tem
 * serial. Guarda os primeiros 192 KB do boot (o diagnostico esta no comeco). */
#define SLOG_MAX (192u * 1024u)
static char slog[SLOG_MAX];
static volatile uint32_t slog_n;

const char* serial_log(uint32_t* n) {
  uint32_t v = slog_n;
  if (n) *n = v > SLOG_MAX ? SLOG_MAX : v;
  return slog;
}

void serial_putc(char c) {
  volatile int spin = 100000;
  if (c != '\r') {
    uint32_t i = __atomic_fetch_add(&slog_n, 1u, __ATOMIC_RELAXED);
    if (i < SLOG_MAX) slog[i] = c;
  }
  while (!tx_ready() && --spin > 0) { }
  outb(COM1, (uint8_t)c);
}

void serial_puts(const char* s) {
  if (!s) return;
  while (*s) {
    if (*s == '\n')
      serial_putc('\r');
    serial_putc(*s++);
  }
}

void serial_hex32(uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 7; i >= 0; i--)
    serial_putc(hex[(v >> (i * 4)) & 0xF]);
}
