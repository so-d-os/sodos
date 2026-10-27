#include "shell.h"

#define SCANCODE_ESC 0x01

static void ps2_wait_write(void) {
  while ((shell_inb(0x64) & 2)) { }
}

static int ps2_mouse_init(void) {
  ps2_wait_write();
  shell_outb(0x64, 0xA8);
  for (volatile int i = 0; i < 10000; i++) { }
  ps2_wait_write();
  shell_outb(0x64, 0xD4);
  ps2_wait_write();
  shell_outb(0x60, 0xF4);
  for (volatile int i = 0; i < 50000; i++) { }
  while (shell_inb(0x64) & 1) (void)shell_inb(0x60);
  return 0;
}

int cmd_mouse(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (ps2_mouse_init() != 0) {
    puts_k("Mouse: init failed.\n");
    return 1;
  }
  clear_screen();
  puts_k("=== Mouse (PS/2) - posicao e botoes ===\n");
  puts_k("ESC = sair\n\n");

  int32_t pos_x = 0, pos_y = 0;
  uint8_t btn_left = 0, btn_right = 0, btn_mid = 0;
  uint8_t packet[3] = { 0, 0, 0 };
  int pkt_idx = 0;
  char num[24];
  const int row_x = 4, row_y = 5, row_btns = 6, row_raw = 7;

  goto_xy(row_x, 0); puts_k("X = 0    "); clear_line_from_cursor();
  goto_xy(row_y, 0); puts_k("Y = 0    "); clear_line_from_cursor();
  goto_xy(row_btns, 0); puts_k("Botoes: L=0 R=0 M=0    "); clear_line_from_cursor();
  goto_xy(row_raw, 0); puts_k("Pacote (hex): 00 00 00   "); clear_line_from_cursor();

  for (;;) {
    if (!(shell_inb(0x64) & 1)) continue;
    if (shell_inb(0x64) & 0x20) {
      uint8_t b = shell_inb(0x60);
      if (pkt_idx == 0 && !(b & 0x08)) continue;
      packet[pkt_idx] = b;
      pkt_idx++;
      if (pkt_idx >= 3) {
        pkt_idx = 0;
        btn_left  = (packet[0] & 0x01) ? 1 : 0;
        btn_right = (packet[0] & 0x02) ? 1 : 0;
        btn_mid   = (packet[0] & 0x04) ? 1 : 0;
        {
          int8_t dx = (int8_t)packet[1];
          int8_t dy = (int8_t)packet[2];
          pos_x += dx;
          pos_y -= dy;
        }
      }
    } else {
      uint8_t sc = shell_inb(0x60);
      if (sc == SCANCODE_ESC) break;
    }

    goto_xy(row_x, 0);
    puts_k("X = ");
    int32_to_string(pos_x, num, sizeof(num));
    puts_k(num);
    puts_k("    ");
    clear_line_from_cursor();

    goto_xy(row_y, 0);
    puts_k("Y = ");
    int32_to_string(pos_y, num, sizeof(num));
    puts_k(num);
    puts_k("    ");
    clear_line_from_cursor();

    goto_xy(row_btns, 0);
    puts_k("Botoes: L=");
    putc(btn_left ? '1' : '0');
    puts_k(" R=");
    putc(btn_right ? '1' : '0');
    puts_k(" M=");
    putc(btn_mid ? '1' : '0');
    puts_k("    ");
    clear_line_from_cursor();

    goto_xy(row_raw, 0);
    puts_k("Pacote (hex): ");
    {
      char hex[] = "00 ";
      for (int i = 0; i < 3; i++) {
        unsigned v = packet[i];
        hex[0] = (v >> 4) > 9 ? (char)('A' + (v >> 4) - 10) : (char)('0' + (v >> 4));
        hex[1] = (v & 15) > 9 ? (char)('A' + (v & 15) - 10) : (char)('0' + (v & 15));
        puts_k(hex);
      }
    }
    clear_line_from_cursor();
  }

  puts_k("\nMouse encerrado. (ESC)\n");
  return 0;
}
