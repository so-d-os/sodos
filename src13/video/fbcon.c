/* fbcon.c - modo texto 80x25 no framebuffer do UEFI.  Ver fbcon.h. */
#include <stdint.h>
#include "bootinfo.h"
#include "serial.h"
#include "fbcon.h"
#include "visual.h"

#define COLS 80
#define ROWS 25
#define CELL_W 8
#define CELL_H 16

volatile uint16_t* vga_text = (volatile uint16_t*)0xB8000;

static uint16_t shadow[COLS * ROWS];
static uint16_t drawn[COLS * ROWS];
static int active;
static int suspended;
static volatile uint8_t* fb;
static uint32_t fb_pitch;
static uint32_t fb_w;
static uint32_t fb_h;
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

/* As 80x25 celulas ocupam a tela toda, como o modo texto VGA num monitor LCD:
 * a celula c vai de c*largura/80 ate (c+1)*largura/80 (idem nas linhas) e a
 * fonte 8x16 e esticada por vizinho mais proximo. Em 1920x1080 cada celula
 * tem 24x43 pixels. (Antes: escala inteira centralizada, 1280x800 no meio de
 * um monitor Full HD.) */
static uint32_t cell_x(int c) { return (uint32_t)((uint64_t)(uint32_t)c * fb_w / COLS); }
static uint32_t cell_y(int r) { return (uint32_t)((uint64_t)(uint32_t)r * fb_h / ROWS); }

int fbcon_init(const struct BootInfo* bi) {
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
  serial_puts(", tela cheia\n");
  return 1;
}

/* "Video: 1920x1080, 32 bpp, texto 80x25 em tela cheia\n" para o boot. */
void fbcon_mode_str(char* out, int max) {
  static const char tail[] = ", 32 bpp, texto 80x25 em tela cheia\n";
  char num[2][12];
  uint32_t v[2];
  int n = 0, k, i;
  const char* p;
  if (!out || max < 2) return;
  out[0] = 0;
  if (!active) return;
  v[0] = fb_w;
  v[1] = fb_h;
  for (k = 0; k < 2; k++) {
    char t[12];
    int m = 0;
    uint32_t x = v[k];
    do { t[m++] = (char)('0' + x % 10u); x /= 10u; } while (x && m < 11);
    for (i = 0; i < m; i++) num[k][i] = t[m - 1 - i];
    num[k][m] = 0;
  }
  for (p = "Video: "; *p && n < max - 1; p++) out[n++] = *p;
  for (p = num[0]; *p && n < max - 1; p++) out[n++] = *p;
  if (n < max - 1) out[n++] = 'x';
  for (p = num[1]; *p && n < max - 1; p++) out[n++] = *p;
  for (p = tail; *p && n < max - 1; p++) out[n++] = *p;
  out[n] = 0;
}

static void draw_cell(int idx, uint16_t cell, int with_cursor) {
  const uint8_t* g = vga_font_glyph((int)(cell & 0xFFu));
  uint32_t fg = pal[(cell >> 8) & 0x0Fu];
  uint32_t bg = pal[(cell >> 12) & 0x0Fu];
  int col = idx % COLS, lin = idx / COLS;
  uint32_t x0 = cell_x(col), x1 = cell_x(col + 1);
  uint32_t y0 = cell_y(lin), y1 = cell_y(lin + 1);
  uint32_t cw = x1 - x0, ch = y1 - y0, py, px;
  uint8_t colbit[128]; /* coluna da fonte -> mascara, por pixel da celula (ate 10240 px) */
  if (!cw || !ch || cw > 128u) return;
  for (px = 0; px < cw; px++) colbit[px] = (uint8_t)(0x80u >> (px * CELL_W / cw));
  for (py = 0; py < ch; py++) {
    uint32_t r = py * CELL_H / ch;
    uint8_t bits = g[r];
    volatile uint32_t* row = (volatile uint32_t*)(fb + (uint64_t)(y0 + py) * fb_pitch) + x0;
    if (with_cursor && r >= CELL_H - 2) bits = 0xFF;
    for (px = 0; px < cw; px++) row[px] = (bits & colbit[px]) ? fg : bg;
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

void fbcon_font_changed(int ch) {
  int i;
  if (!active) return;
  for (i = 0; i < COLS * ROWS; i++)
    if (ch < 0 || (shadow[i] & 0xFFu) == (uint16_t)ch) drawn[i] = (uint16_t)~shadow[i];
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
