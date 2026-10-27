/* trace.c - lightweight kernel trace buffer (freestanding) */
#include <stdint.h>
#include "trace.h"

#define TRACE_SIZE 64
#define ATTR 0x4F00

static const char* trace_tags[TRACE_SIZE];
static uint32_t trace_a[TRACE_SIZE];
static uint32_t trace_b[TRACE_SIZE];
static uint32_t trace_c[TRACE_SIZE];
static uint32_t trace_head = 0;
static uint32_t trace_count = 0;

static volatile uint16_t* const VGA = (volatile uint16_t*)0xB8000;

static void vga_putc(int row, int col, char c) {
  if (row >= 0 && row < 25 && col >= 0 && col < 80)
    VGA[row * 80 + col] = (uint16_t)(unsigned char)c | ATTR;
}

static void vga_puts_at(int row, int* col, const char* s) {
  while (*s && *col < 80) {
    vga_putc(row, *col, *s++);
    (*col)++;
  }
}

static void print_hex(uint32_t v, int row, int* col) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 7; i >= 0; i--) {
    vga_putc(row, (*col)++, hex[(v >> (i * 4)) & 0xF]);
  }
}

void trace_event(const char* tag, uint32_t a, uint32_t b, uint32_t c) {
  trace_tags[trace_head] = tag;
  trace_a[trace_head] = a;
  trace_b[trace_head] = b;
  trace_c[trace_head] = c;
  trace_head = (trace_head + 1u) % TRACE_SIZE;
  if (trace_count < TRACE_SIZE)
    trace_count++;
}

void trace_dump_vga(int row) {
  int c;
  if (trace_count == 0) return;

  c = 0;
  vga_puts_at(row++, &c, "TRACE (most recent last):");

  uint32_t start = (trace_head + TRACE_SIZE - trace_count) % TRACE_SIZE;
  for (uint32_t i = 0; i < trace_count && row < 25; i++) {
    uint32_t idx = (start + i) % TRACE_SIZE;
    c = 0;
    vga_puts_at(row, &c, trace_tags[idx] ? trace_tags[idx] : "null");
    vga_puts_at(row, &c, " a=0x");
    print_hex(trace_a[idx], row, &c);
    vga_puts_at(row, &c, " b=0x");
    print_hex(trace_b[idx], row, &c);
    vga_puts_at(row, &c, " c=0x");
    print_hex(trace_c[idx], row, &c);
    row++;
  }
}
