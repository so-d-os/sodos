/*
 * visual.c - Modo grafico
 * QEMU (-vga std, 16 MB) e v86 (WebAssembly, 16 MB) falam Bochs VBE.
 * Tenta 1920x1080, depois modos menores, em 32 bpp.
 * Sem Bochs VBE: VGA modo 13h 320x200.
 */
#include <stdint.h>
#include "serial.h"
#include "shell.h"
#include "pic.h"
#include "drives.h"
#include "mm.h"
#include "module.h"
#include "kexports.h"
#include "net.h"
#include "sched.h"
#include "visual.h"
#include "c_compiler/sox.h"

static inline void outl(uint16_t port, uint32_t v) {
  __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t v;
  __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t off) {
  uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                  ((uint32_t)fun << 8) | ((uint32_t)(off & 0xFCu));
  outl(0xCF8, addr);
  return inl(0xCFC);
}

static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t off, uint32_t val) {
  uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                  ((uint32_t)fun << 8) | ((uint32_t)(off & 0xFCu));
  outl(0xCF8, addr);
  outl(0xCFC, val);
}

/* BAR0 memoria do primeiro dispositivo classe Display (PCI class 0x03) */
static uint32_t pci_find_display_bar0(void) {
  for (uint8_t dev = 0; dev < 32; dev++) {
    uint32_t vid = pci_cfg_read32(0, dev, 0, 0);
    if (vid == 0xFFFFFFFFu) continue;
    uint32_t cls = pci_cfg_read32(0, dev, 0, 0x08);
    if (((cls >> 24) & 0xFFu) != 0x03u) continue;
    uint32_t bar0 = pci_cfg_read32(0, dev, 0, 0x10);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu) continue;
    if (bar0 & 1u) continue;
    return bar0 & ~0xFu;
  }
  return 0;
}

static int lfb_probe_rw(volatile uint8_t* base) {
  volatile uint32_t* w = (volatile uint32_t*)base;
  uint32_t save = *w;
  uint32_t pat = 0x12345678u;
  *w = pat;
  int ok = (*w == pat);
  *w = save;
  return ok;
}

extern const uint8_t font8x8_basic[95][8];
extern uint32_t mod_depth;

/* Fonte 8x16 da BIOS (256 chars x 32 bytes na plane 2). */
static uint8_t vga_bios_font[256 * 32];
static int vga_bios_font_ok;
static int font_cell_w = 8;
static int font_cell_h = 16;

static void vga_font_plane_enter(void);
static void vga_font_plane_leave(void);
static void vga_restore_bios_font(void);

#define FB13_ADDR      0xA0000u
#define FB13_W         320u
#define FB13_H         200u

/* Bochs VBE (OSDev wiki) */
#define VBE_DISPI_IOPORT_INDEX     0x01CEu
#define VBE_DISPI_IOPORT_DATA      0x01CFu
#define VBE_DISPI_INDEX_ID         0x00u
#define VBE_DISPI_INDEX_XRES       0x01u
#define VBE_DISPI_INDEX_YRES       0x02u
#define VBE_DISPI_INDEX_BPP        0x03u
#define VBE_DISPI_INDEX_ENABLE     0x04u
#define VBE_DISPI_INDEX_VIRT_WIDTH 0x06u
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x07u
#define VBE_DISPI_INDEX_X_OFFSET   0x08u
#define VBE_DISPI_INDEX_Y_OFFSET   0x09u
#define VBE_DISPI_INDEX_VRAM_64K   0x0Au
#define VBE_DISPI_DISABLED         0x00u
#define VBE_DISPI_ENABLED          0x01u
#define VBE_DISPI_LFB_ENABLED      0x40u

#define BOCHS_LFB_PHYS           0xE0000000u

/* 1920x1080x32 com pagina dupla cabe em 16 MB (QEMU -vga std e v86). */
static const uint16_t gfx_mode_w[] = { 1920u, 1600u, 1280u, 1024u, 800u, 640u };
static const uint16_t gfx_mode_h[] = { 1080u, 1200u, 1024u, 768u, 600u, 480u };
static const char* const gfx_mode_name[] = {
  "1920x1080", "1600x1200", "1280x1024", "1024x768", "800x600", "640x480"
};
#define GFX_MODE_N ((int)(sizeof(gfx_mode_w) / sizeof(gfx_mode_w[0])))
/* -1 = a maior que couber (padrao ao abrir o win). Outro valor e a escolha do menu. */
static int gfx_mode_sel = -1;

/* BGRA em memoria: B,G,R,A — empacota em uint32 para APIs */
#define PIXA(r, g, b) \
  ((uint32_t)(uint8_t)(b) << 16 | (uint32_t)(uint8_t)(g) << 8 | (uint32_t)(uint8_t)(r) | (uint32_t)0xFFu << 24)

#define SCANCODE_LSHIFT  0x2A
#define SCANCODE_RSHIFT  0x36
#define SCANCODE_ESC     0x01

#define FONT_W  8
#define FONT_H  16

static inline void outb(uint16_t port, uint8_t data) {
  __asm__ volatile ("outb %0, %1" : : "a" (data), "Nd" (port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t data;
  __asm__ volatile ("inb %1, %0" : "=a" (data) : "Nd" (port));
  return data;
}

static inline void outw(uint16_t port, uint16_t v) {
  __asm__ volatile ("outw %0, %1" : : "a" (v), "Nd" (port));
}

static inline uint16_t inw(uint16_t port) {
  uint16_t v;
  __asm__ volatile ("inw %1, %0" : "=a" (v) : "Nd" (port));
  return v;
}

static void bochs_vbe_write(uint16_t index, uint16_t value) {
  outw(VBE_DISPI_IOPORT_INDEX, index);
  outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t bochs_vbe_read(uint16_t index) {
  outw(VBE_DISPI_IOPORT_INDEX, index);
  return inw(VBE_DISPI_IOPORT_DATA);
}

/* 1 = Bochs LFB 32 bpp; 0 = modo 13h 8 bpp */
static int gfx_bochs;
static int gfx_live;
static uint32_t gfx_w;
static uint32_t gfx_h;
static uint32_t gfx_pitch_bytes;
static volatile uint8_t* const fb13 = (volatile uint8_t*)FB13_ADDR;
static volatile uint8_t* gfx_lfb;

/* Segunda pagina de video (ou RAM em 1 MB) para desenhar fora da tela visivel. */
#define GFX_BACK_PHYS 0x100000u
static uint32_t gfx_draw_y0;
static uint32_t gfx_front_y;
static int gfx_dbuf;
static int gfx_offscreen;
static uint8_t* gfx_back;

static void set_palette_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  outb(0x3C8, index);
  outb(0x3C9, r);
  outb(0x3C9, g);
  outb(0x3C9, b);
}

static void vga_mode13h(void) {
  outb(0x3D4, 0x11);
  outb(0x3D5, inb(0x3D5) & 0x7F);
  outb(0x3C2, 0x63);
  const uint8_t seq[] = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
  for (int i = 0; i < 5; i++) {
    outb(0x3C4, i);
    outb(0x3C5, seq[i]);
  }
  const uint8_t crtc[] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
  };
  for (int i = 0; i < 25; i++) {
    outb(0x3D4, i);
    outb(0x3D5, crtc[i]);
  }
  for (int i = 0; i < 16; i++) {
    (void)inb(0x3DA);
    outb(0x3C0, i);
    outb(0x3C0, i);
  }
  (void)inb(0x3DA);
  outb(0x3C0, 0x20);
  const uint8_t gdc[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF };
  for (int i = 0; i < 9; i++) {
    outb(0x3CE, i);
    outb(0x3CF, gdc[i]);
  }
}

static void vga_font_plane_enter(void) {
  outb(0x3C4, 0x02);
  outb(0x3C5, 0x04);
  outb(0x3C4, 0x04);
  outb(0x3C5, 0x07);
  outb(0x3CE, 0x04);
  outb(0x3CF, 0x02);
  outb(0x3CE, 0x05);
  outb(0x3CF, 0x00);
  outb(0x3CE, 0x06);
  outb(0x3CF, 0x00);
}

static void vga_text_cursor_underline(void) {
  /* Cursor so nas 2 ultimas linhas do glifo 8x16 — evita a barra branca larga. */
  outb(0x3D4, 0x0A);
  outb(0x3D5, 0x0E);
  outb(0x3D4, 0x0B);
  outb(0x3D5, 0x0F);
}

static void vga_font_plane_leave(void) {
  outb(0x3C4, 0x00);
  outb(0x3C5, 0x03);
  outb(0x3C4, 0x01);
  outb(0x3C5, 0x00); /* 9 pontos (720x400), como o BIOS */
  outb(0x3C4, 0x02);
  outb(0x3C5, 0x03);
  outb(0x3C4, 0x03);
  outb(0x3C5, 0x00);
  outb(0x3C4, 0x04);
  outb(0x3C5, 0x02);
  outb(0x3CE, 0x04);
  outb(0x3CF, 0x00);
  outb(0x3CE, 0x05);
  outb(0x3CF, 0x10);
  outb(0x3CE, 0x06);
  outb(0x3CF, 0x0E);
  vga_text_cursor_underline();
}

void vga_capture_bios_font(void) {
  volatile uint8_t* vga = (volatile uint8_t*)0xA0000;
  int nonzero = 0;
  vga_font_plane_enter();
  for (int i = 0; i < 256 * 32; i++) {
    uint8_t b = vga[i];
    vga_bios_font[i] = b;
    if (b) nonzero = 1;
  }
  vga_font_plane_leave();
  vga_bios_font_ok = nonzero;
  serial_puts(nonzero ? "[vga] fonte BIOS 8x16 guardada\n" : "[vga] fonte BIOS vazia\n");
}

static void vga_restore_bios_font(void) {
  volatile uint8_t* vga = (volatile uint8_t*)0xA0000;
  vga_font_plane_enter();
  if (vga_bios_font_ok) {
    for (int i = 0; i < 256 * 32; i++)
      vga[i] = vga_bios_font[i];
  } else {
    for (int ch = 0; ch < 256; ch++) {
      volatile uint8_t* dst = vga + ch * 32;
      const uint8_t* src = (ch >= 32 && ch <= 126) ? font8x8_basic[ch - 32] : 0;
      for (int row = 0; row < 32; row++) {
        if (src && row < 16)
          dst[row] = (row < 8) ? src[row] : 0;
        else
          dst[row] = 0;
      }
    }
  }
  vga_font_plane_leave();
}

/* Modo texto 80x25, 9 pontos, 720x400 — o mesmo do VGA BIOS (DOS). */
static void vga_mode_text(void) {
  outb(0x3C2, 0x67);
  outb(0x3D4, 0x11);
  outb(0x3D5, inb(0x3D5) & 0x7F);
  static const uint8_t crtc_mode3[] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
  };
  for (int i = 0; i < 25; i++) {
    outb(0x3D4, i);
    outb(0x3D5, crtc_mode3[i]);
  }
  static const uint8_t seq_mode3[] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
  for (int i = 0; i < 5; i++) {
    outb(0x3C4, i);
    outb(0x3C5, seq_mode3[i]);
  }
  static const uint8_t gdc_mode3[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF };
  for (int i = 0; i < 9; i++) {
    outb(0x3CE, i);
    outb(0x3CF, gdc_mode3[i]);
  }
  (void)inb(0x3DA);
  for (int i = 0; i < 16; i++) {
    outb(0x3C0, i);
    outb(0x3C0, i);
  }
  outb(0x3C0, 0x10);
  outb(0x3C0, 0x0C);
  outb(0x3C0, 0x11);
  outb(0x3C0, 0x00);
  outb(0x3C0, 0x12);
  outb(0x3C0, 0x0F);
  outb(0x3C0, 0x13);
  outb(0x3C0, 0x08);
  outb(0x3C0, 0x14);
  outb(0x3C0, 0x00);
  (void)inb(0x3DA);
  outb(0x3C0, 0x20);
  vga_restore_bios_font();
  vga_text_cursor_underline();
}

static void vga_dac_restore_std16(void) {
  static const uint8_t rgb[16][3] = {
    {0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42},
    {42, 0, 0}, {42, 0, 42}, {42, 21, 0}, {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
    {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}
  };
  for (int i = 0; i < 16; i++) {
    outb(0x3C8, (uint8_t)i);
    outb(0x3C9, rgb[i][0]);
    outb(0x3C9, rgb[i][1]);
    outb(0x3C9, rgb[i][2]);
  }
}

void vga_force_text_mode(void) {
  bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
  vga_mode_text();
  vga_dac_restore_std16();
}

static void serial_u16(uint16_t v) {
  char buf[8];
  int i = (int)sizeof(buf) - 1;
  buf[i] = '\0';
  if (v == 0) {
    serial_puts("0");
    return;
  }
  while (v && i > 0) {
    buf[--i] = (char)('0' + (v % 10u));
    v = (uint16_t)(v / 10u);
  }
  serial_puts(buf + i);
}

/* 1 se o indice 0x0A trouxe um tamanho crivel (v86 e QEMU). */
static int bochs_vram_bytes(uint32_t* out) {
  uint16_t vram64 = bochs_vbe_read(VBE_DISPI_INDEX_VRAM_64K);
  if (vram64 == 0 || vram64 >= 4096u) return 0;
  *out = (uint32_t)vram64 << 16;
  return 1;
}

static int bochs_present(void) {
  uint16_t id = bochs_vbe_read(VBE_DISPI_INDEX_ID);
  return !(id == 0 || id == 0xFFFFu || (id & 0xFF00u) != 0xB000u);
}

/* Programa um modo 32 bpp. Altura virtual = altura visivel: o dobro
 * fazia o QEMU esticar a imagem na janela. */
static int bochs_program(uint16_t w, uint16_t h) {
  uint32_t vram = 0;
  uint16_t vw;
  uint32_t need = (uint32_t)w * (uint32_t)h * 4u;

  bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
  bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
  bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
  bochs_vbe_write(VBE_DISPI_INDEX_XRES, w);
  bochs_vbe_write(VBE_DISPI_INDEX_YRES, h);
  bochs_vbe_write(VBE_DISPI_INDEX_BPP, 32);
  bochs_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, w);
  bochs_vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, h);
  bochs_vbe_write(VBE_DISPI_INDEX_ENABLE,
                  (uint16_t)(VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED));
  bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
  bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
  if (bochs_vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH) != w)
    bochs_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, w);
  if (bochs_vbe_read(VBE_DISPI_INDEX_VIRT_HEIGHT) != h)
    bochs_vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, h);

  if (bochs_vbe_read(VBE_DISPI_INDEX_XRES) != w ||
      bochs_vbe_read(VBE_DISPI_INDEX_YRES) != h ||
      bochs_vbe_read(VBE_DISPI_INDEX_BPP) != 32) {
    bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    return 0;
  }
  if (bochs_vram_bytes(&vram) && vram < need) {
    bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    return 0;
  }

  gfx_w = w;
  gfx_h = h;
  vw = bochs_vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH);
  /* v86 devolve a largura da tela; valor 0 ou 1 nao e pitch. */
  if (vw < w || vw > 4096u) vw = w;
  gfx_pitch_bytes = (uint32_t)vw * 4u;
  return 1;
}

static int bochs_bind_lfb(void) {
  /* QEMU e v86: LFB em 0xE0000000. BAR PCI depois, para nao aceitar BAR errado. */
  uint32_t cand[4];
  int nc = 0;
  int j;
  cand[nc++] = BOCHS_LFB_PHYS;
  {
    uint32_t bar = pci_find_display_bar0();
    if (bar && bar != BOCHS_LFB_PHYS) cand[nc++] = bar;
  }
  cand[nc++] = 0xFD000000u;
  for (j = 0; j < nc; j++) {
    int k;
    for (k = 0; k < j; k++)
      if (cand[k] == cand[j]) break;
    if (k < j) continue;
    gfx_lfb = (volatile uint8_t*)(uint32_t)cand[j];
    serial_puts("[visual] teste LFB em 0x");
    serial_hex32(cand[j]);
    serial_puts("\n");
    if (lfb_probe_rw(gfx_lfb)) {
      serial_puts("[visual] LFB R/W OK; pitch=");
      serial_hex32(gfx_pitch_bytes);
      serial_puts("\n");
      gfx_bochs = 1;
      return 1;
    }
  }
  return 0;
}

static int bochs_try_mode(void) {
  int i;
  int want = gfx_mode_sel;
  if (!bochs_present()) {
    serial_puts("[visual] Bochs VBE nao detectado, usando 320x200 (modo 13h)\n");
    gfx_mode_sel = -1;
    return 0;
  }

  {
    int seq[8];
    int nseq = 0;
    int step;
    if (want >= 0 && want < GFX_MODE_N)
      seq[nseq++] = want;
    for (i = 0; i < GFX_MODE_N; i++) {
      if (nseq && i == seq[0]) continue;
      seq[nseq++] = i;
    }
    for (step = 0; step < nseq; step++) {
      i = seq[step];
      if (!bochs_program(gfx_mode_w[i], gfx_mode_h[i])) {
        serial_puts("[visual] modo recusado ");
        serial_u16(gfx_mode_w[i]);
        serial_puts("x");
        serial_u16(gfx_mode_h[i]);
        serial_puts("\n");
        continue;
      }
      if (!bochs_bind_lfb()) {
        serial_puts("[visual] nenhum endereco LFB respondeu — fallback 13h\n");
        bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        gfx_lfb = 0;
        gfx_mode_sel = -1;
        return 0;
      }
      serial_puts("[visual] modo ");
      serial_u16(gfx_mode_w[i]);
      serial_puts("x");
      serial_u16(gfx_mode_h[i]);
      serial_puts("x32\n");
      /* So guarda a escolha se ela entrou. Senao a proxima abertura volta ao padrao. */
      gfx_mode_sel = (want >= 0 && i == want) ? i : -1;
      return 1;
    }
  }

  serial_puts("[visual] nenhum modo Bochs coube, usando 320x200 (modo 13h)\n");
  bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
  gfx_mode_sel = -1;
  return 0;
}

static void bochs_disable_if_needed(void) {
  if (gfx_bochs) {
    bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    gfx_bochs = 0;
  }
}

/* 1 se a escolha muda a tela. sel < 0 pede de novo a maior que couber. */
static int gfx_mode_pick(int sel) {
  if (sel < 0) {
    if (gfx_mode_sel < 0) return 0;
    gfx_mode_sel = -1;
    return 1;
  }
  if (sel >= GFX_MODE_N) return 0;
  if (gfx_mode_sel == sel) return 0;
  if (gfx_mode_sel < 0 &&
      (uint32_t)gfx_w == gfx_mode_w[sel] && (uint32_t)gfx_h == gfx_mode_h[sel])
    return 0;
  gfx_mode_sel = sel;
  return 1;
}

static volatile uint8_t* gfx_at(int x, int y) {
  if (gfx_offscreen)
    return gfx_back + ((uint32_t)y * gfx_w + (uint32_t)x) * (gfx_bochs ? 4u : 1u);
  if (gfx_bochs)
    return gfx_lfb + ((uint32_t)y + gfx_draw_y0) * gfx_pitch_bytes + (uint32_t)x * 4u;
  return fb13 + (uint32_t)y * FB13_W + (uint32_t)x;
}

static void put_pixel(int x, int y, uint32_t c) {
  volatile uint8_t* p;
  if (x < 0 || y < 0 || (uint32_t)x >= gfx_w || (uint32_t)y >= gfx_h) return;
  p = gfx_at(x, y);
  if (gfx_bochs) {
    p[0] = (uint8_t)((c >> 16) & 0xFFu);
    p[1] = (uint8_t)((c >> 8) & 0xFFu);
    p[2] = (uint8_t)(c & 0xFFu);
    p[3] = (uint8_t)((c >> 24) & 0xFFu);
  } else {
    *p = (uint8_t)(c & 0xFFu);
  }
}

static uint32_t get_pixel(int x, int y) {
  volatile uint8_t* p;
  if (x < 0 || y < 0 || (uint32_t)x >= gfx_w || (uint32_t)y >= gfx_h) return 0;
  p = gfx_at(x, y);
  if (gfx_bochs)
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | (uint32_t)p[2] | (uint32_t)p[3] << 24;
  return (uint32_t)(*p);
}

static uint32_t pix32(uint32_t c) {
  return (uint32_t)((c >> 16) & 0xFFu) |
         (uint32_t)((c >> 8) & 0xFFu) << 8 |
         (uint32_t)(c & 0xFFu) << 16 |
         (uint32_t)((c >> 24) & 0xFFu) << 24;
}

static void fill32(volatile uint32_t* dst, uint32_t v, uint32_t n) {
  __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(n) : "a"(v) : "memory");
}

static void gfx_clear(uint32_t c) {
  if (gfx_bochs) {
    uint32_t w32 = pix32(c);
    uint32_t y;
    for (y = 0; y < gfx_h; y++) {
      volatile uint32_t* row = gfx_offscreen
        ? (volatile uint32_t*)(gfx_back + y * gfx_w * 4u)
        : (volatile uint32_t*)(gfx_lfb + (y + gfx_draw_y0) * gfx_pitch_bytes);
      fill32(row, w32, gfx_w);
    }
  } else {
    uint8_t v = (uint8_t)(c & 0xFFu);
    volatile uint8_t* dst = gfx_offscreen ? gfx_back : fb13;
    uint32_t n = gfx_w * gfx_h;
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = v;
  }
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
  int x1, y1, x2, y2, dy;
  if (w <= 0 || h <= 0) return;
  x1 = x;
  y1 = y;
  x2 = x + w;
  y2 = y + h;
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > (int)gfx_w) x2 = (int)gfx_w;
  if (y2 > (int)gfx_h) y2 = (int)gfx_h;
  if (x1 >= x2 || y1 >= y2) return;
  if (gfx_bochs) {
    uint32_t w32 = pix32(color);
    uint32_t rw = (uint32_t)(x2 - x1);
    for (dy = y1; dy < y2; dy++) {
      volatile uint32_t* row = gfx_offscreen
        ? (volatile uint32_t*)(gfx_back + ((uint32_t)dy * gfx_w + (uint32_t)x1) * 4u)
        : (volatile uint32_t*)(gfx_lfb + ((uint32_t)dy + gfx_draw_y0) * gfx_pitch_bytes + (uint32_t)x1 * 4u);
      fill32(row, w32, rw);
    }
  } else {
    for (dy = y1; dy < y2; dy++)
      for (x1 = x; x1 < x + w; x1++)
        put_pixel(x1, dy, color);
  }
}

static int gfx_ram_ok(void) {
  uint32_t bytes = gfx_w * gfx_h * (gfx_bochs ? 4u : 1u);
  volatile uint32_t* p = (volatile uint32_t*)GFX_BACK_PHYS;
  volatile uint32_t* q;
  if (bytes < 8u) return 0;
  q = (volatile uint32_t*)(GFX_BACK_PHYS + bytes - 4u);
  uint32_t a = *p;
  uint32_t b = *q;
  int ok;
  *p = 0xA5A5A5A5u;
  *q = 0x5A5A5A5Au;
  ok = (*p == 0xA5A5A5A5u && *q == 0x5A5A5A5Au);
  *p = a;
  *q = b;
  return ok;
}

static void gfx_copy_back(void) {
  uint32_t y;
  if (gfx_bochs) {
    for (y = 0; y < gfx_h; y++) {
      uint32_t* src = (uint32_t*)(gfx_back + y * gfx_w * 4u);
      volatile uint32_t* dst = (volatile uint32_t*)(gfx_lfb + (y + gfx_front_y) * gfx_pitch_bytes);
      uint32_t n = gfx_w;
      __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
    }
  } else {
    uint8_t* src = gfx_back;
    volatile uint8_t* dst = fb13;
    uint32_t n = gfx_w * gfx_h;
    __asm__ volatile ("rep movsb" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
  }
}

/* Desenha o quadro inteiro fora da tela e so depois troca a pagina visivel. */
static void gfx_buf_init(void) {
  gfx_draw_y0 = 0;
  gfx_front_y = 0;
  gfx_offscreen = 0;
  gfx_dbuf = 0;
  gfx_back = 0;
  if (gfx_bochs) {
    uint32_t vram = 0;
    uint32_t frame = gfx_w * gfx_h * 4u;
    uint16_t vh = bochs_vbe_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    int vram_known = bochs_vram_bytes(&vram);
    /* Pagina dupla so com altura virtual exatamente 2*h. Valor maior estica a janela. */
    if (vram_known && vram >= frame * 2u && vh == (uint16_t)(gfx_h * 2u)) {
      gfx_dbuf = 1;
      bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
      serial_puts("[visual] tela dupla\n");
      return;
    }
  }
  if (gfx_ram_ok()) {
    gfx_back = (uint8_t*)GFX_BACK_PHYS;
    serial_puts("[visual] tela em memoria\n");
  }
}

static void gfx_frame_begin(void) {
  gfx_offscreen = 0;
  if (gfx_dbuf) {
    gfx_draw_y0 = gfx_front_y ? 0u : gfx_h;
    return;
  }
  if (gfx_back) {
    gfx_offscreen = 1;
    gfx_draw_y0 = 0;
  }
}

static void gfx_frame_end(void) {
  if (gfx_dbuf) {
    bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)gfx_draw_y0);
    gfx_front_y = gfx_draw_y0;
    gfx_draw_y0 = gfx_front_y;
    return;
  }
  if (gfx_offscreen) {
    gfx_copy_back();
    gfx_offscreen = 0;
  }
}

static const char scancode_map[128] = {
  0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a',
  's','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c',
  'v','b','n','m',',','.','/',0,'*',0,' ',
};

static const char scancode_map_shift[128] = {
  0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
  '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A',
  'S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C',
  'V','B','N','M','<','>','?',0,'*',0,' ',
};

static const uint8_t* glyph16(unsigned char c) {
  if (vga_bios_font_ok)
    return &vga_bios_font[(uint32_t)c * 32u];
  if (c >= 32 && c <= 126)
    return font8x8_basic[c - 32];
  return font8x8_basic['?' - 32];
}

static int glyph_rows(void) {
  return vga_bios_font_ok ? FONT_H : 8;
}

static void draw_char_scaled(int px, int py, char c, uint32_t fg, uint32_t bg, int scale) {
  const uint8_t* g = glyph16((unsigned char)c);
  int rows = glyph_rows();
  if (scale < 1) scale = 1;
  for (int i = 0; i < rows; i++) {
    uint8_t bits = g[i];
    for (int x = 0; x < FONT_W; x++) {
      uint32_t col = (bits & (uint8_t)(1 << (7 - x))) ? fg : bg;
      if (scale == 1)
        put_pixel(px + x, py + i, col);
      else
        fill_rect(px + x * scale, py + i * scale, scale, scale, col);
    }
  }
}

static void draw_char_font(int px, int py, char c, uint32_t fg, uint32_t bg) {
  draw_char_scaled(px, py, c, fg, bg, 1);
}

static void draw_string_font(int px, int py, const char* s, uint32_t fg, uint32_t bg) {
  int step = FONT_W;
  while (*s) {
    draw_char_font(px, py, *s++, fg, bg);
    px += step;
  }
}

static void draw_string_scaled(int px, int py, const char* s, uint32_t fg, uint32_t bg, int scale) {
  if (scale < 1) scale = 1;
  while (*s) {
    draw_char_scaled(px, py, *s++, fg, bg, scale);
    px += FONT_W * scale;
  }
}

static void delay_loop(void) {
  volatile int i;
  for (i = 0; i < 500000; i++) { }
}

static void ps2_wait_write(void) {
  while ((inb(0x64) & 2)) { }
}

static void ps2_mouse_init(void) {
  ps2_wait_write();
  outb(0x64, 0xA8);
  for (volatile int i = 0; i < 10000; i++) { }
  ps2_wait_write();
  outb(0x64, 0xD4);
  ps2_wait_write();
  outb(0x60, 0xF4);
  for (volatile int i = 0; i < 50000; i++) { }
  while (inb(0x64) & 1) (void)inb(0x60);
}

/* Alguns controladores deixam a porta 1 (teclado) desativada apos comandos ao auxiliar. */
static void ps2_enable_keyboard_port(void) {
  ps2_wait_write();
  outb(0x64, 0xAE);
}

/* Seta do Windows: B = contorno preto, W = miolo branco, espaco = transparente.
 * A ponta (canto superior esquerdo) e o hotspot e pode encostar na borda. */
