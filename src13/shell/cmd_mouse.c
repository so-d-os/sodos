#include "shell.h"
#include "reboot.h"
#include "net.h"
#include "xhci.h"

#define SCANCODE_ESC 0x01

static void ps2_wait_write(void) {
  while ((shell_inb(0x64) & 2)) { }
}

/* 1 se um mouse PS/2 respondeu 0xFA (com o bit de porta auxiliar) ao F4. */
static int ps2_mouse_init(void) {
  uint32_t t0, spins = 0;
  int ack = 0;
  ps2_wait_write();
  shell_outb(0x64, 0xA8);
  for (volatile int i = 0; i < 10000; i++) { }
  ps2_wait_write();
  shell_outb(0x64, 0xD4);
  ps2_wait_write();
  shell_outb(0x60, 0xF4);
  t0 = net_ms();
  for (;;) {
    uint8_t st = shell_inb(0x64);
    if (st & 1) {
      uint8_t b = shell_inb(0x60);
      if ((st & 0x20) && b == 0xFA) ack = 1;
      continue;
    }
    if (ack) break;
    if (net_has_clock() ? (net_ms() - t0) >= 50u : ++spins > 2000000u) break;
  }
  return ack;
}

int cmd_mouse(int argc, char** argv) {
  (void)argc;
  (void)argv;
  int ps2_ok = ps2_mouse_init();
  int usb = xhci_mouse_init(ps2_ok) == 0;
  if (!ps2_ok && !usb) {
    puts_k("Mouse: nenhum mouse PS/2 nem USB (xHCI).\n");
    return 1;
  }
  clear_screen();
  puts_k(usb ? "=== Mouse (USB) - posicao e botoes ===\n"
             : "=== Mouse (PS/2) - posicao e botoes ===\n");
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
    int dx, dy, dz, b;
    if (usb && xhci_mouse_poll(&dx, &dy, &dz, &b)) {
      (void)dz;
      btn_left  = (b & 1) ? 1 : 0;
      btn_right = (b & 2) ? 1 : 0;
      btn_mid   = (b & 4) ? 1 : 0;
      pos_x += dx;
      pos_y += dy;
      packet[0] = (uint8_t)b;
      packet[1] = (uint8_t)dx;
      packet[2] = (uint8_t)dy;
    } else {
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
        kbd_watch(sc);
        if (sc == SCANCODE_ESC) break;
      }
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
