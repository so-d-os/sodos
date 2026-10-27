/* isr.c - Exception dispatch com sistema de erro detalhado (VGA direto, freestanding) */
#include <stdint.h>
#include "isr.h"
#include "irq.h"
#include "serial.h"
#include "visual.h"

extern void trace_dump_vga(int row);

static volatile uint16_t* const VGA = (volatile uint16_t*)0xB8000;
#define ATTR 0x1F00  /* fundo azul, texto branco */

static void vga_putc(int row, int col, char c) {
  if (row >= 0 && row < 25 && col >= 0 && col < 80)
    VGA[row * 80 + col] = (uint16_t)(unsigned char)c | ATTR;
}

static void vga_puts_at(int row, int* col, const char* s) {
  while (*s) {
    vga_putc(row, *col, *s++);
    (*col)++;
  }
}

/* Imprime uint32_t em hex (8 dígitos) na posição (row, col); retorna col após */
static void print_hex(uint32_t v, int row, int* col) {
  static const char hex[] = "0123456789ABCDEF";
  int i;
  for (i = 7; i >= 0; i--) {
    vga_putc(row, (*col)++, hex[(v >> (i * 4)) & 0xF]);
  }
}

static int in_range(uint32_t v, uint32_t lo, uint32_t hi) {
  return v >= lo && v <= hi;
}

static int dump_stack_words(uint32_t esp, int row) {
  if (!in_range(esp, 0x00010000, 0x000A0000)) {
    int c = 0;
    vga_puts_at(row, &c, "ESP out of range");
    return row + 1;
  }
  volatile uint32_t* p = (volatile uint32_t*)esp;
  for (int i = 0; i < 6 && row < 25; i++) {
    int c = 0;
    vga_puts_at(row, &c, "ESP+");
    vga_putc(row, c++, '0' + (char)i);
    vga_puts_at(row, &c, "*4: 0x");
    print_hex(p[i], row, &c);
    row++;
  }
  return row;
}

static int dump_backtrace(uint32_t ebp, int row) {
  if ((ebp & 3u) || !in_range(ebp, 0x00010000, 0x000A0000)) {
    int c = 0;
    vga_puts_at(row, &c, "EBP invalid");
    return row + 1;
  }
  volatile uint32_t* frame = (volatile uint32_t*)ebp;
  for (int i = 0; i < 8 && row < 25; i++) {
    uint32_t next = frame[0];
    uint32_t ret = frame[1];
    int c = 0;
    vga_puts_at(row, &c, "EBP=");
    print_hex((uint32_t)frame, row, &c);
    vga_puts_at(row, &c, " RET=");
    print_hex(ret, row, &c);
    row++;
    if ((next & 3u) || !in_range(next, 0x00010000, 0x000A0000) || next <= (uint32_t)frame) {
      break;
    }
    frame = (volatile uint32_t*)next;
  }
  return row;
}

/* Opcional: imprime em decimal (sem libc) */
static void print_dec(uint32_t v, int row, int* col) {
  char buf[12];
  int n = 0;
  if (v == 0) {
    buf[n++] = '0';
  } else {
    while (v > 0 && n < 11) {
      buf[n++] = (char)('0' + (v % 10));
      v /= 10;
    }
  }
  while (n > 0)
    vga_putc(row, (*col)++, buf[--n]);
}

void isr_dispatch(isr_frame_t* f) {
  __asm__ volatile ("cli");

  /* IRQ 32..47: enviar EOI e retornar (nao travar) */
  if (f->int_no >= 32 && f->int_no <= 47) {
    irq_handle(f);
    return;
  }

  /* Excecao 0..31: panic e halt */
  {
    uint64_t cr2 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    serial_puts("[exc] vector=0x");
    serial_hex32((uint32_t)f->int_no);
    serial_puts(" err=0x");
    serial_hex32((uint32_t)f->err_code);
    serial_puts(" rip=0x");
    serial_hex32((uint32_t)(f->eip >> 32));
    serial_hex32((uint32_t)f->eip);
    serial_puts(" rsp=0x");
    serial_hex32((uint32_t)(f->esp_dummy >> 32));
    serial_hex32((uint32_t)f->esp_dummy);
    serial_puts(" cr2=0x");
    serial_hex32((uint32_t)(cr2 >> 32));
    serial_hex32((uint32_t)cr2);
    serial_puts("\n");
  }
  /* Se estiver em modo grafico, volta a modo texto para panic ser visivel */
  vga_force_text_mode();

  /* Limpa tela VGA */
  {
    int i;
    for (i = 0; i < 80 * 25; i++)
      VGA[i] = (uint16_t)0x1F20;
  }

  int row = 0;
  int c;

  /* Linha 0: EXCEPTION */
  c = 0;
  vga_puts_at(row++, &c, "!!! EXCEPTION !!!");

  /* Linha 1: int_no (vetor) */
  c = 0;
  vga_puts_at(row, &c, "vector: 0x");
  print_hex(f->int_no & 0xFF, row, &c);
  row++;

  /* Linha 2: err_code */
  c = 0;
  vga_puts_at(row, &c, "err_code: 0x");
  print_hex(f->err_code, row, &c);
  row++;

  /* Linha 3: EIP */
  c = 0;
  vga_puts_at(row, &c, "EIP: 0x");
  print_hex(f->eip, row, &c);
  row++;

  /* Linha 4: ESP/EBP */
  c = 0;
  vga_puts_at(row, &c, "ESP: 0x");
  print_hex(f->esp_dummy, row, &c);
  vga_puts_at(row, &c, " EBP: 0x");
  print_hex(f->ebp, row, &c);
  row++;

  /* Linha 4: CS */
  c = 0;
  vga_puts_at(row, &c, "CS: 0x");
  print_hex(f->cs, row, &c);
  row++;

  /* Linha 5: SS/DS */
  {
    uint32_t ss = 0, ds = 0;
    __asm__ volatile ("movw %%ss, %w0" : "=r"(ss));
    __asm__ volatile ("movw %%ds, %w0" : "=r"(ds));
    c = 0;
    vga_puts_at(row, &c, "SS: 0x");
    print_hex(ss, row, &c);
    vga_puts_at(row, &c, " DS: 0x");
    print_hex(ds, row, &c);
    row++;
  }

  /* Linha 6: EFLAGS */
  c = 0;
  vga_puts_at(row, &c, "EFLAGS: 0x");
  print_hex(f->eflags, row, &c);
  row++;

  row++;
  c = 0;
  vga_puts_at(row, &c, "System halted.");

  row++;
  row = dump_stack_words(f->esp_dummy, row);
  row = dump_backtrace(f->ebp, row);

  trace_dump_vga(row);

  for (;;) {
    __asm__ volatile ("hlt");
  }
}

void isr_df_panic(isr_frame_t* f) {
  (void)f;
  __asm__ volatile ("cli");
  serial_puts("[exc] DOUBLE FAULT (#DF)\n");
  vga_force_text_mode();
  {
    int i;
    for (i = 0; i < 80 * 25; i++)
      VGA[i] = (uint16_t)0x1F20;
  }
  int c = 0;
  int row = 0;
  vga_puts_at(row++, &c, "!!! DOUBLE FAULT (#DF) !!!");
  c = 0;
  vga_puts_at(row++, &c, "Panic stack in use. System halted.");
  for (;;) {
    __asm__ volatile ("hlt");
  }
}