#define CUR_W 11
#define CUR_H 17
#define CUR_SCALE_MAX 2
static const char cur_row[CUR_H][CUR_W + 1] = {
  "B          ",
  "BB         ",
  "BWB        ",
  "BWWB       ",
  "BWWWB      ",
  "BWWWWB     ",
  "BWWWWWB    ",
  "BWWWWWWB   ",
  "BWWWWWWWB  ",
  "BWWWWWWWWB ",
  "BWWWWBBBBBB",
  "BWWBWB     ",
  "BWB BWB    ",
  "BB   BWB   ",
  "B     BWB  ",
  "       BWB ",
  "        BB ",
};
static uint32_t cursor_under[CUR_W * CUR_SCALE_MAX * CUR_H * CUR_SCALE_MAX];
static int cursor_saved;
static int old_mx, old_my, cur_bw, cur_bh;

static int cur_scale(void) {
  return (gfx_h >= 400u) ? 2 : 1;
}

static void cursor_restore(void) {
  int k = 0;
  int dy, dx;
  if (!cursor_saved) return;
  for (dy = 0; dy < cur_bh; dy++)
    for (dx = 0; dx < cur_bw; dx++)
      put_pixel(old_mx + dx, old_my + dy, cursor_under[k++]);
  cursor_saved = 0;
}

/* Apaga o cursor na ultima posicao (repoe cursor_under) e desenha-o em (mx,my).
 * Para o rato: restore + redesenho. Para teclado: chamar cursor_restore ANTES de
 * desenhar texto na zona do cursor; depois so cursor_redraw_at (sem restore no inicio). */
static void cursor_redraw_at(int mx, int my, uint32_t cursor_color) {
  int scale = cur_scale();
  int bw = CUR_W * scale;
  int bh = CUR_H * scale;
  int k = 0;
  int dy, dx, ry, rx, oy, ox;
  uint32_t black = gfx_bochs ? PIXA(0, 0, 0) : 0u;
  uint32_t white = gfx_bochs ? PIXA(255, 255, 255) : 15u;
  (void)cursor_color;
  if (mx < 0) mx = 0;
  if (my < 0) my = 0;
  if ((uint32_t)mx >= gfx_w) mx = (int)gfx_w - 1;
  if ((uint32_t)my >= gfx_h) my = (int)gfx_h - 1;
  if (mx + bw > (int)gfx_w) bw = (int)gfx_w - mx;
  if (my + bh > (int)gfx_h) bh = (int)gfx_h - my;
  if (bw < 0) bw = 0;
  if (bh < 0) bh = 0;
  old_mx = mx;
  old_my = my;
  cur_bw = bw;
  cur_bh = bh;
  for (dy = 0; dy < bh; dy++)
    for (dx = 0; dx < bw; dx++)
      cursor_under[k++] = get_pixel(mx + dx, my + dy);
  for (ry = 0; ry < CUR_H; ry++) {
    for (rx = 0; rx < CUR_W; rx++) {
      char c = cur_row[ry][rx];
      uint32_t col;
      if (c == ' ') continue;
      col = (c == 'W') ? white : black;
      for (oy = 0; oy < scale; oy++)
        for (ox = 0; ox < scale; ox++)
          put_pixel(mx + rx * scale + ox, my + ry * scale + oy, col);
    }
  }
  cursor_saved = 1;
}

static void cursor_draw(int mx, int my, uint32_t cursor_color) {
  cursor_restore();
  cursor_redraw_at(mx, my, cursor_color);
}

/* Uma so posicao para o desktop e para a janela canvas (navegador). */
static int32_t sys_mx = -1, sys_my = -1;

/* A ponta da seta pode ir ate o ultimo pixel. O desenho que sair da tela e cortado. */
static void sys_mouse_place(int32_t x, int32_t y) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (gfx_w > 0 && x >= (int32_t)gfx_w) x = (int32_t)gfx_w - 1;
  if (gfx_h > 0 && y >= (int32_t)gfx_h) y = (int32_t)gfx_h - 1;
  sys_mx = x;
  sys_my = y;
}

static void sys_mouse_init(void) {
  if (sys_mx >= 0) {
    sys_mouse_place(sys_mx, sys_my);
    return;
  }
  sys_mouse_place((int32_t)(gfx_w / 2u), (int32_t)(gfx_h / 2u));
}

/* 1 = so move, 2 = botao desce, 3 = botao sobe no proximo poll. */
static int api_arm;
static int32_t api_x, api_y;

void mouse_api(int x, int y, int click) {
  sys_mouse_place(x, y);
  api_x = sys_mx;
  api_y = sys_my;
  api_arm = click ? 2 : 1;
}

void mouse_api_pos(int* x, int* y) {
  if (x) *x = sys_mx < 0 ? 0 : (int)sys_mx;
  if (y) *y = sys_my < 0 ? 0 : (int)sys_my;
}

int win_screen_size(int* w, int* h) {
  if (!gfx_live || !gfx_w || !gfx_h) return 0;
  if (w) *w = (int)gfx_w;
  if (h) *h = (int)gfx_h;
  return 1;
}

/* Uma linha BGR (BMP), y a partir do topo. max e em pixels. */
int win_screen_bgr(int y, uint8_t* dst, int max) {
  int x, n;
  if (!gfx_live || !dst || y < 0 || (uint32_t)y >= gfx_h) return -1;
  n = (int)gfx_w;
  if (n > max) n = max;
  for (x = 0; x < n; x++) {
    const uint8_t* p;
    if (gfx_bochs) {
      if (gfx_dbuf && gfx_lfb)
        p = (const uint8_t*)(gfx_lfb + ((uint32_t)y + gfx_front_y) * gfx_pitch_bytes + (uint32_t)x * 4u);
      else if (gfx_back)
        p = gfx_back + ((uint32_t)y * gfx_w + (uint32_t)x) * 4u;
      else if (gfx_lfb)
        p = (const uint8_t*)(gfx_lfb + (uint32_t)y * gfx_pitch_bytes + (uint32_t)x * 4u);
      else
        return -1;
      dst[0] = p[0];
      dst[1] = p[1];
      dst[2] = p[2];
    } else {
      uint8_t ix = fb13[(uint32_t)y * gfx_w + (uint32_t)x];
      uint8_t r, g, b;
      outb(0x3C7, ix);
      r = (uint8_t)(inb(0x3C9) << 2);
      g = (uint8_t)(inb(0x3C9) << 2);
      b = (uint8_t)(inb(0x3C9) << 2);
      dst[0] = b;
      dst[1] = g;
      dst[2] = r;
    }
    dst += 3;
  }
  return n;
}

/* amarelo, igual a bolinha do desktop */
static uint32_t sys_cursor_col(void) {
  return gfx_bochs ? PIXA(255, 255, 0) : 14u;
}

/* Rato absoluto do QEMU (-usb -device usb-tablet). O relatorio HID traz X e Y
 * em 0..32767, cobrindo a janela inteira: a seta fica no mesmo pixel do mouse
 * do emulador. Sem o tablet, continua o PS/2 relativo. */
#define TD_RUN (3u << 27 | 1u << 23)

struct uhci_td {
  volatile uint32_t link;
  volatile uint32_t sts;
  volatile uint32_t token;
  volatile uint32_t buf;
};

struct uhci_qh {
  volatile uint32_t head;
  volatile uint32_t elem;
  uint32_t pad0, pad1;
};

static volatile uint32_t* uhci_frame;
static struct uhci_qh* uhci_q;
static struct uhci_td* uhci_tds;
static volatile uint8_t* usb_setup;
static volatile uint8_t* usb_data;
static volatile uint8_t* usb_report;
static struct uhci_td* tablet_td;
static int usb_low;
static int mouse_abs_mode;
static int tablet_got;
static uint8_t tablet_btn;
static int tablet_dz;

static void* usb_mem(uint32_t n, uint32_t align) {
  uint8_t* raw;
  uint32_t a;
  if (!mm_ready()) return 0;
  raw = (uint8_t*)kmalloc(n + align);
  if (!raw) return 0;
  a = ((uint32_t)raw + align - 1u) & ~(align - 1u);
  return (void*)a;
}

static void usb_sleep(uint32_t ms) {
  uint32_t t0 = net_ms();
  uint32_t spins = 0;
  if (!net_has_clock()) {
    while (spins++ < ms * 200000u) __asm__ volatile ("pause");
    return;
  }
  while ((net_ms() - t0) < ms) __asm__ volatile ("pause");
}

static uint32_t uhci_tok(uint8_t pid, int addr, int ep, int toggle, int len) {
  uint32_t maxlen = (len == 0) ? 0x7FFu : (uint32_t)(len - 1);
  return (uint32_t)pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15) |
         ((uint32_t)toggle << 19) | (maxlen << 21);
}

static int uhci_wait(struct uhci_td* td, int n) {
  uint32_t t0 = net_ms();
  uint32_t spins = 0;
  for (;;) {
    int i, busy = 0;
    for (i = 0; i < n; i++) {
      uint32_t s = td[i].sts;
      if (s & ((1u << 22) | (1u << 21) | (1u << 20) | (1u << 18) | (1u << 17)))
        return -1;
      if (s & (1u << 23)) busy = 1;
    }
    if (!busy) return 0;
    if (net_has_clock()) {
      if ((net_ms() - t0) > 300u) return -1;
    } else if (++spins > 5000000u) {
      return -1;
    }
  }
}

static int usb_ctrl(uint8_t addr, uint8_t rtype, uint8_t req,
                    uint16_t val, uint16_t idx, int len, int din) {
  struct uhci_td* td = uhci_tds;
  int n = 0;
  int toggle = 1;
  int off = 0;
  uint32_t ls = usb_low ? (1u << 26) : 0;
  if (!uhci_q || !td || len < 0 || len > 64) return -1;
  usb_setup[0] = rtype;
  usb_setup[1] = req;
  usb_setup[2] = (uint8_t)val;
  usb_setup[3] = (uint8_t)(val >> 8);
  usb_setup[4] = (uint8_t)idx;
  usb_setup[5] = (uint8_t)(idx >> 8);
  usb_setup[6] = (uint8_t)len;
  usb_setup[7] = (uint8_t)(len >> 8);
  td[0].link = 1;
  td[0].sts = TD_RUN | ls;
  td[0].token = uhci_tok(0x2D, addr, 0, 0, 8);
  td[0].buf = (uint32_t)usb_setup;
  n = 1;
  while (off < len) {
    int chunk = len - off;
    if (chunk > 8) chunk = 8;
    if (n >= 10) return -1;
    td[n].sts = TD_RUN | ls;
    td[n].token = uhci_tok(din ? 0x69 : 0xE1, addr, 0, toggle, chunk);
    td[n].buf = (uint32_t)usb_data + (uint32_t)off;
    td[n].link = 1;
    td[n - 1].link = (uint32_t)&td[n];
    toggle ^= 1;
    off += chunk;
    n++;
  }
  if (n >= 11) return -1;
  td[n].sts = TD_RUN | ls;
  td[n].token = uhci_tok((din && len) ? 0xE1 : 0x69, addr, 0, 1, 0);
  td[n].buf = 0;
  td[n].link = 1;
  td[n - 1].link = (uint32_t)&td[n];
  n++;
  uhci_q->elem = (uint32_t)&td[0];
  if (uhci_wait(td, n) != 0) return -1;
  return 0;
}

static int uhci_find(uint16_t* out_io) {
  uint8_t dev, fun;
  for (dev = 0; dev < 32; dev++) {
    for (fun = 0; fun < 8; fun++) {
      uint32_t id = pci_cfg_read32(0, dev, fun, 0);
      uint32_t cls, bar, cmd;
      if (id == 0xFFFFFFFFu || id == 0) continue;
      cls = pci_cfg_read32(0, dev, fun, 0x08);
      if (((cls >> 8) & 0xFFFFFFu) != 0x0C0300u) continue;
      bar = pci_cfg_read32(0, dev, fun, 0x20);
      if (!(bar & 1u)) continue;
      if ((bar & ~3u) == 0) {
        pci_cfg_write32(0, dev, fun, 0x20, 0xC001u);
        bar = pci_cfg_read32(0, dev, fun, 0x20);
      }
      cmd = pci_cfg_read32(0, dev, fun, 0x04);
      pci_cfg_write32(0, dev, fun, 0x04, (cmd & 0xFFFFu) | 0x5u);
      *out_io = (uint16_t)(bar & ~3u);
      return 0;
    }
  }
  return -1;
}

static void uhci_start(uint16_t io) {
  int i;
  uint32_t qh_p = (uint32_t)uhci_q;
  outw(io, 0x0002);
  for (i = 0; i < 100000; i++)
    if (!(inw(io) & 0x0002)) break;
  outw((uint16_t)(io + 4), 0);
  uhci_q->head = 1;
  uhci_q->elem = 1;
  for (i = 0; i < 1024; i++) uhci_frame[i] = qh_p | 2u;
  outl((uint16_t)(io + 8), (uint32_t)uhci_frame);
  outw((uint16_t)(io + 6), 0);
  outw(io, 0x00C1);
}

static int uhci_reset_port(uint16_t io, int port) {
  uint16_t reg = (uint16_t)(io + 0x10 + port * 2);
  uint16_t s = inw(reg);
  if (!(s & 1)) return 0;
  outw(reg, 0x0200);
  usb_sleep(50);
  outw(reg, 0);
  usb_sleep(10);
  outw(reg, 0x000E);
  usb_sleep(20);
  s = inw(reg);
  if (!(s & 0x0004)) return 0;
  usb_low = (s & 0x0100) ? 1 : 0;
  return 1;
}

static void tablet_arm(int addr, int ep) {
  struct uhci_td* td = &uhci_tds[11];
  uint32_t ls = usb_low ? (1u << 26) : 0;
  td->link = 1;
  td->sts = TD_RUN | ls;
  td->token = uhci_tok(0x69, addr, ep, 0, 8);
  td->buf = (uint32_t)usb_report;
  uhci_q->elem = (uint32_t)td;
  tablet_td = td;
}

static void tablet_rearm(void) {
  uint32_t ls = usb_low ? (1u << 26) : 0;
  if (!tablet_td || !uhci_q) return;
  tablet_td->link = 1;
  tablet_td->sts = TD_RUN | ls;
  uhci_q->elem = (uint32_t)tablet_td;
}

static void abs_from_report(const volatile uint8_t* r) {
  uint32_t x = (uint32_t)r[1] | ((uint32_t)r[2] << 8);
  uint32_t y = (uint32_t)r[3] | ((uint32_t)r[4] << 8);
  if (x > 32767u) x = 32767u;
  if (y > 32767u) y = 32767u;
  if (gfx_w < 2 || gfx_h < 2) return;
  tablet_btn = (uint8_t)(r[0] & 7u);
  sys_mouse_place((int32_t)((x * (gfx_w - 1u)) / 32767u),
                  (int32_t)((y * (gfx_h - 1u)) / 32767u));
  tablet_got = 1;
}

static int tablet_hw_init(void) {
  uint16_t io = 0;
  int port;
  int addr = 1;
  int ep = -1;
  int total, p;
  if (mouse_abs_mode) {
    int addr;
    if (!tablet_td || !uhci_q) return 0;
    addr = (int)((tablet_td->token >> 8) & 0x7Fu);
    uhci_q->elem = 1;
    usb_sleep(2);
    if (usb_ctrl((uint8_t)addr, 0xA1, 0x01, 0x0100, 0, 8, 1) == 0)
      abs_from_report(usb_data);
    tablet_rearm();
    return 0;
  }
  if (!uhci_frame) {
    uhci_frame = (volatile uint32_t*)usb_mem(4096u, 4096u);
    uhci_q = (struct uhci_qh*)usb_mem(sizeof(struct uhci_qh), 16u);
    uhci_tds = (struct uhci_td*)usb_mem(12u * sizeof(struct uhci_td), 16u);
    usb_setup = (volatile uint8_t*)usb_mem(8u, 16u);
    usb_data = (volatile uint8_t*)usb_mem(64u, 16u);
    usb_report = (volatile uint8_t*)usb_mem(8u, 16u);
  }
  if (!uhci_frame || !uhci_q || !uhci_tds || !usb_setup || !usb_data || !usb_report)
    return -1;
  if (uhci_find(&io) != 0) {
    serial_puts("[mouse] sem UHCI, ps/2\n");
    return -1;
  }
  uhci_start(io);
  for (port = 0; port < 2; port++) {
    ep = -1;
    usb_low = 0;
    if (!uhci_reset_port(io, port)) continue;
    if (usb_ctrl(0, 0x80, 6, 0x0100, 0, 8, 1) != 0) continue;
    if (usb_ctrl(0, 0x00, 5, (uint16_t)addr, 0, 0, 0) != 0) continue;
    usb_sleep(2);
    if (usb_ctrl((uint8_t)addr, 0x80, 6, 0x0200, 0, 9, 1) != 0) continue;
    total = (int)usb_data[2] | ((int)usb_data[3] << 8);
    if (total < 9) total = 9;
    if (total > 64) total = 64;
    if (usb_ctrl((uint8_t)addr, 0x80, 6, 0x0200, 0, total, 1) != 0) continue;
    if (usb_ctrl((uint8_t)addr, 0x00, 9, usb_data[5], 0, 0, 0) != 0) continue;
    p = 0;
    while (p + 1 < total) {
      int l = usb_data[p];
      int t = usb_data[p + 1];
      if (l < 2) break;
      if (t == 5 && l >= 7 && (usb_data[p + 2] & 0x80) && (usb_data[p + 3] & 3) == 3) {
        ep = usb_data[p + 2] & 0x0F;
        break;
      }
      p += l;
    }
    if (ep < 0) continue;
    (void)usb_ctrl((uint8_t)addr, 0x21, 0x0A, 0, 0, 0, 0);
    mouse_abs_mode = 1;
    if (usb_ctrl((uint8_t)addr, 0xA1, 0x01, 0x0100, 0, 8, 1) == 0)
      abs_from_report(usb_data);
    tablet_arm(addr, ep);
    serial_puts("[mouse] tablet absoluto\n");
    return 0;
  }
  outw(io, 0);
  serial_puts("[mouse] ps/2 relativo\n");
  return -1;
}

/* 1 se a posicao, os botoes ou a roda mudaram. sys_mx/sys_my e tablet_btn ja atualizados.
 * Byte 5 segue o HID: positivo = roda para cima. tablet_dz guarda o contrario
 * (positivo desce), que e o que as janelas usam para rolar. */
static int tablet_take(void) {
  struct uhci_td* td = tablet_td;
  uint32_t sts;
  int n, old_x, old_y, old_btn, was, z;
  if (!mouse_abs_mode || !td) return 0;
  sts = td->sts;
  __asm__ volatile ("" ::: "memory");
  if (sts & (1u << 23)) return 0;
  if (sts & ((1u << 22) | (1u << 21) | (1u << 20) | (1u << 18))) {
    td->token &= ~(1u << 19);
    tablet_rearm();
    return 0;
  }
  n = (int)(sts & 0x7FFu) + 1;
  if (n < 5) {
    tablet_rearm();
    return 0;
  }
  old_x = (int)sys_mx;
  old_y = (int)sys_my;
  old_btn = (int)tablet_btn;
  was = tablet_got;
  abs_from_report(usb_report);
  z = 0;
  if (n >= 6) {
    z = -(int)(int8_t)usb_report[5];
    if (z >= 120) z = 1;
    else if (z <= -120) z = -1;
    else if (z > 3) z = 3;
    else if (z < -3) z = -3;
    tablet_dz += z;
  }
  tablet_rearm();
  if (!was) return 1;
  return z != 0 || old_x != (int)sys_mx || old_y != (int)sys_my || old_btn != (int)tablet_btn;
}

/* Um passo da roda por evento. Positivo desce, negativo sobe. */
static int mouse_wheel(void) {
  int z = tablet_dz;
  tablet_dz = 0;
  return z;
}

/* Devolve 1 com pacote de rato em packet[3] (*abs=1 se a posicao ja e absoluta).
 * Devolve 0 e o scancode em *sc. Devolve -1 se nao ha nada.
 * Byte de teclado tem prioridade para nao perder tecla enquanto o rato anda. */
static int input_poll(uint8_t packet[3], int* pkt_idx, uint8_t* sc, int* abs_pos) {
  uint8_t st, data;
  int is_aux;
  *abs_pos = 0;
  net_poll();
  if (api_arm) {
    sys_mouse_place(api_x, api_y);
    packet[0] = (api_arm == 2) ? 1 : 0;
    packet[1] = 0;
    packet[2] = 0;
    *abs_pos = 1;
    if (api_arm == 2) api_arm = 3;
    else api_arm = 0;
    return 1;
  }
  __asm__ volatile ("cli");
  st = inb(0x64);
  if (st & 1) {
    is_aux = (st >> 5) & 1;
    data = inb(0x60);
    __asm__ volatile ("sti");
    if (!(mouse_abs_mode && is_aux)) {
      if (!mouse_abs_mode && *pkt_idx > 0) {
        packet[(*pkt_idx)++] = data;
        if (*pkt_idx < 3) return -1;
        *pkt_idx = 0;
        return 1;
      }
      if (!mouse_abs_mode && is_aux && (data & 0x08)) {
        packet[0] = data;
        *pkt_idx = 1;
        return -1;
      }
      *sc = data;
      return 0;
    }
  } else {
    __asm__ volatile ("sti");
  }
  if (mouse_abs_mode && tablet_take()) {
    packet[0] = tablet_btn;
    packet[1] = 0;
    packet[2] = 0;
    *abs_pos = 1;
    return 1;
  }
  return -1;
}

#define GFX_SESSION_VISUAL 0
#define GFX_SESSION_WIN    1

/* Desktop tipo Windows 3.1/95 (escala para 320x200 ou 1024x768). */
static void draw_win_desktop(uint32_t col_desk, uint32_t col_win, uint32_t col_title,
                             uint32_t col_bar, uint32_t col_white, uint32_t col_black,
                             uint32_t col_btn, uint32_t *text_area_y0, uint32_t *text_x0,
                             uint32_t *text_y0, uint32_t *text_x1, uint32_t *text_y1) {
  int w = (int)gfx_w;
  int h = (int)gfx_h;
  int big = (h >= 400);
  int scale = big ? 2 : 1;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int bar_h = ch + (big ? 8 : 2);
  int icon = big ? 32 : 14;
  int win_x = big ? 80 : 36;
  int win_y = big ? 56 : 8;
  int win_w = big ? 640 : 232;
  int win_h = big ? 420 : 140;
  int title_h = ch + (big ? 4 : 2);
  int icon2_y = 12 + icon + (big ? 36 : 18);
  int title_ty = win_y + (title_h - ch) / 2;
  int bar_ty = h - bar_h + (bar_h - ch) / 2;

  if (win_x + win_w > w - 8) win_w = w - win_x - 8;
  if (win_y + win_h > h - bar_h - 4) win_h = h - bar_h - win_y - 4;

  gfx_clear(col_desk);

  fill_rect(12, 12, icon, icon, col_win);
  draw_string_scaled(12, 12 + icon + 2, "DOS", col_white, col_desk, scale);
  fill_rect(12, icon2_y, icon, icon, col_title);
  draw_string_scaled(12, icon2_y + icon + 2, "ARQ", col_white, col_desk, scale);

  fill_rect(win_x, win_y, win_w, win_h, col_win);
  fill_rect(win_x, win_y, win_w, title_h, col_title);
  draw_string_scaled(win_x + 4, title_ty, "Gerenciador", col_white, col_title, scale);
  fill_rect(win_x + win_w - (title_h - 2), win_y + 2, title_h - 4, title_h - 4, col_btn);
  draw_string_scaled(win_x + win_w - (title_h - 2) + 2, title_ty, "x", col_black, col_btn, scale);
  draw_string_scaled(win_x + 8, win_y + title_h + 6, "SO DOS 1.0", col_black, col_win, scale);

  fill_rect(0, h - bar_h, w, bar_h, col_bar);
  fill_rect(2, h - bar_h + 2, big ? 96 : 48, bar_h - 4, col_btn);
  draw_string_scaled(6, bar_ty, "Start", col_black, col_btn, scale);
  draw_string_scaled(big ? w - 8 * 8 * scale : w - 64, bar_ty, "ESC=DOS", col_white, col_bar, scale);

  *text_area_y0 = (uint32_t)(win_y + title_h + (big ? 8 : 4));
  *text_x0 = (uint32_t)(win_x + 8);
  *text_y0 = *text_area_y0;
  *text_x1 = (uint32_t)(win_x + win_w - 8);
  *text_y1 = (uint32_t)(win_y + win_h - 8);
}

#define WM_MAX 12
#define WM_KIND_MGR  0
#define WM_KIND_NOTE 1
#define WM_KIND_APP  2
#define WM_KIND_RUN  3
#define WM_KIND_DISP 4
#define WM_KIND_CMD  5
#define WM_KIND_FONT 6
#define SCANCODE_F2  0x3C

struct app_state;

struct wm_win {
  int used;
  int kind;
  int z;
  int x, y, w, h;
  char title[24];
  struct app_state* st;
};

/* z maior fica na frente. gui_focus >= 0 quando a janela da frente e um .EXE. */
static int zseq;
static int gui_focus = -1;
static int gui_stole;
static int win_api_dirty;

static void wm_raise(struct wm_win* w) {
  w->z = ++zseq;
  gui_focus = -1;
  gui_stole = 0;
}

static char note_path[48];
static char note_buf[1024];
static int note_len;
static int note_scroll;
static int note_dirty;
static int note_missing;
static int note_file_menu;
static int note_dlg; /* 0=none 1=open 2=saveas */
static char note_dlg_name[40];
static int note_dlg_len;
static int note_dlg_caret;
static int note_dlg_sel;
static char note_dlg_drive;
static char note_list[12][48];
static int note_list_n;
static int note_list_off;
static int note_list_vis;

#define NOTE_DLG_OPEN   1
#define NOTE_DLG_SAVEAS 2

static int wm_pt(int px, int py, int x, int y, int w, int h) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

