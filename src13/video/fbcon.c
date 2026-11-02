/* fbcon.c - modo texto 80x25 no framebuffer do UEFI.  Ver fbcon.h. */
#include <stdint.h>
#include "bootinfo.h"
#include "serial.h"
#include "fbcon.h"

#define COLS 80
#define ROWS 25
#define CELL_W 8
#define CELL_H 16

extern const uint8_t font8x16_cp437[256][16];

volatile uint16_t* vga_text = (volatile uint16_t*)0xB8000;

static uint16_t shadow[COLS * ROWS];
static uint16_t drawn[COLS * ROWS];
static int active;
static int suspended;
static volatile uint8_t* fb;
static uint32_t fb_pitch;
static uint32_t fb_w;
static uint32_t fb_h;
static int scale;
static uint32_t org_x;
static uint32_t org_y;
static int cur_pos = -1;
static int cur_drawn = -1;
static uint32_t pal[16];

/* Paleta CGA/VGA padrao do modo texto. */
static const uint8_t cga_rgb[16][3] = {
  {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
  {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
  {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
  {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF}
};

int vga_legacy(void) {
  const struct BootInfo* bi = bootinfo_get();
  return !bi || bi->firmware != BOOTINFO_FIRMWARE_UEFI;
}

int fbcon_active(void) { return active; }

static void fb_clear(void) {
  uint32_t y, x;
  for (y = 0; y < fb_h; y++) {
    volatile uint32_t* row = (volatile uint32_t*)(fb + (uint64_t)y * fb_pitch);
    for (x = 0; x < fb_w; x++) row[x] = 0;
  }
}

int fbcon_init(const struct BootInfo* bi) {
  uint32_t sx, sy;
  int i;
  if (!bi || bi->firmware != BOOTINFO_FIRMWARE_UEFI) return 0;
  if (!bi->fb_addr || bi->fb_bpp != 32 || bi->fb_width < COLS * CELL_W ||
      bi->fb_height < ROWS * CELL_H) {
    serial_puts("[fbcon] UEFI sem framebuffer de 32 bpp: tela vazia, so serial\n");
    return 0;
  }
  fb = (volatile uint8_t*)(uintptr_t)bi->fb_addr;
  fb_pitch = bi->fb_pitch;
  fb_w = bi->fb_width;
  fb_h = bi->fb_height;
  sx = bi->fb_width / (COLS * CELL_W);
  sy = bi->fb_height / (ROWS * CELL_H);
  scale = (int)(sx < sy ? sx : sy);
  if (scale > 4) scale = 4;
  org_x = (bi->fb_width - (uint32_t)(COLS * CELL_W * scale)) / 2u;
  org_y = (bi->fb_height - (uint32_t)(ROWS * CELL_H * scale)) / 2u;
  for (i = 0; i < 16; i++) {
    uint32_t r = cga_rgb[i][0], g = cga_rgb[i][1], b = cga_rgb[i][2];
    /* FORMAT_BGR: bytes B,G,R na memoria (vermelho no bit 16), como o Bochs. */
    pal[i] = bi->fb_format == BOOTINFO_FB_FORMAT_RGB ? (b << 16 | g << 8 | r)
                                                     : (r << 16 | g << 8 | b);
  }
  fb_clear();  /* o GRUB deixa texto dele nas bordas */
  for (i = 0; i < COLS * ROWS; i++) {
    shadow[i] = 0x0720;
    drawn[i] = 0;
  }
  vga_text = shadow;
  active = 1;
  serial_puts("[fbcon] texto 80x25 no framebuffer 0x");
  serial_hex32((uint32_t)(bi->fb_addr >> 32));
  serial_hex32((uint32_t)bi->fb_addr);
  serial_puts(" escala ");
  serial_hex32((uint32_t)scale);
  serial_puts("\n");
  return 1;
}

static void draw_cell(int idx, uint16_t cell, int with_cursor) {
  const uint8_t* g = font8x16_cp437[cell & 0xFFu];
  uint32_t fg = pal[(cell >> 8) & 0x0Fu];
  uint32_t bg = pal[(cell >> 12) & 0x0Fu];
  uint32_t x0 = org_x + (uint32_t)(idx % COLS) * CELL_W * (uint32_t)scale;
  uint32_t y0 = org_y + (uint32_t)(idx / COLS) * CELL_H * (uint32_t)scale;
  int r, sy, b, sx;
  for (r = 0; r < CELL_H; r++) {
    uint8_t bits = g[r];
    if (with_cursor && r >= CELL_H - 2) bits = 0xFF;
    for (sy = 0; sy < scale; sy++) {
      volatile uint32_t* row = (volatile uint32_t*)(fb + (uint64_t)(y0 + (uint32_t)(r * scale + sy)) * fb_pitch) + x0;
      for (b = 0; b < CELL_W; b++) {
        uint32_t c = (bits & (0x80u >> b)) ? fg : bg;
        for (sx = 0; sx < scale; sx++) *row++ = c;
      }
    }
  }
}

void fbcon_sync(void) {
  int i;
  if (!active || suspended) return;
  for (i = 0; i < COLS * ROWS; i++) {
    uint16_t c = shadow[i];
    if (c != drawn[i] || i == cur_pos || i == cur_drawn) {
      draw_cell(i, c, i == cur_pos);
      drawn[i] = c;
    }
  }
  cur_drawn = cur_pos;
}

void fbcon_cursor(int pos) {
  if (!active) return;
  cur_pos = (pos >= 0 && pos < COLS * ROWS) ? pos : -1;
  fbcon_sync();
}

void fbcon_suspend(int on) {
  int i;
  if (!active) return;
  suspended = on;
  if (on) return;
  /* O desktop pintou por cima: tudo precisa ser desenhado de novo. */
  fb_clear();
  for (i = 0; i < COLS * ROWS; i++) drawn[i] = (uint16_t)~shadow[i];
  cur_drawn = -1;
  fbcon_sync();
}
