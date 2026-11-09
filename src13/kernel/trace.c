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
static int trace_off;

#include "fbcon.h"
#define VGA vga_text  /* 0xB8000, ou a copia do fbcon no UEFI */

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

/* Excecao: o que vier depois (as escritas do CRASH.LOG) nao entra no trace. */
void trace_freeze(void) { trace_off = 1; }

void trace_event(const char* tag, uint32_t a, uint32_t b, uint32_t c) {
  if (trace_off) return;
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

/* Mesmo conteudo do trace_dump_vga, em texto (para o CRASH.LOG). Bytes escritos. */
int trace_dump_text(char* out, int max) {
  static const char hex[] = "0123456789ABCDEF";
  int n = 0;
  uint32_t start = (trace_head + TRACE_SIZE - trace_count) % TRACE_SIZE;
  for (uint32_t i = 0; i < trace_count; i++) {
    uint32_t idx = (start + i) % TRACE_SIZE;
    const char* t = trace_tags[idx] ? trace_tags[idx] : "null";
    uint32_t v[3] = { trace_a[idx], trace_b[idx], trace_c[idx] };
    if (n + 80 > max) break;
    while (*t && n < max - 60) out[n++] = *t++;
    for (int k = 0; k < 3; k++) {
      out[n++] = ' ';
      out[n++] = (char)('a' + k);
      out[n++] = '=';
      for (int h = 7; h >= 0; h--) out[n++] = hex[(v[k] >> (h * 4)) & 0xF];
    }
    out[n++] = '\n';
  }
  return n;
}