static void wm_copy_str(char* dst, int max, const char* src) {
  int i = 0;
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void note_load(const char* path) {
  int n;
  char L;
  wm_copy_str(note_path, (int)sizeof(note_path), path);
  L = path_letter(note_path);
  if (!drives_is_mounted(L) && (L == 'D' || L == 'd'))
    drives_mount_ram('D');
  n = file_load(note_path, note_buf, (int)sizeof(note_buf));
  if (n < 0) {
    const char* msg = "Arquivo nao encontrado";
    int i = 0;
    while (msg[i] && i < (int)sizeof(note_buf) - 1) {
      note_buf[i] = msg[i];
      i++;
    }
    note_buf[i] = '\0';
    note_len = i;
    note_missing = 1;
  } else {
    note_len = n;
    note_buf[note_len] = '\0';
    note_missing = 0;
  }
  note_dirty = 0;
  note_scroll = 0;
}

static void note_save(void) {
  char L;
  if (note_missing && !note_dirty) return;
  note_missing = 0;
  note_buf[note_len] = '\0';
  L = path_letter(note_path);
  if (!drives_is_mounted(L) && (L == 'D' || L == 'd'))
    drives_mount_ram('D');
  if (file_save(note_path, note_buf, note_len) == 0)
    note_dirty = 0;
}

static void note_set_title(struct wm_win* w) {
  const char* s = note_path;
  const char* leaf = s;
  int i;
  if (!w) return;
  for (i = 0; s[i]; i++) {
    if (s[i] == '\\' || s[i] == '/' || s[i] == ':') leaf = s + i + 1;
  }
  if (leaf[0])
    wm_copy_str(w->title, (int)sizeof(w->title), leaf);
  else
    wm_copy_str(w->title, (int)sizeof(w->title), "Bloco de notas");
}

static void note_dlg_set_name(const char* s) {
  wm_copy_str(note_dlg_name, (int)sizeof(note_dlg_name), s ? s : "");
  note_dlg_len = 0;
  while (note_dlg_name[note_dlg_len]) note_dlg_len++;
  note_dlg_caret = note_dlg_len;
  note_dlg_sel = note_dlg_len > 0;
}

static void note_dlg_make_path(char* out, int max) {
  int i = 0;
  int j = 0;
  int has_colon = 0;
  int has_dot = 0;
  if (!out || max < 8) return;
  while (note_dlg_name[j]) {
    if (note_dlg_name[j] == ':') has_colon = 1;
    if (note_dlg_name[j] == '.') has_dot = 1;
    j++;
  }
  if (has_colon) {
    wm_copy_str(out, max, note_dlg_name);
    return;
  }
  out[i++] = note_dlg_drive ? note_dlg_drive : 'C';
  out[i++] = ':';
  out[i++] = '\\';
  j = 0;
  while (note_dlg_name[j] && i < max - 5) {
    char c = note_dlg_name[j++];
    if (c == '/') c = '\\';
    if (c == '\\' && i > 3 && out[i - 1] == '\\') continue;
    out[i++] = c;
  }
  if (!has_dot && i < max - 4) {
    out[i++] = '.';
    out[i++] = 't';
    out[i++] = 'x';
    out[i++] = 't';
  }
  out[i] = '\0';
}

static void note_dlg_insert(char c) {
  int i;
  if (note_dlg_sel) {
    note_dlg_name[0] = '\0';
    note_dlg_len = 0;
    note_dlg_caret = 0;
    note_dlg_sel = 0;
  }
  if (note_dlg_len >= (int)sizeof(note_dlg_name) - 1) return;
  for (i = note_dlg_len; i > note_dlg_caret; i--)
    note_dlg_name[i] = note_dlg_name[i - 1];
  note_dlg_name[note_dlg_caret++] = c;
  note_dlg_len++;
  note_dlg_name[note_dlg_len] = '\0';
}

static void note_dlg_backspace(void) {
  int i;
  if (note_dlg_sel) {
    note_dlg_name[0] = '\0';
    note_dlg_len = 0;
    note_dlg_caret = 0;
    note_dlg_sel = 0;
    return;
  }
  if (note_dlg_caret <= 0) return;
  for (i = note_dlg_caret - 1; i < note_dlg_len; i++)
    note_dlg_name[i] = note_dlg_name[i + 1];
  note_dlg_caret--;
  note_dlg_len--;
  note_dlg_name[note_dlg_len] = '\0';
}

static void note_open_dlg(int kind) {
  char L;
  note_dlg = kind;
  note_file_menu = 0;
  note_list_off = 0;
  L = path_letter(note_path);
  note_dlg_drive = L ? L : 'C';
  if (kind == NOTE_DLG_SAVEAS && note_path[0] == '\0')
    note_dlg_set_name("nota.txt");
  else if (note_path[0])
    note_dlg_set_name(note_path);
  else
    note_dlg_set_name("readme.txt");
  note_list_n = drives_enum_paths(note_list, 12);
}

/* Nome sem pasta: se nao esta na raiz, abre o mesmo nome na area de trabalho. */
static void note_try_desktop(char* path, int max) {
  char alt[48];
  const char* leaf;
  const char* pre = "D:\\user\\default\\desktop\\";
  int i;
  int j;
  int slashes;
  if (!path || max < 8 || file_size(path) >= 0) return;
  if (path[0] != 'D' && path[0] != 'd') return;
  slashes = 0;
  leaf = path;
  for (i = 0; path[i]; i++) {
    if (path[i] == '\\' || path[i] == '/') {
      slashes++;
      leaf = path + i + 1;
    }
  }
  if (slashes > 1 || !leaf[0]) return;
  i = 0;
  for (j = 0; pre[j] && i < 46; j++) alt[i++] = pre[j];
  for (j = 0; leaf[j] && i < 46; j++) alt[i++] = leaf[j];
  alt[i] = '\0';
  if (file_size(alt) < 0) return;
  i = 0;
  while (alt[i] && i < max - 1) {
    path[i] = alt[i];
    i++;
  }
  path[i] = '\0';
}

static void note_dlg_apply(struct wm_win* w) {
  char path[48];
  if (note_dlg_len < 1) return;
  note_dlg_name[note_dlg_len] = '\0';
  note_dlg_make_path(path, (int)sizeof(path));
  if (note_dlg == NOTE_DLG_OPEN) {
    note_try_desktop(path, (int)sizeof(path));
    note_load(path);
    note_set_title(w);
  } else if (note_dlg == NOTE_DLG_SAVEAS) {
    wm_copy_str(note_path, (int)sizeof(note_path), path);
    note_save();
    note_set_title(w);
  }
  note_dlg = 0;
}

static int note_menubar_h(int ch) {
  return ch + 4;
}

static int note_file_w(int scale) {
  return 4 * FONT_W * scale + 10;
}

/* Medidas desenhadas para 1024x768; em tela maior ocupam a mesma fracao. */
static int gfx_sx(int v) {
  if (v <= 0 || gfx_w <= 1024u) return v;
  return (int)(((uint32_t)v * gfx_w) / 1024u);
}

static int gfx_sy(int v) {
  if (v <= 0 || gfx_h <= 768u) return v;
  return (int)(((uint32_t)v * gfx_h) / 768u);
}

static void note_filemenu_rect(struct wm_win* w, int title_h, int ch, int big,
                               int* x, int* y, int* mw, int* mh, int* item_h) {
  *item_h = ch + (big ? 8 : 4);
  *x = w->x + 4;
  *y = w->y + title_h + note_menubar_h(ch);
  *mw = big ? gfx_sx(170) : 100;
  *mh = 4 * (*item_h) + 4;
}

static int note_dlg_rows(int list_h, int item_h) {
  int n;
  if (item_h < 1) item_h = 1;
  n = list_h / item_h;
  if (n < 1) n = 1;
  if (n > 12) n = 12;
  return n;
}

static void note_list_clamp(int vis) {
  int max;
  if (vis < 1) vis = 1;
  max = note_list_n - vis;
  if (max < 0) max = 0;
  if (note_list_off < 0) note_list_off = 0;
  if (note_list_off > max) note_list_off = max;
}

static void draw_fit(int x, int y, int max_w, const char* s,
                     uint32_t fg, uint32_t bg, int scale) {
  int cw = FONT_W * scale;
  int n = 0;
  int start = 0;
  if (cw < 1) cw = 1;
  if (!s) s = "";
  while (s[n]) n++;
  if (n * cw > max_w) {
    int fit = max_w / cw;
    if (fit < 1) fit = 1;
    start = n - fit;
  }
  draw_string_scaled(x, y, s + start, fg, bg, scale);
}

static void note_dlg_rect(int scr_w, int scr_h, int bar_h, int big,
                          int* x, int* y, int* dw, int* dh) {
  *dw = big ? gfx_sx(520) : 270;
  *dh = big ? gfx_sy(360) : 170;
  *x = (scr_w - *dw) / 2;
  *y = (scr_h - bar_h - *dh) / 2;
  if (*x < 8) *x = 8;
  if (*y < 8) *y = 8;
}

static int note_dlg_label_w(int scale) {
  return 11 * FONT_W * scale;
}

static void note_dlg_edit_geom(int dx, int dy, int dw, int dh, int ch, int scale,
                               int* ex, int* ey, int* ew, int* eh, int* btn_y) {
  *eh = ch + 10;
  *btn_y = dy + dh - ch - 14;
  *ey = *btn_y - *eh - 12;
  *ex = dx + 8 + note_dlg_label_w(scale);
  *ew = dw - 16 - note_dlg_label_w(scale);
  if (*ew < 40) *ew = 40;
}

static int note_text_lines(struct wm_win* w, int scale) {
  int cw = FONT_W * scale;
  int width = w->w - 14;
  int x = 0;
  int lines = 1;
  int i;
  if (width < cw) width = cw;
  if (note_len <= 0) return 1;
  for (i = 0; i < note_len; i++) {
    char c = note_buf[i];
    if (c == '\n') {
      lines++;
      x = 0;
      continue;
    }
    if (x + cw > width) {
      lines++;
      x = 0;
    }
    x += cw;
  }
  return lines;
}

static int note_vis_lines(struct wm_win* w, int top, int scale) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int y0 = w->y + top + 4;
  int y1 = w->y + w->h - 16;
  int n = (y1 - y0) / ch;
  if (n < 1) n = 1;
  return n;
}

static void note_scroll_by(struct wm_win* w, int top, int scale, int dz) {
  int total = note_text_lines(w, scale);
  int vis = note_vis_lines(w, top, scale);
  int max = total - vis;
  if (max < 0) max = 0;
  note_scroll += dz;
  if (note_scroll < 0) note_scroll = 0;
  if (note_scroll > max) note_scroll = max;
}

/* Acompanha o fim se o usuario ja estava la; senao deixa a vista onde esta. */
static void note_follow_end(struct wm_win* w, int top, int scale) {
  int total = note_text_lines(w, scale);
  int vis = note_vis_lines(w, top, scale);
  int max = total - vis;
  if (max < 0) max = 0;
  if (note_scroll >= max - 1) note_scroll = max;
  if (note_scroll > max) note_scroll = max;
  if (note_scroll < 0) note_scroll = 0;
}

static void draw_note_text(struct wm_win* w, int title_h, uint32_t fg, uint32_t bg, int scale) {
  int cw = FONT_W * scale;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int cx = w->x + 6;
  int cy = w->y + title_h + 4;
  int x1 = w->x + w->w - 8;
  int y1 = w->y + w->h - 16;
  int x = cx;
  int y = cy;
  int line = 0;
  int skip;
  int i;
  note_scroll_by(w, title_h, scale, 0);
  skip = note_scroll;
  for (i = 0; i < note_len; i++) {
    char c = note_buf[i];
    if (c == '\n') {
      x = cx;
      line++;
      if (line > skip) {
        y += ch;
        if (y + ch > y1) break;
      }
      continue;
    }
    if (x + cw > x1) {
      x = cx;
      line++;
      if (line > skip) {
        y += ch;
        if (y + ch > y1) break;
      }
    }
    if (line >= skip)
      draw_char_scaled(x, y, c, fg, bg, scale);
    x += cw;
  }
}

#define APP_LINES 48
#define APP_COLS  28
#define WIN_MSG_INIT  1
#define WIN_MSG_CLICK 2
#define PROG_SLOT ((uint8_t*)0x58000u)
#define PROG_SLOT_N 0x8000u

/* Mesmo layout que explorer.asm le em esi. Nao inserir campo antes de idx. */
struct app_state {
  int click;
  int drive;
  int sel;
  char name[32];
  int idx;
  char lines[APP_LINES][APP_COLS];
  int nlines;
  char status[40];
  int view;
};
typedef char app_layout_ok[(__builtin_offsetof(struct app_state, idx) == 44) ? 1 : -1];

static char app_open_path[48];
static int app_open_req;
static int app_run_req;
static struct wm_win* app_win;
static struct app_state* run_out;
static int run_row;
static int run_col;

typedef int (*win_app_fn)(int msg, void* api, void* arg);
typedef int (*bin_fn)(void (*puts_fn)(const char*));
static win_app_fn app_entry;

static void app_copy(char* dst, int max, const char* src) {
  int i = 0;
  if (!dst || max < 1) return;
  if (!src) src = "";
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static struct app_state* app_st(void) {
  if (!app_win) return 0;
  return app_win->st;
}

static void app_zero(struct app_state* st) {
  unsigned char* p;
  int i;
  if (!st) return;
  p = (unsigned char*)st;
  for (i = 0; i < (int)sizeof(*st); i++) p[i] = 0;
  st->sel = -1;
  st->drive = 67;
}

static void app_set_title(const char* s) {
  if (app_win) app_copy(app_win->title, (int)sizeof(app_win->title), s);
}

static void app_clear(void) {
  struct app_state* st = app_st();
  int i;
  if (!st) return;
  st->nlines = 0;
  st->sel = -1;
  st->view = 0;
  for (i = 0; i < APP_LINES; i++) st->lines[i][0] = '\0';
}

static int app_vis_lines(struct wm_win* w, int title_h, int ch) {
  int vis = (w->h - title_h - ch - 10) / ch;
  if (vis < 1) vis = 1;
  return vis;
}

static void app_scroll_by(struct app_state* st, int vis, int dz) {
  int max;
  if (!st) return;
  if (vis < 1) vis = 1;
  max = st->nlines - vis;
  if (max < 0) max = 0;
  st->view += dz;
  if (st->view < 0) st->view = 0;
  if (st->view > max) st->view = max;
}

static int app_add(const char* s) {
  struct app_state* st = app_st();
  int row;
  if (!st || st->nlines >= APP_LINES) return -1;
  row = st->nlines++;
  app_copy(st->lines[row], APP_COLS, s);
  return row;
}

static int app_files(int letter) {
  char names[24][32];
  return drives_names((char)letter, names, 24);
}

static int app_fname(int letter, int index, char* dst, int n) {
  char names[24][32];
  int c;
  int i;
  c = drives_names((char)letter, names, 24);
  if (!dst || n < 2 || index < 0 || index >= c) return -1;
  i = 0;
  while (names[index][i] && i < n - 1) {
    dst[i] = names[index][i];
    i++;
  }
  dst[i] = '\0';
  return 0;
}

static void app_status_set(const char* s) {
  struct app_state* st = app_st();
  if (!st) return;
  app_copy(st->status, (int)sizeof(st->status), s);
}

static void app_select(int index) {
  struct app_state* st = app_st();
  if (!st) return;
  if (index < 0 || index >= st->nlines) st->sel = -1;
  else st->sel = index;
}

static int app_selected(void) {
  struct app_state* st = app_st();
  if (!st) return -1;
  return st->sel;
}

static int app_drive_ok(int letter) {
  return drives_is_mounted((char)letter);
}

static int app_fold(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
  return (unsigned char)c;
}

static int app_is_prog(const char* name) {
  int n = 0;
  const char* ext;
  if (!name) return 0;
  while (name[n]) n++;
  if (n < 4 || name[n - 4] != '.') return 0;
  ext = name + n - 3;
  if (app_fold(ext[0]) == 'b' && app_fold(ext[1]) == 'i' && app_fold(ext[2]) == 'n')
    return 1;
  if (app_fold(ext[0]) == 'e' && app_fold(ext[1]) == 'x' && app_fold(ext[2]) == 'e')
    return 1;
  return 0;
}

static void app_open_file(int letter, int index) {
  char names[24][32];
  int c;
  int i;
  int j;
  char L;
  c = drives_names((char)letter, names, 24);
  if (index < 0 || index >= c) {
    app_status_set("arquivo invalido");
    return;
  }
  L = (char)letter;
  if (L >= 'a' && L <= 'z') L = (char)(L - 32);
  app_open_path[0] = L;
  app_open_path[1] = ':';
  app_open_path[2] = '\\';
  j = 3;
  for (i = 0; names[index][i] && j < (int)sizeof(app_open_path) - 1; i++)
    app_open_path[j++] = names[index][i];
  app_open_path[j] = '\0';
  app_open_req = 0;
  app_run_req = 0;
  if (app_is_prog(names[index])) app_run_req = 1;
  else app_open_req = 1;
  app_status_set(names[index]);
}

static void* app_api_table[10];

static void app_api_init(void) {
  app_api_table[0] = (void*)app_set_title;
  app_api_table[1] = (void*)app_clear;
  app_api_table[2] = (void*)app_add;
  app_api_table[3] = (void*)app_files;
  app_api_table[4] = (void*)app_fname;
  app_api_table[5] = (void*)app_status_set;
  app_api_table[6] = (void*)app_select;
  app_api_table[7] = (void*)app_selected;
  app_api_table[8] = (void*)app_drive_ok;
  app_api_table[9] = (void*)app_open_file;
}

static int disp_hot = -1;
static int disp_scroll;
static int font_sel;

static void disp_draw(struct wm_win* w, int title_h, int scale, int hot,
                      uint32_t col_win, uint32_t col_title, uint32_t col_white, uint32_t col_black);
static void font_draw(struct wm_win* w, int title_h, int scale,
                      uint32_t col_win, uint32_t col_title, uint32_t col_white, uint32_t col_black);

/* Prompt de comando: varias janelas, fundo preto, texto claro. */
#define CMD_ROWS 40
#define CMD_COLS 80
#define CMD_IN 128

struct cmd_con {
  char line[CMD_ROWS][CMD_COLS];
  int n;
  char in[CMD_IN];
  int inlen;
  int ocol;
  int busy; /* um .EXE digitado aqui ainda roda: sem prompt */
};

static struct cmd_con* cons_cap;
static int cons_painting;
static void (*cons_paint)(void);
static void cons_wipe(struct cmd_con* c);
static void cmd_push(struct cmd_con* c, const char* s);
static int in_task(void);

/* Numa tarefa o desktop redesenha quando ela ceder o turno. */
static void cons_refresh(void) {
  if (in_task()) win_api_dirty = 1;
  else if (cons_paint && !cons_painting) cons_paint();
}

static void cons_emit(char ch) {
  struct cmd_con* c = cons_cap;
  if (!c) return;
  if (ch == '\f') {
    int busy = c->busy;
    cons_wipe(c);
    c->busy = busy;
    cons_refresh();
    return;
  }
  if (ch == '\r' || ch == '\b') return;
  if (ch == '\n') {
    if (c->ocol == 0) cmd_push(c, "");
    c->ocol = 0;
    cons_refresh();
    return;
  }
  if (ch < 32 || (unsigned char)ch > 126) return;
  if (c->ocol == 0) cmd_push(c, "");
  if (c->n < 1) return;
  if (c->ocol >= CMD_COLS - 1) {
    c->ocol = 0;
    cmd_push(c, "");
  }
  c->line[c->n - 1][c->ocol++] = ch;
  c->line[c->n - 1][c->ocol] = '\0';
}

static void cons_wipe(struct cmd_con* c) {
  unsigned char* p = (unsigned char*)c;
  int i;
  for (i = 0; i < (int)sizeof(*c); i++) p[i] = 0;
}

static void cmd_push(struct cmd_con* c, const char* s) {
  int row, i;
  if (c->n >= CMD_ROWS) {
    for (row = 1; row < CMD_ROWS; row++)
      for (i = 0; i < CMD_COLS; i++) c->line[row - 1][i] = c->line[row][i];
    c->n = CMD_ROWS - 1;
  }
  row = c->n++;
  for (i = 0; i < CMD_COLS - 1 && s[i]; i++) c->line[row][i] = s[i];
  c->line[row][i] = '\0';
}

static int cmd_is_exit(const char* s) {
  int i;
  const char* w = "exit";
  for (i = 0; w[i]; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != w[i]) return 0;
  }
  return s[i] == '\0';
}

static void desk_load(void);
static int cmd_spawn(const char* path, int argc, char** argv);

static int cmd_enter(struct wm_win* w) {
  struct cmd_con* c = (struct cmd_con*)w->st;
  char shown[CMD_COLS];
  char buf[CMD_IN];
  char prompt[48];
  int i, plen = 0;
  if (!c) return 0;
  drives_prompt(prompt, (int)sizeof(prompt));
  while (prompt[plen]) plen++;
  for (i = 0; i < plen && i < CMD_COLS - 1; i++) shown[i] = prompt[i];
  for (i = 0; i < c->inlen && plen + i < CMD_COLS - 1; i++) shown[plen + i] = c->in[i];
  shown[plen + i] = '\0';
  if (cmd_is_exit(c->in)) return 1;
  cmd_push(c, shown);
  for (i = 0; i < c->inlen && i < CMD_IN - 1; i++) buf[i] = c->in[i];
  buf[i] = '\0';
  c->inlen = 0;
  c->in[0] = '\0';
  c->ocol = 0;
  if (buf[0]) {
    cons_cap = c;
    shell_set_out(cons_emit);
    shell_set_exec(cmd_spawn);
    shell_exec_line(buf);
    shell_set_exec(0);
    shell_set_out(0);
    cons_cap = 0;
    desk_load();
    if (cons_paint && !cons_painting) cons_paint();
  }
  return 0;
}

static int cmd_key(struct wm_win* w, char ch) {
  struct cmd_con* c = (struct cmd_con*)w->st;
  if (!c || c->busy) return 0;
  if (ch == '\b') {
    if (c->inlen > 0) c->in[--c->inlen] = '\0';
    return 0;
  }
  if (ch == '\n' || ch == '\r') return cmd_enter(w);
  if (ch >= 32 && ch < 127 && c->inlen < CMD_IN - 1) {
    c->in[c->inlen++] = ch;
    c->in[c->inlen] = '\0';
  }
  return 0;
}

static int path_is_command(const char* path) {
  const char* b = path;
  const char* p;
  const char* want = "command.exe";
  int i;
  if (!path) return 0;
  for (p = path; *p; p++)
    if (*p == '\\' || *p == '/') b = p + 1;
  for (i = 0; want[i]; i++) {
    char c = b[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != want[i]) return 0;
  }
  return b[i] == '\0';
}

static int wm_alloc(struct wm_win* wins);
static int wm_count_kind(struct wm_win* wins, int kind);
static void wm_clamp_rect(int* x, int* y, int* w, int* h, int big);

static void wm_open_command(struct wm_win* wins, int* focus, int big) {
  int i = wm_alloc(wins);
  int n;
  struct cmd_con* c;
  if (i < 0) return;
  c = (struct cmd_con*)kmalloc(sizeof(struct cmd_con));
  if (!c) return;
  cons_wipe(c);
  cmd_push(c, "SO DOS Command");
  cmd_push(c, "Mesmos comandos do DOS. HELP lista.");
  n = wm_count_kind(wins, WM_KIND_CMD);
  wins[i].used = 1;
  wins[i].kind = WM_KIND_CMD;
  wins[i].st = (struct app_state*)c;
  wins[i].x = (big ? gfx_sx(70) : 12) + n * (big ? gfx_sx(28) : 12);
  wins[i].y = (big ? gfx_sy(36) : 8) + n * (big ? gfx_sy(24) : 10);
  wins[i].w = big ? gfx_sx(560) : 280;
  wins[i].h = big ? gfx_sy(340) : 150;
  wm_clamp_rect(&wins[i].x, &wins[i].y, &wins[i].w, &wins[i].h, big);
  wm_copy_str(wins[i].title, (int)sizeof(wins[i].title), "Command");
  wm_raise(&wins[i]);
  *focus = i;
}

static void wm_draw_window(struct wm_win* w, int focus, int title_h, int scale,
                           uint32_t col_win, uint32_t col_title, uint32_t col_white,
                           uint32_t col_black, uint32_t col_btn, uint32_t col_inactive) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int close_s = title_h - 4;
  int close_x = w->x + w->w - close_s - 2;
  uint32_t tc = focus ? col_title : col_inactive;
  int ty = w->y + (title_h - ch) / 2;
  fill_rect(w->x, w->y, w->w, w->h, col_win);
  fill_rect(w->x, w->y, w->w, title_h, tc);
  draw_string_scaled(w->x + 4, ty, w->title, col_white, tc, scale);
  fill_rect(close_x, w->y + 2, close_s, close_s, col_btn);
  draw_string_scaled(close_x + (close_s > FONT_W * scale ? (close_s - FONT_W * scale) / 2 : 0),
                     ty, "x", col_black, col_btn, scale);
  fill_rect(w->x + w->w - 12, w->y + w->h - 12, 10, 10, col_black);
  fill_rect(w->x + w->w - 11, w->y + w->h - 11, 8, 8, col_btn);
  if (w->kind == WM_KIND_NOTE) {
    int mb_h = note_menubar_h(ch);
    int fw = note_file_w(scale);
    uint32_t file_bg = note_file_menu ? col_title : col_win;
    uint32_t file_fg = note_file_menu ? col_white : col_black;
    fill_rect(w->x, w->y + title_h, w->w, mb_h, col_win);
    fill_rect(w->x + 4, w->y + title_h + 1, fw, mb_h - 2, file_bg);
    draw_string_scaled(w->x + 8, w->y + title_h + 2, "File", file_fg, file_bg, scale);
    draw_note_text(w, title_h + mb_h, col_black, col_win, scale);
  } else if ((w->kind == WM_KIND_APP || w->kind == WM_KIND_RUN) && w->st) {
    int ih = ch;
    int y0 = w->y + title_h + 2;
    int vis = app_vis_lines(w, title_h, ch);
    int li;
    int top;
    struct app_state* st = w->st;
    app_scroll_by(st, vis, 0);
    top = st->view;
    if (vis > st->nlines - top) vis = st->nlines - top;
    if (vis < 0) vis = 0;
    for (li = 0; li < vis; li++) {
      int src = top + li;
      int ry = y0 + li * ih;
      if (w->kind == WM_KIND_APP && src == st->sel) {
        fill_rect(w->x + 4, ry, w->w - 8, ih, col_title);
        draw_string_scaled(w->x + 8, ry + 1, st->lines[src], col_white, col_title, scale);
      } else {
        draw_string_scaled(w->x + 8, ry + 1, st->lines[src], col_black, col_win, scale);
      }
    }
    draw_string_scaled(w->x + 6, w->y + w->h - ch - 2, st->status, col_black, col_win, scale);
  } else if (w->kind == WM_KIND_CMD && w->st) {
    struct cmd_con* c = (struct cmd_con*)w->st;
    int ih = ch;
    int y0 = w->y + title_h;
    int client_h = w->h - title_h;
    int vis = client_h / ih;
    int start, li, k, prompt_row;
    char row[CMD_COLS];
    if (vis < 1) vis = 1;
    fill_rect(w->x, y0, w->w, client_h, col_black);
    /* Nasce no topo. Desce com o texto. So fica no rodape quando a janela enche. */
    if (c->n + 1 <= vis) {
      start = 0;
      prompt_row = c->n;
    } else {
      start = c->n - (vis - 1);
      prompt_row = vis - 1;
    }
    {
      int cell = FONT_W * scale;
      int maxc = cell > 0 ? (w->w - 12) / cell : 40;
      char prompt[48];
      int plen = 0;
      int room, off, shown;
      char clip[CMD_COLS];
      int py = y0 + prompt_row * ih;
      drives_prompt(prompt, (int)sizeof(prompt));
      while (prompt[plen]) plen++;
      if (maxc < 8) maxc = 8;
      if (maxc > CMD_COLS - 1) maxc = CMD_COLS - 1;
      for (li = 0; start + li < c->n && li < prompt_row; li++) {
        for (k = 0; k < maxc && c->line[start + li][k]; k++) clip[k] = c->line[start + li][k];
        clip[k] = '\0';
        draw_string_scaled(w->x + 6, y0 + li * ih, clip, col_white, col_black, scale);
      }
      if (c->busy) return;
      room = maxc - plen;
      if (room < 1) room = 1;
      off = c->inlen > room ? c->inlen - room : 0;
      shown = c->inlen - off;
      for (k = 0; k < plen && k < CMD_COLS - 1; k++) row[k] = prompt[k];
      for (k = 0; k < shown && plen + k < CMD_COLS - 1; k++) row[plen + k] = c->in[off + k];
      row[plen + k] = '\0';
      draw_string_scaled(w->x + 6, py, row, col_white, col_black, scale);
      if (focus)
        fill_rect(w->x + 6 + (plen + k) * cell, py,
                  cell > 0 ? cell : FONT_W, ih > 2 ? ih - 2 : ih, col_white);
    }
  } else if (w->kind == WM_KIND_DISP) {
    disp_draw(w, title_h, scale, disp_hot, col_win, col_title, col_white, col_black);
  } else if (w->kind == WM_KIND_FONT) {
    font_draw(w, title_h, scale, col_win, col_title, col_white, col_black);
  } else {
    draw_string_scaled(w->x + 8, w->y + title_h + 8, "SO DOS 1.0", col_black, col_win, scale);
    draw_string_scaled(w->x + 8, w->y + title_h + 8 + ch + 2, "Arraste a barra. Canto = tamanho.", col_black, col_win, scale);
    draw_string_scaled(w->x + 8, w->y + title_h + 8 + 2 * (ch + 2), "X fecha. Iniciar: Bloco de notas.", col_black, col_win, scale);
  }
}

static int disp_row_h(int scale) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  return ch + (scale > 1 ? 4 : 2);
}

#define DISP_N (GFX_MODE_N + 1)

static int disp_vis_rows(struct wm_win* w, int title_h, int scale) {
  int ih = disp_row_h(scale);
  int vis = (w->h - title_h - 12) / ih;
  if (vis < 1) vis = 1;
  if (vis > DISP_N) vis = DISP_N;
  return vis;
}

static void disp_scroll_by(struct wm_win* w, int title_h, int scale, int dz) {
  int vis = disp_vis_rows(w, title_h, scale);
  int max = DISP_N - vis;
  if (max < 0) max = 0;
  disp_scroll += dz;
  if (disp_scroll < 0) disp_scroll = 0;
  if (disp_scroll > max) disp_scroll = max;
}

