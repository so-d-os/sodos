#include "shell.h"
#include "mm.h"

static void put_u32(uint32_t n) {
  char num[16];
  uint32_to_string(n, num, (int)sizeof(num));
  puts_k(num);
}

static void put_hex(uint32_t n) {
  static const char hex[] = "0123456789ABCDEF";
  int i;
  puts_k("0x");
  for (i = 7; i >= 0; i--)
    putc(hex[(n >> (i * 4)) & 0xF]);
}

int cmd_free(int argc, char** argv) {
  uint32_t total, used, free_b, largest;
  (void)argc;
  (void)argv;
  mm_stats(&total, &used, &free_b, &largest);
  puts_k("Heap ");
  put_hex(mm_heap_start());
  puts_k("-");
  put_hex(mm_heap_end());
  puts_k("\n");
  if (!mm_ready()) {
    puts_k("Heap indisponivel: o kernel passou do inicio da area livre.\n");
    return 1;
  }
  puts_k("Total  ");
  put_u32(total);
  puts_k(" bytes\nUsado  ");
  put_u32(used);
  puts_k(" bytes\nLivre  ");
  put_u32(free_b);
  puts_k(" bytes\nMaior bloco livre  ");
  put_u32(largest);
  puts_k(" bytes\n");
  if (um_ready()) {
    um_stats(&total, &used, &free_b, &largest);
    puts_k("\nHeap alto (programas) ");
    put_hex(um_start());
    puts_k("-");
    put_hex(um_end());
    puts_k("\nTotal  ");
    put_u32(total / 1024u);
    puts_k(" KB\nUsado  ");
    put_u32(used / 1024u);
    puts_k(" KB\nLivre  ");
    put_u32(free_b / 1024u);
    puts_k(" KB\n");
  } else {
    puts_k("\nHeap alto indisponivel (pouca RAM).\n");
  }
  return 0;
}