/* Lista de resolucoes da janela Configuracoes de tela. */
static void disp_draw(struct wm_win* w, int title_h, int scale, int hot,
                      uint32_t col_win, uint32_t col_title, uint32_t col_white, uint32_t col_black) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int ih = disp_row_h(scale);
  int mi;
  int row = 0;
  int y0 = w->y + title_h + 4;
  int y1 = w->y + w->h - 4;
  disp_scroll_by(w, title_h, scale, 0);
  for (mi = disp_scroll; mi < DISP_N; mi++) {
    int mode = mi - 1;
    int ry = y0 + row * ih;
    int on = mi == 0 ? gfx_mode_sel < 0
                     : ((uint32_t)gfx_w == gfx_mode_w[mode] && (uint32_t)gfx_h == gfx_mode_h[mode]);
    int hi = (mi == hot);
    uint32_t bg = hi ? col_title : col_win;
    uint32_t fg = hi ? col_white : col_black;
    const char* name = mi == 0 ? "Padrao" : gfx_mode_name[mode];
    if (ry + ih > y1) break;
    fill_rect(w->x + 4, ry, w->w - 8, ih, bg);
    draw_string_scaled(w->x + 10, ry + (ih - ch) / 2, name, fg, bg, scale);
    if (on)
      draw_string_scaled(w->x + w->w - FONT_W * scale - 12, ry + (ih - ch) / 2,
                         "*", fg, bg, scale);
    row++;
  }
}

static int wm_find_kind(struct wm_win* wins, int kind) {
  int i;
  for (i = 0; i < WM_MAX; i++)
    if (wins[i].used && wins[i].kind == kind) return i;
  return -1;
}

static int wm_alloc(struct wm_win* wins) {
  int i;
  for (i = 0; i < WM_MAX; i++)
    if (!wins[i].used) return i;
  return -1;
}

static int gfx_bar_h(int big) {
  int scale = big ? 2 : 1;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  return ch + (big ? 8 : 2);
}

/* Mantem a janela dentro da resolucao escolhida (640x480 ate 1024x768). */
static void wm_clamp_rect(int* x, int* y, int* w, int* h, int big) {
  int limit_h = (int)gfx_h - gfx_bar_h(big) - 4;
  int limit_w = (int)gfx_w - 8;
  if (limit_w < 40) limit_w = (int)gfx_w;
  if (limit_h < 40) limit_h = (int)gfx_h;
  if (*w > limit_w) *w = limit_w;
  if (*h > limit_h) *h = limit_h;
  if (*w < 40) *w = limit_w < 40 ? limit_w : 40;
  if (*h < 40) *h = limit_h < 40 ? limit_h : 40;
  if (*x < 0) *x = 0;
  if (*y < 0) *y = 0;
  if (*x + *w > (int)gfx_w) *x = (int)gfx_w - *w;
  if (*y + *h > limit_h) *y = limit_h - *h;
  if (*x < 0) *x = 0;
  if (*y < 0) *y = 0;
}

static void wm_open_notepad(struct wm_win* wins, int* focus, int big) {
  int i = wm_find_kind(wins, WM_KIND_NOTE);
  if (i < 0) i = wm_alloc(wins);
  if (i < 0) return;
  if (!wins[i].used) {
    wins[i].used = 1;
    wins[i].kind = WM_KIND_NOTE;
    wins[i].x = big ? gfx_sx(160) : 20;
    wins[i].y = big ? gfx_sy(70) : 6;
    wins[i].w = big ? gfx_sx(620) : 260;
    wins[i].h = big ? gfx_sy(400) : 130;
    wm_clamp_rect(&wins[i].x, &wins[i].y, &wins[i].w, &wins[i].h, big);
    note_load("C:\\readme.txt");
    note_set_title(&wins[i]);
    note_file_menu = 0;
    note_dlg = 0;
  }
  wm_raise(&wins[i]);
  *focus = i;
}

static int wm_count_kind(struct wm_win* wins, int kind) {
  int i;
  int n = 0;
  for (i = 0; i < WM_MAX; i++)
    if (wins[i].used && wins[i].kind == kind) n++;
  return n;
}

static void wm_place(struct wm_win* w, struct wm_win* wins, int big, int kind) {
  int n = wm_count_kind(wins, kind);
  w->x = (big ? gfx_sx(28) : 4) + (n % 4) * (big ? gfx_sx(26) : 12);
  w->y = (big ? gfx_sy(28) : 2) + (n % 4) * (big ? gfx_sy(22) : 8);
  w->w = big ? gfx_sx(420) : 300;
  w->h = big ? gfx_sy(460) : 170;
  wm_clamp_rect(&w->x, &w->y, &w->w, &w->h, big);
}

static void run_scroll(struct app_state* st) {
  int r;
  int c;
  for (r = 1; r < APP_LINES; r++)
    for (c = 0; c < APP_COLS; c++) st->lines[r - 1][c] = st->lines[r][c];
  st->lines[APP_LINES - 1][0] = '\0';
  run_row = APP_LINES - 1;
}

static void run_add(char ch) {
  struct app_state* st = run_out;
  if (!st) return;
  if (in_task()) win_api_dirty = 1;
  if (ch == '\r') return;
  if (ch == '\b') {
    if (run_col > 0) st->lines[run_row][--run_col] = '\0';
    return;
  }
  if (ch == '\n' || run_col >= APP_COLS - 1) {
    st->lines[run_row][run_col] = '\0';
    st->nlines = run_row + 1;
    run_row++;
    run_col = 0;
    if (run_row >= APP_LINES) run_scroll(st);
    st->lines[run_row][0] = '\0';
    if (ch == '\n') return;
  }
  st->lines[run_row][run_col++] = ch;
  st->lines[run_row][run_col] = '\0';
  st->nlines = run_row + 1;
}

static void run_puts(const char* s) {
  if (!s) return;
  while (*s) run_add(*s++);
}

static void wm_slot_copy(uint8_t* dst, const uint8_t* src) {
  uint32_t i;
  for (i = 0; i < PROG_SLOT_N; i++) dst[i] = src[i];
}

/* Janela de saida para um programa; -1 se nao deu. */
static int wm_new_run_window(struct wm_win* wins, int big, const char* path,
                             struct wm_win* owner) {
  const char* base;
  const char* p;
  int i = wm_alloc(wins);
  if (i < 0) {
    app_status_set("sem janela");
    return -1;
  }
  wins[i].st = (struct app_state*)kmalloc(sizeof(struct app_state));
  if (!wins[i].st) {
    app_status_set("sem memoria");
    return -1;
  }
  app_zero(wins[i].st);
  wins[i].kind = WM_KIND_RUN;
  {
    int nrun = wm_count_kind(wins, WM_KIND_RUN);
    wins[i].x = (owner && owner->used ? owner->x : (big ? gfx_sx(40) : 8)) + nrun * (big ? gfx_sx(18) : 8);
    wins[i].y = (owner && owner->used ? owner->y : (big ? gfx_sy(40) : 8)) + nrun * (big ? gfx_sy(16) : 8);
  }
  wins[i].w = big ? gfx_sx(420) : 280;
  wins[i].h = big ? gfx_sy(300) : 120;
  wm_clamp_rect(&wins[i].x, &wins[i].y, &wins[i].w, &wins[i].h, big);
  wins[i].used = 1;
  base = path;
  for (p = path; *p; p++)
    if (*p == '\\' || *p == ':') base = p + 1;
  app_copy(wins[i].title, (int)sizeof(wins[i].title), base);
  wm_raise(&wins[i]);
  run_out = wins[i].st;
  run_row = 0;
  run_col = 0;
  wins[i].st->lines[0][0] = '\0';
  return i;
}

static void wm_run_status(struct wm_win* w, int rc) {
  char num[16];
  char msg[40];
  int k = 0;
  int j = 0;
  if (rc <= MOD_ERR_NOTFOUND && rc >= MOD_ERR_DEPTH) {
    app_copy(w->st->status, (int)sizeof(w->st->status), mod_last_error());
    return;
  }
  app_copy(msg, (int)sizeof(msg), "retornou ");
  int32_to_string(rc, num, (int)sizeof(num));
  while (msg[k]) k++;
  while (num[j] && k < (int)sizeof(msg) - 1) msg[k++] = num[j++];
  msg[k] = '\0';
  app_copy(w->st->status, (int)sizeof(w->st->status), msg);
}

/* ---------- janelas canvas (k_gui_*) ----------
 * Varios .EXE (navegador, fontes avancadas, ...) ficam abertos juntos.
 * Cada um tem o seu retangulo e a sua fila. Quem espera tecla ou rede
 * devolve o turno ao desktop; um clique traz essa janela para a frente. */
#define GUI_MAX 8
#define GUI_Q 16
#define TASK_MAX 8
#define TASK_STACK (48 * 1024)
#define TASK_ARGC 16
#define TASK_ARGBUF 192
#define WAKE_NONE  0
#define WAKE_BUSY  1
#define WAKE_POLL  2
#define WAKE_BLOCK 3
#define WAKE_DEAD  4

struct gui_win {
  int open;
  int z;
  int ox, oy;
  int title_h;
  int scale;
  int cw, ch;
  uint32_t* pix;
  char title[64];
  int prev_left;
  int pkt_idx;
  uint8_t pkt[3];
  int shift, e0;
  int qn, qr;
  int qt[GUI_Q], qx[GUI_Q], qy[GUI_Q], qk[GUI_Q];
  int ask_close;
  int drag;
  int drag_ox, drag_oy;
};

struct gui_task {
  int used;
  int wake;
  int gid;
  int argc;
  uint64_t rsp;
  struct mod_cpu cpu;
  struct wm_win* run;
  struct cmd_con* con; /* aberto pelo Command: escreve la, nao numa janela propria */
  int con_row, con_col;
  uint8_t* stack;
  char path[80];
  char args[TASK_ARGBUF]; /* argc textos seguidos, cada um com o seu NUL */
};

static struct gui_win* gwins;
static struct gui_task* gtasks;
static int gui_ix;
static int gui_allowed;
static int gui_painting;
static int task_cur = -1;
static int host_on;
static uint64_t host_rsp;
static struct mod_cpu host_cpu;
static int task_rr;
#define gui (gwins[gui_ix])

static int in_task(void) {
  return host_on && task_cur >= 0;
}

static void gui_zero(void* mem, uint32_t n) {
  uint8_t* p = (uint8_t*)mem;
  while (n--) *p++ = 0;
}

/* Fora do BSS: o nucleo tem de acabar antes do slot 0x58000. */
static int gui_ensure(void) {
  if (!gwins) {
    gwins = (struct gui_win*)kmalloc((uint32_t)sizeof(struct gui_win) * GUI_MAX);
    if (gwins) gui_zero(gwins, (uint32_t)sizeof(struct gui_win) * GUI_MAX);
  }
  if (!gtasks) {
    gtasks = (struct gui_task*)kmalloc((uint32_t)sizeof(struct gui_task) * TASK_MAX);
    if (gtasks) gui_zero(gtasks, (uint32_t)sizeof(struct gui_task) * TASK_MAX);
  }
  return gwins && gtasks;
}

static void gui_idle(void);
static void gui_clamp_pos(void);
static int gui_ok(void) {
  if (!gwins || !gtasks) return 0;
  if (gui_ix < 0 || gui_ix >= GUI_MAX || !gui.open) return 0;
  if (task_cur >= 0 && gtasks[task_cur].gid != gui_ix) return 0;
  return 1;
}
static void (*chrome_under)(void);
static void (*chrome_over)(void);
static int (*chrome_click)(int x, int y);
static int* chrome_menu;
static int chrome_leave;

static const uint32_t gui_ega[16] = {
  0x000000, 0x0000AA, 0x00AA00, 0x008080, 0xAA0000, 0xAA00AA, 0xAA5500, 0xA8A8A8,
  0x545454, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

/* cor do app -> cor do fill_rect/put_pixel (PIXA ou indice da paleta) */
static uint32_t gui_col(uint32_t rgb) {
  int i, best = 0;
  int32_t bd = 0x7FFFFFFF;
  int r = (int)((rgb >> 16) & 255), g = (int)((rgb >> 8) & 255), b = (int)(rgb & 255);
  if (gfx_bochs) return PIXA(r, g, b);
  for (i = 0; i < 16; i++) {
    int dr = r - (int)((gui_ega[i] >> 16) & 255);
    int dg = g - (int)((gui_ega[i] >> 8) & 255);
    int db = b - (int)(gui_ega[i] & 255);
    int32_t d = dr * dr + dg * dg + db * db;
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return (uint32_t)best;
}

void gui_session(int allowed) {
  int i, save;
  gui_allowed = allowed;
  if (allowed || !gwins) return;
  save = gui_ix;
  for (i = 0; i < GUI_MAX; i++) {
    if (!gwins[i].open) continue;
    gui_ix = i;
    gui_close();
  }
  gui_ix = save;
}

int gui_open(const char* title) {
  uint32_t bytes;
  int i, n = 0, slot = -1;
  if (!gui_ensure()) return -2;
  if (!gui_allowed || gfx_w == 0) return -1;
  if (task_cur >= 0 && gtasks[task_cur].gid >= 0 && gwins[gtasks[task_cur].gid].open) {
    gui_ix = gtasks[task_cur].gid;
    gui_title(title);
    gui.z = ++zseq;
    gui_focus = gui_ix;
    gui_stole = 1;
    win_api_dirty = 1;
    return 0;
  }
  for (i = 0; i < GUI_MAX; i++) {
    if (gwins[i].open) n++;
    else if (slot < 0) slot = i;
  }
  if (slot < 0) return -1;
  gui_ix = slot;
  if (task_cur >= 0) gtasks[task_cur].gid = slot;
  {
    int scale = (gfx_h >= 400u) ? 2 : 1;
    int font_h = (vga_bios_font_ok ? FONT_H : 8) * scale;
    int bar = font_h + (gfx_h >= 400u ? 8 : 2);
    int margin_l = (int)gfx_w / 8;
    int margin_r = (int)gfx_w / 24;
    int margin_t = (int)gfx_h / 16;
    if (margin_l < 36) margin_l = 36;
    if (margin_r < 12) margin_r = 12;
    if (margin_t < 8) margin_t = 8;
    gui.scale = scale;
    gui.title_h = font_h + (gfx_h >= 400u ? 4 : 2);
    gui.ox = margin_l;
    gui.oy = margin_t;
    gui.cw = (int)gfx_w - margin_l - margin_r;
    gui.ch = (int)gfx_h - margin_t - bar - 6 - gui.title_h;
    if (gui.cw < 160 || gui.ch < 48) {
      gui.ox = 0;
      gui.oy = 0;
      gui.cw = (int)gfx_w;
      gui.ch = (int)gfx_h - bar - gui.title_h;
      if (gui.ch < 32) gui.ch = (int)gfx_h - gui.title_h;
    }
  }
  /* Cada janela nasce um pouco mais para baixo e para a direita, menor
   * se precisar, para a barra de titulo da anterior continuar visivel. */
  gui.ox += n * 36;
  gui.oy += n * 28;
  if (gui.ox < 0) gui.ox = 0;
  if (gui.oy < 0) gui.oy = 0;
  if (gui.ox + gui.cw > (int)gfx_w) gui.cw = (int)gfx_w - gui.ox;
  {
    int scale = gui.scale > 0 ? gui.scale : 1;
    int bar = glyph_rows() * scale + (gfx_h >= 400u ? 8 : 2);
    int limit = (int)gfx_h - bar;
    if (gui.oy + gui.title_h + gui.ch > limit)
      gui.ch = limit - gui.oy - gui.title_h;
  }
  if (gui.cw < 160 || gui.ch < 48) {
    gui.ox -= n * 36;
    gui.oy -= n * 28;
    if (gui.ox < 0) gui.ox = 0;
    if (gui.oy < 0) gui.oy = 0;
    gui_clamp_pos();
  }
  bytes = (uint32_t)gui.cw * (uint32_t)gui.ch * 4u;
  gui.pix = (uint32_t*)umalloc_owned(bytes, 0);
  if (!gui.pix) {
    if (task_cur >= 0 && gtasks[task_cur].gid == gui_ix) gtasks[task_cur].gid = -1;
    return -2;
  }
  fill32(gui.pix, 0xFFFFFFu, (uint32_t)gui.cw * (uint32_t)gui.ch);
  gui.open = 1;
  sys_mouse_init();
  /* o clique que abriu o programa ainda esta apertado */
  gui.prev_left = 1;
  gui.pkt_idx = 0;
  gui.shift = 0;
  gui.e0 = 0;
  gui.qn = 0;
  gui.qr = 0;
  gui.ask_close = 0;
  gui.drag = 0;
  gui.z = ++zseq;
  gui_focus = gui_ix;
  gui_stole = 1;
  gui_title(title);
  if (!host_on) sched_set_idle(gui_idle);
  serial_puts("[visual] janela\n");
  win_api_dirty = 1;
  if (!host_on) gui_present();
  return 0;
}

void gui_close(void) {
  if (gui_ix < 0 || gui_ix >= GUI_MAX) return;
  /* Um programa so fecha a propria janela. */
  if (task_cur >= 0 && gtasks[task_cur].gid != gui_ix) return;
  if (!host_on) sched_set_idle(0);
  if (gui.pix) ufree(gui.pix);
  gui.pix = 0;
  gui.open = 0;
  gui.ask_close = 0;
  gui.drag = 0;
  gui.qn = 0;
  gui.qr = 0;
  if (gui_focus == gui_ix) gui_focus = -1;
  if (task_cur >= 0 && gtasks[task_cur].gid == gui_ix) gtasks[task_cur].gid = -1;
  win_api_dirty = 1;
}

void gui_title(const char* title) {
  if (!gui_ok()) return;
  app_copy(gui.title, (int)sizeof(gui.title), title ? title : "");
}

void gui_size(int* w, int* h) {
  int ok = gui_ok();
  if (w) *w = ok ? gui.cw : 0;
  if (h) *h = ok ? gui.ch : 0;
}

void gui_font(int* cw, int* ch) {
  if (cw) *cw = FONT_W;
  if (ch) *ch = glyph_rows();
}

void gui_pixels(int x, int y, int w, int h, const void* rgba, int stride) {
  const uint8_t* src = (const uint8_t*)rgba;
  int yy, xx;
  if (!gui_ok() || !src || w < 1 || h < 1 || stride < w * 4) return;
  for (yy = 0; yy < h; yy++) {
    int dy = y + yy;
    const uint8_t* row;
    if (dy < 0 || dy >= gui.ch) continue;
    row = src + (size_t)yy * (size_t)stride;
    for (xx = 0; xx < w; xx++) {
      int dx = x + xx;
      const uint8_t* p = row + xx * 4;
      if (dx < 0 || dx >= gui.cw || p[3] < 128) continue;
      gui.pix[dy * gui.cw + dx] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
  }
}

void gui_fill(int x, int y, int w, int h, uint32_t rgb) {
  int x2 = x + w, y2 = y + h, yy;
  if (!gui_ok()) return;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x2 > gui.cw) x2 = gui.cw;
  if (y2 > gui.ch) y2 = gui.ch;
  if (x >= x2 || y >= y2) return;
  for (yy = y; yy < y2; yy++) fill32(gui.pix + yy * gui.cw + x, rgb & 0xFFFFFFu, (uint32_t)(x2 - x));
}

static void gui_glyph(int px, int py, unsigned char c, uint32_t fg, uint32_t bg, int flags) {
  const uint8_t* g = glyph16(c);
  int rows = glyph_rows();
  int i, x;
  for (i = 0; i < rows; i++) {
    int yy = py + i;
    uint8_t bits = g[i];
    uint32_t* row;
    if (yy < 0 || yy >= gui.ch) continue;
    if ((flags & GUI_TEXT_UNDERLINE) && i == rows - 1) bits = 0xFF;
    row = gui.pix + yy * gui.cw;
    for (x = 0; x < FONT_W; x++) {
      int xx = px + x;
      int on = (bits >> (7 - x)) & 1;
      if ((flags & GUI_TEXT_BOLD) && x > 0 && ((bits >> (8 - x)) & 1)) on = 1;
      if (xx < 0 || xx >= gui.cw) continue;
      if (on) row[xx] = fg;
      else if (!(flags & GUI_TEXT_NOBG)) row[xx] = bg;
    }
  }
}

void gui_text(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int flags) {
  int i;
  if (!gui_ok() || !s) return;
  if (n < 0) {
    n = 0;
    while (s[n]) n++;
  }
  fg &= 0xFFFFFFu;
  bg &= 0xFFFFFFu;
  for (i = 0; i < n; i++, x += FONT_W) {
    if (x >= gui.cw) break;
    if (x + FONT_W <= 0) continue;
    gui_glyph(x, y, (unsigned char)s[i], fg, bg, flags);
  }
}

static void gui_blit_win(struct gui_win* w) {
  int y;
  int top = w->oy + w->title_h;
  int left = w->ox;
  if (!w->pix || w->cw < 1 || w->ch < 1) return;
  for (y = 0; y < w->ch; y++) {
    uint32_t* src = w->pix + y * w->cw;
    if (gfx_bochs) {
      volatile uint32_t* dst = gfx_offscreen
        ? (volatile uint32_t*)(gfx_back + (uint32_t)(y + top) * gfx_w * 4u + (uint32_t)left * 4u)
        : (volatile uint32_t*)(gfx_lfb + ((uint32_t)(y + top) + gfx_draw_y0) * gfx_pitch_bytes +
                               (uint32_t)left * 4u);
      uint32_t n = (uint32_t)w->cw;
      __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
    } else {
      int x;
      uint32_t last = 0xFFFFFFFFu, idx = 0;
      for (x = 0; x < w->cw; x++) {
        if (src[x] != last) {
          last = src[x];
          idx = gui_col(last);
        }
        put_pixel(x + left, y + top, idx);
      }
    }
  }
}

static void gui_blit(void) { gui_blit_win(&gui); }


static void gui_clamp_pos(void) {
  int scale = gui.scale > 0 ? gui.scale : 1;
  int bar = glyph_rows() * scale + (gfx_h >= 400u ? 8 : 2);
  int limit = (int)gfx_h - bar;
  if (gui.ox < 0) gui.ox = 0;
  if (gui.oy < 0) gui.oy = 0;
  if (gui.cw <= (int)gfx_w && gui.ox + gui.cw > (int)gfx_w)
    gui.ox = (int)gfx_w - gui.cw;
  if (gui.title_h + gui.ch <= limit && gui.oy + gui.title_h + gui.ch > limit)
    gui.oy = limit - gui.title_h - gui.ch;
}

static void gui_move_to(int x, int y) {
  gui.ox = x - gui.drag_ox;
  gui.oy = y - gui.drag_oy;
  gui_clamp_pos();
}

void gui_present(void) {
  int scale, ch, close_s, close_x, ty, xoff;
  uint32_t title_bg = gui_col(0x000080), white = gui_col(0xFFFFFF), btn = gui_col(0xC0C0C0);
  uint32_t edge = gui_col(0x000000);
  if (host_on && task_cur >= 0) {
    win_api_dirty = 1;
    return;
  }
  if (!gui.open || gui_painting) return;
  gui_painting = 1;
  scale = gui.scale > 0 ? gui.scale : 1;
  ch = glyph_rows() * scale;
  close_s = gui.title_h - 4;
  close_x = gui.ox + gui.cw - close_s - 2;
  ty = gui.oy + (gui.title_h - ch) / 2;
  xoff = close_s > FONT_W * scale ? (close_s - FONT_W * scale) / 2 : 0;
  gfx_frame_begin();
  cursor_saved = 0;
  if (chrome_under) chrome_under();
  else gfx_clear(gui_col(0x008080));
  fill_rect(gui.ox, gui.oy, gui.cw, gui.title_h, title_bg);
  draw_string_scaled(gui.ox + 4, ty, gui.title, white, title_bg, scale);
  fill_rect(close_x, gui.oy + 2, close_s, close_s, btn);
  draw_string_scaled(close_x + xoff, ty, "x", gui_col(0), btn, scale);
  gui_blit();
  fill_rect(gui.ox, gui.oy, gui.cw, 1, edge);
  fill_rect(gui.ox, gui.oy + gui.title_h + gui.ch - 1, gui.cw, 1, edge);
  fill_rect(gui.ox, gui.oy, 1, gui.title_h + gui.ch, edge);
  fill_rect(gui.ox + gui.cw - 1, gui.oy, 1, gui.title_h + gui.ch, edge);
  if (chrome_over) chrome_over();
  cursor_redraw_at((int)sys_mx, (int)sys_my, sys_cursor_col());
  gfx_frame_end();
  gui_painting = 0;
}

static int gui_key_ext(uint8_t sc) {
  switch (sc) {
    case 0x48: return GUI_KEY_UP;
    case 0x50: return GUI_KEY_DOWN;
    case 0x4B: return GUI_KEY_LEFT;
    case 0x4D: return GUI_KEY_RIGHT;
    case 0x49: return GUI_KEY_PGUP;
    case 0x51: return GUI_KEY_PGDN;
    case 0x47: return GUI_KEY_HOME;
    case 0x4F: return GUI_KEY_END;
    case 0x53: return GUI_KEY_DEL;
  }
  return 0;
}

/* 1 se ha tecla, clique ou fechar esperando. Movimento do rato nao conta, nem
 * o soltar do botao: no duplo clique num link, o soltar do segundo clique
 * ficava na fila e interrompia de novo cada conexao (a pagina parava em 5%). */
int gui_input_pending(void) {
  int k;
  if (!gui_ok()) return 0;
  if (gui.ask_close) return 1;
  for (k = 0; k < gui.qn; k++) {
    int t = gui.qt[(gui.qr + k) % GUI_Q];
    if (t == GUI_EV_KEY || t == GUI_EV_DOWN || t == GUI_EV_CLOSE) return 1;
  }
  return 0;
}

static void gq_push(struct gui_win* w, int type, int x, int y, int key) {
  int i;
  if (!w || !w->open) return;
  if (w->qn >= GUI_Q) {
    w->qr = (w->qr + 1) % GUI_Q;
    w->qn--;
  }
  i = (w->qr + w->qn) % GUI_Q;
  w->qt[i] = type;
  w->qx[i] = x;
  w->qy[i] = y;
  w->qk[i] = key;
  w->qn++;
}

static void gui_q_push(int type, int x, int y, int key) {
  gq_push(&gui, type, x, y, key);
}

static int gui_q_pop(int* ev) {
  int i;
  if (gui.qn <= 0) return 0;
  i = gui.qr;
  ev[0] = gui.qt[i];
  ev[1] = gui.qx[i];
  ev[2] = gui.qy[i];
  ev[3] = gui.qk[i];
  gui.qr = (gui.qr + 1) % GUI_Q;
  gui.qn--;
  return ev[0];
}

/* 1 se o scancode vira uma tecla para o programa. */
static int gui_key_event(uint8_t data, int* key) {
  if (data == 0xE0) {
    gui.e0 = 1;
    return 0;
  }
  if (data == SCANCODE_LSHIFT || data == SCANCODE_RSHIFT) {
    gui.shift = 1;
    return 0;
  }
  if (data == (SCANCODE_LSHIFT | 0x80) || data == (SCANCODE_RSHIFT | 0x80)) {
    gui.shift = 0;
    return 0;
  }
  if (data & 0x80) {
    gui.e0 = 0;
    return 0;
  }
  {
    int k = gui_key_ext(data);
    int was_e0 = gui.e0;
    gui.e0 = 0;
    if (!k && !was_e0) {
      char c = gui.shift ? scancode_map_shift[data] : scancode_map[data];
      k = (unsigned char)c;
      if (c == '\n') k = '\r';
    }
    if (!k) return 0;
    *key = k;
    return 1;
  }
}

/* run_row e run_col sao do programa que esta rodando: cada tarefa guarda os seus. */
static void console_detach(struct gui_task* t) {
  if (t && !t->con && run_out) {
    t->con_row = run_row;
    t->con_col = run_col;
  }
  kexp_set_console_hook(0);
  run_out = 0;
  cons_cap = 0;
}

static void console_attach(struct gui_task* t) {
  if (!t) return;
  if (t->con) {
    cons_cap = t->con;
    kexp_set_console_hook(cons_emit);
  } else if (t->run && t->run->used && t->run->st) {
    run_out = t->run->st;
    run_row = t->con_row;
    run_col = t->con_col;
    kexp_set_console_hook(run_add);
  }
}

/* O programa fica parado nesta funcao ate o desktop retomar a tarefa.
 * Sem janela propria, gid fica -1: herdar gui_ix fazia o gui_open dele
 * tomar a janela de outro programa. */
static void gui_yield(int wake) {
  struct gui_task* t;
  if (task_cur < 0 || !host_on) return;
  t = &gtasks[task_cur];
  t->wake = wake;
  console_detach(t);
  mod_cpu_save(&t->cpu);
  mod_cpu_load(&host_cpu);
  ctx_switch(&t->rsp, host_rsp);
  console_attach(&gtasks[task_cur]);
}

int gui_coop_pause(void) {
  if (task_cur < 0 || !host_on) return 0;
  gui_yield(WAKE_BUSY);
  return 1;
}

static int gui_event_coop(int* ev, int timeout_ms) {
  int i;
  for (i = 0; i < 4; i++) ev[i] = 0;
  if (!gui_ok()) return -1;
  for (;;) {
    if (gui.ask_close) {
      gui.ask_close = 0;
      ev[0] = GUI_EV_CLOSE;
      return GUI_EV_CLOSE;
    }
    if (gui_q_pop(ev)) return ev[0];
    if (timeout_ms == 0) {
      gui_yield(WAKE_POLL);
      if (!gui_ok()) return -1;
      if (gui.ask_close) {
        gui.ask_close = 0;
        ev[0] = GUI_EV_CLOSE;
        return GUI_EV_CLOSE;
      }
      if (gui_q_pop(ev)) return ev[0];
      return GUI_EV_NONE;
    }
    gui_yield(WAKE_BLOCK);
    if (!gui_ok()) return -1;
  }
}

/* Espera um evento ate timeout_ms (-1 = sem limite). 0 = nada aconteceu. */
int gui_event(int* ev, int timeout_ms) {
  if (host_on && task_cur >= 0) return gui_event_coop(ev, timeout_ms);
  uint32_t t0 = net_ms();
  int i;
  for (i = 0; i < 4; i++) ev[i] = 0;
  if (!gui.open) return -1;
  for (;;) {
    uint8_t scb = 0;
    int from_abs = 0;
    int kind;
    if (gui.ask_close) {
      gui.ask_close = 0;
      ev[0] = GUI_EV_CLOSE;
      return GUI_EV_CLOSE;
    }
    if (gui_q_pop(ev)) return ev[0];
    kind = input_poll(gui.pkt, &gui.pkt_idx, &scb, &from_abs);
    if (kind < 0) {
      if (timeout_ms >= 0 && (int)(net_ms() - t0) >= timeout_ms) return GUI_EV_NONE;
      __asm__ volatile ("pause");
      continue;
    }
    if (kind == 1) {
      int left, right, x, y;
      int dz = mouse_wheel();
      if (!from_abs)
        sys_mouse_place(sys_mx + (int8_t)gui.pkt[1], sys_my - (int8_t)gui.pkt[2]);
      if (chrome_menu && *chrome_menu) gui_present();
      else cursor_draw((int)sys_mx, (int)sys_my, sys_cursor_col());
      left = gui.pkt[0] & 1;
      right = (gui.pkt[0] >> 1) & 1;
      x = (int)sys_mx - gui.ox;
      y = (int)sys_my - (gui.oy + gui.title_h);
      ev[1] = x;
      ev[2] = y;
      if (gui.drag) {
        if (left) {
          gui_move_to((int)sys_mx, (int)sys_my);
          gui_present();
        } else {
          gui.drag = 0;
          gui.prev_left = 0;
        }
        continue;
      }
      if ((left || right) && !gui.prev_left) {
        int close_s = gui.title_h - 4;
        int hit = 0;
        gui.prev_left = 1;
        if (chrome_click) hit = chrome_click((int)sys_mx, (int)sys_my);
        if (hit == 2) {
          ev[0] = GUI_EV_CLOSE;
          return GUI_EV_CLOSE;
        }
        if (hit == 1) {
          gui_present();
          continue;
        }
        if (wm_pt((int)sys_mx, (int)sys_my, gui.ox + gui.cw - close_s - 2, gui.oy + 2,
                  close_s, close_s)) {
          ev[0] = GUI_EV_CLOSE;
          return GUI_EV_CLOSE;
        }
        if (wm_pt((int)sys_mx, (int)sys_my, gui.ox, gui.oy, gui.cw, gui.title_h)) {
          gui.drag = 1;
          gui.drag_ox = (int)sys_mx - gui.ox;
          gui.drag_oy = (int)sys_my - gui.oy;
          continue;
        }
        if (y < 0 || x < 0 || x >= gui.cw || y >= gui.ch) continue;
        ev[0] = GUI_EV_DOWN;
        ev[3] = left ? 1 : 2;
        return GUI_EV_DOWN;
      }
      if (!left && !right && gui.prev_left) {
        gui.prev_left = 0;
        ev[0] = GUI_EV_UP;
        return GUI_EV_UP;
      }
      if (left && gui.prev_left && (from_abs || (int8_t)gui.pkt[1] || (int8_t)gui.pkt[2])) {
        ev[0] = GUI_EV_DRAG;
        return GUI_EV_DRAG;
      }
      if (dz && x >= 0 && y >= 0 && x < gui.cw && y < gui.ch) {
        ev[0] = GUI_EV_WHEEL;
        ev[3] = dz;
        return GUI_EV_WHEEL;
      }
      continue;
    }
    {
      int k = 0;
      if (!gui_key_event(scb, &k)) continue;
      ev[0] = GUI_EV_KEY;
      ev[3] = k;
      return GUI_EV_KEY;
    }
  }
}

/* Esvazia a fila do controlador. O cursor acompanha cada pacote.
 * So tecla, clique novo e fechar entram na fila: o soltar do clique que
 * comecou o carregamento nao pode cancela-lo. */
static void gui_idle(void) {
  int n, moved = 0, touch = 0, dragged = 0;
  if (!gui.open || gui_painting) return;
  for (n = 0; n < 24; n++) {
    uint8_t scb = 0;
    int from_abs = 0;
    int kind = input_poll(gui.pkt, &gui.pkt_idx, &scb, &from_abs);
    if (kind < 0) break;
    if (kind == 0) {
      int key = 0;
      if (gui_key_event(scb, &key)) gui_q_push(GUI_EV_KEY, 0, 0, key);
      continue;
    }
    moved = 1;
    {
      int dz = mouse_wheel();
      if (!from_abs)
        sys_mouse_place(sys_mx + (int8_t)gui.pkt[1], sys_my - (int8_t)gui.pkt[2]);
      if (dz) {
        int wx = (int)sys_mx - gui.ox, wy = (int)sys_my - (gui.oy + gui.title_h);
        if (wx >= 0 && wy >= 0 && wx < gui.cw && wy < gui.ch) gui_q_push(GUI_EV_WHEEL, wx, wy, dz);
      }
    }
    {
      int left = gui.pkt[0] & 1;
      int right = (gui.pkt[0] >> 1) & 1;
      if ((left || right) && !gui.prev_left) {
        int close_s = gui.title_h - 4;
        int x = (int)sys_mx - gui.ox;
        int y = (int)sys_my - (gui.oy + gui.title_h);
        int hit = 0;
        gui.prev_left = 1;
        if (chrome_click) hit = chrome_click((int)sys_mx, (int)sys_my);
        if (hit == 2) {
          gui_q_push(GUI_EV_CLOSE, 0, 0, 0);
          touch = 1;
        } else if (hit == 1) {
          touch = 1;
        } else if (wm_pt((int)sys_mx, (int)sys_my, gui.ox + gui.cw - close_s - 2,
                         gui.oy + 2, close_s, close_s)) {
          gui_q_push(GUI_EV_CLOSE, 0, 0, 0);
        } else if (wm_pt((int)sys_mx, (int)sys_my, gui.ox, gui.oy, gui.cw, gui.title_h)) {
          gui.drag = 1;
          gui.drag_ox = (int)sys_mx - gui.ox;
          gui.drag_oy = (int)sys_my - gui.oy;
        } else if (y >= 0 && x >= 0 && x < gui.cw && y < gui.ch) {
          gui_q_push(GUI_EV_DOWN, x, y, left ? 1 : 2);
        }
      } else if (gui.drag && left) {
        gui_move_to((int)sys_mx, (int)sys_my);
        dragged = 1;
      } else if (!left && !right) {
        gui.drag = 0;
        gui.prev_left = 0;
      }
    }
  }
  if (touch || dragged) gui_present();
  else if (moved) cursor_draw((int)sys_mx, (int)sys_my, sys_cursor_col());
}

static int g_drag = -1;
static int g_drag_ox, g_drag_oy;
static int g_cap = -1;

static void task_arm(struct gui_task* t, void (*fn)(void)) {
  uintptr_t top = ((uintptr_t)t->stack + TASK_STACK) & ~(uintptr_t)15;
  uint64_t* sp = (uint64_t*)top;
  /* 8 qwords: 6 registros, o endereco de fn, e um byte de alinhamento.
   * ctx_switch desempilha 6 e da ret; rsp fica % 16 == 8. */
  sp -= 8;
  sp[0] = sp[1] = sp[2] = sp[3] = sp[4] = sp[5] = 0;
  sp[6] = (uint64_t)(uintptr_t)fn;
  t->rsp = (uint64_t)(uintptr_t)sp;
}

static void cmd_refocus(struct cmd_con* c);

static void task_reap(int ti) {
  struct gui_task* t = &gtasks[ti];
  if (t->gid >= 0 && t->gid < GUI_MAX && gwins[t->gid].open) {
    int save = gui_ix;
    gui_ix = t->gid;
    gui_close();
    gui_ix = save;
  }
  if (t->con) cmd_refocus(t->con);
  if (t->stack) kfree(t->stack);
  t->stack = 0;
  t->run = 0;
  t->con = 0;
  t->used = 0;
  t->wake = WAKE_NONE;
  t->gid = -1;
}

static void cons_puts(const char* s) {
  while (s && *s) cons_emit(*s++);
}

static void task_entry(void) {
  struct gui_task* t = &gtasks[task_cur];
  char* av[TASK_ARGC + 1];
  char* p = t->args;
  int rc, i;
  for (i = 0; i < t->argc; i++) {
    av[i] = p;
    while (*p) p++;
    p++;
  }
  av[i] = 0;
  console_attach(t);
  rc = mod_exec(t->path, t->argc, av);
  console_detach(t);
  if (t->gid >= 0 && t->gid < GUI_MAX && gwins[t->gid].open) {
    gui_ix = t->gid;
    gui_close();
  }
  if (t->con) {
    cons_cap = t->con;
    if (rc <= MOD_ERR_NOTFOUND && rc >= MOD_ERR_DEPTH) {
      cons_puts("Erro ao executar: ");
      cons_puts(mod_last_error());
      cons_puts("\n");
    }
    t->con->busy = 0;
    cons_cap = 0;
    win_api_dirty = 1;
  }
  if (t->run && t->run->used && t->run->st) wm_run_status(t->run, rc);
  t->wake = WAKE_DEAD;
  mod_cpu_load(&host_cpu);
  ctx_switch(&t->rsp, host_rsp);
  for (;;) __asm__ volatile ("hlt");
}

static void task_switch_to(int ti) {
  struct gui_task* t = &gtasks[ti];
  if (!t->used || t->wake == WAKE_DEAD || t->wake == WAKE_NONE) return;
  mod_cpu_save(&host_cpu);
  mod_cpu_load(&t->cpu);
  task_cur = ti;
  gui_ix = (t->gid >= 0 && t->gid < GUI_MAX) ? t->gid : 0;
  ctx_switch(&host_rsp, t->rsp);
  task_cur = -1;
  if (t->wake == WAKE_DEAD) task_reap(ti);
}

static void task_start(int ti) {
  struct gui_task* t = &gtasks[ti];
  mod_cpu_save(&host_cpu);
  task_cur = ti;
  gui_ix = 0;
  ctx_switch(&host_rsp, t->rsp);
  task_cur = -1;
  if (t->wake == WAKE_DEAD) task_reap(ti);
}

/* Copia o argv para t->args. Sem argv[0], usa o caminho. */
static void task_set_args(struct gui_task* t, int argc, char** argv) {
  int k = 0, a;
  t->argc = 0;
  for (a = 0; a < argc && a < TASK_ARGC; a++) {
    const char* s = (argv && argv[a]) ? argv[a] : "";
    int n = 0;
    if (a == 0 && !s[0]) s = t->path;
    while (s[n]) n++;
    if (k + n + 1 > TASK_ARGBUF) break;
    while (*s) t->args[k++] = *s++;
    t->args[k++] = '\0';
    t->argc++;
  }
  if (t->argc == 0) {
    wm_copy_str(t->args, TASK_ARGBUF, t->path);
    t->argc = 1;
  }
}

/* con: o Command que abriu o programa recebe a saida. Sem ele, janela propria. */
static int task_spawn(struct wm_win* wins, int* focus, int big, const char* path,
                      struct wm_win* owner, int argc, char** argv, struct cmd_con* con) {
  int ti, i = -1;
  struct gui_task* t;
  if (!gui_ensure()) return -1;
  for (ti = 0; ti < TASK_MAX; ti++)
    if (!gtasks[ti].used) break;
  if (ti >= TASK_MAX) {
    serial_puts("[visual] janelas de programa cheias\n");
    return -1;
  }
  t = &gtasks[ti];
  t->used = 1;
  t->wake = WAKE_NONE;
  t->gid = -1;
  t->run = 0;
  t->con = con;
  t->con_row = 0;
  t->con_col = 0;
  t->rsp = 0;
  t->stack = (uint8_t*)kmalloc(TASK_STACK);
  if (!t->stack) {
    t->used = 0;
    t->con = 0;
    serial_puts("[visual] sem pilha para o programa\n");
    return -1;
  }
  wm_copy_str(t->path, (int)sizeof(t->path), path ? path : "");
  task_set_args(t, argc, argv);
  if (!con) {
    i = wm_new_run_window(wins, big, path, owner);
    if (i >= 0) t->run = &wins[i];
  }
  task_arm(t, task_entry);
  task_start(ti);
  if (focus) {
    if (gui_focus >= 0) *focus = -1;
    else if (i >= 0) *focus = i;
  }
  return 0;
}

static int task_has_event(int ti) {
  struct gui_task* t = &gtasks[ti];
  if (!t->used || t->wake == WAKE_DEAD || t->wake == WAKE_NONE) return 0;
  if (t->gid < 0 || t->gid >= GUI_MAX || !gwins[t->gid].open) return 0;
  return gwins[t->gid].qn > 0;
}

static int task_pick(void) {
  int i, n, best = -1, bz = -1;
  if (!gtasks || !gwins) return -1;
  for (i = 0; i < TASK_MAX; i++) {
    if (!task_has_event(i)) continue;
    if (gwins[gtasks[i].gid].z >= bz) {
      bz = gwins[gtasks[i].gid].z;
      best = i;
    }
  }
  if (best >= 0) return best;
  for (n = 0; n < TASK_MAX; n++) {
    i = (task_rr + n) % TASK_MAX;
    if (!gtasks[i].used) continue;
    if (gtasks[i].wake == WAKE_BUSY || gtasks[i].wake == WAKE_POLL) return i;
  }
  return -1;
}

static void tasks_pump(void) {
  int ti = task_pick();
  if (ti < 0) return;
  task_rr = ti + 1;
  task_switch_to(ti);
}

static void tasks_shutdown(void) {
  int i, n;
  if (!gtasks) {
    host_on = 0;
    return;
  }
  for (i = 0; i < TASK_MAX; i++) {
    if (!gtasks[i].used) continue;
    if (gtasks[i].gid >= 0 && gtasks[i].gid < GUI_MAX && gwins[gtasks[i].gid].open)
      gq_push(&gwins[gtasks[i].gid], GUI_EV_CLOSE, 0, 0, 0);
    for (n = 0; n < 256 && gtasks[i].used && gtasks[i].wake != WAKE_DEAD; n++) {
      if (gtasks[i].wake == WAKE_NONE) break;
      task_switch_to(i);
    }
    if (gtasks[i].used && gtasks[i].wake != WAKE_DEAD) {
      gtasks[i].used = 0;
      gtasks[i].stack = 0;
    } else if (gtasks[i].used) {
      task_reap(i);
    }
  }
  gui_focus = -1;
  g_drag = -1;
  g_cap = -1;
  host_on = 0;
}

/* A janela vai sumir: o programa que escrevia nela segue sem saida. */
static void task_forget_win(struct wm_win* w) {
  int i;
  if (!gtasks || !w) return;
  for (i = 0; i < TASK_MAX; i++) {
    if (gtasks[i].run == w) gtasks[i].run = 0;
    if (w->kind == WM_KIND_CMD && w->st && gtasks[i].con == (struct cmd_con*)w->st)
      gtasks[i].con = 0;
  }
}

static int win_ieq(const char* a, const char* b);

static int gui_raise_title(const char* title) {
  int i;
  if (!gwins) return 0;
  for (i = 0; i < GUI_MAX; i++) {
    if (!gwins[i].open || !win_ieq(gwins[i].title, title)) continue;
    gwins[i].z = ++zseq;
    gui_focus = i;
    gui_stole = 1;
    win_api_dirty = 1;
    return 1;
  }
  return 0;
}

/* .EXE/.DLL do cc+link (SOX): loader de modulos, saida de texto na janela. */
static void wm_exec_sox(struct wm_win* wins, int* focus, int big, const char* path,
                        struct wm_win* owner, int argc, char** argv) {
  int i;
  if (host_on && task_cur < 0) {
    task_spawn(wins, focus, big, path, owner, argc, argv, 0);
    return;
  }
  if (mod_depth > 0) {
    serial_puts("[visual] programa ocupado, sem outro por cima\n");
    return;
  }
  i = wm_new_run_window(wins, big, path, owner);
  int rc;
  if (i < 0) return;
  kexp_set_console_hook(run_add);
  gui_session(1);
  rc = mod_exec(path, argc, argv);
  if (!host_on) gui_session(0);
  kexp_set_console_hook(0);
  run_out = 0;
  wm_run_status(&wins[i], rc);
  *focus = i;
}

static void wm_exec(struct wm_win* wins, int* focus, int big, const char* path) {
  uint8_t* bak;
  int n;
  int i;
  int rc;
  uint32_t magic[2] = {0, 0};
  struct wm_win* owner = app_win;
  if (!path) return;
  if (path_is_command(path)) {
    wm_open_command(wins, focus, big);
    return;
  }
  /* file_load le no maximo max-1 bytes (poe um NUL no fim) */
  if (file_load(path, (char*)magic, 5) == 4 && magic[0] == SOX_MAGIC) {
    char* argv[2];
    argv[0] = (char*)path;
    argv[1] = 0;
    wm_exec_sox(wins, focus, big, path, owner, 1, argv);
    app_win = owner;
    if (owner && owner->st && gui_focus < 0) app_status_set("executou");
    return;
  }
  if (mod_depth > 0) {
    serial_puts("[visual] programa ocupado, sem outro por cima\n");
    return;
  }
  bak = (uint8_t*)kmalloc(PROG_SLOT_N);
  if (!bak) {
    app_status_set("sem memoria");
    return;
  }
  wm_slot_copy(bak, PROG_SLOT);
  n = file_load(path, (char*)PROG_SLOT, (int)PROG_SLOT_N);
  if (n <= 0) {
    wm_slot_copy(PROG_SLOT, bak);
    kfree(bak);
    app_status_set("nao executou");
    return;
  }
  for (i = n; i < (int)PROG_SLOT_N; i++) PROG_SLOT[i] = 0xC3;
  i = wm_new_run_window(wins, big, path, owner);
  if (i < 0) {
    wm_slot_copy(PROG_SLOT, bak);
    kfree(bak);
    return;
  }
  rc = ((bin_fn)PROG_SLOT)(run_puts);
  wm_slot_copy(PROG_SLOT, bak);
  kfree(bak);
  run_out = 0;
  wm_run_status(&wins[i], rc);
  *focus = i;
  app_win = owner;
  if (owner && owner->st)
    app_status_set("executou");
}

/* Icones de D:\user\default\desktop. Pasta termina sem barra.
 * Imagem com miniatura ao lado (NOME.THM) aparece como a propria figura. */
#define DESK_MAX 24
#define DESK_ROOT "D:\\user\\default\\desktop\\"
struct desk_item {
  char name[16];
  char path[48];
  int image;
  int dir;
  int tw, th;
  uint8_t* px;
};
static struct desk_item desk_items[DESK_MAX];
static int desk_n;

static void desk_free(void) {
  int i;
  for (i = 0; i < desk_n; i++) {
    if (desk_items[i].px) kfree(desk_items[i].px);
    desk_items[i].px = 0;
  }
  desk_n = 0;
}

static int desk_ext_is(const char* name, const char* ext) {
  int n = 0;
  int i;
  while (name[n]) n++;
  if (n < 4 || name[n - 4] != '.') return 0;
  for (i = 0; i < 3; i++) {
    char c = name[n - 3 + i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c != ext[i]) return 0;
  }
  return 1;
}

static int desk_is_image(const char* name) {
  return desk_ext_is(name, "PNG") || desk_ext_is(name, "JPG") || desk_ext_is(name, "JPE") ||
         desk_ext_is(name, "GIF") || desk_ext_is(name, "ICO") || desk_ext_is(name, "BMP");
}

static int desk_is_video(const char* name) {
  return desk_ext_is(name, "MP4") || desk_ext_is(name, "MOV") || desk_ext_is(name, "M4V") ||
         desk_ext_is(name, "AVI") || desk_ext_is(name, "MPG") || desk_ext_is(name, "MPE") ||
         desk_ext_is(name, "MJG") || desk_ext_is(name, "MJP") || desk_ext_is(name, "3GP") ||
         desk_ext_is(name, "MKV") || desk_ext_is(name, "WMV") || desk_ext_is(name, "FLV") ||
         desk_ext_is(name, "OGV");
}

static void desk_load_thm(struct desk_item* it) {
  char thm[48];
  int i, n = 0, dot = -1, sz, w, h;
  uint8_t* b;
  while (it->path[n]) n++;
  for (i = 0; i < n; i++)
    if (it->path[i] == '.') dot = i;
  if (dot <= 0 || n - dot != 4 || dot + 5 > (int)sizeof(thm)) return;
  for (i = 0; i < dot; i++) thm[i] = it->path[i];
  thm[dot] = '.';
  thm[dot + 1] = 'T';
  thm[dot + 2] = 'H';
  thm[dot + 3] = 'M';
  thm[dot + 4] = 0;
  sz = file_size(thm);
  if (sz < 8 || sz > 8 + 64 * 64 * 4) return;
  b = (uint8_t*)kmalloc((uint32_t)sz + 1);
  if (!b) return;
  if (file_load(thm, (char*)b, sz + 1) < 8 ||
      b[0] != 'T' || b[1] != 'H' || b[2] != 'M' || b[3] != '1') {
    kfree(b);
    return;
  }
  w = b[4] | (b[5] << 8);
  h = b[6] | (b[7] << 8);
  if (w < 1 || h < 1 || w > 64 || h > 64 || 8 + w * h * 4 > sz) {
    kfree(b);
    return;
  }
  it->tw = w;
  it->th = h;
  it->px = b;
}

static void desk_load(void) {
  char list[1024];
  int n, p;
  const char* prefix = DESK_ROOT;
  int plen = 0;
  desk_free();
  if (!drives_is_mounted('D')) return;
  while (prefix[plen]) plen++;
  n = file_list(DESK_ROOT, list, (int)sizeof(list));
  if (n <= 0) return;
  p = 0;
  while (p < n && desk_n < DESK_MAX) {
    struct desk_item* it;
    int s = p, e, k, i, dir = 0;
    while (p < n && list[p] != '\n' && list[p] != '\r') p++;
    e = p;
    while (p < n && (list[p] == '\n' || list[p] == '\r')) p++;
    while (s < e && list[s] == ' ') s++;
    if (e > s && list[e - 1] == '\\') {
      dir = 1;
      e--;
    }
    while (e > s && list[e - 1] == ' ') e--;
    if (e <= s) continue;
    k = e - s;
    if (k > 12) continue;
    it = &desk_items[desk_n];
    for (i = 0; i < k; i++) {
      char c = list[s + i];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      it->name[i] = c;
    }
    it->name[k] = 0;
    if (!dir && desk_ext_is(it->name, "THM")) continue;
    if (plen + k >= (int)sizeof(it->path)) continue;
    for (i = 0; i < plen; i++) it->path[i] = prefix[i];
    for (i = 0; i < k; i++) it->path[plen + i] = it->name[i];
    it->path[plen + k] = 0;
    it->dir = dir;
    it->image = 0;
    if (!dir && desk_is_image(it->name)) it->image = 1;
    else if (!dir && desk_is_video(it->name)) it->image = 2;
    it->tw = it->th = 0;
    it->px = 0;
    if (it->image == 1) desk_load_thm(it);
    desk_n++;
  }
}

static int desk_place(int index, int icon, int ch, int scale, int sw, int sh, int bar_h,
                      int* x, int* y) {
  int label_w = 12 * FONT_W * scale;
  int cell_w = icon + 16;
  int cell_h = icon + 6 + ch;
  int x0, cols, rows, col, row;
  if (cell_w < label_w) cell_w = label_w;
  x0 = 12 + cell_w;
  cols = (sw - x0 - 4) / cell_w;
  rows = (sh - bar_h - 8) / cell_h;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  col = index % cols;
  row = index / cols;
  if (row >= rows) return 0;
  *x = x0 + col * cell_w;
  *y = 12 + row * cell_h;
  return 1;
}

static uint32_t desk_color(int r, int g, int b) {
  static const int pr[16] = {0, 0, 0, 0, 168, 168, 168, 168, 84, 84, 84, 84, 255, 255, 255, 255};
  static const int pg[16] = {0, 0, 168, 168, 0, 0, 84, 168, 84, 84, 255, 255, 84, 84, 255, 255};
  static const int pb[16] = {0, 168, 0, 168, 0, 168, 0, 168, 84, 255, 84, 255, 84, 255, 84, 255};
  int i, best = 0, bd = 1 << 30;
  if (gfx_bochs) return PIXA(r, g, b);
  for (i = 0; i < 16; i++) {
    int dr = r - pr[i], dg = g - pg[i], db = b - pb[i];
    int d = dr * dr + dg * dg + db * db;
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return (uint32_t)best;
}

static void desk_blit(int x, int y, int side, const uint8_t* rgba, int tw, int th) {
  int iw, ih, ox, oy, iy, ix;
  if (tw < 1 || th < 1 || side < 1) return;
  if (tw >= th) {
    iw = side;
    ih = th * side / tw;
    if (ih < 1) ih = 1;
  } else {
    ih = side;
    iw = tw * side / th;
    if (iw < 1) iw = 1;
  }
  ox = x + (side - iw) / 2;
  oy = y + (side - ih) / 2;
  for (iy = 0; iy < ih; iy++) {
    int sy = iy * th / ih;
    if (sy >= th) sy = th - 1;
    for (ix = 0; ix < iw; ix++) {
      int sx = ix * tw / iw;
      const uint8_t* p;
      if (sx >= tw) sx = tw - 1;
      p = rgba + ((uint32_t)sy * (uint32_t)tw + (uint32_t)sx) * 4;
      if (p[3] < 128) continue;
      put_pixel(ox + ix, oy + iy, desk_color(p[0], p[1], p[2]));
    }
  }
}

static void desk_draw(int icon, int ch, int scale, int sw, int sh, int bar_h,
                      uint32_t col_desk, uint32_t col_white, uint32_t col_black) {
  int i;
  for (i = 0; i < desk_n; i++) {
    int x, y;
    struct desk_item* it = &desk_items[i];
    if (!desk_place(i, icon, ch, scale, sw, sh, bar_h, &x, &y)) break;
    if (it->px && it->tw > 0) {
      desk_blit(x, y, icon, it->px + 8, it->tw, it->th);
    } else if (it->image == 2) {
      uint32_t bg = desk_color(24, 24, 28);
      uint32_t fg = desk_color(220, 48, 48);
      int s = icon / 5;
      int h1, h2;
      if (s < 2) s = 2;
      h1 = icon - 2 * s;
      h2 = icon - 3 * s;
      if (h1 < 1) h1 = 1;
      if (h2 < 1) h2 = 1;
      fill_rect(x, y, icon, icon, bg);
      fill_rect(x, y, icon, 1, col_black);
      fill_rect(x, y + icon - 1, icon, 1, col_black);
      fill_rect(x, y, 1, icon, col_black);
      fill_rect(x + icon - 1, y, 1, icon, col_black);
      fill_rect(x + s, y + s, s / 2 > 1 ? s / 2 : 1, h1, fg);
      fill_rect(x + s + s / 2, y + s + s / 2, s, h2, fg);
    } else if (it->dir) {
      int tab = icon / 3;
      int top = icon / 5;
      uint32_t yel = desk_color(220, 176, 32);
      if (tab < 4) tab = 4;
      if (top < 3) top = 3;
      fill_rect(x, y + top, tab, top, yel);
      fill_rect(x, y + top * 2, icon, icon - top * 2, yel);
      fill_rect(x, y + top * 2, icon, 1, col_black);
      fill_rect(x, y + icon - 1, icon, 1, col_black);
      fill_rect(x, y + top * 2, 1, icon - top * 2, col_black);
      fill_rect(x + icon - 1, y + top * 2, 1, icon - top * 2, col_black);
    } else {
      fill_rect(x, y, icon, icon, col_white);
      fill_rect(x, y, icon, 1, col_black);
      fill_rect(x, y + icon - 1, icon, 1, col_black);
      fill_rect(x, y, 1, icon, col_black);
      fill_rect(x + icon - 1, y, 1, icon, col_black);
    }
    draw_string_scaled(x, y + icon + 2, it->name, col_white, col_desk, scale);
  }
}

static int desk_hit(int mx, int my, int icon, int ch, int scale, int sw, int sh, int bar_h) {
  int i;
  for (i = 0; i < desk_n; i++) {
    int x, y;
    if (!desk_place(i, icon, ch, scale, sw, sh, bar_h, &x, &y)) break;
    if (wm_pt(mx, my, x, y, icon, icon + 2 + ch)) return i;
  }
  return -1;
}

static void wm_open_browser(struct wm_win* wins, int* focus, int big) {
  if (gui_raise_title("Navegador")) return;
  if (drives_is_mounted('D') && file_size("D:\\BROWSER.EXE") > 0) {
    wm_exec(wins, focus, big, "D:\\BROWSER.EXE");
    return;
  }
  {
    int nw = wm_new_run_window(wins, big, "Navegador", 0);
    serial_puts("[visual] sem D:BROWSER.EXE (make disk-files)\n");
    if (nw >= 0) {
      run_puts("D:\\BROWSER.EXE nao encontrado.\n\n"
               "Feche o QEMU e rode no Linux/WSL:\n"
               "  make disk-files\n"
               "  make run\n");
      run_out = 0;
      *focus = nw;
    }
  }
}

static void wm_open_video(struct wm_win* wins, int* focus, int big, const char* file) {
  char* argv[3];
  if (drives_is_mounted('D') && file_size("D:\\VIDPLAY.EXE") > 0 && file) {
    argv[0] = "D:\\VIDPLAY.EXE";
    argv[1] = (char*)file;
    argv[2] = 0;
    wm_exec_sox(wins, focus, big, argv[0], app_win, 2, argv);
    return;
  }
  {
    int nw = wm_new_run_window(wins, big, "Video", 0);
    if (nw >= 0) {
      run_puts("D:\\VIDPLAY.EXE nao encontrado.\n\n"
               "Feche o QEMU e rode:\n"
               "  make disk-files\n"
               "  make run\n");
      run_out = 0;
      *focus = nw;
    }
  }
}

static void wm_open_image(struct wm_win* wins, int* focus, int big, const char* file) {
  char* argv[3];
  if (drives_is_mounted('D') && file_size("D:\\IMAGEVW.EXE") > 0 && file) {
    argv[0] = "D:\\IMAGEVW.EXE";
    argv[1] = (char*)file;
    argv[2] = 0;
    wm_exec_sox(wins, focus, big, argv[0], app_win, 2, argv);
    return;
  }
  {
    int nw = wm_new_run_window(wins, big, "Imagem", 0);
    if (nw >= 0) {
      run_puts("D:\\IMAGEVW.EXE nao encontrado.\n\n"
               "Feche o QEMU e rode:\n"
               "  make disk-files\n"
               "  make run\n");
      run_out = 0;
      *focus = nw;
    }
  }
}

/* janela preta com ">_", o atalho do prompt no desktop */
static void draw_cmd_icon(int x, int y, int s, uint32_t fg, uint32_t bg) {
  int i;
  fill_rect(x, y, s, s, bg);
  fill_rect(x, y, s, 1, fg);
  fill_rect(x, y + s - 1, s, 1, fg);
  fill_rect(x, y, 1, s, fg);
  fill_rect(x + s - 1, y, 1, s, fg);
  if (s >= FONT_W * 2 + 6) {
    int rows = glyph_rows();
    draw_string_scaled(x + 4, y + (s - rows) / 2, ">_", fg, bg, 1);
    return;
  }
  for (i = 0; i < 4 && 3 + i < s; i++) {
    put_pixel(x + 3 + i, y + s / 2 - 3 + i, fg);
    put_pixel(x + 3 + i, y + s / 2 + 3 - i, fg);
  }
}

/* globo: circulo, equador e um meridiano, para o icone WEB do desktop */
static void draw_nav_icon(int x, int y, int s, uint32_t bg, uint32_t fg) {
  int cx, cy, r, i, j;
  fill_rect(x, y, s, s, bg);
  fill_rect(x, y, s, 1, fg);
  fill_rect(x, y + s - 1, s, 1, fg);
  fill_rect(x, y, 1, s, fg);
  fill_rect(x + s - 1, y, 1, s, fg);
  cx = x + s / 2;
  cy = y + s / 2;
  r = s / 2 - (s > 20 ? 4 : 2);
  if (r < 2) return;
  for (j = -r; j <= r; j++) {
    for (i = -r; i <= r; i++) {
      int d = i * i + j * j;
      int rim = d <= r * r && d >= (r - 1) * (r - 1);
      int eq = (j == 0) && d <= r * r;
      int mer = 0;
      if (d <= r * r) {
        int e = i * i * 4 + j * j;
        int inner = r > 2 ? (r - 2) * (r - 2) : 0;
        mer = e <= r * r && e >= inner;
      }
      if (rim || eq || mer) put_pixel(cx + i, cy + j, fg);
    }
  }
}

static void wm_open_app(struct wm_win* wins, int* focus, int big) {
  int i = wm_alloc(wins);
  struct app_state* st;
  if (i < 0) return;
  st = (struct app_state*)kmalloc(sizeof(struct app_state));
  if (!st) return;
  app_zero(st);
  wins[i].kind = WM_KIND_APP;
  wins[i].st = st;
  wm_place(&wins[i], wins, big, WM_KIND_APP);
  wins[i].used = 1;
  app_win = &wins[i];
  app_api_init();
  if (asm_build("explorer.asm") != 0) {
    app_set_title("Explorer");
    app_add("nao montou explorer.asm");
    app_status_set("asm explorer.asm no DOS");
    wm_raise(&wins[i]);
    *focus = i;
    return;
  }
  app_entry = (win_app_fn)PROG_SLOT;
  app_entry(WIN_MSG_INIT, app_api_table, st);
  wm_raise(&wins[i]);
  *focus = i;
}

static void wm_open_mgr(struct wm_win* wins, int* focus, int big) {
  int i = wm_find_kind(wins, WM_KIND_MGR);
  if (i < 0) i = wm_alloc(wins);
  if (i < 0) return;
  if (!wins[i].used) {
    wins[i].used = 1;
    wins[i].kind = WM_KIND_MGR;
    wins[i].x = big ? gfx_sx(80) : 36;
    wins[i].y = big ? gfx_sy(56) : 8;
    wins[i].w = big ? gfx_sx(480) : 220;
    wins[i].h = big ? gfx_sy(280) : 120;
    wm_clamp_rect(&wins[i].x, &wins[i].y, &wins[i].w, &wins[i].h, big);
    wm_copy_str(wins[i].title, (int)sizeof(wins[i].title), "Gerenciador");
  }
  wm_raise(&wins[i]);
  *focus = i;
}

static void wm_open_display(struct wm_win* wins, int* focus, int big) {
  int scale = big ? 2 : 1;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int ih = ch + (big ? 4 : 2);
  int title_h = ch + (big ? 4 : 2);
  int i = wm_find_kind(wins, WM_KIND_DISP);
  if (i < 0) i = wm_alloc(wins);
  if (i < 0) return;
  if (!wins[i].used) {
    wins[i].used = 1;
    wins[i].kind = WM_KIND_DISP;
    wins[i].st = 0;
    wins[i].w = 12 * FONT_W * scale + 48;
    wins[i].h = title_h + 8 + DISP_N * ih + 8;
    wins[i].x = big ? gfx_sx(160) : 40;
    wins[i].y = big ? gfx_sy(80) : 16;
    wm_clamp_rect(&wins[i].x, &wins[i].y, &wins[i].w, &wins[i].h, big);
    wm_copy_str(wins[i].title, (int)sizeof(wins[i].title), "Configuracoes de tela");
  }
  disp_hot = -1;
  disp_scroll = 0;
  wm_raise(&wins[i]);
  *focus = i;
}

static int font_sample_h(int scale) {
  if (scale < 1) scale = 1;
  return FONT_H * scale;
}

static int font_list_y(struct wm_win* w, int title_h, int scale) {
  return w->y + title_h + 8 + font_sample_h(scale) + 8;
}

static void font_blit(int px, int py, int max_x, const char* s, int which, int scale,
                      uint32_t fg, uint32_t bg) {
  int rows = (which == 0 && vga_bios_font_ok) ? FONT_H : 8;
  if (scale < 1) scale = 1;
  while (*s) {
    unsigned char c = (unsigned char)*s++;
    const uint8_t* g;
    int i, x;
    if (px + FONT_W * scale > max_x) break;
    if (which == 0 && vga_bios_font_ok)
      g = &vga_bios_font[(uint32_t)c * 32u];
    else if (c >= 32 && c <= 126)
      g = font8x8_basic[c - 32];
    else
      g = font8x8_basic['?' - 32];
    for (i = 0; i < rows; i++) {
      uint8_t bits = g[i];
      for (x = 0; x < FONT_W; x++) {
        uint32_t col = (bits & (uint8_t)(1u << (7 - x))) ? fg : bg;
        if (scale == 1)
          put_pixel(px + x, py + i, col);
        else
          fill_rect(px + x * scale, py + i * scale, scale, scale, col);
      }
    }
    px += FONT_W * scale;
  }
}

static void font_draw(struct wm_win* w, int title_h, int scale,
                      uint32_t col_win, uint32_t col_title, uint32_t col_white, uint32_t col_black) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int ih = ch + (scale > 1 ? 4 : 2);
  int y0 = font_list_y(w, title_h, scale);
  int which = (font_sel == 0 && vga_bios_font_ok) ? 0 : 1;
  int i;
  font_blit(w->x + 8, w->y + title_h + 8, w->x + w->w - 4,
            "Exemplo de fonte digitada", which, scale, col_black, col_win);
  for (i = 0; i < 2; i++) {
    int on = (i == which);
    int ry = y0 + i * ih;
    const char* name = i == 0 ? "VGA" : "8x8";
    fill_rect(w->x + 4, ry, w->w - 8, ih, col_win);
    draw_string_scaled(w->x + 10, ry + (ih - ch) / 2, name, col_black, col_win, scale);
    if (on)
      draw_string_scaled(w->x + w->w - FONT_W * scale - 12, ry + (ih - ch) / 2,
                         "*", col_black, col_win, scale);
  }
  (void)col_title;
  (void)col_white;
}

static void wm_open_fonts(struct wm_win* wins, int* focus, int big) {
  int scale = big ? 2 : 1;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int ih = ch + (big ? 4 : 2);
  int title_h = ch + (big ? 4 : 2);
  int i = wm_find_kind(wins, WM_KIND_FONT);
  if (i < 0) i = wm_alloc(wins);
  if (i < 0) return;
  if (!wins[i].used) {
    wins[i].used = 1;
    wins[i].kind = WM_KIND_FONT;
    wins[i].st = 0;
    wins[i].w = 26 * FONT_W * scale + 24;
    wins[i].h = title_h + 8 + font_sample_h(scale) + 8 + 2 * ih + 8;
    wins[i].x = big ? gfx_sx(180) : 36;
    wins[i].y = big ? gfx_sy(90) : 18;
    wm_clamp_rect(&wins[i].x, &wins[i].y, &wins[i].w, &wins[i].h, big);
    wm_copy_str(wins[i].title, (int)sizeof(wins[i].title), "Fontes");
  }
  wm_raise(&wins[i]);
  *focus = i;
}

/* Menu de contexto do desktop: Console e Configuracoes de tela. */
#define POP_N 2
static void pop_geom(int ax, int ay, int scale, int scr_w, int scr_h, int bar_h,
                     int* x, int* y, int* rw, int* rh) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int item_h = ch + (scale > 1 ? 8 : 4);
  *rw = 22 * FONT_W * scale + 16;
  *rh = item_h * POP_N + 4;
  *x = ax;
  *y = ay;
  if (*x + *rw > scr_w - 2) *x = scr_w - 2 - *rw;
  if (*y + *rh > scr_h - bar_h - 2) *y = scr_h - bar_h - 2 - *rh;
  if (*x < 2) *x = 2;
  if (*y < 2) *y = 2;
}

struct start_menu {
  int x, y, w, h, item_h, n;
  int sub_x, sub_y, sub_w, sub_h;
  int mode_x, mode_y, mode_w, mode_h;
};

static void start_menu_geom(struct start_menu* m, int scr_w, int scr_h, int bar_h,
                            int menu_w, int menu_item_h, int scale) {
  m->n = 7;
  m->x = 2;
  m->w = menu_w;
  m->item_h = menu_item_h;
  m->h = menu_item_h * m->n + 4;
  m->y = scr_h - bar_h - m->h;
  m->sub_w = 18 * FONT_W * scale + 16;
  m->sub_h = menu_item_h * 3 + 4;
  m->sub_x = m->x + m->w - 4;
  m->sub_y = m->y + 3 * menu_item_h;
  if (m->sub_x + m->sub_w > scr_w - 2)
    m->sub_x = m->x - m->sub_w + 4;
  if (m->sub_y + m->sub_h > scr_h - bar_h)
    m->sub_y = scr_h - bar_h - m->sub_h;
  m->mode_w = 10 * FONT_W * scale + 16;
  m->mode_h = DISP_N * menu_item_h + 4;
  m->mode_x = m->sub_x + m->sub_w - 4;
  m->mode_y = m->sub_y;
  if (m->mode_x + m->mode_w > scr_w - 2)
    m->mode_x = m->sub_x - m->mode_w + 4;
  if (m->mode_y + m->mode_h > scr_h - bar_h)
    m->mode_y = scr_h - bar_h - m->mode_h;
  if (m->mode_y < 2) m->mode_y = 2;
}

static void menu_row(int x, int y, int w, int h, const char* label, int hot, int arrow,
                     uint32_t col_win, uint32_t col_title, uint32_t col_white,
                     uint32_t col_black, int scale) {
  uint32_t bg = hot ? col_title : col_win;
  uint32_t fg = hot ? col_white : col_black;
  fill_rect(x, y, w, h, bg);
  draw_string_scaled(x + 6, y + (h - (vga_bios_font_ok ? FONT_H : 8) * scale) / 2,
                     label, fg, bg, scale);
  if (arrow)
    draw_string_scaled(x + w - FONT_W * scale - 4,
                       y + (h - (vga_bios_font_ok ? FONT_H : 8) * scale) / 2,
                       ">", fg, bg, scale);
}

static void menu_frame(int x, int y, int w, int h, uint32_t edge) {
  fill_rect(x, y, w, 1, edge);
  fill_rect(x, y + h - 1, w, 1, edge);
  fill_rect(x, y, 1, h, edge);
  fill_rect(x + w - 1, y, 1, h, edge);
}

/* Desktop vivo enquanto um .EXE (o navegador) ocupa o processador. */
static struct wm_win* c_wins;
static int* c_focus;
static int* c_cfg;
static int* c_tela;
static int c_big, c_scale, c_bar_h, c_title_h, c_start_w, c_menu_w, c_item_h;
static int c_icon, c_icon2_y, c_icon3_y, c_icon4_y;
static uint32_t c_desk, c_win, c_title, c_bar, c_white, c_black, c_btn, c_inactive;

/* A janela de texto do .EXE fica escondida enquanto a janela grafica dele esta aberta. */
static int run_hidden(struct wm_win* w) {
  int i;
  if (!w || !gtasks || !gwins || w->kind != WM_KIND_RUN) return 0;
  for (i = 0; i < TASK_MAX; i++) {
    if (!gtasks[i].used || gtasks[i].run != w) continue;
    if (gtasks[i].gid >= 0 && gtasks[i].gid < GUI_MAX && gwins[gtasks[i].gid].open) return 1;
  }
  return 0;
}

static int stack_top_z(struct wm_win* wins) {
  int i, top = 0;
  if (wins) {
    for (i = 0; i < WM_MAX; i++)
      if (wins[i].used && !run_hidden(&wins[i]) && wins[i].z > top) top = wins[i].z;
  }
  if (gwins) for (i = 0; i < GUI_MAX; i++)
    if (gwins[i].open && gwins[i].z > top) top = gwins[i].z;
  return top;
}

static void gui_draw_frame(struct gui_win* w, int active, uint32_t col_title, uint32_t col_white,
                           uint32_t col_black, uint32_t col_btn, uint32_t col_inactive) {
  int scale = w->scale > 0 ? w->scale : 1;
  int ch = glyph_rows() * scale;
  int close_s = w->title_h - 4;
  int close_x = w->ox + w->cw - close_s - 2;
  int ty = w->oy + (w->title_h - ch) / 2;
  int xoff = close_s > FONT_W * scale ? (close_s - FONT_W * scale) / 2 : 0;
  uint32_t tb = active ? col_title : col_inactive;
  int hh = w->title_h + w->ch;
  fill_rect(w->ox, w->oy, w->cw, w->title_h, tb);
  draw_string_scaled(w->ox + 4, ty, w->title, col_white, tb, scale);
  fill_rect(close_x, w->oy + 2, close_s, close_s, col_btn);
  draw_string_scaled(close_x + xoff, ty, "x", col_black, col_btn, scale);
  gui_blit_win(w);
  fill_rect(w->ox, w->oy, w->cw, 1, col_black);
  fill_rect(w->ox, w->oy + hh - 1, w->cw, 1, col_black);
  fill_rect(w->ox, w->oy, 1, hh, col_black);
  fill_rect(w->ox + w->cw - 1, w->oy, 1, hh, col_black);
}

static void paint_stacked(struct wm_win* wins, int title_h, int scale, uint32_t col_win,
                          uint32_t col_title, uint32_t col_white, uint32_t col_black,
                          uint32_t col_btn, uint32_t col_inactive) {
  int order[WM_MAX + GUI_MAX];
  int kind[WM_MAX + GUI_MAX];
  int n = 0, i, top;
  if (!wins) return;
  for (i = 0; i < WM_MAX; i++) {
    if (!wins[i].used || run_hidden(&wins[i])) continue;
    order[n] = i;
    kind[n] = 1;
    n++;
  }
  for (i = 0; gwins && i < GUI_MAX; i++) {
    if (!gwins[i].open) continue;
    order[n] = i;
    kind[n] = 2;
    n++;
  }
  for (i = 1; i < n; i++) {
    int oz = order[i], kz = kind[i], jpos;
    int z = kz == 1 ? wins[oz].z : gwins[oz].z;
    jpos = i;
    while (jpos > 0) {
      int pz = kind[jpos - 1] == 1 ? wins[order[jpos - 1]].z : gwins[order[jpos - 1]].z;
      if (pz <= z) break;
      order[jpos] = order[jpos - 1];
      kind[jpos] = kind[jpos - 1];
      jpos--;
    }
    order[jpos] = oz;
    kind[jpos] = kz;
  }
  top = stack_top_z(wins);
  for (i = 0; i < n; i++) {
    if (kind[i] == 1)
      wm_draw_window(&wins[order[i]], wins[order[i]].z == top && top > 0, title_h, scale,
                     col_win, col_title, col_white, col_black, col_btn, col_inactive);
    else
      gui_draw_frame(&gwins[order[i]], gwins[order[i]].z == top && top > 0,
                     col_title, col_white, col_black, col_btn, col_inactive);
  }
}

#define HIT_NONE 0
#define HIT_WM   1
#define HIT_GUI  2

static int z_top(struct wm_win* wins, int x, int y, int* kind) {
  int i, best = -1, bk = HIT_NONE, bz = -1;
  for (i = 0; i < WM_MAX; i++) {
    if (!wins[i].used || run_hidden(&wins[i])) continue;
    if (!wm_pt(x, y, wins[i].x, wins[i].y, wins[i].w, wins[i].h)) continue;
    if (wins[i].z >= bz) {
      bz = wins[i].z;
      best = i;
      bk = HIT_WM;
    }
  }
  for (i = 0; gwins && i < GUI_MAX; i++) {
    struct gui_win* w = &gwins[i];
    if (!w->open) continue;
    if (!wm_pt(x, y, w->ox, w->oy, w->cw, w->title_h + w->ch)) continue;
    if (w->z >= bz) {
      bz = w->z;
      best = i;
      bk = HIT_GUI;
    }
  }
  if (kind) *kind = bk;
  return best;
}

static void chrome_unbind(void) {
  tasks_shutdown();
  gui_session(0);
  chrome_under = 0;
  chrome_over = 0;
  chrome_click = 0;
  chrome_menu = 0;
  cons_paint = 0;
  c_wins = 0;
  c_focus = 0;
  c_cfg = 0;
  c_tela = 0;
}

static int win_ieq(const char* a, const char* b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

/* .EXE digitado no Command: vira tarefa e escreve nesta janela, que fica sem
 * prompt ate ele acabar. O desktop e os outros programas seguem rodando. */
static int cmd_spawn(const char* path, int argc, char** argv) {
  struct cmd_con* c = cons_cap;
  if (!host_on || task_cur >= 0 || !c_wins || !c) return 0;
  c->busy = 1;
  if (task_spawn(c_wins, c_focus, c_big, path, 0, argc, argv, c) != 0) {
    c->busy = 0;
    puts_k("Sem vaga para outro programa.\n");
  }
  return 1;
}

/* O programa do Command acabou e nada tem foco: o teclado volta para o Command. */
static void cmd_refocus(struct cmd_con* c) {
  int i;
  if (!c_wins || !c_focus || *c_focus >= 0 || gui_focus >= 0) return;
  for (i = 0; i < WM_MAX; i++) {
    if (!c_wins[i].used || c_wins[i].kind != WM_KIND_CMD) continue;
    if (c_wins[i].st != (struct app_state*)c) continue;
    wm_raise(&c_wins[i]);
    *c_focus = i;
    win_api_dirty = 1;
    return;
  }
}

int win_windows(char* out, int max) {
  int i, n = 0, pos = 0;
  if (!out || max < 2) return -1;
  out[0] = 0;
  if (!c_wins) return -1;
  for (i = 0; i < WM_MAX; i++) {
    int k;
    char num[16];
    if (!c_wins[i].used) continue;
    if (c_focus && *c_focus == i && pos < max - 1) out[pos++] = '*';
    if (pos < max - 1) out[pos++] = ' ';
    for (k = 0; c_wins[i].title[k] && pos < max - 2; k++) out[pos++] = c_wins[i].title[k];
    int32_to_string(c_wins[i].x, num, (int)sizeof(num));
    if (pos < max - 1) out[pos++] = ' ';
    for (k = 0; num[k] && pos < max - 2; k++) out[pos++] = num[k];
    int32_to_string(c_wins[i].y, num, (int)sizeof(num));
    if (pos < max - 1) out[pos++] = ' ';
    for (k = 0; num[k] && pos < max - 2; k++) out[pos++] = num[k];
    int32_to_string(c_wins[i].w, num, (int)sizeof(num));
    if (pos < max - 1) out[pos++] = ' ';
    for (k = 0; num[k] && pos < max - 2; k++) out[pos++] = num[k];
    int32_to_string(c_wins[i].h, num, (int)sizeof(num));
    if (pos < max - 1) out[pos++] = ' ';
    for (k = 0; num[k] && pos < max - 2; k++) out[pos++] = num[k];
    if (pos < max - 1) out[pos++] = '\n';
    n++;
  }
  if (n == 0 && pos < max - 1) {
    const char* z = "nenhuma\n";
    for (i = 0; z[i] && pos < max - 1; i++) out[pos++] = z[i];
  }
  out[pos] = 0;
  return n;
}

int win_close(const char* name) {
  int i;
  if (!c_wins || !name || !name[0]) return -1;
  for (i = 0; i < WM_MAX; i++) {
    int j;
    if (!c_wins[i].used || !win_ieq(c_wins[i].title, name)) continue;
    task_forget_win(&c_wins[i]);
    if (c_wins[i].st) {
      kfree(c_wins[i].st);
      c_wins[i].st = 0;
    }
    c_wins[i].used = 0;
    if (c_focus && *c_focus == i) {
      *c_focus = -1;
      for (j = 0; j < WM_MAX; j++)
        if (c_wins[j].used) { *c_focus = j; break; }
    }
    win_api_dirty = 1;
    return 0;
  }
  return -1;
}

static void chrome_paint_under(void) {
  int w = (int)gfx_w;
  int h = (int)gfx_h;
  gfx_clear(c_desk);
  if (!c_wins) return;
  fill_rect(12, 12, c_icon, c_icon, c_win);
  draw_string_scaled(12, 12 + c_icon + 2, "DOS", c_white, c_desk, c_scale);
  fill_rect(12, c_icon2_y, c_icon, c_icon, c_title);
  draw_string_scaled(12, c_icon2_y + c_icon + 2, "ARQ", c_white, c_desk, c_scale);
  draw_nav_icon(12, c_icon3_y, c_icon, c_title, c_white);
  draw_string_scaled(12, c_icon3_y + c_icon + 2, "WEB", c_white, c_desk, c_scale);
  draw_cmd_icon(12, c_icon4_y, c_icon, c_white, c_black);
  draw_string_scaled(12, c_icon4_y + c_icon + 2, "CMD", c_white, c_desk, c_scale);
  desk_draw(c_icon, (vga_bios_font_ok ? FONT_H : 8) * c_scale, c_scale, w, h, c_bar_h,
            c_desk, c_white, c_black);
  paint_stacked(c_wins, c_title_h, c_scale, c_win, c_title, c_white, c_black, c_btn, c_inactive);
}

static void chrome_paint_over(void) {
  int w = (int)gfx_w;
  int h = (int)gfx_h;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * c_scale;
  int bar_ty = h - c_bar_h + (c_bar_h - ch) / 2;
  struct start_menu gm;
  int mi;
  if (!chrome_menu) return;
  start_menu_geom(&gm, w, h, c_bar_h, c_menu_w, c_item_h, c_scale);
  fill_rect(0, h - c_bar_h, w, c_bar_h, c_bar);
  fill_rect(2, h - c_bar_h + 2, c_start_w, c_bar_h - 4, c_btn);
  draw_string_scaled(6, bar_ty, "Iniciar", c_black, c_btn, c_scale);
  draw_string_scaled(c_big ? w - 8 * 8 * c_scale : w - 64, bar_ty, "ESC=DOS", c_white, c_bar, c_scale);
  if (!*chrome_menu) return;
  {
    static const char* labels[] = {
      "Bloco de notas", "Gerenciador", "Explorer", "Configuracoes", "Navegador", "Command", "Sair"
    };
    fill_rect(gm.x, gm.y, gm.w, gm.h, c_win);
    for (mi = 0; mi < gm.n; mi++) {
      int hot = (mi == 3 && c_cfg && *c_cfg) ||
                ((int)sys_mx >= gm.x && (int)sys_mx < gm.x + gm.w &&
                 (int)sys_my >= gm.y + mi * gm.item_h &&
                 (int)sys_my < gm.y + (mi + 1) * gm.item_h);
      menu_row(gm.x + 1, gm.y + 2 + mi * gm.item_h, gm.w - 2, gm.item_h - 2,
               labels[mi], hot, mi == 3, c_win, c_title, c_white, c_black, c_scale);
    }
    menu_frame(gm.x, gm.y, gm.w, gm.h, c_black);
  }
  if (c_cfg && *c_cfg) {
    int tela_hot = (c_tela && *c_tela) ||
                   wm_pt((int)sys_mx, (int)sys_my, gm.sub_x, gm.sub_y, gm.sub_w, gm.item_h);
    int font_hot = wm_pt((int)sys_mx, (int)sys_my, gm.sub_x, gm.sub_y + gm.item_h,
                         gm.sub_w, gm.item_h);
    int adv_hot = wm_pt((int)sys_mx, (int)sys_my, gm.sub_x, gm.sub_y + 2 * gm.item_h,
                        gm.sub_w, gm.item_h);
    fill_rect(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h, c_win);
    menu_row(gm.sub_x + 1, gm.sub_y + 2, gm.sub_w - 2, gm.item_h - 2,
             "Tela", tela_hot, 1, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Fontes", font_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + 2 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Fontes avancadas", adv_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_frame(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h, c_black);
  }
  if (c_cfg && *c_cfg && c_tela && *c_tela) {
    fill_rect(gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h, c_win);
    for (mi = 0; mi < DISP_N; mi++) {
      int mode = mi - 1;
      int hot = (int)sys_mx >= gm.mode_x && (int)sys_mx < gm.mode_x + gm.mode_w &&
                (int)sys_my >= gm.mode_y + 2 + mi * c_item_h &&
                (int)sys_my < gm.mode_y + 2 + (mi + 1) * c_item_h;
      int on = mi == 0 ? gfx_mode_sel < 0
                       : ((uint32_t)gfx_w == gfx_mode_w[mode] &&
                          (uint32_t)gfx_h == gfx_mode_h[mode]);
      menu_row(gm.mode_x + 1, gm.mode_y + 2 + mi * c_item_h, gm.mode_w - 2, c_item_h,
               mi == 0 ? "Padrao" : gfx_mode_name[mode], hot, 0,
               c_win, c_title, c_white, c_black, c_scale);
      if (on)
        draw_string_scaled(gm.mode_x + gm.mode_w - FONT_W * c_scale - 6,
                           gm.mode_y + 2 + mi * c_item_h +
                           (c_item_h - ch) / 2,
                           "*", hot ? c_white : c_black, hot ? c_title : c_win, c_scale);
    }
    menu_frame(gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h, c_black);
  }
}

static void host_open_advanced(struct wm_win* wins, int* focus, int big) {
  if (gui_raise_title("Fontes avancadas")) return;
  wm_exec(wins, focus, big, "D:\\SODOS\\FONTADV.EXE");
}

static int chrome_in_browser(int x, int y) {
  return gui.open && x >= gui.ox && y >= gui.oy &&
         x < gui.ox + gui.cw && y < gui.oy + gui.title_h + gui.ch;
}

/* 0 = o navegador trata o clique. 1 = o desktop consumiu. 2 = fechar o .EXE. */
static int chrome_on_click(int x, int y) {
  struct start_menu gm;
  int w = (int)gfx_w;
  int h = (int)gfx_h;
  if (!c_wins || !chrome_menu || !c_focus) return 0;
  start_menu_geom(&gm, w, h, c_bar_h, c_menu_w, c_item_h, c_scale);
  if (*chrome_menu && c_tela && *c_tela &&
      wm_pt(x, y, gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h)) {
    int item = (y - (gm.mode_y + 2)) / c_item_h;
    if (item >= 0 && item < DISP_N && gfx_mode_pick(item - 1)) {
      serial_puts("[visual] tela ");
      serial_puts(item == 0 ? "padrao" : gfx_mode_name[item - 1]);
      serial_puts("\n");
      chrome_leave = 2;
      return 2;
    }
    return 1;
  }
  if (*chrome_menu && c_cfg && *c_cfg &&
      wm_pt(x, y, gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h)) {
    int item = (y - gm.sub_y) / c_item_h;
    if (item <= 0) {
      *c_tela = 1;
    } else if (item == 1) {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      wm_open_fonts(c_wins, c_focus, c_big);
    } else {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      host_open_advanced(c_wins, c_focus, c_big);
    }
    return 1;
  }
  if (*chrome_menu && wm_pt(x, y, gm.x, gm.y, gm.w, gm.h)) {
    int item = (y - gm.y) / c_item_h;
    if (item == 3) {
      *c_cfg = 1;
      *c_tela = 0;
    } else {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      if (item <= 0) wm_open_notepad(c_wins, c_focus, c_big);
      else if (item == 1) wm_open_mgr(c_wins, c_focus, c_big);
      else if (item == 2) wm_open_app(c_wins, c_focus, c_big);
      else if (item == 4) wm_open_browser(c_wins, c_focus, c_big);
      else if (item == 5) wm_open_command(c_wins, c_focus, c_big);
      else if (item >= 6) {
        serial_puts("[visual] Sair (menu)\n");
        chrome_leave = 1;
        return 2;
      }
    }
    return 1;
  }
  if (wm_pt(x, y, 2, h - c_bar_h + 2, c_start_w, c_bar_h - 4)) {
    *chrome_menu = !*chrome_menu;
    *c_cfg = 0;
    *c_tela = 0;
    return 1;
  }
  if (*chrome_menu) {
    *chrome_menu = 0;
    *c_cfg = 0;
    *c_tela = 0;
    return 1;
  }
  if (wm_pt(x, y, 12, c_icon4_y, c_icon, c_icon)) {
    wm_open_command(c_wins, c_focus, c_big);
    return 1;
  }
  if (wm_pt(x, y, 12, c_icon3_y, c_icon, c_icon)) {
    wm_open_browser(c_wins, c_focus, c_big);
    return 1;
  }
  if (wm_pt(x, y, 12, c_icon2_y, c_icon, c_icon)) {
    wm_open_notepad(c_wins, c_focus, c_big);
    return 1;
  }
  if (wm_pt(x, y, 12, 12, c_icon, c_icon)) {
    wm_open_mgr(c_wins, c_focus, c_big);
    return 1;
  }
  if (!chrome_in_browser(x, y)) return 1;
  return 0;
}

static void cons_redraw(void) {
  if (!c_wins || cons_painting) return;
  cons_painting = 1;
  gfx_frame_begin();
  cursor_saved = 0;
  chrome_paint_under();
  chrome_paint_over();
  cursor_redraw_at((int)sys_mx, (int)sys_my, sys_cursor_col());
  gfx_frame_end();
  cons_painting = 0;
}

static void chrome_bind(struct wm_win* wins, int* focus, int* menu, int* cfg, int* tela,
                        int big, int scale, int bar_h, int title_h, int start_w,
                        int menu_w, int item_h, int icon, int icon2, int icon3, int icon4,
                        uint32_t desk, uint32_t win, uint32_t title, uint32_t bar,
                        uint32_t white, uint32_t black, uint32_t btn, uint32_t inactive) {
  c_wins = wins;
  c_focus = focus;
  chrome_menu = menu;
  c_cfg = cfg;
  c_tela = tela;
  c_big = big;
  c_scale = scale;
  c_bar_h = bar_h;
  c_title_h = title_h;
  c_start_w = start_w;
  c_menu_w = menu_w;
  c_item_h = item_h;
  c_icon = icon;
  c_icon2_y = icon2;
  c_icon3_y = icon3;
  c_icon4_y = icon4;
  c_desk = desk;
  c_win = win;
  c_title = title;
  c_bar = bar;
  c_white = white;
  c_black = black;
  c_btn = btn;
  c_inactive = inactive;
  chrome_under = chrome_paint_under;
  chrome_over = chrome_paint_over;
  chrome_click = chrome_on_click;
  cons_paint = cons_redraw;
  host_on = 1;
  gui_session(1);
  sched_set_idle(0);
}

/* 0 = voltar ao DOS. 1 = aplicar outra resolucao e redesenhar o desktop. */
static int win_wm_run(void) {
  struct wm_win wins[WM_MAX];
  int focus = 0;
  int menu_open = 0;
  int menu_cfg = 0;
  int menu_tela = 0;
  int drag = -1;
  int drag_mode = 0;
  int drag_ox = 0;
  int drag_oy = 0;
  int prev_left = 0;
  int prev_right = 0;
  int pop_open = 0;
  int pop_x = 0;
  int pop_y = 0;
  int pop_hot = 0;
  int shift_on = 0;
  int kbd_e0 = 0;
  int pkt_idx = 0;
  uint8_t packet[3];
  int32_t mx;
  int32_t my;
  int w = (int)gfx_w;
  int h = (int)gfx_h;
  int big = (h >= 400);
  int scale = big ? 2 : 1;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int bar_h = ch + (big ? 8 : 2);
  int title_h = ch + (big ? 4 : 2);
  int start_w = (big ? 7 : 4) * FONT_W * scale + (big ? 16 : 8);
  int menu_w = 14 * FONT_W * scale + FONT_W * scale + 20;
  int menu_item_h = ch + (big ? 8 : 4);
  int icon = big ? gfx_sx(32) : 14;
  int icon_gap = big ? gfx_sy(36) : 18;
  int icon2_y = 12 + icon + icon_gap;
  int icon3_y = icon2_y + icon + icon_gap;
  int icon4_y = icon3_y + icon + icon_gap;
  uint32_t col_desk, col_win, col_title, col_bar, col_white, col_black, col_btn, col_cursor, col_inactive;
  int need_draw = 1;
  int i;
  int desk_i;

  if (!gfx_bochs) {
    set_palette_color(3, 0, 32, 32);
    set_palette_color(7, 42, 42, 42);
    set_palette_color(8, 21, 21, 21);
    col_desk = 3u;
    col_win = 7u;
    col_title = 1u;
    col_bar = 8u;
    col_white = 15u;
    col_black = 0u;
    col_btn = 7u;
    col_cursor = sys_cursor_col();
    col_inactive = 8u;
  } else {
    col_desk = PIXA(0, 128, 128);
    col_win = PIXA(192, 192, 192);
    col_title = PIXA(0, 0, 128);
    col_bar = PIXA(128, 128, 128);
    col_white = PIXA(255, 255, 255);
    col_black = PIXA(0, 0, 0);
    col_btn = PIXA(192, 192, 192);
    col_cursor = sys_cursor_col();
    col_inactive = PIXA(128, 128, 128);
  }

  for (i = 0; i < WM_MAX; i++) {
    wins[i].used = 0;
    wins[i].st = 0;
    wins[i].z = 0;
  }
  note_dlg = 0;
  note_file_menu = 0;
  note_dlg_len = 0;
  note_dlg_caret = 0;
  note_dlg_sel = 0;
  note_dlg_name[0] = '\0';
  note_dlg_drive = 'C';
  wm_open_mgr(wins, &focus, big);

  sys_mouse_init();
  mx = sys_mx;
  my = sys_my;
  desk_load();
  chrome_leave = 0;
  gui_ensure();
  chrome_bind(wins, &focus, &menu_open, &menu_cfg, &menu_tela, big, scale, bar_h, title_h,
              start_w, menu_w, menu_item_h, icon, icon2_y, icon3_y, icon4_y,
              col_desk, col_win, col_title, col_bar, col_white, col_black, col_btn, col_inactive);

  for (;;) {
    uint8_t data = 0;

    if (gui_stole) {
      focus = -1;
      gui_stole = 0;
      need_draw = 1;
    }
    if (task_pick() >= 0) tasks_pump();
    if (win_api_dirty) {
      win_api_dirty = 0;
      need_draw = 1;
    }
    if (chrome_leave) {
      int leave = chrome_leave;
      chrome_leave = 0;
      chrome_unbind();
      desk_free();
      return leave == 1 ? 0 : 1;
    }
    mx = sys_mx;
    my = sys_my;
    if (need_draw) {
      int bar_ty = h - bar_h + (bar_h - ch) / 2;
      struct start_menu gm;
      int mi;
      start_menu_geom(&gm, w, h, bar_h, menu_w, menu_item_h, scale);
      gfx_frame_begin();
      cursor_saved = 0;
      gfx_clear(col_desk);
      fill_rect(12, 12, icon, icon, col_win);
      draw_string_scaled(12, 12 + icon + 2, "DOS", col_white, col_desk, scale);
      fill_rect(12, icon2_y, icon, icon, col_title);
      draw_string_scaled(12, icon2_y + icon + 2, "ARQ", col_white, col_desk, scale);
      draw_nav_icon(12, icon3_y, icon, col_title, col_white);
      draw_string_scaled(12, icon3_y + icon + 2, "WEB", col_white, col_desk, scale);
      draw_cmd_icon(12, icon4_y, icon, col_white, col_black);
      draw_string_scaled(12, icon4_y + icon + 2, "CMD", col_white, col_desk, scale);
      desk_draw(icon, ch, scale, w, h, bar_h, col_desk, col_white, col_black);
      paint_stacked(wins, title_h, scale, col_win, col_title, col_white, col_black,
                    col_btn, col_inactive);
      fill_rect(0, h - bar_h, w, bar_h, col_bar);
      fill_rect(2, h - bar_h + 2, start_w, bar_h - 4, col_btn);
      draw_string_scaled(6, bar_ty, "Iniciar", col_black, col_btn, scale);
      draw_string_scaled(big ? w - 8 * 8 * scale : w - 64, bar_ty, "ESC=DOS", col_white, col_bar, scale);
      if (menu_open) {
        static const char* labels[] = {
          "Bloco de notas", "Gerenciador", "Explorer", "Configuracoes", "Navegador", "Command", "Sair"
        };
        fill_rect(gm.x, gm.y, gm.w, gm.h, col_win);
        for (mi = 0; mi < gm.n; mi++) {
          int hot = (mi == 3 && menu_cfg) ||
                    ((int)mx >= gm.x && (int)mx < gm.x + gm.w &&
                     (int)my >= gm.y + mi * gm.item_h &&
                     (int)my < gm.y + (mi + 1) * gm.item_h);
          menu_row(gm.x + 1, gm.y + 2 + mi * gm.item_h, gm.w - 2, gm.item_h - 2,
                   labels[mi], hot, mi == 3, col_win, col_title, col_white, col_black, scale);
        }
        menu_frame(gm.x, gm.y, gm.w, gm.h, col_black);
      }
      if (menu_open && menu_cfg) {
        int tela_hot = menu_tela ||
                       wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y, gm.sub_w, gm.item_h);
        int font_hot = wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + gm.item_h,
                             gm.sub_w, gm.item_h);
        int adv_hot = wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + 2 * gm.item_h,
                            gm.sub_w, gm.item_h);
        fill_rect(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h, col_win);
        menu_row(gm.sub_x + 1, gm.sub_y + 2, gm.sub_w - 2, gm.item_h - 2,
                 "Tela", tela_hot, 1, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Fontes", font_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + 2 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Fontes avancadas", adv_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_frame(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h, col_black);
      }
      if (menu_open && menu_cfg && menu_tela) {
        fill_rect(gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h, col_win);
        for (mi = 0; mi < DISP_N; mi++) {
          int mode = mi - 1;
          int hot = (int)mx >= gm.mode_x && (int)mx < gm.mode_x + gm.mode_w &&
                    (int)my >= gm.mode_y + 2 + mi * gm.item_h &&
                    (int)my < gm.mode_y + 2 + (mi + 1) * gm.item_h;
          int on = mi == 0 ? gfx_mode_sel < 0
                           : ((uint32_t)gfx_w == gfx_mode_w[mode] &&
                              (uint32_t)gfx_h == gfx_mode_h[mode]);
          menu_row(gm.mode_x + 1, gm.mode_y + 2 + mi * gm.item_h, gm.mode_w - 2, gm.item_h,
                   mi == 0 ? "Padrao" : gfx_mode_name[mode], hot, 0,
                   col_win, col_title, col_white, col_black, scale);
          if (on)
            draw_string_scaled(gm.mode_x + gm.mode_w - FONT_W * scale - 6,
                               gm.mode_y + 2 + mi * gm.item_h +
                               (gm.item_h - (vga_bios_font_ok ? FONT_H : 8) * scale) / 2,
                               "*", hot ? col_white : col_black, hot ? col_title : col_win, scale);
        }
        menu_frame(gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h, col_black);
      }
      {
        int ni = wm_find_kind(wins, WM_KIND_NOTE);
        if (ni >= 0 && wins[ni].used && note_file_menu) {
          int fx, fy, fw, fh, ih;
          note_filemenu_rect(&wins[ni], title_h, ch, big, &fx, &fy, &fw, &fh, &ih);
          fill_rect(fx, fy, fw, fh, col_win);
          fill_rect(fx, fy, fw, 1, col_black);
          fill_rect(fx, fy + fh - 1, fw, 1, col_black);
          fill_rect(fx, fy, 1, fh, col_black);
          fill_rect(fx + fw - 1, fy, 1, fh, col_black);
          draw_string_scaled(fx + 8, fy + 2, "Open", col_black, col_win, scale);
          draw_string_scaled(fx + 8, fy + 2 + ih, "Save", col_black, col_win, scale);
          draw_string_scaled(fx + 8, fy + 2 + 2 * ih, "Save as...", col_black, col_win, scale);
          draw_string_scaled(fx + 8, fy + 2 + 3 * ih, "Exit", col_black, col_win, scale);
        }
        if (note_dlg) {
          int dx, dy, dw, dh;
          int ex, ey, ew, eh, btn_y, list_y, li, shown;
          int item_h = ch + (big ? 4 : 2);
          int save_w = 5 * FONT_W * scale + 16;
          int cancel_w = 7 * FONT_W * scale + 12;
          uint32_t col_hi = gfx_bochs ? PIXA(0, 0, 128) : 1u;
          note_dlg_rect(w, h, bar_h, big, &dx, &dy, &dw, &dh);
          note_dlg_edit_geom(dx, dy, dw, dh, ch, scale, &ex, &ey, &ew, &eh, &btn_y);
          fill_rect(dx, dy, dw, dh, col_win);
          fill_rect(dx, dy, dw, title_h, col_title);
          draw_string_scaled(dx + 8, dy + 2,
                             note_dlg == NOTE_DLG_OPEN ? "Open" : "Save as...",
                             col_white, col_title, scale);
          list_y = dy + title_h + 8;
          fill_rect(dx + 8, list_y, dw - 16, ey - list_y - 8, col_white);
          note_list_vis = note_dlg_rows(ey - list_y - 8, item_h);
          note_list_clamp(note_list_vis);
          shown = note_list_n - note_list_off;
          if (shown > note_list_vis) shown = note_list_vis;
          if (shown < 0) shown = 0;
          for (li = 0; li < shown; li++)
            draw_fit(dx + 12, list_y + 4 + li * item_h, dw - 28,
                     note_list[note_list_off + li], col_black, col_white, scale);
          draw_string_scaled(dx + 8, ey + 4, "File name:", col_black, col_win, scale);
          fill_rect(ex - 2, ey - 2, ew + 4, eh + 4, col_black);
          if (note_dlg_sel && note_dlg_len > 0) {
            fill_rect(ex, ey, ew, eh, col_hi);
            draw_fit(ex + 4, ey + 4, ew - 8, note_dlg_name, col_white, col_hi, scale);
          } else {
            fill_rect(ex, ey, ew, eh, col_white);
            draw_fit(ex + 4, ey + 4, ew - 8, note_dlg_name, col_black, col_white, scale);
            fill_rect(ex + 4 + note_dlg_caret * FONT_W * scale, ey + 3, scale > 1 ? 2 : 1, ch, col_black);
          }
          fill_rect(dx + 8, btn_y, save_w, ch + 6, col_btn);
          draw_string_scaled(dx + 12, btn_y + 2,
                             note_dlg == NOTE_DLG_OPEN ? "Open" : "Save",
                             col_black, col_btn, scale);
          fill_rect(dx + dw - cancel_w - 8, btn_y, cancel_w, ch + 6, col_btn);
          draw_string_scaled(dx + dw - cancel_w - 4, btn_y + 2, "Cancel", col_black, col_btn, scale);
        }
      }
      if (pop_open) {
        int px, py, pw, ph;
        pop_geom(pop_x, pop_y, scale, w, h, bar_h, &px, &py, &pw, &ph);
        fill_rect(px, py, pw, ph, col_win);
        {
          int ih = (ph - 4) / POP_N;
          menu_row(px + 1, py + 2, pw - 2, ih, "Console", pop_hot == 0, 0,
                   col_win, col_title, col_white, col_black, scale);
          menu_row(px + 1, py + 2 + ih, pw - 2, ih, "Configuracoes de tela", pop_hot == 1, 0,
                   col_win, col_title, col_white, col_black, scale);
        }
        menu_frame(px, py, pw, ph, col_black);
      }
      cursor_redraw_at((int)mx, (int)my, col_cursor);
      gfx_frame_end();
      need_draw = 0;
    }

    {
      uint8_t scb = 0;
      int from_abs = 0;
      int kind = input_poll(packet, &pkt_idx, &scb, &from_abs);
      if (kind < 0) {
        __asm__ volatile ("pause");
        continue;
      }
      if (kind == 0) {
        data = scb;
        goto wm_key;
      }
      {
        int left;
        if (!from_abs)
          sys_mouse_place(mx + (int8_t)packet[1], my - (int8_t)packet[2]);
        mx = sys_mx;
        my = sys_my;
        if (menu_open) {
          struct start_menu gm;
          int cfg = menu_cfg;
          int tela = menu_tela;
          int over_cfg, over_tela, over_font, over_adv, over_modes, over_main;
          start_menu_geom(&gm, w, h, bar_h, menu_w, menu_item_h, scale);
          over_cfg = wm_pt((int)mx, (int)my, gm.x, gm.y + 3 * gm.item_h, gm.w, gm.item_h);
          over_tela = menu_cfg && wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y, gm.sub_w, gm.item_h);
          over_font = menu_cfg && wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + gm.item_h,
                                       gm.sub_w, gm.item_h);
          over_adv = menu_cfg && wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + 2 * gm.item_h,
                                      gm.sub_w, gm.item_h);
          over_modes = menu_tela && wm_pt((int)mx, (int)my, gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h);
          over_main = wm_pt((int)mx, (int)my, gm.x, gm.y, gm.w, gm.h);
          if (over_modes || over_tela) {
            cfg = 1;
            tela = 1;
          } else if (over_font || over_adv || over_cfg) {
            cfg = 1;
            tela = 0;
          } else if (over_main) {
            cfg = 0;
            tela = 0;
          }
          if (cfg != menu_cfg || tela != menu_tela) {
            menu_cfg = cfg;
            menu_tela = tela;
            need_draw = 1;
          }
        }
        {
          int di = wm_find_kind(wins, WM_KIND_DISP);
          int hot = -1;
          if (di >= 0 && wins[di].used) {
            int ih = disp_row_h(scale);
            int vis = disp_vis_rows(&wins[di], title_h, scale);
            int y0 = wins[di].y + title_h + 4;
            disp_scroll_by(&wins[di], title_h, scale, 0);
            if (wm_pt((int)mx, (int)my, wins[di].x + 4, y0, wins[di].w - 8, vis * ih)) {
              hot = ((int)my - y0) / ih + disp_scroll;
              if (hot < 0 || hot >= DISP_N) hot = -1;
            }
          }
          if (hot != disp_hot) {
            disp_hot = hot;
            need_draw = 1;
          }
        }
        if (pop_open) {
          int px, py, pw, ph, hot;
          pop_geom(pop_x, pop_y, scale, w, h, bar_h, &px, &py, &pw, &ph);
          hot = -1;
          if (wm_pt((int)mx, (int)my, px, py, pw, ph)) {
            int ih = (ph - 4) / POP_N;
            if (ih > 0) hot = ((int)my - (py + 2)) / ih;
            if (hot < 0 || hot >= POP_N) hot = -1;
          }
          if (hot != pop_hot) {
            pop_hot = hot;
            need_draw = 1;
          }
        }
        {
          int dz = mouse_wheel();
          int wh = -1;
          if (dz && note_dlg) {
            note_list_off += dz;
            note_list_clamp(note_list_vis > 0 ? note_list_vis : 6);
            need_draw = 1;
          } else if (dz) {
            int hk = HIT_NONE;
            int id = z_top(wins, (int)mx, (int)my, &hk);
            if (hk == HIT_WM) wh = id;
            if (hk == HIT_GUI && id >= 0) {
              /* janela de programa (navegador etc.): a roda vira evento dela */
              struct gui_win* gw = &gwins[id];
              int ry = (int)my - (gw->oy + gw->title_h);
              if (ry >= 0) gq_push(gw, GUI_EV_WHEEL, (int)mx - gw->ox, ry, dz);
            }
            if (wh >= 0) {
              if (wins[wh].kind == WM_KIND_NOTE) {
                note_scroll_by(&wins[wh], title_h + note_menubar_h(ch), scale, dz);
              } else if ((wins[wh].kind == WM_KIND_APP || wins[wh].kind == WM_KIND_RUN) &&
                         wins[wh].st) {
                app_scroll_by(wins[wh].st, app_vis_lines(&wins[wh], title_h, ch), dz);
              } else if (wins[wh].kind == WM_KIND_DISP) {
                disp_scroll_by(&wins[wh], title_h, scale, dz);
                if (disp_hot >= 0 &&
                    (disp_hot < disp_scroll ||
                     disp_hot >= disp_scroll + disp_vis_rows(&wins[wh], title_h, scale)))
                  disp_hot = -1;
              }
              need_draw = 1;
            }
          }
        }
        left = packet[0] & 1;
        if (left && gwins && g_drag >= 0 && g_drag < GUI_MAX && gwins[g_drag].open) {
          int save = gui_ix;
          gwins[g_drag].ox = (int)mx - g_drag_ox;
          gwins[g_drag].oy = (int)my - g_drag_oy;
          gui_ix = g_drag;
          gui_clamp_pos();
          gui_ix = save;
          need_draw = 1;
        } else if (left && gwins && g_cap >= 0 && g_cap < GUI_MAX && gwins[g_cap].open) {
          gq_push(&gwins[g_cap], GUI_EV_DRAG, (int)mx - gwins[g_cap].ox,
                  (int)my - (gwins[g_cap].oy + gwins[g_cap].title_h), 1);
        } else if (left && drag >= 0 && wins[drag].used) {
          int min_w = big ? 180 : 120;
          int min_h = big ? 100 : 70;
          if (drag_mode == 0) {
            wins[drag].x = (int)mx - drag_ox;
            wins[drag].y = (int)my - drag_oy;
          } else {
            wins[drag].w = (int)mx - wins[drag].x + drag_ox;
            wins[drag].h = (int)my - wins[drag].y + drag_oy;
            if (wins[drag].w < min_w) wins[drag].w = min_w;
            if (wins[drag].h < min_h) wins[drag].h = min_h;
          }
          if (wins[drag].x < 0) wins[drag].x = 0;
          if (wins[drag].y < 0) wins[drag].y = 0;
          if (wins[drag].x + wins[drag].w > w) wins[drag].x = w - wins[drag].w;
          if (wins[drag].y + wins[drag].h > h - bar_h)
            wins[drag].y = h - bar_h - wins[drag].h;
          need_draw = 1;
        } else if (left && !prev_left) {
          int handled = 0;
          if (pop_open) {
            int px, py, pw, ph;
            pop_geom(pop_x, pop_y, scale, w, h, bar_h, &px, &py, &pw, &ph);
            pop_open = 0;
            if (wm_pt((int)mx, (int)my, px, py, pw, ph)) {
              int ih = (ph - 4) / POP_N;
              int item = ih > 0 ? ((int)my - (py + 2)) / ih : 0;
              if (item <= 0) wm_open_command(wins, &focus, big);
              else wm_open_display(wins, &focus, big);
            }
            handled = 1;
            need_draw = 1;
          } else if (note_dlg) {
            int dx, dy, dw, dh;
            int ex, ey, ew, eh, btn_y, list_y, item_h, shown, li;
            int save_w = 5 * FONT_W * scale + 16;
            int cancel_w = 7 * FONT_W * scale + 12;
            int ni = wm_find_kind(wins, WM_KIND_NOTE);
            note_dlg_rect(w, h, bar_h, big, &dx, &dy, &dw, &dh);
            note_dlg_edit_geom(dx, dy, dw, dh, ch, scale, &ex, &ey, &ew, &eh, &btn_y);
            item_h = ch + (big ? 4 : 2);
            list_y = dy + title_h + 8;
            note_list_vis = note_dlg_rows(ey - list_y - 8, item_h);
            note_list_clamp(note_list_vis);
            shown = note_list_n - note_list_off;
            if (shown > note_list_vis) shown = note_list_vis;
            if (shown < 0) shown = 0;
            if (wm_pt((int)mx, (int)my, dx + 8, btn_y, save_w, ch + 6)) {
              if (ni >= 0) note_dlg_apply(&wins[ni]);
              else note_dlg = 0;
            } else if (wm_pt((int)mx, (int)my, dx + dw - cancel_w - 8, btn_y, cancel_w, ch + 6) ||
                       !wm_pt((int)mx, (int)my, dx, dy, dw, dh)) {
              note_dlg = 0;
            } else if (wm_pt((int)mx, (int)my, ex, ey, ew, eh)) {
              int col = ((int)mx - ex - 4) / (FONT_W * scale);
              if (col < 0) col = 0;
              if (col > note_dlg_len) col = note_dlg_len;
              note_dlg_caret = col;
              note_dlg_sel = 0;
            } else {
              for (li = 0; li < shown; li++) {
                if (wm_pt((int)mx, (int)my, dx + 8, list_y + 4 + li * item_h, dw - 16, item_h)) {
                  const char* row = note_list[note_list_off + li];
                  if (row[0] && row[1] == ':') note_dlg_drive = row[0];
                  note_dlg_set_name(row);
                  break;
                }
              }
            }
            handled = 1;
            need_draw = 1;
          } else if (note_file_menu) {
            int ni = wm_find_kind(wins, WM_KIND_NOTE);
            int fx, fy, fw, fh, ih;
            if (ni >= 0 && wins[ni].used) {
              note_filemenu_rect(&wins[ni], title_h, ch, big, &fx, &fy, &fw, &fh, &ih);
              if (wm_pt((int)mx, (int)my, fx, fy, fw, fh)) {
                int item = ((int)my - fy) / ih;
                note_file_menu = 0;
                if (item <= 0) note_open_dlg(NOTE_DLG_OPEN);
                else if (item == 1) {
                  note_save();
                  note_set_title(&wins[ni]);
                } else if (item == 2) note_open_dlg(NOTE_DLG_SAVEAS);
                else {
                  if (note_dirty) note_save();
                  wins[ni].used = 0;
                  focus = wm_find_kind(wins, WM_KIND_MGR);
                  if (focus < 0) focus = 0;
                }
              } else {
                note_file_menu = 0;
              }
            } else {
              note_file_menu = 0;
            }
            handled = 1;
            need_draw = 1;
          }
          if (handled) {
            /* ja tratado */
          } else if (wm_pt((int)mx, (int)my, 2, h - bar_h + 2, start_w, bar_h - 4)) {
            menu_open = !menu_open;
            menu_cfg = 0;
            menu_tela = 0;
            note_file_menu = 0;
            handled = 1;
            need_draw = 1;
          } else if (menu_open) {
            struct start_menu gm;
            start_menu_geom(&gm, w, h, bar_h, menu_w, menu_item_h, scale);
            if (menu_tela && wm_pt((int)mx, (int)my, gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h)) {
              int item = ((int)my - (gm.mode_y + 2)) / gm.item_h;
              if (item >= 0 && item < DISP_N && gfx_mode_pick(item - 1)) {
                serial_puts("[visual] tela ");
                serial_puts(item == 0 ? "padrao" : gfx_mode_name[item - 1]);
                serial_puts("\n");
                chrome_unbind();
                desk_free();
                return 1;
              }
              handled = 1;
              need_draw = 1;
            } else if (menu_cfg && wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h)) {
              int item = ((int)my - gm.sub_y) / gm.item_h;
              if (item <= 0) {
                menu_tela = 1;
              } else if (item == 1) {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                wm_open_fonts(wins, &focus, big);
              } else {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                host_open_advanced(wins, &focus, big);
              }
              handled = 1;
              need_draw = 1;
            } else if (wm_pt((int)mx, (int)my, gm.x, gm.y, gm.w, gm.h)) {
              int item = ((int)my - gm.y) / gm.item_h;
              if (item == 3) {
                menu_cfg = 1;
                menu_tela = 0;
              } else {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                if (item <= 0) wm_open_notepad(wins, &focus, big);
                else if (item == 1) wm_open_mgr(wins, &focus, big);
                else if (item == 2) wm_open_app(wins, &focus, big);
                else if (item == 4) wm_open_browser(wins, &focus, big);
                else if (item == 5) wm_open_command(wins, &focus, big);
                else {
                  serial_puts("[visual] Sair (menu)\n");
                  chrome_unbind();
                  desk_free();
                  return 0;
                }
              }
              handled = 1;
              need_draw = 1;
            }
          }
          if (handled) {
            /* ja tratado */
          } else if (wm_pt((int)mx, (int)my, 12, icon4_y, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            wm_open_command(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if (wm_pt((int)mx, (int)my, 12, icon3_y, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            wm_open_browser(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if (wm_pt((int)mx, (int)my, 12, icon2_y, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            wm_open_notepad(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if (wm_pt((int)mx, (int)my, 12, 12, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            wm_open_mgr(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if ((desk_i = desk_hit((int)mx, (int)my, icon, ch, scale, w, h, bar_h)) >= 0) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            if (desk_items[desk_i].dir) {
              char listing[512];
              int nw = wm_new_run_window(wins, big, desk_items[desk_i].name, 0);
              if (nw >= 0) {
                run_puts("Pasta ");
                run_puts(desk_items[desk_i].path);
                run_puts("\n");
                if (file_list(desk_items[desk_i].path, listing, (int)sizeof(listing)) > 0)
                  run_puts(listing);
                else run_puts("(vazio)\n");
                run_out = 0;
                focus = nw;
              }
            } else if (desk_items[desk_i].image == 1)
              wm_open_image(wins, &focus, big, desk_items[desk_i].path);
            else if (desk_items[desk_i].image == 2)
              wm_open_video(wins, &focus, big, desk_items[desk_i].path);
            else if (app_is_prog(desk_items[desk_i].name))
              wm_exec(wins, &focus, big, desk_items[desk_i].path);
            else {
              int nw = wm_new_run_window(wins, big, desk_items[desk_i].name, 0);
              if (nw >= 0) {
                run_puts(desk_items[desk_i].name);
                run_puts("\n");
                run_out = 0;
                focus = nw;
              }
            }
            handled = 1;
            need_draw = 1;
          } else {
            int hit = -1;
            int hk = HIT_NONE;
            int id = z_top(wins, (int)mx, (int)my, &hk);
            if (menu_open) {
              menu_open = 0;
              menu_cfg = 0;
              menu_tela = 0;
              need_draw = 1;
            }
            if (hk == HIT_GUI) {
              struct gui_win* gw = &gwins[id];
              int relx = (int)mx - gw->ox;
              int rely = (int)my - (gw->oy + gw->title_h);
              int close_s = gw->title_h - 4;
              gw->z = ++zseq;
              gui_focus = id;
              focus = -1;
              if (wm_pt((int)mx, (int)my, gw->ox + gw->cw - close_s - 2, gw->oy + 2,
                        close_s, close_s)) {
                gq_push(gw, GUI_EV_CLOSE, 0, 0, 0);
              } else if (rely < 0) {
                g_drag = id;
                g_drag_ox = (int)mx - gw->ox;
                g_drag_oy = (int)my - gw->oy;
              } else {
                gq_push(gw, GUI_EV_DOWN, relx, rely, 1);
                g_cap = id;
              }
              need_draw = 1;
              handled = 1;
            } else if (hk == HIT_WM) {
              hit = id;
            }
            if (hit >= 0) {
              int close_s = title_h - 4;
              int close_x = wins[hit].x + wins[hit].w - close_s - 2;
              int mb_h = note_menubar_h(ch);
              int fw = note_file_w(scale);
              wm_raise(&wins[hit]);
              focus = hit;
              if (wm_pt((int)mx, (int)my, close_x, wins[hit].y + 2, close_s, close_s)) {
                task_forget_win(&wins[hit]);
                if (wins[hit].st) {
                  kfree(wins[hit].st);
                  wins[hit].st = 0;
                }
                if (app_win == &wins[hit]) app_win = 0;
                wins[hit].used = 0;
                if (wins[hit].kind == WM_KIND_NOTE && note_dirty) note_save();
                note_file_menu = 0;
                note_dlg = 0;
                focus = wm_find_kind(wins, WM_KIND_MGR);
                if (focus < 0) focus = wm_find_kind(wins, WM_KIND_NOTE);
                if (focus < 0) focus = wm_find_kind(wins, WM_KIND_APP);
                if (focus < 0) focus = 0;
                if (focus >= 0 && focus < WM_MAX && wins[focus].used) wm_raise(&wins[focus]);
              } else if (wins[hit].kind == WM_KIND_APP && wins[hit].st && app_entry &&
                         wm_pt((int)mx, (int)my, wins[hit].x + 4,
                               wins[hit].y + title_h + 2, wins[hit].w - 8,
                               wins[hit].h - title_h - ch - 8)) {
                int ih = ch;
                int idx = ((int)my - (wins[hit].y + title_h + 2)) / ih;
                int cap = app_vis_lines(&wins[hit], title_h, ch);
                int row;
                app_scroll_by(wins[hit].st, cap, 0);
                row = wins[hit].st->view + idx;
                app_win = &wins[hit];
                if (idx >= 0 && idx < cap && row < wins[hit].st->nlines) {
                  wins[hit].st->click = row;
                  app_entry(WIN_MSG_CLICK, app_api_table, wins[hit].st);
                  if (app_open_req) {
                    int ni;
                    app_open_req = 0;
                    if (desk_is_video(app_open_path))
                      wm_open_video(wins, &focus, big, app_open_path);
                    else {
                      wm_open_notepad(wins, &focus, big);
                      note_load(app_open_path);
                      ni = wm_find_kind(wins, WM_KIND_NOTE);
                      if (ni >= 0) note_set_title(&wins[ni]);
                    }
                  }
                  if (app_run_req) {
                    app_run_req = 0;
                    wm_exec(wins, &focus, big, app_open_path);
                  }
                }
              } else if (wins[hit].kind == WM_KIND_NOTE &&
                         wm_pt((int)mx, (int)my, wins[hit].x + 4, wins[hit].y + title_h, fw, mb_h)) {
                note_file_menu = !note_file_menu;
                menu_open = 0;
              } else if (wins[hit].kind == WM_KIND_FONT &&
                         wm_pt((int)mx, (int)my, wins[hit].x + 4,
                               font_list_y(&wins[hit], title_h, scale),
                               wins[hit].w - 8, 2 * (ch + (big ? 4 : 2)))) {
                int ih = ch + (big ? 4 : 2);
                int y0 = font_list_y(&wins[hit], title_h, scale);
                int item = ((int)my - y0) / ih;
                if (item <= 0) font_sel = vga_bios_font_ok ? 0 : 1;
                else if (item == 1) font_sel = 1;
              } else if (wins[hit].kind == WM_KIND_DISP &&
                         wm_pt((int)mx, (int)my, wins[hit].x + 4,
                               wins[hit].y + title_h + 4, wins[hit].w - 8,
                               disp_vis_rows(&wins[hit], title_h, scale) * disp_row_h(scale))) {
                int ih = disp_row_h(scale);
                int y0 = wins[hit].y + title_h + 4;
                int item = ((int)my - y0) / ih + disp_scroll;
                if (item >= 0 && item < DISP_N && gfx_mode_pick(item - 1)) {
                  serial_puts("[visual] tela ");
                  serial_puts(item == 0 ? "padrao" : gfx_mode_name[item - 1]);
                  serial_puts("\n");
                  chrome_unbind();
                  desk_free();
                  return 1;
                }
              } else if (wm_pt((int)mx, (int)my, wins[hit].x + wins[hit].w - 14,
                               wins[hit].y + wins[hit].h - 14, 14, 14)) {
                drag = hit;
                drag_mode = 1;
                drag_ox = wins[hit].x + wins[hit].w - (int)mx;
                drag_oy = wins[hit].y + wins[hit].h - (int)my;
              } else if (wm_pt((int)mx, (int)my, wins[hit].x, wins[hit].y, wins[hit].w, title_h)) {
                drag = hit;
                drag_mode = 0;
                drag_ox = (int)mx - wins[hit].x;
                drag_oy = (int)my - wins[hit].y;
              }
              need_draw = 1;
              handled = 1;
            }
          }
          (void)handled;
        }
        if (!left) {
          if (gwins && g_cap >= 0 && g_cap < GUI_MAX && gwins[g_cap].open)
            gq_push(&gwins[g_cap], GUI_EV_UP, (int)mx - gwins[g_cap].ox,
                    (int)my - (gwins[g_cap].oy + gwins[g_cap].title_h), 1);
          g_cap = -1;
          g_drag = -1;
          drag = -1;
        }
        prev_left = left;
        {
          int right = (packet[0] >> 1) & 1;
          if (right && !prev_right && !note_dlg) {
            int t, on = 0;
            if (wm_pt((int)mx, (int)my, 0, h - bar_h, w, bar_h)) on = 1;
            if (menu_open) on = 1;
            if (wm_pt((int)mx, (int)my, 12, 12, icon, icon)) on = 1;
            if (wm_pt((int)mx, (int)my, 12, icon2_y, icon, icon)) on = 1;
            if (wm_pt((int)mx, (int)my, 12, icon3_y, icon, icon)) on = 1;
            if (wm_pt((int)mx, (int)my, 12, icon4_y, icon, icon)) on = 1;
            if (desk_hit((int)mx, (int)my, icon, ch, scale, w, h, bar_h) >= 0) on = 1;
            for (t = 0; t < WM_MAX && !on; t++) {
              if (wins[t].used && !run_hidden(&wins[t]) &&
                  wm_pt((int)mx, (int)my, wins[t].x, wins[t].y, wins[t].w, wins[t].h))
                on = 1;
            }
            for (t = 0; gwins && t < GUI_MAX && !on; t++) {
              if (gwins[t].open &&
                  wm_pt((int)mx, (int)my, gwins[t].ox, gwins[t].oy, gwins[t].cw,
                        gwins[t].title_h + gwins[t].ch))
                on = 1;
            }
            if (!on) {
              pop_open = 1;
              pop_x = (int)mx;
              pop_y = (int)my;
              pop_hot = 0;
              menu_open = 0;
              menu_cfg = 0;
              menu_tela = 0;
              note_file_menu = 0;
              need_draw = 1;
            } else if (pop_open) {
              pop_open = 0;
              need_draw = 1;
            }
          }
          prev_right = right;
        }
        if (!need_draw) cursor_draw((int)mx, (int)my, col_cursor);
      }
      continue;
    }

    wm_key:
    {
      uint8_t sc = data;
      if (kbd_e0) {
        kbd_e0 = 0;
        if (!(sc & 0x80) && gwins && gui_focus >= 0 && gui_focus < GUI_MAX && gwins[gui_focus].open) {
          int k = gui_key_ext(sc);
          if (k) gq_push(&gwins[gui_focus], GUI_EV_KEY, 0, 0, k);
          continue;
        }
        if (note_dlg && !(sc & 0x80)) {
          if (sc == 0x4B && note_dlg_caret > 0) {
            note_dlg_sel = 0;
            note_dlg_caret--;
            need_draw = 1;
          } else if (sc == 0x4D && note_dlg_caret < note_dlg_len) {
            note_dlg_sel = 0;
            note_dlg_caret++;
            need_draw = 1;
          } else if (sc == 0x47) {
            note_dlg_sel = 0;
            note_dlg_caret = 0;
            need_draw = 1;
          } else if (sc == 0x4F) {
            note_dlg_sel = 0;
            note_dlg_caret = note_dlg_len;
            need_draw = 1;
          }
        }
        continue;
      }
      if (sc == 0xE0) {
        kbd_e0 = 1;
        continue;
      }
      if (sc == SCANCODE_LSHIFT || sc == SCANCODE_RSHIFT) {
        shift_on = 1;
        continue;
      }
      if (sc == (SCANCODE_LSHIFT | 0x80) || sc == (SCANCODE_RSHIFT | 0x80)) {
        shift_on = 0;
        continue;
      }
      if (sc & 0x80) continue;
      if (sc >= 128) continue;
      if (sc == SCANCODE_ESC) {
        if (menu_open) {
          menu_open = 0;
          menu_cfg = 0;
          menu_tela = 0;
          need_draw = 1;
          continue;
        }
        if (note_dlg) {
          note_dlg = 0;
          need_draw = 1;
          continue;
        }
        if (note_file_menu) {
          note_file_menu = 0;
          need_draw = 1;
          continue;
        }
        if (gwins && gui_focus >= 0 && gui_focus < GUI_MAX && gwins[gui_focus].open) {
          gq_push(&gwins[gui_focus], GUI_EV_KEY, 0, 0, 27);
          continue;
        }
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE && note_dirty)
          note_save();
        serial_puts("[visual] ESC\n");
        chrome_unbind();
        desk_free();
        return 0;
      }
      if (sc == SCANCODE_F2) {
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE) {
          note_save();
          note_set_title(&wins[focus]);
          need_draw = 1;
        }
        continue;
      }
      {
        char c = shift_on ? scancode_map_shift[sc] : scancode_map[sc];
        if (c == 27) {
          if (note_dlg) {
            note_dlg = 0;
            need_draw = 1;
            continue;
          }
          if (gwins && gui_focus >= 0 && gui_focus < GUI_MAX && gwins[gui_focus].open) {
            gq_push(&gwins[gui_focus], GUI_EV_KEY, 0, 0, 27);
            continue;
          }
          if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE && note_dirty)
            note_save();
          serial_puts("[visual] ESC (map)\n");
          chrome_unbind();
          desk_free();
          return 0;
        }
        if (c == 0) continue;
        if (note_dlg) {
          if (c == '\b') {
            note_dlg_backspace();
            need_draw = 1;
          } else if (c == '\n' || c == '\r') {
            int ni = wm_find_kind(wins, WM_KIND_NOTE);
            if (ni >= 0) note_dlg_apply(&wins[ni]);
            else note_dlg = 0;
            need_draw = 1;
          } else if (c >= 32 && c < 127) {
            note_dlg_insert(c);
            need_draw = 1;
          }
          continue;
        }
        if (gwins && gui_focus >= 0 && gui_focus < GUI_MAX && gwins[gui_focus].open) {
          int k = (c == '\n') ? '\r' : (unsigned char)c;
          gq_push(&gwins[gui_focus], GUI_EV_KEY, 0, 0, k);
          continue;
        }
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_CMD) {
          if (cmd_key(&wins[focus], c)) {
            if (wins[focus].st) kfree(wins[focus].st);
            wins[focus].st = 0;
            wins[focus].used = 0;
            focus = wm_find_kind(wins, WM_KIND_MGR);
            if (focus < 0) focus = 0;
          }
          need_draw = 1;
          continue;
        }
        if (!(focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE))
          continue;
        if (note_missing) {
          note_buf[0] = '\0';
          note_len = 0;
          note_missing = 0;
        }
        if (c == '\b') {
          if (note_len > 0) {
            note_len--;
            note_buf[note_len] = '\0';
            note_dirty = 1;
            note_follow_end(&wins[focus], title_h + note_menubar_h(ch), scale);
            need_draw = 1;
          }
          continue;
        }
        if (c == '\n' || c == '\r') c = '\n';
        if (note_len < (int)sizeof(note_buf) - 1) {
          note_buf[note_len++] = c;
          note_buf[note_len] = '\0';
          note_dirty = 1;
          note_follow_end(&wins[focus], title_h + note_menubar_h(ch), scale);
          need_draw = 1;
        }
      }
    }
  }
}

static void gfx_session_run(int session) {
  static int shift_on;
  static int kbd_e0;
  int pkt_idx;
  uint8_t packet[3];
  int32_t mx;
  int32_t my;
  int tx;
  int ty;
  uint32_t text_area_y0;
  uint32_t text_x0;
  uint32_t text_y0;
  uint32_t text_x1;
  uint32_t text_y1;
  uint32_t col_bg;
  uint32_t col_fg;
  uint32_t col_accent;
  uint32_t col_frame;
  uint32_t col_cursor;

  serial_puts("[visual] enter\n");
  if (!vga_bios_font_ok)
    vga_capture_bios_font();
  shift_on = 0;
  kbd_e0 = 0;
  pkt_idx = 0;
  cursor_saved = 0;
gfx_restart:
  gfx_bochs = 0;

  if (!bochs_try_mode()) {
    vga_mode13h();
    gfx_w = FB13_W;
    gfx_h = FB13_H;
    gfx_pitch_bytes = FB13_W;
    set_palette_color(0, 0, 0, 0);
    set_palette_color(1, 0, 0, 42);
    set_palette_color(2, 0, 42, 0);
    set_palette_color(14, 63, 63, 0);
    set_palette_color(15, 63, 63, 63);
    col_bg = 1u;
    col_fg = 15u;
    col_accent = 14u;
    col_frame = 15u;
    col_cursor = 14u;
    text_area_y0 = 128;
    text_x0 = 16;
    text_y0 = 132;
    text_x1 = (uint32_t)gfx_w - 16u;
    text_y1 = (uint32_t)gfx_h - 10u;
  } else {
    col_bg = PIXA(0, 0, 170);
    col_fg = PIXA(255, 255, 255);
    col_accent = PIXA(255, 255, 0);
    col_frame = PIXA(255, 255, 255);
    col_cursor = PIXA(255, 255, 0);
    text_area_y0 = (gfx_h * 2u) / 3u;
    text_x0 = 40;
    text_y0 = text_area_y0 + 24;
    text_x1 = gfx_w - 40;
    text_y1 = gfx_h - 24;
  }

  gfx_buf_init();
  gfx_live = 1;
  font_cell_w = FONT_W;
  font_cell_h = vga_bios_font_ok ? FONT_H : 8;

  if (session == GFX_SESSION_WIN) {
    serial_puts("[visual] win desktop\n");
  } else {
    serial_puts("[visual] gfx_clear...\n");
    gfx_clear(col_bg);
    serial_puts("[visual] gfx_clear fim\n");
    {
      int m = (int)(gfx_w > 40 ? 20 : 10);
      fill_rect(m, m, (int)gfx_w - 2 * m, 6, col_frame);
      fill_rect(m, (int)gfx_h - 14, (int)gfx_w - 2 * m, 6, col_frame);
      fill_rect(m, m, 6, (int)gfx_h - 2 * m, col_frame);
      fill_rect((int)gfx_w - 14, m, 6, (int)gfx_h - 2 * m, col_frame);
    }

    {
      int title_x = (int)(gfx_w / 2u) - 16;
      int sub_x = (int)(gfx_w / 2u) - 144;
      if (title_x < 8) title_x = 8;
      if (sub_x < 8) sub_x = 8;
      draw_string_font(title_x, 60, "SO", col_fg, col_bg);
      draw_string_font(sub_x, 100, "Modo grafico", col_accent, col_bg);
      draw_string_font(24, 140, "ESC = sair | digite na area azul", col_fg, col_bg);
    }

    fill_rect((int)text_x0 - 4, (int)text_area_y0, (int)(text_x1 - text_x0 + 8),
              (int)(gfx_h - 8 - text_area_y0), col_bg);
  }

  ps2_mouse_init();
  ps2_enable_keyboard_port();
  tablet_hw_init();
  {
    int n = 0;
    while ((inb(0x64) & 1) && n < 200) {
      (void)inb(0x60);
      n++;
    }
  }
  delay_loop();

  serial_puts("[visual] entrada no loop (ESC sai)\n");

  /* Polling PS/2: mascara o timer. Teclado fica unmasked (handler so envia EOI). */
  pic_mask_irq(0);
  pic_mask_irq(1);

  if (session == GFX_SESSION_WIN) {
    if (win_wm_run()) {
      bochs_disable_if_needed();
      serial_puts("[visual] troca de tela\n");
      goto gfx_restart;
    }
    goto gfx_session_leave;
  }

  sys_mouse_init();
  mx = sys_mx;
  my = sys_my;
  tx = (int)text_x0;
  ty = (int)text_y0;
  cursor_draw((int)mx, (int)my, col_cursor);

  /* PS/2: teclado (porto 1) vs rato (porto 2). Status bit 5 = auxiliar.
   * Pacote de rato: 3 bytes; o 1º tem sempre bit 3 (0x08). A meio do pacote
   * so aceitamos bytes como rato. Caso contrario e teclado (ou byte aux sem
   * cabecalho valido = tratar como tecla, nunca descartar em loop apertado). */
  for (;;) {
    uint8_t data = 0;
    uint8_t scb = 0;
    int from_abs = 0;
    int kind = input_poll(packet, &pkt_idx, &scb, &from_abs);
    if (kind < 0) {
      __asm__ volatile ("pause");
      continue;
    }
    if (kind == 1) {
      (void)mouse_wheel();
      if (from_abs) {
        mx = sys_mx;
        my = sys_my;
      } else {
        sys_mouse_place(mx + (int8_t)packet[1], my - (int8_t)packet[2]);
        mx = sys_mx;
        my = sys_my;
      }
      cursor_draw((int)mx, (int)my, col_cursor);
      continue;
    }
    data = scb;

    {
      uint8_t sc = data;

      if (kbd_e0) {
        kbd_e0 = 0;
        continue;
      }
      if (sc == 0xE0) {
        kbd_e0 = 1;
        continue;
      }
      if (sc == SCANCODE_LSHIFT || sc == SCANCODE_RSHIFT) {
        shift_on = 1;
        continue;
      }
      if (sc == (SCANCODE_LSHIFT | 0x80) || sc == (SCANCODE_RSHIFT | 0x80)) {
        shift_on = 0;
        continue;
      }
      if (sc & 0x80)
        continue;

      if (sc >= 128)
        continue;

      if (sc == SCANCODE_ESC) {
        serial_puts("[visual] ESC\n");
        break;
      }

      {
        char c = shift_on ? scancode_map_shift[sc] : scancode_map[sc];
        if (c == 27) {
          serial_puts("[visual] ESC (map)\n");
          break;
        }
        if (c == 0)
          continue;

        if (c == '\b') {
          if (tx > (int)text_x0) {
            cursor_restore();
            tx -= font_cell_w;
            fill_rect(tx, ty, font_cell_w, font_cell_h, col_bg);
          }
          cursor_redraw_at((int)mx, (int)my, col_cursor);
          continue;
        }
        if (c == '\n' || c == '\r') {
          tx = (int)text_x0;
          ty += font_cell_h;
          if (ty + font_cell_h > (int)text_y1) {
            cursor_restore();
            fill_rect((int)text_x0 - 4, (int)text_area_y0, (int)(text_x1 - text_x0 + 8),
                      (int)(gfx_h - 8 - text_area_y0), col_bg);
            tx = (int)text_x0;
            ty = (int)text_y0;
          }
          cursor_redraw_at((int)mx, (int)my, col_cursor);
          continue;
        }
        if (c == '\t') {
          {
            int col = (tx - (int)text_x0) / font_cell_w;
            int next_col = ((col / 4) + 1) * 4;
            tx = (int)text_x0 + next_col * font_cell_w;
          }
          if (tx > (int)text_x1 - font_cell_w) {
            tx = (int)text_x0;
            ty += font_cell_h;
            if (ty + font_cell_h > (int)text_y1) {
              cursor_restore();
              fill_rect((int)text_x0 - 4, (int)text_area_y0, (int)(text_x1 - text_x0 + 8),
                        (int)(gfx_h - 8 - text_area_y0), col_bg);
              tx = (int)text_x0;
              ty = (int)text_y0;
            }
          }
          cursor_redraw_at((int)mx, (int)my, col_cursor);
          continue;
        }

        if (tx + font_cell_w > (int)text_x1) {
          tx = (int)text_x0;
          ty += font_cell_h;
          if (ty + font_cell_h > (int)text_y1) {
            cursor_restore();
            fill_rect((int)text_x0 - 4, (int)text_area_y0, (int)(text_x1 - text_x0 + 8),
                      (int)(gfx_h - 8 - text_area_y0), col_bg);
            tx = (int)text_x0;
            ty = (int)text_y0;
          }
        }
        cursor_restore();
        draw_char_font(tx, ty, c, col_fg, col_bg);
        tx += font_cell_w;
        cursor_redraw_at((int)mx, (int)my, col_cursor);
      }
    }
  }

gfx_session_leave:
  gfx_live = 0;
  pic_mask_all();

  cursor_restore();
  gfx_mode_sel = -1;
  serial_puts("[visual] modo texto 80x25 + DAC\n");
  bochs_disable_if_needed();
  vga_mode_text();
  vga_dac_restore_std16();
  clear_screen();
  serial_puts("[visual] fim\n");
}

void visual_run(void) {
  gfx_session_run(GFX_SESSION_VISUAL);
}

void win_run(void) {
  gfx_session_run(GFX_SESSION_WIN);
}
