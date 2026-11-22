/*
 * visual.c - Modo grafico
 * QEMU (-vga std, 16 MB) e v86 (WebAssembly, 16 MB) falam Bochs VBE.
 * Tenta 1920x1080, depois modos menores, em 32 bpp.
 * UEFI: usa o framebuffer GOP que o GRUB entregou (resolucao fixa).
 * Sem Bochs VBE: VGA modo 13h 320x200.
 */
#include "kthread.h"
#include <stdint.h>
#include "serial.h"
#include "shell.h"
#include "pic.h"
#include "ps2in.h"
#include "reboot.h"
#include "drives.h"
#include "mm.h"
#include "module.h"
#include "wine.h"
#include "kexports.h"
#include "net.h"
#include "sched.h"
#include "irq.h"
#include "visual.h"
#include "prtscr.h"
#include "keymap.h"
#include "clip.h"
#include "bootinfo.h"
#include "fbcon.h"
#include "efistub.h"
#include "xhci.h"
#include "vt.h"
#include "env.h"
#include "gpu.h"
#include "wfont.h"
#include "linkext.h"
#include "smp.h"
#include "c_compiler/sox.h"
const char* sodos_version(void); /* kernel/version.c */

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

/* BAR0 memoria do primeiro dispositivo classe Display (PCI class 0x03).
 * id (se nao for NULL) recebe vendor << 16 | device. */
static uint32_t pci_find_display(uint32_t* id) {
  for (uint8_t dev = 0; dev < 32; dev++) {
    uint32_t vid = pci_cfg_read32(0, dev, 0, 0);
    if (vid == 0xFFFFFFFFu) continue;
    uint32_t cls = pci_cfg_read32(0, dev, 0, 0x08);
    if (((cls >> 24) & 0xFFu) != 0x03u) continue;
    uint32_t bar0 = pci_cfg_read32(0, dev, 0, 0x10);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu) continue;
    if (bar0 & 1u) continue;
    if (id) *id = (vid & 0xFFFFu) << 16 | vid >> 16;
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
extern const uint8_t font8x16_cp850[256][16];
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
/* No UEFI a tabela vira a lista de modos da placa (efistub.c, gop_modes). */
#define GFX_MODE_MAX 12
static uint16_t gfx_mode_w[GFX_MODE_MAX] = { 1920u, 1600u, 1280u, 1024u, 800u, 640u };
static uint16_t gfx_mode_h[GFX_MODE_MAX] = { 1080u, 1200u, 1024u, 768u, 600u, 480u };
static char gfx_mode_txt[GFX_MODE_MAX][24];
static const char* gfx_mode_name[GFX_MODE_MAX] = {
  "1920x1080", "1600x1200", "1280x1024", "1024x768", "800x600", "640x480"
};
static int gfx_mode_n = 6;
#define GFX_MODE_N gfx_mode_n
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

/* 1 = LFB 32 bpp (Bochs ou GOP); 0 = modo 13h 8 bpp */
static int gfx_bochs;
/* 1 = o LFB e o framebuffer GOP do UEFI: sem registros Bochs, sem troca de modo */
static int gfx_gop;
static int gfx_live;
/* 1 do inicio ao fim de uma sessao grafica, mesmo atras de uma tela do DOS
 * (gfx_live fica 0 nesse caso). So o TOP le, por win_vram_info. */
static int gfx_session_on;
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
/* 1 so enquanto gfx_gpu_try() remonta o quadro para um driver de placa:
 * o driver usa o back buffer em RAM, entao a pagina dupla fica de fora. */
static int gfx_for_gpu;

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

/* Sem fonte na VGA (UEFI, ou BIOS que nao a deixou): usa a 8x16 embutida. */
static void font_load_builtin(void) {
  for (int ch = 0; ch < 256; ch++)
    for (int row = 0; row < 32; row++)
      vga_bios_font[ch * 32 + row] = row < 16 ? font8x16_cp850[ch][row] : 0;
  vga_bios_font_ok = 1;
}

/* A BIOS deixa a fonte CP437 (sem ã, õ, Á...). O SODOS usa CP850, a pagina
 * do DOS em portugues: a fonte embutida vai para a VGA no boot e volta para
 * la a cada saida do modo grafico. */
void vga_capture_bios_font(void) {
  font_load_builtin();
  if (!vga_legacy()) {
    serial_puts("[vga] UEFI: fonte 8x16 CP850 embutida\n");
    return;
  }
  vga_restore_bios_font();
  serial_puts("[vga] fonte 8x16 CP850 carregada\n");
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
  if (!vga_legacy()) {
    fbcon_suspend(0);
    return;
  }
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
  /* No Bochs/QEMU 0x0A e a VRAM em unidades de 64 KiB.  No VirtualBox o
   * mesmo indice e VBE_DISPI_INDEX_VBOX_VIDEO e devolve 1; 64 KiB nao cabe
   * nem o modo 640x480x32.  Nesse caso trate como desconhecido e deixe a
   * verificacao de XRES/YRES/BPP decidir. */
  if (vram64 <= 1u || vram64 >= 4096u) return 0;
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
    uint32_t bar = pci_find_display(0);
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
      /* 16 MB: a VRAM inteira do Bochs VBE, para qualquer modo de video. */
      if (mm_map_wc((uint64_t)cand[j], 16u << 20) > 0)
        serial_puts("[visual] LFB write-combining\n");
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

/* So o kernel_multiboot.elf tem efistub.o; no kernel.elf o simbolo e 0. */
extern const struct efistub_video* efistub_video(void) __attribute__((weak));

/* Escolha feita no WIN que so vale no proximo boot (GOP). 0 = nenhuma;
 * 0xFFFF = "Padrao" (tela=auto). */
static uint16_t gop_pend_w, gop_pend_h;

static void gop_mode_label(int i) {
  char* t = gfx_mode_txt[i];
  int n = 0;
  uint32_t v[2];
  int k;
  v[0] = gfx_mode_w[i];
  v[1] = gfx_mode_h[i];
  for (k = 0; k < 2; k++) {
    char d[6];
    int m = 0;
    uint32_t x = v[k];
    do { d[m++] = (char)('0' + x % 10u); x /= 10u; } while (x && m < 5);
    while (m) t[n++] = d[--m];
    if (k == 0) t[n++] = 'x';
  }
  if (gop_pend_w == gfx_mode_w[i] && gop_pend_h == gfx_mode_h[i]) {
    const char* s = " (reinicie)";
    while (*s && n < 23) t[n++] = *s++;
  }
  t[n] = 0;
  gfx_mode_name[i] = t;
}

/* UEFI: a lista de resolucoes e a dos modos da placa (GOP), maiores
 * primeiro, sem repetir e sem os menores que 640x480. Trocar so vale no
 * proximo boot: o firmware liga o modo antes do kernel (efistub.c). */
static void gop_modes(void) {
  const struct efistub_video* ev = efistub_video ? efistub_video() : 0;
  uint32_t i;
  int j, k;
  if (!ev) return;
  gfx_mode_n = 0;
  for (i = 0; i < ev->n; i++) {
    uint16_t w = ev->w[i], h = ev->h[i];
    if (w < 640 || h < 480) continue;
    for (j = 0; j < gfx_mode_n; j++)
      if (gfx_mode_w[j] == w && gfx_mode_h[j] == h) break;
    if (j < gfx_mode_n) continue;
    /* insere ordenado por area (maior primeiro) */
    for (j = 0; j < gfx_mode_n; j++)
      if ((uint32_t)w * h > (uint32_t)gfx_mode_w[j] * gfx_mode_h[j]) break;
    if (j >= GFX_MODE_MAX) continue;
    if (gfx_mode_n == GFX_MODE_MAX) gfx_mode_n--;
    for (k = gfx_mode_n; k > j; k--) {
      gfx_mode_w[k] = gfx_mode_w[k - 1];
      gfx_mode_h[k] = gfx_mode_h[k - 1];
    }
    gfx_mode_w[j] = w;
    gfx_mode_h[j] = h;
    gfx_mode_n++;
  }
  for (j = 0; j < gfx_mode_n; j++) gop_mode_label(j);
}

/* Grava D:\SODOS\TELA.CFG (tela=LxA ou tela=auto) para o proximo boot. */
static int gop_mode_save(int sel) {
  char t[32] = "tela=";
  int n = 5;
  int j;
  if (sel < 0) {
    const char* s = "auto\r\n";
    while (*s) t[n++] = *s++;
  } else {
    const char* s = gfx_mode_txt[sel];
    while (*s && *s != ' ') t[n++] = *s++;
    t[n++] = '\r';
    t[n++] = '\n';
  }
  t[n] = 0;
  if (file_save("D:\\SODOS\\TELA.CFG", t, n) != 0) {
    serial_puts("[visual] nao gravou D:\\SODOS\\TELA.CFG\n");
    return 0;
  }
  if (sel < 0) {
    gop_pend_w = gop_pend_h = 0xFFFFu;
  } else {
    gop_pend_w = gfx_mode_w[sel];
    gop_pend_h = gfx_mode_h[sel];
  }
  /* escolher a que ja esta na tela desfaz a pendencia */
  if (gop_pend_w == (uint16_t)gfx_w && gop_pend_h == (uint16_t)gfx_h) gop_pend_w = gop_pend_h = 0;
  for (j = 0; j < gfx_mode_n; j++) gop_mode_label(j);
  serial_puts("[visual] D:\\SODOS\\TELA.CFG: ");
  serial_puts(t);
  return 1;
}

/* UEFI: o framebuffer que o firmware ligou antes do kernel (efistub.c, com
 * o \SODOS\TELA.CFG) ou o que o GRUB pediu ao GOP. Nao troca sem reiniciar. */
static int gop_try_mode(void) {
  const struct BootInfo* bi = bootinfo_get();
  const struct efistub_video* ev;
  int j;
  if (vga_legacy() || !bi || !bi->fb_addr || bi->fb_bpp != 32) return 0;
  gfx_lfb = (volatile uint8_t*)(uintptr_t)bi->fb_addr;
  gfx_w = bi->fb_width;
  gfx_h = bi->fb_height;
  gfx_pitch_bytes = bi->fb_pitch;
  gfx_bochs = 1;
  gfx_gop = 1;
  gfx_mode_sel = -1;
  gop_modes();
  /* tela=LxA no TELA.CFG: a marca fica na resolucao, nao em "Padrao" */
  ev = efistub_video ? efistub_video() : 0;
  if (ev && ev->from_cfg)
    for (j = 0; j < gfx_mode_n; j++)
      if (gfx_mode_w[j] == gfx_w && gfx_mode_h[j] == gfx_h) gfx_mode_sel = j;
  serial_puts("[visual] GOP ");
  serial_u16((uint16_t)gfx_w);
  serial_puts("x");
  serial_u16((uint16_t)gfx_h);
  serial_puts("x32\n");
  return 1;
}

static void bochs_disable_if_needed(void) {
  if (gfx_gop) {
    gfx_gop = 0;
    gfx_bochs = 0;
    return;
  }
  if (gfx_bochs) {
    bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    gfx_bochs = 0;
  }
}

/* 1 se a escolha muda a tela. sel < 0 pede de novo a maior que couber. */
static int gfx_mode_pick(int sel) {
  if (gfx_gop) {
    /* GOP: o modo so muda no proximo boot. Grava e fica na mesma tela. */
    if (sel >= gfx_mode_n) return 0;
    if (gop_mode_save(sel)) gfx_mode_sel = sel;
    return 0;
  }
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

/* ---------------------------------------------------------------------------
 * Temas (Configuracoes > Temas, TEMAS.EXE).
 *
 * Cada tema e uma pasta \SODOS\ICONS\<NOME> com TEMA.CFG (cores, estilo) e os
 * icones NOME.ICN. A escolha do usuario fica em D:\SODOS\TEMA.CFG
 * ("tema=MODERNO"); sem esse arquivo vale CLASSICO, que e o WIN de sempre: as
 * cores abaixo sao as dele e os icones continuam desenhados pelo kernel. Chave
 * que falta no TEMA.CFG de um tema fica com o valor classico, entao um tema
 * novo so precisa dizer o que muda.
 * Em 8bpp (modo 13h) nao ha mistura: tudo fica solido e sem icones de arquivo.
 * ------------------------------------------------------------------------- */
struct ui_theme {
  uint32_t bar, panel, title_a, title_i, text, ttext, dim, accent, border;
  uint32_t hot, hot_text, close, tbtn, desk, label, label_sh;
  int bar_a, panel_a, radius, label_sh_on, icon_px;
  int modern;          /* estilo=moderno: claro, chapado, cantos maiores */
  int icons;           /* icones=arquivo: usa os .ICN da pasta */
  char plate[12];      /* prato atras dos icones do desktop (FUNDO) */
  /* widgets dos programas (GUI.DLL, k_ui_theme) */
  uint32_t w_face, w_bg, w_panel, w_text, w_dim, w_dis, w_sel, w_sel_text, w_field,
      w_border, w_accent;
  int w_radius, w_btn_h, w_row_h;
  char name[16];       /* pasta em \SODOS\ICONS */
  char dir[40];        /* "X:\SODOS\ICONS\NOME\" ou "" */
  uint32_t gen;        /* muda a cada troca: programas redesenham */
};

static const struct ui_theme ui_th_classic = {
  0x0B1522u, 0x101C2Bu, 0x16283Eu, 0x1B2230u, 0xE8F0FAu, 0xE8F0FAu, 0x93A8C4u,
  0x2F80D0u, 0x2E4A6Cu, 0x2F80D0u, 0xE8F0FAu, 0xC23440u, 0x3A4250u, 0, 0xFFFFFFu, 0,
  205, 230, 8, 1, 32,
  0, 0, "",
  0xC0C0C0u, 0xC0C0C0u, 0xD4D0C8u, 0x000000u, 0x404040u, 0x808080u, 0x000080u,
  0xFFFFFFu, 0xFFFFFFu, 0x808080u, 0x000080u,
  0, 0, 0,
  "CLASSICO", "", 0
};
static struct ui_theme ui_th = {
  0x0B1522u, 0x101C2Bu, 0x16283Eu, 0x1B2230u, 0xE8F0FAu, 0xE8F0FAu, 0x93A8C4u,
  0x2F80D0u, 0x2E4A6Cu, 0x2F80D0u, 0xE8F0FAu, 0xC23440u, 0x3A4250u, 0, 0xFFFFFFu, 0,
  205, 230, 8, 1, 32,
  0, 0, "",
  0xC0C0C0u, 0xC0C0C0u, 0xD4D0C8u, 0x000000u, 0x404040u, 0x808080u, 0x000080u,
  0xFFFFFFu, 0xFFFFFFu, 0x808080u, 0x000080u,
  0, 0, 0,
  "CLASSICO", "", 0
};

#define UI_BAR        ui_th.bar
#define UI_BAR_A      ui_th.bar_a
#define UI_PANEL      ui_th.panel
#define UI_PANEL_A    ui_th.panel_a
#define UI_TITLE_A    ui_th.title_a
#define UI_TITLE_I    ui_th.title_i
#define UI_TEXT       ui_th.text
#define UI_TTEXT      ui_th.ttext
#define UI_DIM        ui_th.dim
#define UI_ACCENT     ui_th.accent
#define UI_BORDER     ui_th.border
#define UI_HOT        ui_th.hot
#define UI_HOT_TEXT   ui_th.hot_text
#define UI_RADIUS     ui_th.radius

static int th_streq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void th_copy(char* d, int max, const char* s, int n) {
  int i;
  for (i = 0; i < n && s[i] && i < max - 1; i++) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    d[i] = c;
  }
  d[i] = 0;
}

static int th_hex(const char* s, int n, uint32_t* out) {
  uint32_t v = 0;
  int i;
  if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    n -= 2;
  }
  if (n < 6) return 0;
  for (i = 0; i < 6; i++) {
    char c = s[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return 0;
    v = v * 16u + (uint32_t)d;
  }
  *out = v;
  return 1;
}

static int th_int(const char* s, int n) {
  int v = 0, i;
  for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
  return v;
}

/* Uma linha chave=valor do TEMA.CFG da pasta do tema. */
static void th_key(struct ui_theme* t, const char* k, int kn, const char* v, int vn) {
  static const struct { const char* k; int off; } cols[] = {
#define TC(n, f) { n, (int)__builtin_offsetof(struct ui_theme, f) }
    TC("barra", bar), TC("painel", panel), TC("titulo", title_a), TC("tituloin", title_i),
    TC("texto", text), TC("textotitulo", ttext), TC("fraco", dim), TC("destaque", accent),
    TC("borda", border), TC("realce", hot), TC("realcetexto", hot_text),
    TC("fechar", close), TC("botaotitulo", tbtn), TC("desk", desk), TC("rotulo", label),
    TC("w_face", w_face), TC("w_bg", w_bg), TC("w_panel", w_panel), TC("w_texto", w_text),
    TC("w_fraco", w_dim), TC("w_desab", w_dis), TC("w_sel", w_sel),
    TC("w_seltexto", w_sel_text), TC("w_campo", w_field), TC("w_borda", w_border),
    TC("w_destaque", w_accent),
#undef TC
  };
  char key[16];
  int i;
  if (kn <= 0 || kn >= (int)sizeof(key)) return;
  for (i = 0; i < kn; i++) key[i] = (char)(k[i] >= 'A' && k[i] <= 'Z' ? k[i] + 32 : k[i]);
  key[kn] = 0;
  for (i = 0; i < (int)(sizeof(cols) / sizeof(cols[0])); i++) {
    if (th_streq(key, cols[i].k)) {
      th_hex(v, vn, (uint32_t*)((char*)t + cols[i].off));
      return;
    }
  }
  if (th_streq(key, "estilo")) t->modern = vn >= 7 && (v[0] == 'm' || v[0] == 'M');
  else if (th_streq(key, "icones")) t->icons = vn >= 7 && (v[0] == 'a' || v[0] == 'A');
  else if (th_streq(key, "prato")) th_copy(t->plate, (int)sizeof(t->plate), v, vn);
  else if (th_streq(key, "rotulosombra")) {
    t->label_sh_on = th_hex(v, vn, &t->label_sh);
  } else if (th_streq(key, "barraalfa")) t->bar_a = th_int(v, vn);
  else if (th_streq(key, "painelalfa")) t->panel_a = th_int(v, vn);
  else if (th_streq(key, "raio")) t->radius = th_int(v, vn);
  else if (th_streq(key, "icone")) t->icon_px = th_int(v, vn);
  else if (th_streq(key, "w_raio")) t->w_radius = th_int(v, vn);
  else if (th_streq(key, "w_btn")) t->w_btn_h = th_int(v, vn);
  else if (th_streq(key, "w_linha")) t->w_row_h = th_int(v, vn);
}

/* Le chave=valor de um arquivo de texto; ';' e '#' comecam comentario. */
static void th_parse(const char* cfg, int n, struct ui_theme* t, char* tema, int tmax) {
  int i = 0;
  while (i < n) {
    int j = i, e, kq;
    while (j < n && cfg[j] != '\n' && cfg[j] != '\r') j++;
    while (i < j && (cfg[i] == ' ' || cfg[i] == '\t')) i++;
    if (i < j && cfg[i] != ';' && cfg[i] != '#') {
      for (kq = i; kq < j && cfg[kq] != '='; kq++) { }
      if (kq < j) {
        int ke = kq, vs = kq + 1;
        while (ke > i && cfg[ke - 1] == ' ') ke--;
        while (vs < j && cfg[vs] == ' ') vs++;
        e = j;
        while (e > vs && (cfg[e - 1] == ' ' || cfg[e - 1] == '\t')) e--;
        if (tema && ke - i == 4 && (cfg[i] == 't' || cfg[i] == 'T') &&
            (cfg[i + 1] == 'e' || cfg[i + 1] == 'E'))
          th_copy(tema, tmax, cfg + vs, e - vs);
        else if (t)
          th_key(t, cfg + i, ke - i, cfg + vs, e - vs);
      }
    }
    while (j < n && (cfg[j] == '\n' || cfg[j] == '\r')) j++;
    i = j;
  }
}

/* Acha "X:\SODOS\ICONS\<nome>\" na primeira unidade que tem a pasta. */
static int th_find_dir(const char* name, char* out, int max) {
  static const char order[] = "DUCEFGHIJKLMNOPQRSTVWXYZ";
  static const char pre[] = "X:\\SODOS\\ICONS\\";
  int d;
  for (d = 0; order[d]; d++) {
    char path[56];
    int i = 0, j;
    if (order[d] != 'U' && !drives_is_mounted(order[d])) continue;
    if (order[d] == 'U' && !drives_is_mounted('U')) continue;
    for (j = 0; pre[j]; j++) path[i++] = pre[j];
    for (j = 0; name[j] && i < 44; j++) path[i++] = name[j];
    path[i++] = '\\';
    path[i] = 0;
    path[0] = order[d];
    {
      char cfgp[64];
      int k;
      for (k = 0; path[k]; k++) cfgp[k] = path[k];
      for (j = 0; "TEMA.CFG"[j]; j++) cfgp[k++] = "TEMA.CFG"[j];
      cfgp[k] = 0;
      if (file_size(cfgp) < 0) continue;
    }
    if (i >= max) return 0;
    for (j = 0; j <= i; j++) out[j] = path[j];
    return 1;
  }
  return 0;
}

static void icn_flush(void);   /* cache de icones, mais abaixo */
static int win_api_dirty;

/* Carrega o tema 'name' (pasta em \SODOS\ICONS). Sem a pasta, so CLASSICO
 * vale (as cores de sempre). 0 = trocou. */
static int ui_theme_load(const char* name) {
  struct ui_theme t = ui_th_classic;
  char nm[16];
  static char cfg[2048];
  int n;
  th_copy(nm, (int)sizeof(nm), name && name[0] ? name : "CLASSICO", 15);
  for (n = 0; nm[n]; n++)
    if (nm[n] == '\\' || nm[n] == '/' || nm[n] == ':' || nm[n] == '.') return -1;
  th_copy(t.name, (int)sizeof(t.name), nm, 15);
  if (th_find_dir(nm, t.dir, (int)sizeof(t.dir))) {
    char p[64];
    int i, j;
    for (i = 0; t.dir[i]; i++) p[i] = t.dir[i];
    for (j = 0; "TEMA.CFG"[j]; j++) p[i++] = "TEMA.CFG"[j];
    p[i] = 0;
    n = file_load(p, cfg, (int)sizeof(cfg));
    if (n > 0) th_parse(cfg, n, &t, 0, 0);
  } else if (!th_streq(nm, "CLASSICO")) {
    serial_puts("[tema] sem \\SODOS\\ICONS\\");
    serial_puts(nm);
    serial_puts("\n");
    return -1;
  }
  if (t.radius < 0 || t.radius > 20) t.radius = 8;
  if (t.icon_px < 16 || t.icon_px > 96) t.icon_px = 32;
  t.gen = ui_th.gen + 1;
  ui_th = t;
  icn_flush();
  win_api_dirty = 1;
  serial_puts("[tema] ");
  serial_puts(ui_th.name);
  serial_puts(ui_th.dir[0] ? " em " : " (interno)");
  if (ui_th.dir[0]) serial_puts(ui_th.dir);
  serial_puts("\n");
  return 0;
}

/* D:\SODOS\TEMA.CFG (escolha do usuario). */
static void ui_theme_boot(void) {
  static char cfg[256];
  char tema[16];
  int n;
  tema[0] = 0;
  n = file_load("D:\\SODOS\\TEMA.CFG", cfg, (int)sizeof(cfg));
  if (n <= 0) n = file_load("U:\\SODOS\\TEMA.CFG", cfg, (int)sizeof(cfg));
  if (n > 0) th_parse(cfg, n, 0, tema, (int)sizeof(tema));
  if (!tema[0] || ui_theme_load(tema) != 0) ui_theme_load("CLASSICO");
}

/* ---- icones do tema (NOME.ICN: "ICN1", u16 w, u16 h, RGBA) --------------
 * Cache por nome (inclusive os que faltam, para nao reler o disco a cada
 * quadro) e uma copia ja reduzida, em alfa pre-multiplicado, no ultimo
 * tamanho pedido. Memoria do alto (umalloc_top): sai na troca de tema. */
#define ICN_MAX 96
struct icn_ent {
  char name[12];
  uint8_t* src;     /* arquivo inteiro (cabecalho + RGBA) */
  int w, h;
  uint32_t* sc;     /* reduzida: 0xAARRGGBB pre-multiplicada */
  int ss, sw, sh;   /* lado pedido e tamanho real da reduzida */
  int miss;
};
static struct icn_ent icn_tab[ICN_MAX];
static int icn_n;

static void icn_flush(void) {
  int i;
  for (i = 0; i < icn_n; i++) {
    if (icn_tab[i].src) ufree(icn_tab[i].src);
    if (icn_tab[i].sc) ufree(icn_tab[i].sc);
    icn_tab[i].src = 0;
    icn_tab[i].sc = 0;
  }
  icn_n = 0;
}

static struct icn_ent* icn_get(const char* name) {
  struct icn_ent* e;
  char path[64];
  int i, j, sz, w, h;
  uint8_t* b;
  if (!ui_th.icons || !ui_th.dir[0] || !gfx_bochs || !um_ready() || !name || !name[0])
    return 0;
  for (i = 0; i < icn_n; i++)
    if (th_streq(icn_tab[i].name, name)) return icn_tab[i].miss ? 0 : &icn_tab[i];
  if (icn_n >= ICN_MAX) return 0;
  e = &icn_tab[icn_n++];
  th_copy(e->name, (int)sizeof(e->name), name, 11);
  e->miss = 1;
  e->src = 0;
  e->sc = 0;
  e->ss = 0;
  for (i = 0; ui_th.dir[i] && i < 40; i++) path[i] = ui_th.dir[i];
  for (j = 0; e->name[j]; j++) path[i++] = e->name[j];
  path[i++] = '.';
  path[i++] = 'I';
  path[i++] = 'C';
  path[i++] = 'N';
  path[i] = 0;
  sz = file_size(path);
  if (sz < 9 || sz > 8 + 128 * 128 * 4) return 0;
  b = (uint8_t*)umalloc_top((uint32_t)sz + 1);
  if (!b) return 0;
  if (file_load(path, (char*)b, sz + 1) != sz || b[0] != 'I' || b[1] != 'C' || b[2] != 'N' ||
      b[3] != '1') {
    ufree(b);
    return 0;
  }
  w = b[4] | (b[5] << 8);
  h = b[6] | (b[7] << 8);
  if (w < 1 || h < 1 || w > 128 || h > 128 || 8 + w * h * 4 > sz) {
    ufree(b);
    return 0;
  }
  e->src = b;
  e->w = w;
  e->h = h;
  e->miss = 0;
  return e;
}

/* Reduz para caber em s x s (media da area; para cima, vizinho mais perto). */
static int icn_scale(struct icn_ent* e, int s) {
  int iw, ih, x, y;
  const uint8_t* px;
  if (s < 1) return 0;
  if (e->sc && e->ss == s) return 1;
  if (e->sc) ufree(e->sc);
  e->sc = 0;
  if (e->w >= e->h) {
    iw = s;
    ih = e->h * s / e->w;
  } else {
    ih = s;
    iw = e->w * s / e->h;
  }
  if (iw < 1) iw = 1;
  if (ih < 1) ih = 1;
  e->sc = (uint32_t*)umalloc_top((uint32_t)(iw * ih * 4));
  if (!e->sc) return 0;
  px = e->src + 8;
  for (y = 0; y < ih; y++) {
    int y0 = y * e->h / ih, y1 = (y + 1) * e->h / ih;
    if (y1 <= y0) y1 = y0 + 1;
    for (x = 0; x < iw; x++) {
      int x0 = x * e->w / iw, x1 = (x + 1) * e->w / iw, sx, sy;
      uint32_t ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
      if (x1 <= x0) x1 = x0 + 1;
      for (sy = y0; sy < y1; sy++)
        for (sx = x0; sx < x1; sx++) {
          const uint8_t* p = px + (sy * e->w + sx) * 4;
          uint32_t a = p[3];
          ar += p[0] * a;
          ag += p[1] * a;
          ab += p[2] * a;
          aa += a;
          n++;
        }
      ar /= 255u * n;
      ag /= 255u * n;
      ab /= 255u * n;
      aa /= n;
      e->sc[y * iw + x] = (aa << 24) | (ar << 16) | (ag << 8) | ab;
    }
  }
  e->ss = s;
  e->sw = iw;
  e->sh = ih;
  return 1;
}

/* Desenha centrado no quadrado (x,y,s), misturando com o que ja esta na tela. */
static void icn_draw(struct icn_ent* e, int x, int y, int s) {
  int ox, oy, r;
  if (!e || !icn_scale(e, s)) return;
  ox = x + (s - e->sw) / 2;
  oy = y + (s - e->sh) / 2;
  for (r = 0; r < e->sh; r++) {
    int dy = oy + r, c0 = 0, c1 = e->sw, c;
    const uint32_t* src = e->sc + r * e->sw;
    volatile uint32_t* row;
    if (dy < 0 || dy >= (int)gfx_h) continue;
    if (ox < 0) c0 = -ox;
    if (ox + c1 > (int)gfx_w) c1 = (int)gfx_w - ox;
    if (c0 >= c1) continue;
    row = gfx_offscreen
      ? (volatile uint32_t*)(gfx_back + ((uint32_t)dy * gfx_w + (uint32_t)ox) * 4u)
      : (volatile uint32_t*)(gfx_lfb + ((uint32_t)dy + gfx_draw_y0) * gfx_pitch_bytes +
                             (uint32_t)ox * 4u);
    for (c = c0; c < c1; c++) {
      uint32_t p = src[c], a = p >> 24, k, d;
      if (!a) continue;
      if (a == 255) {
        row[c] = p & 0xFFFFFFu;
        continue;
      }
      k = 255u - a;
      d = row[c];
      row[c] = ((((d >> 16) & 0xFFu) * k / 255u + ((p >> 16) & 0xFFu)) << 16) |
               ((((d >> 8) & 0xFFu) * k / 255u + ((p >> 8) & 0xFFu)) << 8) |
               ((d & 0xFFu) * k / 255u + (p & 0xFFu));
    }
  }
}

/* Icone do desktop no tema: prato (FUNDO) e o glifo no centro dele.
 * O prato do mkicons.py tem o circulo centrado em 29/64. 1 = desenhou. */
static int th_desk_icon(const char* name, int x, int y, int s) {
  struct icn_ent* g = icn_get(name);
  struct icn_ent* p;
  if (!g) return 0;
  p = ui_th.plate[0] ? icn_get(ui_th.plate) : 0;
  if (p) {
    int gs = s * 54 / 100, c = s * 29 / 64;
    icn_draw(p, x, y, s);
    icn_draw(g, x + c - gs / 2, y + c - gs / 2, gs);
  } else {
    icn_draw(g, x, y, s);
  }
  return 1;
}

/* Icone de uma linha de menu, pelo texto dela. */
static const char* th_menu_icon(const char* label) {
  static const char* const map[][2] = {
    {"Bloco de notas", "NOTAS"}, {"Gerenciador", "GERENC"}, {"Explorer", "EXPLORER"},
    {"Configuracoes", "CONFIG"}, {"Navegador", "NAVEGA"}, {"Command", "COMMAND"},
    {"Sair", "SAIR"}, {"Tela", "TELA"}, {"Fontes", "FONTES"},
    {"Fontes avancadas", "FONTADV"}, {"Design", "DESIGN"}, {"Menu", "MENU"},
    {"Abrir com", "ABRIRCOM"}, {"Temas", "TEMAS"}, {"Nova pasta", "PASTA"},
    {"Console", "CMD"}, {"Configuracoes de tela", "TELA"},
  };
  int i;
  for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
    if (th_streq(map[i][0], label)) return map[i][1];
  return 0;
}

/* ---------------------------------------------------------------------------
 * Configuracao de video escolhida pelo VIDEOCFG.EXE:
 *   gfx_quality  0 = retro (como sempre foi: alfa binario, sem mistura)
 *                1 = avancado (mistura alfa em gui_pixels)
 *   desk_rgb     cor de fundo do desktop (0xRRGGBB; 0 = o teal padrao)
 *   desk_wall    papel de parede ja escalado para a tela, na ordem do LFB
 *                (palavra 0x00RRGGBB, que vira B,G,R no byte a byte)
 * ------------------------------------------------------------------------- */
static uint32_t* desk_wall;
static uint32_t desk_rgb;
static int gfx_quality;
/* Interface escolhida em Design (VIDEO.CFG). menu_pos: 0 esquerda, 1 centro, 2 direita. */
static int ui_menu_pos;
static int ui_glass;      /* 1 = menu Iniciar translucido */
static int ui_ia;         /* 1 = painel de IA ligado */
static int ui_ia_launch;  /* pedido de abrir o AIPANEL pendente */
/* pedido de abrir um programa no WIN (k_win_launch) */
static char launch_path[LINKEXT_PATH_MAX];
/* arquivo ou endereco (LINKEXT, ShellExecute de URL do WINE); "" = sem.
 * Enderecos de login passam de 500 caracteres: nao e o LINKEXT_PATH_MAX. */
static char launch_arg[2048];
static int launch_pending;

static uint32_t gui_col(uint32_t rgb);   /* definido mais abaixo */

/* 0xRRGGBB -> palavra no formato que fill_rect/gfx_clear esperam */
static uint32_t rgb_to_pixa(uint32_t rgb) {
  return ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu) |
         (0xFFu << 24);
}

/* Fundo do desktop: papel de parede, cor escolhida ou o teal padrao. */
static void gfx_desk_clear(void) {
  if (gfx_bochs && desk_wall) {
    uint32_t y;
    for (y = 0; y < gfx_h; y++) {
      uint32_t* src = desk_wall + y * gfx_w;
      volatile uint32_t* dst = gfx_offscreen
        ? (volatile uint32_t*)(gfx_back + y * gfx_w * 4u)
        : (volatile uint32_t*)(gfx_lfb + (y + gfx_draw_y0) * gfx_pitch_bytes);
      uint32_t n = gfx_w;
      __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
    }
    return;
  }
  if (!gfx_bochs) {
    gfx_clear(0);
    return;
  }
  gfx_clear(desk_rgb ? rgb_to_pixa(desk_rgb)
            : ui_th.desk ? rgb_to_pixa(ui_th.desk) : gui_col(0x008080));
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


/* Retangulo com alfa sobre o que ja esta na tela (alpha <= 0 = opaco). */
static void fill_rect_a(int x, int y, int w, int h, uint32_t rgb, int alpha) {
  int x1, y1, x2, y2, dy;
  uint32_t a, na, sr, sg, sb;
  if (w <= 0 || h <= 0) return;
  if (alpha >= 255) {
    fill_rect(x, y, w, h, rgb);       /* mesma convencao do fill_rect (PIXA) */
    return;
  }
  if (alpha <= 0) return;
  if (!gfx_bochs) return;
  x1 = x;
  y1 = y;
  x2 = x + w;
  y2 = y + h;
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > (int)gfx_w) x2 = (int)gfx_w;
  if (y2 > (int)gfx_h) y2 = (int)gfx_h;
  if (x1 >= x2 || y1 >= y2) return;
  a = (uint32_t)alpha;
  na = 255u - a;
  /* rgb vem no formato PIXA (0xFF_BB_GG_RR): B no alto, R no byte baixo */
  sr = rgb & 0xFFu;
  sg = (rgb >> 8) & 0xFFu;
  sb = (rgb >> 16) & 0xFFu;
  for (dy = y1; dy < y2; dy++) {
    volatile uint32_t* row = gfx_offscreen
      ? (volatile uint32_t*)(gfx_back + ((uint32_t)dy * gfx_w + (uint32_t)x1) * 4u)
      : (volatile uint32_t*)(gfx_lfb + ((uint32_t)dy + gfx_draw_y0) * gfx_pitch_bytes +
                             (uint32_t)x1 * 4u);
    int dx;
    for (dx = 0; dx < x2 - x1; dx++) {
      uint32_t d = row[dx];
      uint32_t dr = (d >> 16) & 0xFFu, dg = (d >> 8) & 0xFFu, db = d & 0xFFu;
      row[dx] = (((sr * a + dr * na) / 255u) << 16) |
                (((sg * a + dg * na) / 255u) << 8) |
                ((sb * a + db * na) / 255u);
    }
  }
}

/* Retangulo de cantos arredondados (raio r), com alfa opcional. */
static void round_rect(int x, int y, int w, int h, int r, uint32_t rgb, int alpha) {
  int j, d;
  if (w <= 0 || h <= 0) return;
  if (r < 2) {
    fill_rect_a(x, y, w, h, rgb, alpha);
    return;
  }
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  /* miolo */
  fill_rect_a(x + r, y, w - 2 * r, h, rgb, alpha);
  fill_rect_a(x, y + r, r, h - 2 * r, rgb, alpha);
  fill_rect_a(x + w - r, y + r, r, h - 2 * r, rgb, alpha);
  /* cantos: para cada linha do arco, acha o recuo d da circunferencia */
  for (j = 0; j < r; j++) {
    int dy = r - j;
    d = 0;
    while (d < r && (r - d) * (r - d) + dy * dy > r * r) d++;
    fill_rect_a(x + d, y + j, w - 2 * d, 1, rgb, alpha);
    fill_rect_a(x + d, y + h - 1 - j, w - 2 * d, 1, rgb, alpha);
  }
}

static void fill_round(int x, int y, int w, int h, int r, uint32_t rgb) {
  round_rect(x, y, w, h, r, rgb, 255);
}

/* Contorno de 1px (sem preencher o miolo). */
static void round_frame(int x, int y, int w, int h, int r, uint32_t rgb) {
  (void)r;
  if (w < 2 || h < 2) return;
  fill_rect(x + 1, y, w - 2, 1, rgb);
  fill_rect(x + 1, y + h - 1, w - 2, 1, rgb);
  fill_rect(x, y + 1, 1, h - 2, rgb);
  fill_rect(x + w - 1, y + 1, 1, h - 2, rgb);
}

/* Recuo da linha j (0 = topo) no arco de raio r. */
static int round_inset(int j, int r) {
  int dy = r - j, d = 0;
  while (d < r && (r - d) * (r - d) + dy * dy > r * r) d++;
  return d;
}

/* Retangulo opaco so com os cantos de cima arredondados. */
static void round_top(int x, int y, int w, int h, int r, uint32_t rgb) {
  int j;
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r > h) r = h;
  for (j = 0; j < r; j++) {
    int d = round_inset(j, r);
    fill_rect(x + d, y + j, w - 2 * d, 1, rgb);
  }
  fill_rect(x, y + r, w, h - r, rgb);
}

/* Contorno de 1px que acompanha o round_top. */
static void round_top_frame(int x, int y, int w, int h, int r, uint32_t rgb) {
  int j, prev = 0;
  if (w < 2 || h < 2) return;
  if (r * 2 > w) r = w / 2;
  if (r > h) r = h;
  for (j = 0; j < r; j++) {
    int d = round_inset(j, r);
    /* no arco, a borda vai de d ate onde a linha de cima comecava */
    int len = j == 0 ? w - 2 * d : (prev - d > 1 ? prev - d : 1);
    if (j == 0) {
      fill_rect(x + d, y, len, 1, rgb);
    } else {
      fill_rect(x + d, y + j, len, 1, rgb);
      fill_rect(x + w - d - len, y + j, len, 1, rgb);
    }
    prev = d;
  }
  fill_rect(x, y + r, 1, h - r, rgb);
  fill_rect(x + w - 1, y + r, 1, h - r, rgb);
  fill_rect(x, y + h - 1, w, 1, rgb);
}

/* ---------------------------------------------------------------------------
 * Sombra das janelas: caixa desfocada, deslocada um pouco para baixo, bem
 * clara (estilo macOS). Ela escurece o que ja esta na tela (multiplica), entao
 * so pode ser desenhada uma vez sobre um fundo recem-pintado.
 * ------------------------------------------------------------------------- */
#define SH_R    18   /* raio do desfoque */
#define SH_DY   5    /* deslocamento para baixo */
#define SH_A    96   /* opacidade no miolo (0..255) */
#define SH_PL   SH_R
#define SH_PR   SH_R
#define SH_PT   (SH_R - SH_DY)
#define SH_PB   (SH_R + SH_DY)

static uint8_t sh_tab[2 * SH_R + 1];
static int sh_ready;

/* Perfil da borda: indice 0 = SH_R para dentro (cheio), 2*SH_R = fora (zero).
 * Smoothstep vezes (1 + s)/2: perto da borda ~metade, cauda longa e macia. */
static void sh_init(void) {
  int k;
  for (k = 0; k <= 2 * SH_R; k++) {
    int s = (2 * SH_R - k) * 1024 / (2 * SH_R);          /* 1024 = 1.0 */
    int v = s * s / 1024 * (3072 - 2 * s) / 1024;        /* smoothstep */
    v = v * (1024 + s) / 2048;
    sh_tab[k] = (uint8_t)(v * 255 / 1024);
  }
  sh_ready = 1;
}

/* Fator (0..255) na coordenada p para a caixa [b0,b1). */
static inline uint32_t sh_f(int p, int b0, int b1) {
  int t = b0 - p;
  if (p - (b1 - 1) > t) t = p - (b1 - 1);
  t += SH_R;
  if (t <= 0) return sh_tab[0];
  if (t >= 2 * SH_R) return 0;
  return sh_tab[t];
}

/* Escurece n pixels de row; a da coluna c = ay * sh_f(c) / 256. */
static void sh_span(volatile uint32_t* row, int c0, int c1, uint32_t ay, int bx0, int bx1) {
  int c;
  for (c = c0; c < c1; c++) {
    uint32_t a = (ay * sh_f(c, bx0, bx1)) >> 8;
    uint32_t k, p;
    if (!a) continue;
    k = 256u - a;
    p = row[c];
    row[c] = ((((p & 0xFF00FFu) * k) >> 8) & 0xFF00FFu) |
             ((((p & 0x00FF00u) * k) >> 8) & 0x00FF00u) | (p & 0xFF000000u);
  }
}

/* Alfa da linha r (ja vezes SH_A) para a janela em y com altura h. */
static inline uint32_t sh_row_a(int r, int y, int h) {
  return SH_A * sh_f(r, y + SH_DY, y + SH_DY + h) / 255u;
}

/* Sombra da janela (x,y,w,h), so dentro do retangulo de corte [cx0,cx1) x
 * [cy0,cy1). O miolo da janela (abaixo dos cantos redondos) fica de fora: ela
 * e pintada por cima. Linhas a partir de ylim (barra de tarefas) ficam sem. */
static void win_shadow(int x, int y, int w, int h, int cx0, int cy0, int cx1, int cy1,
                       int ylim) {
  int r, r0, r1, c0, c1;
  if (!gfx_bochs || w < 2 * UI_RADIUS || h < 2 * UI_RADIUS) return;
  if (!sh_ready) sh_init();
  /* maximizada (ocupa a tela): sombra nenhuma aparece */
  if (x <= 0 && y <= 0 && x + w >= (int)gfx_w) return;
  if (cx0 < 0) cx0 = 0;
  if (cy0 < 0) cy0 = 0;
  if (cx1 > (int)gfx_w) cx1 = (int)gfx_w;
  if (cy1 > (int)gfx_h) cy1 = (int)gfx_h;
  if (cy1 > ylim) cy1 = ylim;
  r0 = y - SH_PT > cy0 ? y - SH_PT : cy0;
  r1 = y + h + SH_PB < cy1 ? y + h + SH_PB : cy1;
  c0 = x - SH_PL > cx0 ? x - SH_PL : cx0;
  c1 = x + w + SH_PR < cx1 ? x + w + SH_PR : cx1;
  if (r0 >= r1 || c0 >= c1) return;
  for (r = r0; r < r1; r++) {
    volatile uint32_t* row = (volatile uint32_t*)gfx_at(0, r);
    uint32_t ay = sh_row_a(r, y, h);
    if (!ay) continue;
    if (r >= y + UI_RADIUS && r < y + h) {
      /* so as laterais: o meio e da janela */
      sh_span(row, c0, x < c1 ? (x > c0 ? x : c0) : c1, ay, x, x + w);
      sh_span(row, x + w > c0 ? (x + w < c1 ? x + w : c1) : c0, c1, ay, x, x + w);
    } else {
      sh_span(row, c0, c1, ay, x, x + w);
    }
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

/* A tela em memoria mora na RAM de 1 MB a ~10 MB. Um EXE do Windows sem
 * relocacoes precisa carregar em 0x400000, bem ali (kernel/wine.c). Enquanto
 * um deles roda, a tela em memoria vai para o heap alto (gfx_back_heap) e a
 * faixa baixa fica com o programa; quando ele sai, volta. Sem EXE desses,
 * nada muda e nada e alocado. Chamado com o BKL, pela tarefa do mundo. */
static uint8_t* gfx_back_heap;
static int gfx_low_claimed;
static void gfx_gpu_try(void);

/* Tela estendida (docs/GPU.md, etapa M; telas=estendida no SODOS.CFG): o
 * driver de placa mostra uma imagem 2x mais larga nas duas telas. O quadro
 * em memoria vai para o heap alto (3840x1080 = 16,6 MB nao cabe na faixa de
 * 1 MB) e gfx_w passa a ser a largura das duas. Ao suspender, parar o driver
 * ou sair do WIN, volta a uma tela (o driver volta o fb do GOP). */
static int gfx_wide;
static uint32_t gfx_wide_x2;          /* onde a segunda tela comeca */
static uint32_t gfx_uma_w, gfx_uma_pitch;
static volatile uint8_t* gfx_uma_lfb;

/* Volta as variaveis de uma tela. O quadro largo (gfx_back_heap) fica: quem
 * chama remonta o quadro (gfx_buf_init) ou usa o largo, que tambem serve. */
static void gfx_tela_uma(void) {
  if (!gfx_wide) return;
  gfx_wide = 0;
  gfx_wide_x2 = 0;
  gfx_w = gfx_uma_w;
  gfx_pitch_bytes = gfx_uma_pitch;
  gfx_lfb = gfx_uma_lfb;
  serial_puts("[visual] uma tela de novo\n");
}

/* Monitor de um ponto: x0 e largura da tela onde x cai. */
static void gfx_tela_de(int x, int* x0, int* w) {
  if (gfx_wide && x >= (int)gfx_wide_x2) {
    *x0 = (int)gfx_wide_x2;
    *w = (int)gfx_w - (int)gfx_wide_x2;
  } else {
    *x0 = 0;
    *w = gfx_wide ? (int)gfx_wide_x2 : (int)gfx_w;
  }
}

static uint32_t gfx_back_bytes(void) { return gfx_w * gfx_h * (gfx_bochs ? 4u : 1u); }

int gfx_back_move(int to_heap) {
  uint32_t bytes = gfx_back_bytes(), i;
  if (to_heap) {
    gfx_low_claimed = 1;
    if (gfx_back == (uint8_t*)GFX_BACK_PHYS && bytes) {
      uint8_t* p = (uint8_t*)umalloc_top(bytes);
      if (!p) {
        gfx_low_claimed = 0;
        return -1;
      }
      gpu_suspend();   /* o driver da placa guarda o ponteiro do back buffer */
      for (i = 0; i < bytes; i++) p[i] = gfx_back[i];
      gfx_back = p;
      gfx_back_heap = p;
      if (gpu_suspended()) gfx_gpu_try();
      serial_puts("[visual] tela em memoria movida para o heap (EXE de base fixa)\n");
    }
    return 0;
  }
  gfx_low_claimed = 0;
  if (gfx_back_heap && !gfx_wide) {
    if (gfx_back == gfx_back_heap) {
      gpu_suspend();
      for (i = 0; i < bytes; i++) ((uint8_t*)GFX_BACK_PHYS)[i] = gfx_back_heap[i];
      gfx_back = (uint8_t*)GFX_BACK_PHYS;
      if (gpu_suspended()) gfx_gpu_try();
    }
    ufree(gfx_back_heap);
    gfx_back_heap = 0;
    serial_puts("[visual] tela em memoria de volta a 1 MB\n");
  }
  return 0;
}

/* Desenha o quadro inteiro fora da tela e so depois troca a pagina visivel. */
static void gfx_buf_init(void) {
  gfx_tela_uma();
  gfx_draw_y0 = 0;
  gfx_front_y = 0;
  gfx_offscreen = 0;
  gfx_dbuf = 0;
  gfx_back = 0;
  if (gfx_back_heap) {
    ufree(gfx_back_heap);
    gfx_back_heap = 0;
  }
  if (gfx_bochs && !gfx_gop && !gfx_for_gpu) {
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
  if (gfx_low_claimed) {
    /* troca de modo com a faixa baixa cedida a um EXE do Windows */
    gfx_back_heap = (uint8_t*)umalloc_top(gfx_back_bytes());
    gfx_back = gfx_back_heap;
    return;
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

static void gfx_gpu_frame(void);   /* abaixo, depois do cursor */

static void gfx_frame_end(void) {
  if (gfx_dbuf) {
    bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)gfx_draw_y0);
    gfx_front_y = gfx_draw_y0;
    gfx_draw_y0 = gfx_front_y;
    return;
  }
  if (gfx_offscreen) {
    /* driver de placa: o unico teste dele por quadro (docs/GPU.md) */
    if (gpu_drv) {
      gfx_gpu_frame();
      return;
    }
    gfx_copy_back();
    gfx_offscreen = 0;
  }
}

/* Na troca de modo do WIN: se ha placa com driver (ou gpudrv= no
 * VIDEO.CFG), monta o quadro em RAM e tenta o driver. Se nao der, volta a
 * escolha de sempre. Sem placa conhecida, so gpu_wanted() roda. */
static void gfx_hwcur_on(void);   /* com o cursor, mais abaixo */

/* Com o driver ativo e telas=estendida: pede ao driver a imagem larga e
 * passa a desenhar nela. Qualquer falha deixa uma tela. */
static void gfx_tela_estende(void) {
  struct gpu_telas t;
  struct gpu_mode mm;
  uint8_t* nb;
  uint32_t bytes, y, i;
  if (gfx_wide || !gpu_drv || !gfx_back || !gpu_telas_cfg()) return;
  if (gpu_telas(GPU_TELAS_CONSULTA, &t, 0) != 0 || t.n < 2 || t.h != gfx_h || t.w <= gfx_w) return;
  bytes = t.w * t.h * 4u;
  nb = (uint8_t*)umalloc_top(bytes);
  if (!nb) {
    serial_puts("[visual] sem memoria para a tela estendida\n");
    return;
  }
  /* a tela de agora na esquerda, a direita preta (o WIN redesenha tudo) */
  for (y = 0; y < gfx_h; y++) {
    uint32_t* d = (uint32_t*)(nb + y * t.w * 4u);
    const uint32_t* o = (const uint32_t*)(gfx_back + y * gfx_w * 4u);
    for (i = 0; i < gfx_w; i++) d[i] = o[i];
    for (; i < t.w; i++) d[i] = 0;
  }
  mm.w = t.w;
  mm.h = t.h;
  mm.pitch = t.w * 4u;
  mm.back = nb;
  mm.fb = t.fb;
  mm.fb_pitch = t.fb_pitch;
  mm.pci_id = mm.pci_bus = mm.pci_dev = mm.pci_fun = 0;
  if (gpu_telas(GPU_TELAS_ESTENDE, &t, &mm) != 0) {
    ufree(nb);
    serial_puts("[visual] o driver nao estendeu a tela\n");
    return;
  }
  gfx_uma_w = gfx_w;
  gfx_uma_pitch = gfx_pitch_bytes;
  gfx_uma_lfb = gfx_lfb;
  if (gfx_back_heap) ufree(gfx_back_heap);
  gfx_back_heap = nb;
  gfx_back = nb;
  gfx_w = t.w;
  gfx_pitch_bytes = t.fb_pitch;
  gfx_lfb = t.fb;
  gfx_wide = 1;
  gfx_wide_x2 = t.x2;
  serial_puts("[visual] tela estendida nas duas telas\n");
}

static void gfx_gpu_try(void) {
  struct gpu_mode m;
  int volta = gpu_suspended();
  if (!gfx_bochs || (!volta && !gpu_wanted())) return;
  if (gfx_dbuf || !gfx_back) {
    gfx_for_gpu = 1;
    gfx_buf_init();
    gfx_for_gpu = 0;
  }
  if (gfx_back) {
    m.w = gfx_w;
    m.h = gfx_h;
    m.pitch = gfx_w * 4u;
    m.back = gfx_back;
    m.fb = gfx_lfb;
    m.fb_pitch = gfx_pitch_bytes;
    m.pci_id = m.pci_bus = m.pci_dev = m.pci_fun = 0;
    if (volta ? gpu_resume(&m) == 0 : gpu_start(&m) == 0) {
      gfx_tela_estende();
      gfx_hwcur_on();
      return;
    }
  } else {
    serial_puts("[gpu] sem quadro em RAM: driver nao entra\n");
    gpu_stop("sem quadro em RAM");
  }
  gfx_buf_init();
}

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

/* Texto sem pintar o fundo do glifo: para desenhar sobre vidro/papel. */
static void draw_char_t(int px, int py, char c, uint32_t fg, int scale) {
  const uint8_t* g = glyph16((unsigned char)c);
  int rows = glyph_rows();
  int i, x;
  if (scale < 1) scale = 1;
  for (i = 0; i < rows; i++) {
    uint8_t bits = g[i];
    for (x = 0; x < FONT_W; x++)
      if (bits & (uint8_t)(1 << (7 - x)))
        fill_rect(px + x * scale, py + i * scale, scale, scale, fg);
  }
}

static void draw_text_t(int px, int py, const char* s, uint32_t fg, int scale) {
  if (scale < 1) scale = 1;
  while (*s) {
    draw_char_t(px, py, *s++, fg, scale);
    px += FONT_W * scale;
  }
}

/* ---------------------------------------------------------------------------
 * Fonte TrueType dos titulos de janela e dos rotulos do desktop (wfont.c).
 * Ao abrir o WIN procura \SODOS\FONTS em D:, U: e nas outras unidades
 * montadas; sem o arquivo (ou em 8 bits) fica a fonte de bitmap de sempre.
 * ------------------------------------------------------------------------- */
#define WF_PX_BIG   16   /* tela >= 400 linhas (escala 2 da fonte de bitmap) */
#define WF_PX_SMALL 9
static const char* const wf_files[] = { "INTER.TTF", "ROBOTO.TTF", "NOTOSANS.TTF" };
static int wf_tried_px;   /* tamanho ja procurado: nao rele o disco a cada WIN */

static void* wf_alloc(uint32_t n) { return umalloc_top(n); }
static void wf_release(void* p) { ufree(p); }

static int wf_on(void) { return gfx_bochs && wfont_ready(); }

static void wf_init(int scale) {
  static const char order[] = "DUCEFGHIJKLMNOPQRSTVWXYZ";
  int want = scale > 1 ? WF_PX_BIG : WF_PX_SMALL;
  int d, f;
  if (!gfx_bochs || !um_ready()) return;
  if (wfont_ready() && wfont_px() == want) return;
  if (wf_tried_px == want) return;
  wf_tried_px = want;
  for (d = 0; order[d]; d++) {
    if (order[d] != 'U' && !drives_is_mounted(order[d])) continue;
    for (f = 0; f < (int)(sizeof(wf_files) / sizeof(wf_files[0])); f++) {
      static const char dir[] = "X:\\SODOS\\FONTS\\";
      char path[40];
      int i = 0, j, sz, rc;
      uint8_t* buf;
      for (j = 0; dir[j]; j++) path[i++] = dir[j];
      for (j = 0; wf_files[f][j]; j++) path[i++] = wf_files[f][j];
      path[i] = 0;
      path[0] = order[d];
      sz = file_size(path);
      if (sz < 1024 || sz > 8 * 1024 * 1024) continue;
      buf = (uint8_t*)umalloc_owned((uint32_t)sz + 1, 0);
      if (!buf) return;
      rc = file_load(path, (char*)buf, sz + 1) == sz
             ? wfont_build(buf, sz, want, wf_alloc, wf_release) : -9;
      ufree(buf);
      serial_puts("[wfont] ");
      serial_puts(path);
      serial_puts(rc == 0 ? " ok\n" : " falhou\n");
      if (rc == 0) return;
    }
  }
  serial_puts("[wfont] sem \\SODOS\\FONTS: titulos com a fonte de bitmap\n");
}

/* Texto com a fonte TrueType por cima do que ja esta na tela (cor PIXA, como
 * fill_rect). (x,y) = canto de cima da linha; nenhuma letra passa de x_lim
 * (0 = sem limite). Devolve a largura desenhada. */
static int wf_text(int x, int y, const char* s, uint32_t color, int x_lim) {
  uint32_t c = pix32(color);
  uint32_t cr = (c >> 16) & 0xFFu, cg = (c >> 8) & 0xFFu, cb = c & 0xFFu;
  int pen = x * 256, base = y + wfont_ascent();
  int xr = (int)gfx_w;
  if (x_lim > 0 && x_lim < xr) xr = x_lim;
  for (; *s; s++) {
    const struct wfont_glyph* g = wfont_glyph((unsigned char)*s);
    int gx = ((pen + 128) >> 8) + g->left, gy = base - g->top, i, j;
    if (x_lim > 0 && ((pen + g->adv + 128) >> 8) > x_lim) break;
    for (j = 0; g->a && j < g->h; j++) {
      int yy = gy + j;
      volatile uint32_t* row;
      const uint8_t* al = g->a + j * g->w;
      if (yy < 0 || yy >= (int)gfx_h) continue;
      row = (volatile uint32_t*)gfx_at(0, yy);
      for (i = 0; i < g->w; i++) {
        int xx = gx + i;
        uint32_t a = al[i], na, d;
        if (!a || xx < 0 || xx >= xr) continue;
        if (a == 255) {
          row[xx] = c;
          continue;
        }
        na = 255u - a;
        d = row[xx];
        row[xx] = (c & 0xFF000000u) |
                  (((cr * a + ((d >> 16) & 0xFFu) * na + 127u) / 255u) << 16) |
                  (((cg * a + ((d >> 8) & 0xFFu) * na + 127u) / 255u) << 8) |
                  ((cb * a + (d & 0xFFu) * na + 127u) / 255u);
      }
    }
    pen += g->adv;
  }
  return ((pen + 128) >> 8) - x;
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

/* 1 se o 8042 aceita um byte; 0 se ficou ocupado (~100 ms) ou nao existe.
 * Sem limite, um PC sem 8042 (0x64 = 0xFF) ou com o buffer travado prendia o
 * WIN aqui para sempre, antes de desenhar a tela. */
static int ps2_wait_write(void) {
  uint32_t t0 = net_ms(), spins = 0;
  for (;;) {
    uint8_t st = inb(0x64);
    if (st == 0xFF) return 0;
    if (!(st & 2)) return 1;
    if (net_has_clock() ? (net_ms() - t0) >= 100u : ++spins > 2000000u) return 0;
  }
}

/* 1 se um mouse PS/2 (de verdade ou emulado pelo firmware) respondeu 0xFA ao F4.
 * Sem porta auxiliar (notebooks atuais), o F4 vai ao teclado ou some. */
static int ps2_mouse_init(void) {
  uint32_t t0, spins = 0;
  int ack = 0;
  if (inb(0x64) == 0xFF) return 0;   /* sem controlador */
  if (!ps2_wait_write()) return 0;
  outb(0x64, 0xA8);
  for (volatile int i = 0; i < 10000; i++) { }
  if (!ps2_wait_write()) return 0;
  outb(0x64, 0xD4);
  if (!ps2_wait_write()) return 0;
  outb(0x60, 0xF4);
  t0 = net_ms();
  for (;;) {
    uint8_t st = inb(0x64);
    /* O limite vale tambem com dado pendente: 0xFF ou um buffer que nunca
     * esvazia nao podem segurar o laco. */
    if (net_has_clock() ? (net_ms() - t0) >= 50u : ++spins > 2000000u) break;
    if (st == 0xFF) break;
    if (st & 1) {
      uint8_t b = inb(0x60);
      if ((st & 0x20) && b == 0xFA) ack = 1;
      continue;
    }
    if (ack) break;
  }
  return ack;
}

/* Alguns controladores deixam a porta 1 (teclado) desativada apos comandos ao auxiliar. */
static void ps2_enable_keyboard_port(void) {
  if (!ps2_wait_write()) return;
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
static void gfx_hwcur_move(int x, int y);   /* abaixo */

static void cursor_redraw_at(int mx, int my, uint32_t cursor_color) {
  int scale = cur_scale();
  /* cursor da placa (docs/GPU.md, etapa 4): fica fora do quadro */
  if (gpu_hwcur) {
    gfx_hwcur_move(mx, my);
    return;
  }
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

/* ---------------------------------------------------------------------------
 * Areas alteradas para o driver de placa (docs/GPU.md, etapa 3). So sao
 * gravadas com driver ativo; sem ele nada aqui roda alem dos testes listados
 * na tabela de custo. Com driver, gfx_offscreen fica 1 entre os quadros: o
 * back buffer e a tela, e o que se desenha fora de um quadro (cursor,
 * arrasto) vai para ele e e apresentado so naquele retangulo.
 * ------------------------------------------------------------------------- */
#define GFX_DMG_MAX 10
static struct gpu_rect gfx_dmg[GFX_DMG_MAX];
static int gfx_dmg_n, gfx_dmg_over;
/* 1 = o quadro em montagem so mudou nas areas de gfx_dmg (need_draw == 2) */
static int gfx_frame_dmg;
/* cursor na tela pela ultima apresentacao */
static struct gpu_rect gfx_cur_shown;

static void gfx_dmg_add(int x, int y, int w, int h) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > (int)gfx_w) w = (int)gfx_w - x;
  if (y + h > (int)gfx_h) h = (int)gfx_h - y;
  if (w <= 0 || h <= 0) return;
  if (gfx_dmg_n >= GFX_DMG_MAX) {
    gfx_dmg_over = 1;
    return;
  }
  gfx_dmg[gfx_dmg_n].x = x;
  gfx_dmg[gfx_dmg_n].y = y;
  gfx_dmg[gfx_dmg_n].w = w;
  gfx_dmg[gfx_dmg_n].h = h;
  gfx_dmg_n++;
}

/* Mostra o back buffer pela placa; se o driver cair, pela CPU. */
static int win_api_dirty;   /* definida mais abaixo */

static void gfx_gpu_show(const struct gpu_rect* r, int n) {
  if (gpu_present(r, n) != 0) {
    if (gfx_wide) {
      /* o fini do driver ja voltou o fb do GOP; o proximo quadro e inteiro */
      gfx_buf_init();
      win_api_dirty = 1;
      return;
    }
    gfx_copy_back();
    gfx_offscreen = 0;
  }
}

static void gfx_cur_mark(void) {
  gfx_cur_shown.x = old_mx;
  gfx_cur_shown.y = old_my;
  gfx_cur_shown.w = cursor_saved ? cur_bw : 0;
  gfx_cur_shown.h = cursor_saved ? cur_bh : 0;
}

static void gfx_gpu_frame(void) {
  const struct gpu_rect* r = 0;
  int n = 0;
  if (gfx_frame_dmg && !gfx_dmg_over && gfx_dmg_n > 0) {
    /* o quadro redesenha o cursor onde o mouse esta: as duas posicoes */
    gfx_dmg_add(gfx_cur_shown.x, gfx_cur_shown.y, gfx_cur_shown.w, gfx_cur_shown.h);
    if (cursor_saved) gfx_dmg_add(old_mx, old_my, cur_bw, cur_bh);
    if (!gfx_dmg_over) {
      r = gfx_dmg;
      n = gfx_dmg_n;
    }
  }
  gfx_cur_mark();
  gfx_dmg_n = 0;
  gfx_dmg_over = 0;
  gfx_frame_dmg = 0;
  gfx_gpu_show(r, n);
}

/* definidas mais abaixo (posicao do mouse e pedido de redesenho do WIN) */
static int32_t sys_mx, sys_my;
static int win_api_dirty;

/* Cursor da placa: so a posicao. Se o driver cair, o quadro volta a CPU e o
 * proximo quadro inteiro ja desenha o cursor por software. */
static void gfx_hwcur_move(int x, int y) {
  if (gpu_cursor_move(x, y) != 0) {
    if (gfx_wide) {
      gfx_buf_init();
      win_api_dirty = 1;
      return;
    }
    gfx_copy_back();
    gfx_offscreen = 0;
    win_api_dirty = 1;
  }
}

/* Entrega a seta (cur_row, na escala da tela) ao driver, se ele tem cursor. */
static void gfx_hwcur_on(void) {
  int scale = cur_scale(), w = CUR_W * scale, h = CUR_H * scale, x, y;
  uint32_t* img;
  if (!gpu_drv || !(gpu_drv->caps & GPU_CAP_CURSOR) || w > 64 || h > 64) return;
  img = (uint32_t*)umalloc_owned((uint32_t)(w * h) * 4u, 0);
  if (!img) return;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      char c = cur_row[y / scale][x / scale];
      img[y * w + x] = c == 'W' ? 0xFFFFFFFFu : c == 'B' ? 0xFF000000u : 0u;
    }
  gpu_cursor_set(img, w, h, 0, 0);
  ufree(img);
  if (gpu_hwcur) {
    /* a seta saiu do quadro: o proximo quadro inteiro vem sem ela */
    cursor_saved = 0;
    gfx_cur_shown.w = gfx_cur_shown.h = 0;
    gfx_hwcur_move((int)sys_mx, (int)sys_my);
    win_api_dirty = 1;
  }
}

static void cursor_draw(int mx, int my, uint32_t cursor_color) {
  struct gpu_rect r[2];
  if (gpu_hwcur) {
    gfx_hwcur_move(mx, my);
    return;
  }
  cursor_restore();
  cursor_redraw_at(mx, my, cursor_color);
  /* com driver o cursor foi para o back buffer: mostra so as duas caixas */
  if (gpu_drv && gfx_offscreen) {
    r[0] = gfx_cur_shown;
    gfx_cur_mark();
    r[1] = gfx_cur_shown;
    gfx_gpu_show(r[0].w > 0 ? r : r + 1, r[0].w > 0 ? 2 : 1);
  }
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
  /* Uma tela do DOS rodando ao fundo do WIN nao esta no WIN. */
  if (!gfx_live || !gfx_w || !gfx_h || vt_bg()) return 0;
  if (w) *w = (int)gfx_w;
  if (h) *h = (int)gfx_h;
  return 1;
}

/* Tela principal (a primeira, com a tela estendida): o que o Windows chama de
 * SM_CXSCREEN. Programas do Windows dimensionam e centralizam a janela por ela;
 * com 3840 o Claude (Electron) nao abria janela no PC de duas telas. */
int win_primary_size(int* w, int* h) {
  if (!win_screen_size(w, h)) return 0;
  if (gfx_wide && gfx_wide_x2 && w) *w = (int)gfx_wide_x2;
  return 1;
}

/* vga_bios_font e a fonte em uso: a saida do modo grafico e o fbcon leem dela. */
const uint8_t* vga_font_glyph(int ch) {
  return vga_bios_font_ok ? &vga_bios_font[ch * 32] : font8x16_cp850[ch];
}

/* A saida da plane 2 sublinha o cursor: k_cursor pode te-lo escondido. */
static void vga_font_load(int ch) {
  volatile uint8_t* vga = (volatile uint8_t*)0xA0000;
  uint8_t start;
  int i;
  if (!vga_legacy() || gfx_live) return;
  outb(0x3D4, 0x0A);
  start = inb(0x3D5);
  if (ch < 0) {
    vga_restore_bios_font();
  } else {
    vga_font_plane_enter();
    for (i = 0; i < 16; i++) vga[ch * 32 + i] = vga_bios_font[ch * 32 + i];
    vga_font_plane_leave();
  }
  outb(0x3D4, 0x0A);
  outb(0x3D5, start);
}

/* Modo do controlador de atributos, como em vga_mode_text. O fbcon sempre usa
 * o fundo claro: nada a fazer no UEFI. */
static int text_blink = 1;

int vga_text_blink_get(void) { return text_blink; }

void vga_text_blink(int on) {
  if (!vga_legacy() || gfx_live) return;
  text_blink = on ? 1 : 0;
  (void)inb(0x3DA);
  outb(0x3C0, 0x30);
  outb(0x3C0, on ? 0x0C : 0x04);
}

int vga_font_get(int ch, uint8_t* glyph) {
  const uint8_t* g;
  int i;
  if (ch < 0 || ch > 255 || !glyph) return -1;
  g = vga_font_glyph(ch);
  for (i = 0; i < 16; i++) glyph[i] = g[i];
  return 0;
}

int vga_font_set(int ch, const uint8_t* glyph) {
  int i;
  if (ch < 0 || ch > 255 || !glyph || gfx_live || !vga_bios_font_ok) return -1;
  for (i = 0; i < 16; i++) vga_bios_font[ch * 32 + i] = glyph[i];
  vga_font_load(ch);
  fbcon_font_changed(ch);
  return 0;
}

void vga_font_reset(void) {
  font_load_builtin();
  vga_font_load(-1);
  fbcon_font_changed(-1);
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

/* 1 enquanto uma transferencia de controle usa a fila (tablet_take nao
 * religa o tablet por cima dela) */
static volatile int usb_ctrl_busy;
static int usb_ctrl_raw(uint8_t addr, uint8_t rtype, uint8_t req,
                        uint16_t val, uint16_t idx, int len, int din);
static int usb_ctrl(uint8_t addr, uint8_t rtype, uint8_t req,
                    uint16_t val, uint16_t idx, int len, int din) {
  int r;
  usb_ctrl_busy = 1;
  r = usb_ctrl_raw(addr, rtype, req, val, idx, len, din);
  usb_ctrl_busy = 0;
  return r;
}
static int usb_ctrl_raw(uint8_t addr, uint8_t rtype, uint8_t req,
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
static void lat_note(uint32_t now);   /* medida da latencia, mais abaixo */
static void in_note(uint32_t t);      /* idem, com o tick de chegada da IRQ */

static int tablet_take(void) {
  struct uhci_td* td = tablet_td;
  uint32_t sts;
  int n, old_x, old_y, old_btn, was, z;
  if (!mouse_abs_mode || !td) return 0;
  sts = td->sts;
  __asm__ volatile ("" ::: "memory");
  if (sts & (1u << 23)) {
    /* TD do tablet ativo mas fora da fila: uma transferencia de controle
     * (usb_ctrl) usou a mesma QH e, no fim, a controladora deixou elem = T.
     * O mouse ficava parado para sempre (VM do Claude: captcha sem mouse). */
    if (uhci_q && !usb_ctrl_busy && (uhci_q->elem & ~0xFu) != ((uint32_t)(uintptr_t)td & ~0xFu))
      uhci_q->elem = (uint32_t)(uintptr_t)td;
    return 0;
  }
  lat_note((uint32_t)irq_ticks());
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
  uint8_t data;
  *abs_pos = 0;
  {
    /* A placa de rede no maximo uma vez por tick (1 ms): cada volta do laco
     * passa por aqui, e no QEMU (NE2000 por portas de I/O) o nic_poll era
     * mais da metade do tempo do WIN. Cada chamada esvazia ate 64 quadros. */
    static uint32_t net_t;
    uint32_t now = (uint32_t)irq_ticks();
    if (now != net_t) {
      net_t = now;
      net_poll();
    }
  }
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
  {
    /* Bytes ja tirados do 8042 pela IRQ1/IRQ12 (kernel/ps2in.c), com o tick
     * de chegada. Teclado primeiro: uma tecla nunca espera o mouse, e um
     * byte de teclado nao entra mais no meio de um pacote do mouse. */
    uint32_t t;
    if (ps2in_kbd(&data, &t)) {
      in_note(t);
      *sc = data;
      return 0;
    }
    /* Pacote inteiro de uma vez se os 3 bytes ja chegaram. */
    while (ps2in_aux(&data, &t)) {
      in_note(t);
      if (mouse_abs_mode) continue;          /* tablet USB manda; PS/2 e eco */
      if (*pkt_idx > 0) {
        packet[(*pkt_idx)++] = data;
        if (*pkt_idx < 3) continue;
        *pkt_idx = 0;
        return 1;
      }
      if (data & 0x08) {                     /* bit 3: primeiro byte */
        packet[0] = data;
        *pkt_idx = 1;
      }
      /* sem o bit 3: fora de sincronia, descarta ate achar o inicio */
    }
    if (*pkt_idx > 0) return -1;             /* resto do pacote ainda vem */
  }
  if (mouse_abs_mode && tablet_take()) {
    packet[0] = tablet_btn;
    packet[1] = 0;
    packet[2] = 0;
    *abs_pos = 1;
    return 1;
  }
  {
    /* Mouse USB no xHCI (PC de verdade): relativo, ja aplicado aqui como o tablet. */
    int dx, dy, dz, b;
    if (xhci_mouse_poll(&dx, &dy, &dz, &b)) {
      sys_mouse_place(sys_mx + dx, sys_my + dy);
      dz = -dz;
      if (dz > 3) dz = 3;
      if (dz < -3) dz = -3;
      tablet_dz += dz;
      packet[0] = (uint8_t)b;
      packet[1] = 0;
      packet[2] = 0;
      *abs_pos = 1;
      return 1;
    }
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

  gfx_desk_clear();

  fill_rect(12, 12, icon, icon, col_win);
  draw_string_scaled(12, 12 + icon + 2, "DOS", col_white, col_desk, scale);
  fill_rect(12, icon2_y, icon, icon, col_title);
  draw_string_scaled(12, icon2_y + icon + 2, "ARQ", col_white, col_desk, scale);

  fill_rect(win_x, win_y, win_w, win_h, col_win);
  fill_rect(win_x, win_y, win_w, title_h, col_title);
  draw_string_scaled(win_x + 4, title_ty, "Gerenciador", col_white, col_title, scale);
  fill_rect(win_x + win_w - (title_h - 2), win_y + 2, title_h - 4, title_h - 4, col_btn);
  draw_string_scaled(win_x + win_w - (title_h - 2) + 2, title_ty, "x", col_black, col_btn, scale);
  draw_string_scaled(win_x + 8, win_y + title_h + 6, sodos_version(), col_black, col_win, scale);

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

#define WM_MAX 24
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
  int auto_close;   /* RUN terminou com 0: o laco do WIN fecha a janela */
};

/* z maior fica na frente. gui_focus >= 0 quando a janela da frente e um .EXE. */
static int zseq;
static int gui_focus = -1;
static int gui_stole;
static int win_api_dirty;
static int run_close_pending;   /* alguma janela RUN marcada com auto_close */

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
static int note_caret;    /* cursor do texto: indice em note_buf */
static int note_anc = -1; /* inicio da marcacao; -1 = nada marcado */
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
  note_caret = note_len;
  note_anc = -1;
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

/* Colunas por linha do texto: a mesma conta de quebra de note_text_lines. */
static int note_cols(struct wm_win* w, int scale) {
  int cw = FONT_W * scale;
  int n = cw > 0 ? (w->w - 14) / cw : 1;
  return n < 1 ? 1 : n;
}

/* Linha (sem rolagem) e coluna do cursor posto antes de note_buf[idx]. */
static void note_rc(struct wm_win* w, int scale, int idx, int* r, int* c) {
  int cols = note_cols(w, scale);
  int line = 0, col = 0, i;
  for (i = 0; i < idx && i < note_len; i++) {
    if (note_buf[i] == '\n') {
      line++;
      col = 0;
      continue;
    }
    if (col >= cols) {
      line++;
      col = 0;
    }
    col++;
  }
  if (col >= cols && idx < note_len && note_buf[idx] != '\n') {
    line++;
    col = 0;
  }
  *r = line;
  *c = col;
}

/* Indice mais perto da linha tl, coluna tc. */
static int note_at_rc(struct wm_win* w, int scale, int tl, int tc) {
  int cols = note_cols(w, scale);
  int line = 0, col = 0, i, best = -1;
  if (tl < 0) return 0;
  for (i = 0; i <= note_len; i++) {
    int r = line, c = col;
    if (c >= cols && i < note_len && note_buf[i] != '\n') {
      r++;
      c = 0;
    }
    if (r > tl) break;
    if (r == tl && c <= tc) best = i;
    if (i == note_len) break;
    if (note_buf[i] == '\n') {
      line++;
      col = 0;
    } else {
      if (col >= cols) {
        line++;
        col = 0;
      }
      col++;
    }
  }
  return best < 0 ? note_len : best;
}

/* Indice do texto sob o ponto (mx, my) da tela; top = barra de titulo + menu. */
static int note_hit(struct wm_win* w, int top, int scale, int mx, int my) {
  int cw = FONT_W * scale;
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int dy = my - (w->y + top + 4);
  int tl = note_scroll + (dy < 0 ? -1 : dy / ch);
  int tc = (mx - (w->x + 6) + cw / 2) / cw;
  if (tl < note_scroll) tl = note_scroll;
  if (tc < 0) tc = 0;
  return note_at_rc(w, scale, tl, tc);
}

/* Caractere sob o mouse (sem arredondar para a borda mais perto). */
static int note_dbl_pos(struct wm_win* w, int top, int scale, int mx, int my) {
  return note_hit(w, top, scale, mx - FONT_W * scale / 2, my);
}

/* Rola so o necessario para o cursor aparecer. */
static void note_follow_caret(struct wm_win* w, int top, int scale) {
  int r, c;
  int vis = note_vis_lines(w, top, scale);
  note_rc(w, scale, note_caret, &r, &c);
  if (r < note_scroll) note_scroll = r;
  if (r >= note_scroll + vis) note_scroll = r - vis + 1;
  note_scroll_by(w, top, scale, 0);
}

/* Trecho marcado [*a, *b); 0 se nada esta marcado. */
static int note_sel(int* a, int* b) {
  if (note_anc < 0 || note_anc > note_len || note_anc == note_caret) return 0;
  *a = note_anc < note_caret ? note_anc : note_caret;
  *b = note_anc < note_caret ? note_caret : note_anc;
  return 1;
}

/* Apaga o trecho marcado; 0 se nao havia. */
static int note_del_sel(void) {
  int a, b, i;
  if (!note_sel(&a, &b)) {
    note_anc = -1;
    return 0;
  }
  for (i = b; i <= note_len; i++) note_buf[a + i - b] = note_buf[i];
  note_len -= b - a;
  note_caret = a;
  note_anc = -1;
  note_dirty = 1;
  return 1;
}

/* Poe n bytes de s no cursor (o que nao cabe no buffer fica de fora). */
static void note_insert(const char* s, int n) {
  int room = (int)sizeof(note_buf) - 1 - note_len;
  int i;
  if (n > room) n = room;
  if (n <= 0) return;
  for (i = note_len; i >= note_caret; i--) note_buf[i + n] = note_buf[i];
  for (i = 0; i < n; i++) note_buf[note_caret + i] = s[i] == '\r' ? '\n' : s[i];
  note_len += n;
  note_caret += n;
  note_dirty = 1;
}

/* Setas, Home e End (scancode sem E0). Com Shift a marcacao acompanha. */
static void note_move(struct wm_win* w, int scale, uint8_t sc, int shift) {
  int r, c;
  if (shift) {
    if (note_anc < 0) note_anc = note_caret;
  } else {
    note_anc = -1;
  }
  note_rc(w, scale, note_caret, &r, &c);
  if (sc == 0x4B && note_caret > 0) note_caret--;
  else if (sc == 0x4D && note_caret < note_len) note_caret++;
  else if (sc == 0x48) note_caret = r > 0 ? note_at_rc(w, scale, r - 1, c) : 0;
  else if (sc == 0x50) note_caret = note_at_rc(w, scale, r + 1, c);
  else if (sc == 0x47) note_caret = note_at_rc(w, scale, r, 0);
  else if (sc == 0x4F) note_caret = note_at_rc(w, scale, r, 0x7FFF);
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
  int sa = 0, sb = 0;
  int cr, cc;
  note_scroll_by(w, title_h, scale, 0);
  skip = note_scroll;
  note_sel(&sa, &sb);
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
    if (line >= skip) {
      /* trecho marcado em video invertido */
      if (i >= sa && i < sb) draw_char_scaled(x, y, c, bg, fg, scale);
      else draw_char_scaled(x, y, c, fg, bg, scale);
    }
    x += cw;
  }
  /* cursor: barra fina antes do caractere */
  note_rc(w, scale, note_caret, &cr, &cc);
  cr -= skip;
  if (cr >= 0 && cy + (cr + 1) * ch <= y1) {
    int px = cx + cc * cw;
    if (px > x1 - 2) px = x1 - 2;
    fill_rect(px, cy + cr * ch, scale > 1 ? 2 : 1, ch, fg);
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
/* Linhas guardadas (a roda do mouse e PgUp/PgDn rolam por elas). */
#define CMD_ROWS 400
#define CMD_COLS 80
#define CMD_IN 128

/* Tela de texto de um programa que roda no Command: k_vput, k_cursor e k_getc
 * dele vem para ca (kexp_set_textscr). Pertence a tarefa; a janela so mostra.
 * E 80x25 ate o programa perguntar k_textsize: dai a grade segue o tamanho da
 * janela (cols x rows) e cada mudanca chega como a tecla KEY_RESIZE. As celulas
 * ficam sempre com o passo TTY_MC, para mudar de tamanho sem copiar nada. */
#define TTY_KQ 64
#define TTY_MC 256
#define TTY_MR 96
#define TCELL(y, r, k) ((y)->cell[(r) * TTY_MC + (k)])
struct con_tty {
  uint16_t cell[TTY_MR * TTY_MC];
  int cols, rows; /* tamanho que o programa ve */
  int dyn;        /* perguntou k_textsize: a grade acompanha a janela */
  int wcols, wrows; /* quanto cabe na janela agora (o desenho mede) */
  int on;      /* o programa usou a tela de celulas: a janela mostra a grade */
  int fit;     /* a janela ainda nao cresceu para caber 80x25 */
  int r, c;    /* cursor (onde k_putc escreve) */
  int hide;
  int font_on; /* o programa trocou glifos: font vale */
  uint8_t font[256 * 16];
  int kq[TTY_KQ];
  int kr, kn;
  int gone;    /* a janela fechou: k_getc devolve -1 */
};

struct cmd_con {
  struct con_tty* tty; /* programa rodando aqui (da tarefa), ou 0 */
  char line[CMD_ROWS][CMD_COLS];
  int n;
  char in[CMD_IN];
  int inlen;
  int ocol;
  int busy; /* um .EXE digitado aqui ainda roda: sem prompt */
  int hist; /* posicao no historico: 0 = linha nova, 1 = comando mais recente */
  int cur;  /* cursor na linha digitada */
  /* Marcacao de (ar, ac) ate (br, bc): linha virtual (0..n-1 = line[], n = a do
   * prompt) e coluna; na linha do prompt a coluna conta o prompt e depois in[]. */
  int sel;
  int ar, ac, br, bc;
  int view; /* linhas roladas para cima (roda/PgUp); 0 = o fim, com o prompt */
};

static struct cmd_con* cons_cap;
static int cons_painting;
static void (*cons_paint)(void);
static void cons_wipe(struct cmd_con* c);
static void cmd_push(struct cmd_con* c, const char* s);
static int in_task(void);
static int tty_emit(struct cmd_con* c, char ch);
static struct cmd_con* cons_dirty; /* Command com saida nova de uma tarefa */
static int cons_late;              /* saida que ainda nao foi para a tela */
static uint64_t cons_last;

#define CONS_GAP 30000000u   /* ciclos entre redesenhos: as linhas saem em rajada */

static uint64_t cons_tsc(void) {
  uint32_t a, d;
  __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
  return ((uint64_t)d << 32) | a;
}

/* Mostra o que ficou para tras. O kernel chama antes de esperar uma tecla. */
static void cons_flush(void) {
  if (!cons_late || !cons_paint || cons_painting) return;
  cons_late = 0;
  cons_paint();
  cons_last = cons_tsc();
}

/* Numa tarefa o desktop redesenha quando ela ceder o turno. */
static void cons_refresh(void) {
  if (in_task()) {
    if (cons_dirty && cons_dirty != cons_cap) win_api_dirty = 1;
    cons_dirty = cons_cap;
    return;
  }
  cons_late = 1;
  if (cons_tsc() - cons_last >= CONS_GAP) cons_flush();
}

static void cons_emit(char ch) {
  struct cmd_con* c = cons_cap;
  if (!c) return;
  if (tty_emit(c, ch)) return;
  if (ch == '\f') {
    int busy = c->busy;
    struct con_tty* tty = c->tty;
    cons_wipe(c);
    c->busy = busy;
    c->tty = tty;
    cons_refresh();
    return;
  }
  if (ch == '\b') {
    /* eco do k_readline: apaga o ultimo caractere da linha */
    if (c->ocol > 0 && c->n > 0) {
      c->ocol--;
      c->line[c->n - 1][c->ocol] = '\0';
      cons_refresh();
    }
    return;
  }
  if (ch == '\r') return;
  if (ch == '\n') {
    if (c->ocol == 0) cmd_push(c, "");
    c->ocol = 0;
    cons_refresh();
    return;
  }
  if ((unsigned char)ch < 32 || (unsigned char)ch == 127) return;
  if (c->ocol == 0) cmd_push(c, "");
  if (c->n < 1) return;
  if (c->ocol >= CMD_COLS - 1) {
    c->ocol = 0;
    cmd_push(c, "");
  }
  c->line[c->n - 1][c->ocol++] = ch;
  c->line[c->n - 1][c->ocol] = '\0';
  /* Numa tarefa so marca: um prompt sem \n aparece quando ela ceder a vez. */
  if (in_task()) cons_refresh();
}

static void cons_wipe(struct cmd_con* c) {
  unsigned char* p = (unsigned char*)c;
  int i;
  for (i = 0; i < (int)sizeof(*c); i++) p[i] = 0;
}

static void cmd_push(struct cmd_con* c, const char* s) {
  int row, i;
  if (c->n >= CMD_ROWS) {
    /* sobe tudo uma linha (rep movsq: com 400 linhas o laco por byte pesa) */
    void* d = c->line[0];
    const void* src = c->line[1];
    uint64_t q = (uint64_t)(CMD_ROWS - 1) * CMD_COLS / 8u;
    __asm__ volatile ("rep movsq" : "+D"(d), "+S"(src), "+c"(q) : : "memory");
    c->n = CMD_ROWS - 1;
  } else if (c->view) {
    c->view++; /* quem esta lendo la em cima nao ve o texto andar */
  }
  c->sel = 0; /* as linhas mudam: a marcacao nao vale mais */
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

static int desk_refresh(void);
static int cmd_spawn(const char* path, int argc, char** argv);
static int cmd_spawn_screen(int (*cmd)(int, char**), int argc, char** argv);

/* Troca a linha digitada e sai da navegacao no historico (ESC, Enter). */
static void cmd_set_input(struct cmd_con* c, const char* s) {
  int i;
  if (!c) return;
  for (i = 0; i < CMD_IN - 1 && s[i]; i++) c->in[i] = s[i];
  c->in[i] = '\0';
  c->inlen = i;
  c->cur = i;
  c->sel = 0;
  c->hist = 0;
}

/* Setas: dir 1 = mais antigo, -1 = mais novo. O historico e o mesmo do prompt
 * do DOS, gravado por shell_exec_line. */
static void cmd_hist(struct wm_win* w, int dir) {
  struct cmd_con* c = (struct cmd_con*)w->st;
  int n;
  if (!c || c->busy) return;
  n = c->hist + dir;
  if (n < 0 || n > history_count) return;
  cmd_set_input(c, n ? history[history_count - n] : "");
  c->hist = n;
}

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
  cmd_set_input(c, "");
  c->ocol = 0;
  if (buf[0]) {
    cons_cap = c;
    shell_set_out(cons_emit);
    shell_set_exec(cmd_spawn);
    shell_set_screen_exec(cmd_spawn_screen);
    shell_set_flush(cons_flush);
    shell_exec_line(buf);
    shell_set_flush(0);
    shell_set_screen_exec(0);
    shell_set_exec(0);
    shell_set_out(0);
    cons_cap = 0;
    /* Com cons_cap, cons_paint refaz so esta janela. O desktop so se mudou. */
    if (!desk_refresh()) cons_cap = c;
  } else {
    cons_cap = c;
  }
  cons_late = 0;
  if (cons_paint && !cons_painting) cons_paint();
  cons_cap = 0;
  return 0;
}

static int cmd_plen(void) {
  char prompt[48];
  int n = 0;
  drives_prompt(prompt, (int)sizeof(prompt));
  while (prompt[n]) n++;
  return n;
}

/* Texto da linha virtual r (n = prompt + linha digitada). */
static int cmd_row_text(struct cmd_con* c, int r, char* out, int max) {
  int n = 0, k;
  if (max < 1) return 0;
  if (r >= 0 && r < c->n) {
    for (k = 0; c->line[r][k] && n < max - 1; k++) out[n++] = c->line[r][k];
  } else if (r == c->n) {
    drives_prompt(out, max);
    while (out[n]) n++;
    for (k = 0; k < c->inlen && n < max - 1; k++) out[n++] = c->in[k];
  }
  out[n] = '\0';
  return n;
}

/* Marcacao em ordem: (*r0, *c0) antes de (*r1, *c1). 0 se vazia. */
static int cmd_sel_span(struct cmd_con* c, int* r0, int* c0, int* r1, int* c1) {
  int fwd;
  if (!c->sel) return 0;
  fwd = c->ar < c->br || (c->ar == c->br && c->ac <= c->bc);
  *r0 = fwd ? c->ar : c->br;
  *c0 = fwd ? c->ac : c->bc;
  *r1 = fwd ? c->br : c->ar;
  *c1 = fwd ? c->bc : c->ac;
  return *r0 != *r1 || *c0 != *c1;
}

/* Ctrl+C: o trecho marcado vai para a area de transferencia do kernel. */
static void cmd_clip_copy(struct cmd_con* c) {
  static char out[CLIP_MAX];
  char row[CMD_COLS + 48 + CMD_IN];
  int r0, c0, r1, c1, r, n = 0;
  if (!cmd_sel_span(c, &r0, &c0, &r1, &c1)) return;
  for (r = r0; r <= r1; r++) {
    int len = cmd_row_text(c, r, row, (int)sizeof(row));
    int a = r == r0 ? c0 : 0;
    int b = r == r1 ? c1 : len;
    if (b > len) b = len;
    while (a < b && n < CLIP_MAX - 2) out[n++] = row[a++];
    if (r < r1) out[n++] = '\n';
  }
  clip_set(out, n);
}

/* Apaga o que esta marcado dentro da linha digitada; 1 se apagou. */
static int cmd_del_sel(struct cmd_con* c) {
  int r0, c0, r1, c1, plen, a, b, i;
  if (!cmd_sel_span(c, &r0, &c0, &r1, &c1) || r0 != c->n || r1 != c->n) {
    c->sel = 0;
    return 0;
  }
  plen = cmd_plen();
  a = c0 - plen;
  b = c1 - plen;
  c->sel = 0;
  if (a < 0) a = 0;
  if (b > c->inlen) b = c->inlen;
  if (b <= a) return 0;
  for (i = b; i <= c->inlen; i++) c->in[a + i - b] = c->in[i];
  c->inlen -= b - a;
  c->cur = a;
  return 1;
}

static void cmd_insert(struct cmd_con* c, const char* s, int n) {
  int i;
  if (c->cur > c->inlen) c->cur = c->inlen;
  if (n > CMD_IN - 1 - c->inlen) n = CMD_IN - 1 - c->inlen;
  if (n <= 0) return;
  for (i = c->inlen; i >= c->cur; i--) c->in[i + n] = c->in[i];
  for (i = 0; i < n; i++) c->in[c->cur + i] = s[i];
  c->inlen += n;
  c->cur += n;
}

/* Ctrl+V: cola na linha digitada; quebras de linha viram espaco. */
static void cmd_paste(struct cmd_con* c) {
  static char t[CLIP_MAX];
  int n = clip_get(t, (int)sizeof(t));
  int k, m = 0;
  if (c->busy) return;
  cmd_del_sel(c);
  while (n > 0 && (t[n - 1] == '\n' || t[n - 1] == '\r')) n--;
  for (k = 0; k < n; k++) {
    char ch = t[k];
    if (ch == '\n' || ch == '\t') ch = ' ';
    if ((unsigned char)ch < 32 || (unsigned char)ch == 127) continue;
    t[m++] = ch;
  }
  cmd_insert(c, t, m);
  c->hist = 0;
}

/* Setas, Home e End (sem E0) na linha digitada; com Shift marcam. */
static void cmd_move(struct cmd_con* c, uint8_t sc, int shift) {
  int plen;
  if (c->busy) return;
  c->view = 0;
  plen = cmd_plen();
  if (shift && !(c->sel && c->ar == c->n)) {
    c->sel = 1;
    c->ar = c->n;
    c->ac = plen + c->cur;
  }
  if (sc == 0x4B && c->cur > 0) c->cur--;
  else if (sc == 0x4D && c->cur < c->inlen) c->cur++;
  else if (sc == 0x47) c->cur = 0;
  else if (sc == 0x4F) c->cur = c->inlen;
  if (shift) {
    c->br = c->n;
    c->bc = plen + c->cur;
  } else {
    c->sel = 0;
  }
}

/* Tab: completa o que esta antes do cursor (comando, arquivo, pasta), como no
 * prompt do DOS. Com varios candidatos a linha vai para o log e a lista embaixo. */
static char tab_out[2048];
static int tab_outn;
static void tab_cap(char ch) {
  if (tab_outn < (int)sizeof(tab_out) - 1) tab_out[tab_outn++] = ch;
}

static void cmd_tab(struct cmd_con* c) {
  char buf[CMD_IN];
  char tail[CMD_IN];
  char shown[CMD_COLS];
  char prompt[48];
  int len, tl, i, plen = 0;
  if (c->cur > c->inlen) c->cur = c->inlen;
  len = c->cur;
  tl = c->inlen - c->cur;
  for (i = 0; i < len; i++) buf[i] = c->in[i];
  buf[len] = '\0';
  for (i = 0; i < tl; i++) tail[i] = c->in[c->cur + i];
  tail[tl] = '\0';
  tab_outn = 0;
  shell_set_out(tab_cap);
  shell_complete(buf, &len, CMD_IN - tl);
  shell_set_out(0);
  tab_out[tab_outn] = '\0';
  if (tab_outn > 0) {
    /* varios: mostra a linha como estava e a lista, igual ao DOS */
    drives_prompt(prompt, (int)sizeof(prompt));
    while (prompt[plen]) plen++;
    for (i = 0; i < plen && i < CMD_COLS - 1; i++) shown[i] = prompt[i];
    for (i = 0; i < c->inlen && plen + i < CMD_COLS - 1; i++) shown[plen + i] = c->in[i];
    shown[plen + i] = '\0';
    cmd_push(c, shown);
    {
      struct cmd_con* saved = cons_cap;
      cons_cap = c;
      c->ocol = 0;
      for (i = 0; i < tab_outn; i++) cons_emit(tab_out[i]);
      if (c->ocol) cons_emit('\n');
      c->ocol = 0;
      cons_cap = saved;
    }
  }
  if (len + tl > CMD_IN - 1) return;
  for (i = 0; i < len; i++) c->in[i] = buf[i];
  for (i = 0; i < tl; i++) c->in[len + i] = tail[i];
  c->inlen = len + tl;
  c->in[c->inlen] = '\0';
  c->cur = len;
  c->sel = 0;
}

/* Acento que nao combina (~ e x): o keymap devolve o ~ e guarda o x em
 * keymap_pending(). No WIN o x vira uma tecla a mais no proximo giro do laco
 * (win_pend). Antes ele sumia aqui e reaparecia depois no prompt do DOS. */
static int win_pend, win_syn;
static int win_keychar(uint8_t sc) {
  int c = keymap_char(sc), p = keymap_pending();
  if (p) win_pend = p;
  return c;
}

static int cmd_key(struct wm_win* w, char ch) {
  struct cmd_con* c = (struct cmd_con*)w->st;
  if (!c || c->busy) return 0;
  c->view = 0; /* digitar volta para o prompt */
  if (ch == '\t') {
    cmd_tab(c);
    return 0;
  }
  c->hist = 0; /* editou a linha: as setas recomecam do mais recente */
  if (ch == '\b' || ch == 0x7f) {
    /* com algo marcado na linha, apaga a marcacao; senao o anterior (Del: o seguinte) */
    if (cmd_del_sel(c)) return 0;
    if (ch == 0x7f) {
      if (c->cur < c->inlen) {
        c->cur++;
        ch = '\b';
      } else {
        return 0;
      }
    }
    if (c->cur > 0) {
      int i;
      for (i = c->cur - 1; i < c->inlen; i++) c->in[i] = c->in[i + 1];
      c->inlen--;
      c->cur--;
    }
    return 0;
  }
  if (ch == '\n' || ch == '\r') return cmd_enter(w);
  /* ASCII e o CP850 de 0x80 em diante (ç é ã...): a fonte do WIN e CP850 */
  if ((unsigned char)ch >= 32 && (unsigned char)ch != 127) {
    cmd_del_sel(c);
    cmd_insert(c, &ch, 1);
  }
  return 0;
}

/* Onde cada coisa fica na janela do Command (desenho e mouse usam a mesma conta). */
struct cmd_geo {
  int ih, y0, x0, cell;
  int start;      /* primeira linha de line[] mostrada */
  int prompt_row; /* linha da tela com o prompt */
  int maxc;       /* colunas que cabem */
  int plen;       /* tamanho do prompt */
  int off;        /* primeiro caractere de in[] mostrado */
  int vis;        /* linhas que cabem */
};

static void cmd_geom(struct wm_win* w, struct cmd_con* c, int title_h, int scale,
                     struct cmd_geo* g) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int vis, room;
  g->ih = ch;
  g->y0 = w->y + title_h;
  g->x0 = w->x + 6;
  vis = (w->h - title_h) / ch;
  if (vis < 1) vis = 1;
  g->vis = vis;
  /* Nasce no topo. Desce com o texto. So fica no rodape quando a janela enche.
   * Rolado (view > 0), o prompt fica abaixo da janela (prompt_row >= vis). */
  if (c->n + 1 <= vis) {
    c->view = 0;
    g->start = 0;
    g->prompt_row = c->n;
  } else {
    int maxv = c->n - (vis - 1);
    if (c->view > maxv) c->view = maxv;
    if (c->view < 0) c->view = 0;
    g->start = c->n - (vis - 1) - c->view;
    g->prompt_row = vis - 1 + c->view;
  }
  g->cell = FONT_W * scale;
  g->maxc = g->cell > 0 ? (w->w - 12) / g->cell : 40;
  if (g->maxc < 8) g->maxc = 8;
  if (g->maxc > CMD_COLS - 1) g->maxc = CMD_COLS - 1;
  g->plen = cmd_plen();
  room = g->maxc - g->plen;
  if (room < 1) room = 1;
  g->off = c->inlen > room ? c->inlen - room : 0;
  if (c->cur < g->off) g->off = c->cur;
}

/* Rola o texto do Command: dz > 0 desce (para o prompt), < 0 sobe. */
static void cmd_scroll(struct cmd_con* c, int dz) {
  if (!c || (c->tty && c->tty->on)) return;
  c->view -= dz;
  if (c->view < 0) c->view = 0;
  if (c->view > c->n) c->view = c->n; /* cmd_geom corta pelo tamanho da janela */
}

/* Linha virtual e coluna sob o ponto (mx, my). */
/* Letra de palavra para o duplo clique: letras, digitos, _ e acentos. */
static int word_ch(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
         ch == '_' || ch >= 128;
}

/* Palavra em s[0..n) que contem k (ou termina em k): [*a, *b). 0 se nao ha. */
static int word_at(const char* s, int n, int k, int* a, int* b) {
  if (k > n) k = n;
  if (k >= n || !word_ch((unsigned char)s[k])) {
    if (k > 0 && word_ch((unsigned char)s[k - 1])) k--;
    else return 0;
  }
  *a = k;
  *b = k;
  while (*a > 0 && word_ch((unsigned char)s[*a - 1])) (*a)--;
  while (*b < n && word_ch((unsigned char)s[*b])) (*b)++;
  return 1;
}

/* Dois cliques na mesma janela, perto e em ate 450 ms. O terceiro clique
 * recomeca a contagem. */
static uint32_t dbl_t;
static int dbl_w = -1, dbl_x, dbl_y;

static int dbl_click(int win, int x, int y) {
  uint32_t now = (uint32_t)irq_ticks();
  int dx = x - dbl_x, dy = y - dbl_y;
  int hit = win == dbl_w && now - dbl_t < 450u && dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4;
  dbl_t = now;
  dbl_w = hit ? -1 : win;
  dbl_x = x;
  dbl_y = y;
  return hit;
}

/* Duplo clique no Command: marca a palavra da linha r sob a coluna k. */
static void cmd_word(struct cmd_con* c, int r, int k) {
  char row[CMD_COLS + 48 + CMD_IN];
  int len = cmd_row_text(c, r, row, (int)sizeof(row));
  int a, b;
  if (!word_at(row, len, k, &a, &b)) return;
  c->sel = 1;
  c->ar = c->br = r;
  c->ac = a;
  c->bc = b;
  if (r == c->n && !c->busy) {
    int plen = cmd_plen();
    if (b >= plen) c->cur = b - plen;
  }
}

static void cmd_hit(struct wm_win* w, struct cmd_con* c, int title_h, int scale, int mx, int my,
                    int* r, int* col) {
  struct cmd_geo g;
  int li, k;
  cmd_geom(w, c, title_h, scale, &g);
  li = my < g.y0 ? 0 : (my - g.y0) / g.ih;
  k = (mx - g.x0 + g.cell / 2) / (g.cell > 0 ? g.cell : 1);
  if (k < 0) k = 0;
  if (li >= g.prompt_row) {
    *r = c->n;
    if (k > g.plen) k += g.off;
    if (k > g.plen + c->inlen) k = g.plen + c->inlen;
  } else {
    *r = g.start + li;
  }
  *col = k;
}

/* Como cmd_hit, mas a coluna e a da celula sob o mouse. */
static void cmd_hit_cell(struct wm_win* w, struct cmd_con* c, int title_h, int scale, int mx,
                         int my, int* r, int* col) {
  struct cmd_geo g;
  cmd_geom(w, c, title_h, scale, &g);
  cmd_hit(w, c, title_h, scale, mx - g.cell / 2, my, r, col);
}

/* Cores da VGA em modo texto. */
static const uint32_t tty_pal[16] = {
  0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
  0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static void tty_push(struct con_tty* y, int k);

/* A janela mudou de tamanho: a grade de quem perguntou k_textsize muda junto.
 * O que aparece de novo vem em branco; o programa redesenha com KEY_RESIZE. */
static void tty_fit_window(struct con_tty* y, int ncol, int nrow) {
  int r, k;
  if (ncol > TTY_MC) ncol = TTY_MC;
  if (nrow > TTY_MR) nrow = TTY_MR;
  if (ncol < 20) ncol = 20;
  if (nrow < 5) nrow = 5;
  y->wcols = ncol;
  y->wrows = nrow;
  if (!y->dyn || y->fit || (ncol == y->cols && nrow == y->rows)) return;
  for (r = 0; r < nrow; r++)
    for (k = (r < y->rows ? y->cols : 0); k < ncol; k++) TCELL(y, r, k) = 0x0720;
  y->cols = ncol;
  y->rows = nrow;
  if (y->r >= nrow) y->r = nrow - 1;
  if (y->c >= ncol) y->c = ncol - 1;
  tty_push(y, KEY_RESIZE);
}

/* Grade do programa. O bit 7 do atributo e fundo claro (sem piscar). */
static void tty_draw(struct wm_win* w, struct con_tty* y, int focus, int title_h, int scale) {
  int rows = glyph_rows();
  int cw = FONT_W * (scale < 1 ? 1 : scale);
  int chh = rows * (scale < 1 ? 1 : scale);
  int x0 = w->x + 6, y0 = w->y + title_h + 2;
  int ncol = (w->w - 12) / cw, nrow = (w->h - title_h - 4) / chh;
  int r, k, i, x;
  uint32_t pal[16];
  if (scale < 1) scale = 1;
  tty_fit_window(y, ncol, nrow);
  if (ncol > y->cols) ncol = y->cols;
  if (nrow > y->rows) nrow = y->rows;
  for (i = 0; i < 16; i++) pal[i] = gui_col(tty_pal[i]);
  fill_rect(w->x, w->y + title_h, w->w, w->h - title_h, pal[0]);
  for (r = 0; r < nrow; r++) {
    for (k = 0; k < ncol; k++) {
      uint16_t v = TCELL(y, r, k);
      unsigned char chr = (unsigned char)(v & 0xFF);
      uint32_t fg = pal[(v >> 8) & 15], bg = pal[(v >> 12) & 15];
      const uint8_t* g = y->font_on ? &y->font[chr * 16] : glyph16(chr);
      int px = x0 + k * cw, py = y0 + r * chh;
      for (i = 0; i < rows; i++) {
        uint8_t bits = g[i];
        if (scale == 1) {
          for (x = 0; x < FONT_W; x++)
            put_pixel(px + x, py + i, (bits & (uint8_t)(0x80 >> x)) ? fg : bg);
        } else {
          for (x = 0; x < FONT_W; x++)
            fill_rect(px + x * scale, py + i * scale, scale, scale,
                      (bits & (uint8_t)(0x80 >> x)) ? fg : bg);
        }
      }
    }
  }
  if (focus && !y->hide && y->r >= 0 && y->r < nrow && y->c >= 0 && y->c < ncol) {
    uint16_t v = TCELL(y, y->r, y->c);
    fill_rect(x0 + y->c * cw, y0 + y->r * chh + chh - 2 * scale, cw, 2 * scale,
              pal[(v >> 8) & 15] == pal[(v >> 12) & 15] ? pal[7] : pal[(v >> 8) & 15]);
  }
}

static void cmd_draw(struct wm_win* w, int focus, int title_h, int scale,
                     uint32_t col_white, uint32_t col_black) {
  struct cmd_con* c = (struct cmd_con*)w->st;
  struct cmd_geo g;
  char row[CMD_COLS + 48];
  int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
  int has;
  int li, k, last;
  if (c->tty && c->tty->on) {
    tty_draw(w, c->tty, focus, title_h, scale);
    return;
  }
  has = cmd_sel_span(c, &r0, &c0, &r1, &c1);
  cmd_geom(w, c, title_h, scale, &g);
  fill_rect(w->x, g.y0, w->w, w->h - title_h, col_black);
  last = c->busy ? g.prompt_row - 1 : g.prompt_row;
  if (last > g.vis - 1) last = g.vis - 1;
  for (li = 0; li <= last; li++) {
    int r = li == g.prompt_row ? c->n : g.start + li;
    int len, py = g.y0 + li * g.ih;
    if (r > c->n || (r == c->n && li != g.prompt_row)) break;
    len = cmd_row_text(c, r, row, (int)sizeof(row));
    for (k = 0; k < g.maxc; k++) {
      /* na linha do prompt, a parte de in[] antes de off nao aparece */
      int src = (r == c->n && k >= g.plen) ? k + g.off : k;
      int on = has && (r > r0 || (r == r0 && src >= c0)) && (r < r1 || (r == r1 && src < c1));
      int px = g.x0 + k * g.cell;
      char chr = src < len ? row[src] : ' ';
      if (src >= len && !on) break;
      if (on) draw_char_scaled(px, py, chr, col_black, col_white, scale);
      else draw_char_scaled(px, py, chr, col_white, col_black, scale);
    }
    if (r == c->n && focus) {
      int px = g.x0 + (g.plen + c->cur - g.off) * g.cell;
      fill_rect(px, py, g.cell > 0 ? g.cell : FONT_W, g.ih > 2 ? g.ih - 2 : g.ih, col_white);
      if (c->cur < c->inlen)
        draw_char_scaled(px, py, c->in[c->cur], col_black, col_white, scale);
    }
  }
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

static void title_max_btn(int x, int y, int s, int maxed);
static int wm_is_max(struct wm_win* w);
/* X e maximizar ficam TB_PAD pixels longe da borda direita: o canto de cima
 * a direita fica livre para redimensionar. */
#define TB_PAD 10

/* ---- redimensionar por qualquer canto ---------------------------------- */
#define RS_L 1
#define RS_R 2
#define RS_T 4
#define RS_B 8
static int rs_code, rs_mx0, rs_my0, rs_x0, rs_y0, rs_w0, rs_h0;

/* Canto (RS_L|RS_T, ...) sob o rato, ou 0. k = tamanho da zona do canto. */
static int win_corner(int mx, int my, int x, int y, int w, int h, int k) {
  int c = 0;
  if (mx < x || my < y || mx >= x + w || my >= y + h) return 0;
  if (mx < x + k) c |= RS_L;
  else if (mx >= x + w - k) c |= RS_R;
  if (my < y + k) c |= RS_T;
  else if (my >= y + h - k) c |= RS_B;
  /* so cantos: precisa de um lado horizontal e um vertical */
  if (!(c & (RS_L | RS_R)) || !(c & (RS_T | RS_B))) return 0;
  return c;
}

static void rs_begin(int code, int mx, int my, int x, int y, int w, int h) {
  rs_code = code;
  rs_mx0 = mx;
  rs_my0 = my;
  rs_x0 = x;
  rs_y0 = y;
  rs_w0 = w;
  rs_h0 = h;
}

/* Novo retangulo: o canto agarrado segue o rato, o oposto fica parado. */
static void rs_apply(int mx, int my, int min_w, int min_h, int max_x, int max_y,
                     int* x, int* y, int* w, int* h) {
  int dx = mx - rs_mx0, dy = my - rs_my0;
  int x0 = rs_x0, y0 = rs_y0, x1 = rs_x0 + rs_w0, y1 = rs_y0 + rs_h0;
  if (rs_code & RS_L) {
    x0 += dx;
    if (x0 > x1 - min_w) x0 = x1 - min_w;
    if (x0 < 0) x0 = 0;
  }
  if (rs_code & RS_R) {
    x1 += dx;
    if (x1 < x0 + min_w) x1 = x0 + min_w;
    if (x1 > max_x) x1 = max_x;
  }
  if (rs_code & RS_T) {
    y0 += dy;
    if (y0 > y1 - min_h) y0 = y1 - min_h;
    if (y0 < 0) y0 = 0;
  }
  if (rs_code & RS_B) {
    y1 += dy;
    if (y1 < y0 + min_h) y1 = y0 + min_h;
    if (y1 > max_y) y1 = max_y;
  }
  *x = x0;
  *y = y0;
  *w = x1 - x0;
  *h = y1 - y0;
}

/* Gerenciador: versao do kernel (build, data, commit) e as CPUs. */
static void mgr_draw_info(struct wm_win* w, int title_h, int scale, uint32_t fg, uint32_t bg) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int y = w->y + title_h + 8;
  int n = smp_cpu_count(), on = 0, busy = 0, i;
  char b[64];
  int k = 0;
  struct k_cpu_stat st;
  for (i = 0; i < n; i++)
    if (smp_cpu_stat(i, &st) == 0 && st.online) {
      on++;
      if (st.state) busy++;
    }
  draw_string_scaled(w->x + 8, y, sodos_version(), fg, bg, scale);
  y += ch + 2;
  {
    const char* p = "CPUs online: ";
    while (*p) b[k++] = *p++;
    b[k++] = (char)('0' + on / 10 % 10);
    if (b[k - 1] == '0') k--;
    b[k++] = (char)('0' + on % 10);
    b[k++] = '/';
    if (n >= 10) b[k++] = (char)('0' + n / 10 % 10);
    b[k++] = (char)('0' + n % 10);
    p = "  ativas agora: ";
    while (*p) b[k++] = *p++;
    if (busy >= 10) b[k++] = (char)('0' + busy / 10 % 10);
    b[k++] = (char)('0' + busy % 10);
    b[k] = 0;
  }
  draw_string_scaled(w->x + 8, y, b, fg, bg, scale);
  y += ch + 2;
  draw_string_scaled(w->x + 8, y, "Arraste a barra. Canto = tamanho.", fg, bg, scale);
  y += ch + 2;
  draw_string_scaled(w->x + 8, y, "X fecha. Iniciar: Bloco de notas.", fg, bg, scale);
}

static void wm_draw_inner(struct wm_win* w, int focus, int title_h, int scale,
                          uint32_t col_win, uint32_t col_title, uint32_t col_white,
                          uint32_t col_black, uint32_t col_btn, uint32_t col_inactive) {
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int close_s = title_h - 4;
  int close_x = w->x + w->w - close_s - TB_PAD;
  uint32_t tc = focus ? col_title : col_inactive;
  int ty = w->y + (title_h - ch) / 2;
  /* barra de titulo de vidro, cantos de cima arredondados; nos cantos aparece
     o que esta atras da janela */
  round_top(w->x, w->y, w->w, title_h, UI_RADIUS,
            gui_col(focus ? UI_TITLE_A : UI_TITLE_I));
  fill_rect(w->x, w->y + title_h, w->w, w->h - title_h, col_win);
  if (wf_on())
    wf_text(w->x + 12, w->y + (title_h - wfont_height()) / 2, w->title, gui_col(UI_TTEXT),
            close_x - close_s - 10);
  else
    draw_text_t(w->x + 12, ty, w->title, gui_col(UI_TTEXT), scale);
  fill_round(close_x, w->y + 3, close_s, close_s, ui_th.modern ? close_s / 2 : 5,
             gui_col(ui_th.close));
  draw_text_t(close_x + (close_s > FONT_W * scale ? (close_s - FONT_W * scale) / 2 : 0),
              ty, "x", gui_col(ui_th.modern ? 0xFFFFFFu : UI_TEXT), scale);
  title_max_btn(close_x - close_s - 4, w->y + 3, close_s, wm_is_max(w));
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
    cmd_draw(w, focus, title_h, scale, col_white, col_black);
  } else if (w->kind == WM_KIND_DISP) {
    disp_draw(w, title_h, scale, disp_hot, col_win, col_title, col_white, col_black);
  } else if (w->kind == WM_KIND_FONT) {
    font_draw(w, title_h, scale, col_win, col_title, col_white, col_black);
  } else {
    mgr_draw_info(w, title_h, scale, col_black, col_win);
  }
}

static void wm_draw_window(struct wm_win* w, int focus, int title_h, int scale,
                           uint32_t col_win, uint32_t col_title, uint32_t col_white,
                           uint32_t col_black, uint32_t col_btn, uint32_t col_inactive) {
  wm_draw_inner(w, focus, title_h, scale, col_win, col_title, col_white, col_black, col_btn,
                col_inactive);
  round_top_frame(w->x, w->y, w->w, w->h, UI_RADIUS, gui_col(UI_BORDER));
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
    if (!wins[i].used) {
      /* vaga de uma janela RUN que fechou sem passar pelo laco do auto_close:
       * a marca velha fecharia a janela nova no proximo programa que acabar */
      wins[i].auto_close = 0;
      return i;
    }
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
  /* tela estendida: no maximo a largura de uma tela */
  int limit_w = (gfx_wide ? (int)gfx_wide_x2 : (int)gfx_w) - 8;
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
  wins[i].auto_close = 0;
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
  if (rc == 0) {   /* terminou bem: a janela some sozinha */
    w->auto_close = 1;
    run_close_pending = 1;
    win_api_dirty = 1;
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
#define GUI_MAX 16
#define GUI_Q 64   /* com entrada crua cada tecla sao 2+ bytes */
#define TASK_MAX 4096 /* Claude (WINE): Electron + claude-code abrem 17+ processos em paralelo */
/* Tarefas: a tabela, a pilha, a tela de texto e o SSE/AVX salvo saem da arena
 * alta (umalloc_top). A do kernel (kmalloc) tem so 2 MB e acabava com ~20
 * programas abertos ("Sem vaga para outro programa", 2026-11-22). As voltas
 * pela tabela vao so ate task_n (a maior vaga ja usada + 1). */
static int task_n;
#define task_mem(n) umalloc_top((uint32_t)(n))
#define task_free(p) ufree(p)
#define TASK_STACK (48 * 1024)
#define TASK_ARGC 16
#define TASK_ARGBUF 192
#define WAKE_NONE  0
#define WAKE_BUSY  1
#define WAKE_POLL  2
#define WAKE_BLOCK 3
#define WAKE_DEAD  4
/* Esperando *wait_addr mudar (futex do mundo) ou o prazo vencer: nao pede
 * CPU, o laco do WIN pode fazer hlt (gui_coop_wait). */
#define WAKE_WAIT  5

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
  int raw;      /* k_gui_raw: scancodes e movimento crus (emuladores) */
  int raw_btn;  /* botoes apertados dentro da janela (captura do arrasto) */
  int clip;     /* k_gui_clip: o programa trata Ctrl+V (recebe 22) */
  int maxed;    /* maximizada pelo botao/duplo clique no titulo */
  int panel;    /* k_gui_panel: sem titulo nem moldura (WINTAB) */
  uint32_t pid; /* programa que abriu: as threads dele pintam aqui (gui_thread_enter) */
};

static int (*task_pending_cmd)(int, char**); /* proximo task_spawn roda isto */

/* Estado do shell (shell_out, exec, flush, pipe) de um lado da troca. */
struct task_shell {
  struct con_state cs;
  int (*scr)(int (*)(int, char**), int, char**);
};

struct gui_task {
  int used;
  int wake;
  int gid;
  int argc;
  uint64_t rsp;
  struct mod_cpu cpu;
  struct wm_win* run;
  int logout;          /* SET PRODUCTION=1: sem janela, a saida vai ao log (Ctrl+F12) */
  struct cmd_con* con; /* aberto pelo Command: escreve la, nao numa janela propria */
  struct con_tty* tty; /* tela de texto e teclas do programa do Command */
  int (*cmd)(int, char**); /* comando interno (EDIT) em vez de .EXE */
  struct task_shell sh;    /* shell da tarefa entre uma fatia e outra */
  int sh_ok;
  int con_row, con_col;
  uint8_t* stack;
  uint8_t* xs_raw;     /* SSE/AVX salvos na preempcao (kmalloc, alinhado em uso) */
  uint32_t poll_t;     /* tick+1 do ultimo k_trygetc vazio (0 = nenhum): ver gui_coop_pause */
  volatile int* wait_addr; /* WAKE_WAIT: volta quando *wait_addr != wait_val */
  int wait_val;
  uint32_t wait_until;     /* ... ou quando irq_ticks passar disto */
  char path[80];
  char args[TASK_ARGBUF]; /* argc textos seguidos, cada um com o seu NUL */
};

static struct gui_win* gwins;
static struct gui_task* gtasks;
static int gui_ix;
static int gui_allowed;
static int gui_painting;
static int task_cur = -1;
/* tarefa do WIN rodando agora (-1 = o proprio desktop ou fora do WIN): a
 * captura de saida do kernel (k_exec_capture) fica presa a ela */
int win_task_current(void) { return task_cur; }
/* Tarefa do WIN em execucao (-1 = o laco do WIN / o mundo). kexports.c usa
 * para saber quem esta dentro do sistema de arquivos. */
int gui_task_cur(void) { return task_cur; }
static int host_on;
static uint64_t host_rsp;
static struct mod_cpu host_cpu;
static int task_rr;
#define gui (gwins[gui_ix])

static int in_task(void) {
  return host_on && task_cur >= 0;
}

/* ---------- tela de texto do programa do Command ---------- */
static void gui_yield(int wake);

void mod_exit(int code);

/* Fechar o Command encerra o programa dele, como Ctrl+C no DOS: sem janela
 * ele ficaria rodando para ninguem (TOP em laco de k_trygetc, por exemplo). */
static struct con_tty* tty_cur(void) {
  struct con_tty* y;
  if (!in_task() || kt_in_thread()) return 0;
  y = gtasks[task_cur].tty;
  if (y && y->gone && !gtasks[task_cur].cmd) mod_exit(1);
  return y;
}

static void tty_blank(struct con_tty* y) {
  int i;
  for (i = 0; i < TTY_MR * TTY_MC; i++) y->cell[i] = 0x0720;
  y->r = y->c = 0;
}

/* Primeira vez: a grade comeca com o que o Command mostrava, como no DOS. */
static void tty_start(struct con_tty* y) {
  struct cmd_con* c = gtasks[task_cur].con;
  int i, n = 0, first;
  if (y->on) return;
  y->on = 1;
  y->fit = 1;
  y->hide = 0;
  if (!y->dyn) {
    y->cols = 80;
    y->rows = 25;
  }
  tty_blank(y);
  if (!c) return;
  n = c->n < y->rows - 1 ? c->n : y->rows - 1;
  first = c->n - n;
  for (i = 0; i < n; i++) {
    const char* s = c->line[first + i];
    int k;
    for (k = 0; k < y->cols && s[k]; k++) TCELL(y, i, k) = (uint16_t)(0x0700 | (unsigned char)s[k]);
  }
  if (c->ocol > 0 && n > 0) {
    y->r = n - 1;
    y->c = c->ocol < y->cols ? c->ocol : y->cols - 1;
  } else {
    y->r = n < y->rows ? n : y->rows - 1;
  }
}

static void tty_scroll(struct con_tty* y) {
  int r, k;
  for (r = 0; r + 1 < y->rows; r++)
    for (k = 0; k < y->cols; k++) TCELL(y, r, k) = TCELL(y, r + 1, k);
  for (k = 0; k < y->cols; k++) TCELL(y, y->rows - 1, k) = 0x0720;
}

/* k_putc com a grade ligada: escreve no cursor, como o console do DOS. */
static int tty_emit(struct cmd_con* c, char ch) {
  struct con_tty* y = c->tty;
  unsigned char u = (unsigned char)ch;
  if (!y || !y->on) return 0;
  if (u == '\f') {
    tty_blank(y);
  } else if (u == '\r') {
    y->c = 0;
  } else if (u == '\n') {
    y->c = 0;
    y->r++;
  } else if (u == '\b') {
    if (y->c > 0) y->c--;
  } else if (u == '\t') {
    y->c = (y->c + 8) & ~7;
  } else if (u >= 32) {
    if (y->r < 0) y->r = 0;
    if (y->c < 0) y->c = 0;
    TCELL(y, y->r, y->c) = (uint16_t)(0x0700 | u);
    y->c++;
  }
  if (y->c >= y->cols) {
    y->c = 0;
    y->r++;
  }
  while (y->r >= y->rows) {
    tty_scroll(y);
    y->r--;
  }
  cons_refresh();
  return 1;
}

static int tty_vput(int row, int col, const uint16_t* cells, int n) {
  struct con_tty* y = tty_cur();
  int pos, i;
  if (!y) return -1;
  tty_start(y);
  if (!cells || row < 0 || row >= y->rows || col < 0 || col >= y->cols) return 0;
  pos = row * y->cols + col;
  if (n > y->cols * y->rows - pos) n = y->cols * y->rows - pos;
  for (i = 0; i < n; i++, pos++) TCELL(y, pos / y->cols, pos % y->cols) = cells[i];
  if (n > 0) cons_refresh();
  return 0;
}

static void tty_cursor(int row, int col) {
  struct con_tty* y = tty_cur();
  if (!y) return;
  if (row < 0 || row >= y->rows || col < 0 || col >= y->cols) {
    y->hide = 1;
  } else {
    tty_start(y);
    y->hide = 0;
    y->r = row;
    y->c = col;
  }
  if (y->on) cons_refresh();
}

static void tty_clear(void) {
  struct con_tty* y = tty_cur();
  if (!y) return;
  if (!y->on) {
    cons_emit('\f');
    return;
  }
  tty_blank(y);
  cons_refresh();
}

static int tty_font_get(int ch, uint8_t* glyph) {
  struct con_tty* y = tty_cur();
  const uint8_t* g;
  int i;
  if (ch < 0 || ch > 255 || !glyph) return -1;
  g = (y && y->font_on) ? &y->font[ch * 16] : vga_font_glyph(ch);
  for (i = 0; i < 16; i++) glyph[i] = g[i];
  return 0;
}

/* Glifos trocados valem so nesta janela; o desktop segue com a fonte dele. */
static int tty_font_set(int ch, const uint8_t* glyph) {
  struct con_tty* y = tty_cur();
  int i, k;
  if (!y || ch < 0 || ch > 255 || !glyph || !vga_bios_font_ok) return -1;
  if (!y->font_on) {
    for (k = 0; k < 256; k++)
      for (i = 0; i < 16; i++) y->font[k * 16 + i] = vga_bios_font[k * 32 + i];
    y->font_on = 1;
  }
  for (i = 0; i < 16; i++) y->font[ch * 16 + i] = glyph[i];
  if (y->on) cons_refresh();
  return 0;
}

static void tty_font_reset(void) {
  struct con_tty* y = tty_cur();
  if (!y) return;
  y->font_on = 0;
  if (y->on) cons_refresh();
}

/* Tecla da fila que o WIN enche. Vazia: cede a vez ao desktop (que desenha). */
static int tty_key(int wait) {
  for (;;) {
    struct con_tty* y = tty_cur();
    int k;
    if (!y) return -1;
    if (y->kn > 0) {
      k = y->kq[y->kr];
      y->kr = (y->kr + 1) % TTY_KQ;
      y->kn--;
      gtasks[task_cur].poll_t = 0;
      return k;
    }
    if (y->gone) return -1;
    gui_yield(WAKE_POLL);
    if (!wait) {
      y = tty_cur();
      if (!y || y->kn == 0) {
        /* sem tecla: um k_yield logo em seguida e espera, nao calculo */
        gtasks[task_cur].poll_t = (uint32_t)irq_ticks() + 1u;
        return (y && !y->gone) ? 0 : -1;
      }
    }
  }
}

/* k_textsize: quem pergunta passa a ter a grade do tamanho da janela. Antes de
 * a janela crescer para o programa (fit) vale 80x25, como no DOS. */
static void tty_size(int* cols, int* rows) {
  struct con_tty* y = tty_cur();
  if (!y) return;
  if (!y->dyn) {
    y->dyn = 1;
    if (!y->on) {
      y->cols = 80;
      y->rows = 25;
    }
    if (!y->fit && y->wcols > 0 && (y->wcols != y->cols || y->wrows != y->rows)) {
      int r, k;
      for (r = 0; r < y->wrows; r++)
        for (k = (r < y->rows ? y->cols : 0); k < y->wcols; k++) TCELL(y, r, k) = 0x0720;
      y->cols = y->wcols;
      y->rows = y->wrows;
    }
  }
  if (cols) *cols = y->cols;
  if (rows) *rows = y->rows;
}

static const struct kexp_textscr tty_ops = {
  tty_vput, tty_cursor, tty_clear, tty_font_get, tty_font_set, tty_font_reset, tty_key, tty_size
};

static void tty_push(struct con_tty* y, int k) {
  if (!y || !k || y->kn >= TTY_KQ) return;
  y->kq[(y->kr + y->kn) % TTY_KQ] = k;
  y->kn++;
}

/* Scancode (sem 0x80) para o valor de k_getc, como get_key do kernel. */
static int tty_key_of(uint8_t sc, int e0, int kp) {
  if (kp) return kp;
  if (e0) {
    switch (sc) {
      case 0x48: return KEY_UP;
      case 0x50: return KEY_DOWN;
      case 0x4B: return KEY_LEFT;
      case 0x4D: return KEY_RIGHT;
      case 0x49: return KEY_PGUP;
      case 0x51: return KEY_PGDN;
      case 0x47: return KEY_HOME;
      case 0x4F: return KEY_END;
      case 0x53: return KEY_DEL;
      case 0x52: return KEY_INS;
    }
    return 0;
  }
  if (sc >= 0x3B && sc <= 0x44) return KEY_F(sc - 0x3A);
  if (sc == 0x57 || sc == 0x58) return KEY_F(sc - 0x57 + 11);
  {
    int c = win_keychar(sc);
    if (c && kbd_ctrl_down()) {
      int l = c | 0x20;
      if (l >= 'a' && l <= 'z') return l - 'a' + 1;
    }
    return c;
  }
}

/* O programa acabou: o que ficou na grade vira o texto do Command. */
static void tty_to_log(struct cmd_con* c, struct con_tty* y) {
  int r, k, last = y->c > 0 ? y->r : y->r - 1;
  int busy = c->busy;
  for (r = 0; r < y->rows; r++)
    for (k = 0; k < y->cols; k++) {
      unsigned char u = (unsigned char)(TCELL(y, r, k) & 0xFF);
      if (u > 32 && r > last) last = r;
    }
  cons_wipe(c);
  c->busy = busy;
  if (last - CMD_ROWS + 1 > 0) r = last - CMD_ROWS + 1; else r = 0;
  for (; r <= last && r < y->rows; r++) {
    char s[CMD_COLS];
    int n = 0;
    for (k = 0; k < y->cols && k < CMD_COLS - 1; k++) {
      unsigned char u = (unsigned char)(TCELL(y, r, k) & 0xFF);
      s[k] = (u >= 32 && u <= 126) ? (char)u : ' ';
      if (s[k] != ' ') n = k + 1;
    }
    s[n] = '\0';
    cmd_push(c, s);
  }
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
    gtasks = (struct gui_task*)task_mem(sizeof(struct gui_task) * TASK_MAX);
    if (gtasks) gui_zero(gtasks, (uint32_t)sizeof(struct gui_task) * TASK_MAX);
  }
  return gwins && gtasks;
}

static void gui_idle(void);
static void gui_clamp_pos(void);
static int gui_ok(void) {
  if (!gwins || !gtasks) return 0;
  if (gui_ix < 0 || gui_ix >= GUI_MAX || !gui.open) return 0;
  /* thread de programa: task_cur e do mundo; vale a janela do programa dela */
  if (kt_in_thread()) return gui.pid && gui.pid == kt_cur_pid();
  if (task_cur >= 0 && gtasks[task_cur].gid != gui_ix) return 0;
  return 1;
}

/* Thread de programa (kthread.c, outra CPU) dentro de um k_gui_*. gui_ix e
 * task_cur sao do mundo, que nessa hora pode estar rodando outro programa ou
 * o laco do WIN: os pixels da thread do compositor do Firefox iam para a
 * janela do programa da vez (conteudo de uma janela dentro de outra) ou eram
 * descartados (faixas velhas e novas misturadas, a janela piscava).
 * Aqui gui_ix passa a ser a janela aberta pelo programa da thread ate o
 * gui_thread_leave. A thread segura o BKL: o mundo so volta ao kernel depois
 * que ela soltar, entao ninguem ve o gui_ix trocado. Devolve o gui_ix a
 * repor, ou -1 (nao e thread, ou o programa nao tem janela: gui_ok recusa). */
int gui_thread_enter(void) {
  uint32_t pid;
  int i, save;
  if (!kt_in_thread() || !gwins) return -1;
  pid = kt_cur_pid();
  if (!pid) return -1;
  for (i = 0; i < GUI_MAX; i++) {
    if (!gwins[i].open || gwins[i].pid != pid) continue;
    save = gui_ix;
    gui_ix = i;
    return save;
  }
  return -1;
}

void gui_thread_leave(int save) {
  if (save >= 0) gui_ix = save;
}
static void (*chrome_under)(void);
static void (*chrome_over)(void);
static int (*chrome_click)(int x, int y);
static int tb_direito;   /* barra de tarefas: o clique que chega ao chrome e o direito */
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

/* Pedido de encaixe: a proxima gui_open nasce docada a direita. */
static int gui_dock_w;

int gui_dock_open(const char* title, int w) {
  gui_dock_w = w > 0 ? w : 360;
  return gui_open(title);
}

/* Pedido de tamanho: a proxima gui_open nasce com wpct% da largura da tela e
 * hpct% da altura acima da barra de tarefas (Explorer: 1/3 x 1/2). */
static int gui_pct_w, gui_pct_h;

int gui_open_pct(const char* title, int wpct, int hpct) {
  gui_pct_w = wpct > 0 && wpct <= 100 ? wpct : 0;
  gui_pct_h = hpct > 0 && hpct <= 100 ? hpct : 0;
  {
    int rc = gui_open(title);
    gui_pct_w = gui_pct_h = 0;
    return rc;
  }
}

int gui_open(const char* title) {
  uint32_t bytes;
  int i, n = 0, slot = -1, tw;
  if (!gui_ensure()) return -2;
  if (!gui_allowed || !gfx_live || vt_bg() || gfx_w == 0) return -1;
  /* a janela fica ligada a tarefa do mundo (task_cur): so ele abre e fecha */
  if (kt_in_thread()) return -1;
  if (task_cur >= 0 && gtasks[task_cur].gid >= 0 && gwins[gtasks[task_cur].gid].open) {
    gui_ix = gtasks[task_cur].gid;
    gui.pid = mod_current_pid();   /* programa aninhado (k_exec) herda a janela */
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
  /* Tela estendida: o tamanho e a posicao padrao saem da primeira tela,
   * para a janela nova nao nascer cortada na segunda (2026-11-19). */
  tw = gfx_wide ? (int)gfx_wide_x2 : (int)gfx_w;
  {
    int scale = (gfx_h >= 400u) ? 2 : 1;
    int font_h = (vga_bios_font_ok ? FONT_H : 8) * scale;
    int bar = font_h + (gfx_h >= 400u ? 8 : 2);
    int margin_l = tw / 8;
    int margin_r = tw / 24;
    int margin_t = (int)gfx_h / 16;
    if (margin_l < 36) margin_l = 36;
    if (margin_r < 12) margin_r = 12;
    if (margin_t < 8) margin_t = 8;
    gui.scale = scale;
    gui.title_h = font_h + (gfx_h >= 400u ? 4 : 2);
    gui.ox = margin_l;
    gui.oy = margin_t;
    gui.cw = tw - margin_l - margin_r;
    gui.ch = (int)gfx_h - margin_t - bar - 6 - gui.title_h;
    if (gui.cw < 160 || gui.ch < 48) {
      gui.ox = 0;
      gui.oy = 0;
      gui.cw = tw;
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
  if (gui.ox + gui.cw > tw) gui.cw = tw - gui.ox;
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
  if (gui_pct_w > 0 || gui_pct_h > 0) {
    int scale = gui.scale > 0 ? gui.scale : 1;
    int bar = glyph_rows() * scale + (gfx_h >= 400u ? 8 : 2);
    int avail = (int)gfx_h - bar;
    int w = gui_pct_w > 0 ? tw * gui_pct_w / 100 : gui.cw;
    int h = gui_pct_h > 0 ? avail * gui_pct_h / 100 - gui.title_h : gui.ch;
    if (w < 360) w = 360;
    if (h < 240) h = 240;
    if (w > tw) w = tw;
    if (h > avail - gui.title_h) h = avail - gui.title_h;
    gui.cw = w;
    gui.ch = h;
    if (gui.ox + gui.cw > tw) gui.ox = tw - gui.cw;
    if (gui.oy + gui.title_h + gui.ch > avail) gui.oy = avail - gui.title_h - gui.ch;
    if (gui.ox < 0) gui.ox = 0;
    if (gui.oy < 0) gui.oy = 0;
  }
  if (gui_dock_w > 0) {
    int scale = (gfx_h >= 400u) ? 2 : 1;
    int font_h = (vga_bios_font_ok ? FONT_H : 8) * scale;
    int bar = font_h + (gfx_h >= 400u ? 8 : 2);
    int w = gui_dock_w;
    if (w < 240) w = 240;
    if (w > (int)gfx_w / 2) w = (int)gfx_w / 2;
    gui.scale = scale;
    gui.title_h = font_h + (gfx_h >= 400u ? 4 : 2);
    gui.ox = (int)gfx_w - w - 4;
    gui.oy = 4;
    gui.cw = w;
    gui.ch = (int)gfx_h - bar - gui.title_h - 8;
    gui_dock_w = 0;
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
  gui.clip = 0;
  gui.maxed = 0;
  gui.panel = 0;
  gui.pid = mod_current_pid();
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
  if (kt_in_thread()) return;   /* task_cur e do mundo, nao da thread */
  if (!host_on) sched_set_idle(0);
  if (gui.pix) ufree(gui.pix);
  gui.pix = 0;
  gui.open = 0;
  gui.pid = 0;
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
      if (dx < 0 || dx >= gui.cw) continue;
      if (gfx_quality < 1 && !ui_th.modern) {
        /* retro: alfa binario (custo zero, como sempre foi) */
        if (p[3] < 128) continue;
        gui.pix[dy * gui.cw + dx] =
            ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      } else if (p[3] >= 255) {
        gui.pix[dy * gui.cw + dx] =
            ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      } else if (p[3]) {
        /* avancado: mistura alfa sobre o que ja esta na janela */
        uint32_t* d = &gui.pix[dy * gui.cw + dx];
        uint32_t a = p[3];
        uint32_t na = 255u - a;
        uint32_t dr = (*d >> 16) & 0xFFu, dg = (*d >> 8) & 0xFFu, db = *d & 0xFFu;
        uint32_t r = ((uint32_t)p[0] * a + dr * na) / 255u;
        uint32_t g = ((uint32_t)p[1] * a + dg * na) / 255u;
        uint32_t b = ((uint32_t)p[2] * a + db * na) / 255u;
        *d = (r << 16) | (g << 8) | b;
      }
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
  int y, x0, x1, y0, y1;
  int top = w->oy + w->title_h;
  int left = w->ox;
  if (!w->pix || w->cw < 1 || w->ch < 1) return;
  /* A janela pode estar meio fora da tela (arrastada): so copia o pedaco
   * do buffer que cai dentro do quadro. */
  x0 = left < 0 ? -left : 0;
  x1 = left + w->cw > (int)gfx_w ? (int)gfx_w - left : w->cw;
  y0 = top < 0 ? -top : 0;
  y1 = top + w->ch > (int)gfx_h ? (int)gfx_h - top : w->ch;
  if (x0 >= x1 || y0 >= y1) return;
  for (y = y0; y < y1; y++) {
    uint32_t* src = w->pix + y * w->cw;
    if (gfx_bochs) {
      uint32_t* s = src + x0;
      volatile uint32_t* dst = gfx_offscreen
        ? (volatile uint32_t*)(gfx_back + (uint32_t)(y + top) * gfx_w * 4u +
                               (uint32_t)(left + x0) * 4u)
        : (volatile uint32_t*)(gfx_lfb + ((uint32_t)(y + top) + gfx_draw_y0) * gfx_pitch_bytes +
                               (uint32_t)(left + x0) * 4u);
      uint32_t n = (uint32_t)(x1 - x0);
      __asm__ volatile ("rep movsl" : "+S"(s), "+D"(dst), "+c"(n) : : "memory");
    } else {
      int x;
      uint32_t last = 0xFFFFFFFFu, idx = 0;
      for (x = x0; x < x1; x++) {
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

/* Arrastar pela barra de titulo: a janela pode sair da tela pela esquerda,
 * pela direita e por baixo. Ficam sempre na tela WIN_KEEP px de barra livre
 * (fora dos botoes, btn_w px na ponta direita) para pegar de volta, e a barra
 * inteira fica entre o topo da tela e a barra de tarefas (bar_h). */
#define WIN_KEEP 40
static void win_keep_title(int* x, int* y, int w, int title_h, int bar_h, int btn_w) {
  int keep_l = WIN_KEEP;           /* saindo pela direita: sobra o comeco */
  int keep_r = WIN_KEEP + btn_w;   /* saindo pela esquerda: botoes + barra livre */
  int ymax = (int)gfx_h - bar_h - title_h;
  if (keep_l > w) keep_l = w;
  if (keep_r > w) keep_r = w;
  if (keep_l > (int)gfx_w) keep_l = (int)gfx_w;
  if (keep_r > (int)gfx_w) keep_r = (int)gfx_w;
  if (*x > (int)gfx_w - keep_l) *x = (int)gfx_w - keep_l;
  if (*x < keep_r - w) *x = keep_r - w;
  if (*y > ymax) *y = ymax;
  if (*y < 0) *y = 0;
}

static void gui_move_to(int x, int y) {
  int scale = gui.scale > 0 ? gui.scale : 1;
  int bar = glyph_rows() * scale + (gfx_h >= 400u ? 8 : 2);
  gui.ox = x - gui.drag_ox;
  gui.oy = y - gui.drag_oy;
  /* sem o WIN so ha o botao de fechar (gui_present) */
  win_keep_title(&gui.ox, &gui.oy, gui.cw, gui.title_h, bar, gui.title_h - 4 + 2);
}

void gui_present(void) {
  int scale, ch, close_s, close_x, ty, xoff;
  uint32_t title_bg = gui_col(0x000080), white = gui_col(0xFFFFFF), btn = gui_col(0xC0C0C0);
  uint32_t edge = gui_col(0x000000);
  /* Thread de programa: so marca a janela dela. Com o mundo no laco do WIN
   * (task_cur < 0) ela cairia no desenho sem WIN, que limpa a tela inteira. */
  if (kt_in_thread()) {
    if (!host_on || !gui_ok()) return;
    win_api_dirty |= 2;
    if (gpu_drv) gfx_dmg_add(gui.ox, gui.oy, gui.cw, gui.title_h + gui.ch);
    return;
  }
  if (host_on && task_cur >= 0) {
    /* 2 = so o conteudo desta janela mudou; qualquer outro "= 1" vence */
    win_api_dirty |= 2;
    if (gpu_drv) gfx_dmg_add(gui.ox, gui.oy, gui.cw, gui.title_h + gui.ch);
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
  else gfx_desk_clear();
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

int gui_raw_set(int on) {
  if (!gui_ok()) return -1;
  gui.raw = on ? 1 : 0;
  gui.raw_btn = 0;
  return 0;
}

int gui_clip_set(int on) {
  if (!gui_ok()) return -1;
  gui.clip = on ? 1 : 0;
  return 0;
}

/* Janela com foco em modo cru, ou -1. */
static int gui_raw_focus(void) {
  if (!gwins || gui_focus < 0 || gui_focus >= GUI_MAX) return -1;
  if (!gwins[gui_focus].open || !gwins[gui_focus].raw) return -1;
  return gui_focus;
}

/* Movimento cru: junta com o anterior se os botoes nao mudaram. */
static void gq_push_move(struct gui_win* w, int x, int y, int btn) {
  if (w->qn > 0) {
    int last = (w->qr + w->qn - 1) % GUI_Q;
    if (w->qt[last] == GUI_EV_MOVE && w->qk[last] == btn) {
      w->qx[last] = x;
      w->qy[last] = y;
      return;
    }
  }
  gq_push(w, GUI_EV_MOVE, x, y, btn);
}

/* Injeta uma tecla (ASCII) na janela GUI com foco. Usado pelo httpd, para
 * automatizar testes de digito sem teclado fisico. */
int win_api_key(int ch) {
  if (!gwins || gui_focus < 0 || gui_focus >= GUI_MAX || !gwins[gui_focus].open)
    return -1;
  gq_push(&gwins[gui_focus], GUI_EV_KEY, 0, 0, ch);
  win_api_dirty = 1;
  return 0;
}

/* Ctrl+V numa janela de programa que nao trata a area de transferencia: o
 * texto entra como teclas, aos poucos, porque a fila so tem GUI_Q lugares.
 * Quebras de linha e Tab viram espaco (Enter enviaria o campo no meio). */
static char gpaste[CLIP_MAX];
static int gpaste_n, gpaste_i;
static int gpaste_win = -1;

static void gui_paste_pump(void) {
  struct gui_win* w;
  if (gpaste_win < 0) return;
  if (!gwins || gpaste_win != gui_focus || !gwins[gpaste_win].open) {
    gpaste_win = -1;
    return;
  }
  w = &gwins[gpaste_win];
  if (w->qn >= GUI_Q - 4) return;
  while (gpaste_i < gpaste_n && w->qn < GUI_Q - 4)
    gq_push(w, GUI_EV_KEY, 0, 0, (unsigned char)gpaste[gpaste_i++]);
  if (gpaste_i >= gpaste_n) gpaste_win = -1;
  win_api_dirty = 1;
}

static void gui_paste_start(int gid) {
  int n = clip_get(gpaste, (int)sizeof(gpaste));
  int k, m = 0;
  while (n > 0 && (gpaste[n - 1] == '\n' || gpaste[n - 1] == '\r')) n--;
  for (k = 0; k < n; k++) {
    unsigned char ch = (unsigned char)gpaste[k];
    if (ch == '\r') continue;
    if (ch == '\n' || ch == '\t') ch = ' ';
    if (ch < 32 || ch == 127) continue;
    gpaste[m++] = (char)ch;
  }
  gpaste_n = m;
  gpaste_i = 0;
  gpaste_win = m > 0 ? gid : -1;
  gui_paste_pump();
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
    int was_e0 = gui.e0;
    int k = (was_e0 || keymap_kp_nav(data)) ? gui_key_ext(data) : 0;
    gui.e0 = 0;
    if (!k) {
      int c = was_e0 ? keymap_ext_char(data) : win_keychar(data);
      k = c;
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
  kexp_set_textscr(0);
  run_out = 0;
  cons_cap = 0;
}

static void console_attach(struct gui_task* t) {
  if (!t) return;
  if (t->con) {
    cons_cap = t->con;
    kexp_set_console_hook(cons_emit);
    if (t->tty) kexp_set_textscr(&tty_ops);
  } else if (t->tty) {
    /* O Command fechou: a saida se perde e k_getc devolve -1 (fim). */
    cons_cap = 0;
    kexp_set_console_hook(cons_emit);
    kexp_set_textscr(&tty_ops);
  } else if (t->run && t->run->used && t->run->st) {
    run_out = t->run->st;
    run_row = t->con_row;
    run_col = t->con_col;
    kexp_set_console_hook(run_add);
  } else if (t->logout) {
    kexp_set_console_hook(syslog_putc);
  }
}

/* O programa fica parado nesta funcao ate o desktop retomar a tarefa.
 * Sem janela propria, gid fica -1: herdar gui_ix fazia o gui_open dele
 * tomar a janela de outro programa. */
static void gui_yield(int wake) {
  struct gui_task* t;
  /* Thread de programa (outra CPU): nao troca a tarefa do WIN, so cede. */
  if (kt_in_thread()) {
    kt_yield_in_thread();
    return;
  }
  if (task_cur < 0 || !host_on) return;
  t = &gtasks[task_cur];
  t->wake = wake;
  console_detach(t);
  mod_cpu_save(&t->cpu);
  mod_cpu_load(&host_cpu);
  ctx_switch(&t->rsp, host_rsp);
  console_attach(&gtasks[task_cur]);
}

/* k_yield de uma tarefa do WIN. Logo depois de um k_trygetc vazio (mesmo
 * tick ou o seguinte) e um programa esperando tecla em laco (TOP): cede como
 * WAKE_POLL, e o laco do WIN pode emprestar a CPU0 (hlt) em vez de girar.
 * Quem calcula entre um e outro (mais de 1 ms) continua WAKE_BUSY. */
int gui_coop_pause(void) {
  struct gui_task* t;
  int wake = WAKE_BUSY;
  if (task_cur < 0 || !host_on || kt_in_thread()) return 0;
  t = &gtasks[task_cur];
  if (t->poll_t && (uint32_t)irq_ticks() + 1u - t->poll_t <= 1u) wake = WAKE_POLL;
  t->poll_t = 0;
  gui_yield(wake);
  return 1;
}

/* Espera de futex de uma tarefa do WIN (world_wait no kthread.c): cede como
 * WAKE_WAIT ate *addr != val ou ms passarem. Antes cedia como WAKE_BUSY, e
 * cada processo do WINE (o laco do start() espera o ExitProcess de 20 em
 * 20 ms) contava como "calculando": o laco do WIN nunca chegava ao hlt e a
 * CPU0 ficava em 100% com o Claude parado (~15 W, ~82 C no notebook). */
int gui_coop_wait(volatile int* addr, int val, uint32_t ms) {
  struct gui_task* t;
  if (task_cur < 0 || !host_on || kt_in_thread()) return 0;
  t = &gtasks[task_cur];
  t->poll_t = 0;
  t->wait_addr = addr;
  t->wait_val = val;
  t->wait_until = (uint32_t)irq_ticks() + (ms ? ms : 1u);
  gui_yield(WAKE_WAIT);
  t = &gtasks[task_cur];
  t->wait_addr = 0;
  return 1;
}

/* WAKE_WAIT pronta: o valor mudou ou o prazo venceu */
static int task_wait_ready(const struct gui_task* t) {
  if (!t->wait_addr) return 1;
  if (*t->wait_addr != t->wait_val) return 1;
  return (int32_t)((uint32_t)irq_ticks() - t->wait_until) >= 0;
}

/* ---------- preempcao das janelas de programa ----------
 * As tarefas do WIN sao cooperativas: um programa que calcula sem ceder a vez
 * (layout/JS do Firefox, por exemplo) prendia o laco do WIN, e com ele o
 * mouse, o teclado e a tela. No IRQ0, se a tarefa passou da fatia e o RIP
 * esta no codigo do proprio programa (nunca no kernel ou num driver), ela e
 * tirada da CPU como num k_yield. O estado SSE/AVX vai junto: outro programa
 * pode usa-lo enquanto esta espera. */
#define GUI_SLICE_MS 10
#define XS_MAX 4096
static volatile uint32_t task_slice_t0;
static int xs_mode = -1;      /* -1 = nao sondado, 0 = fxsave, 1 = xsave */
static uint32_t xs_lo, xs_hi; /* XCR0 */
static uint32_t gui_preempts;

static void xs_probe(void) {
  uint64_t cr4;
  uint32_t a, b, c, d;
  xs_mode = 0;
  __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
  if (!(cr4 & (1u << 18))) return;              /* OSXSAVE */
  __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xD), "c"(0));
  if (b == 0 || b > XS_MAX) return;
  __asm__ volatile ("xgetbv" : "=a"(xs_lo), "=d"(xs_hi) : "c"(0));
  xs_mode = 1;
}

/* ---------- entrada esperando e latencia da entrada ----------
 * No tick (1 ms) so se olha se ha entrada esperando: o byte no 8042 ou o
 * relatorio do tablet USB do QEMU. Com entrada esperando, a tarefa GUI e
 * preemptada sem esperar o fim da fatia (gui_preempt_tick). lat_note marca
 * quando o WIN retirou o dado do hardware.
 *
 * WIN_INLAT_LOG 1: a cada 3 s vai para a serial o histograma da latencia
 * (chegada -> WIN le o dado), onde a CPU estava em cada tick (codigo do
 * programa, kernel a pedido dele, WIN) e o tempo do laco do WIN por parte.
 * Foi assim que se achou o LFB uncached (quadro de 38-197 ms). */
#define WIN_INLAT_LOG 0
static volatile uint32_t in_pend_t;   /* tick em que a entrada chegou; 0 = nada */
static uint32_t prof_pump, prof_draw, prof_frames, prof_poll;
#if WIN_INLAT_LOG
static uint32_t lat_n, lat_sum, lat_max, lat_hist[8];
static volatile uint32_t st_user, st_kern, st_host;
static uint32_t lat_rep_t;

static void lat_put(const char* k, uint32_t v) {
  char num[16];
  serial_puts(k);
  int32_to_string((int32_t)v, num, (int)sizeof(num));
  serial_puts(num);
}
#endif

static int input_peek(void) {
  if (ps2in_pending()) return 1;
  if (mouse_abs_mode && tablet_td && !(tablet_td->sts & (1u << 23))) return 1;
  return 0;
}

/* Byte tirado da fila: a latencia conta desde a IRQ que o trouxe. */
static void in_note(uint32_t t) {
  uint32_t p = in_pend_t;
  if (t && (!p || (int32_t)(t - p) < 0)) in_pend_t = t;
  lat_note((uint32_t)irq_ticks());
}

static void lat_note(uint32_t now) {
#if WIN_INLAT_LOG
  static const uint32_t lim[7] = {2, 5, 10, 20, 50, 100, 200};
  uint32_t t0 = in_pend_t, d;
  int b = 0;
  in_pend_t = 0;
  if (t0) {
    d = now - t0;
    while (b < 7 && d >= lim[b]) b++;
    lat_hist[b]++;
    lat_n++;
    lat_sum += d;
    if (d > lat_max) lat_max = d;
  }
  if (now - lat_rep_t < 3000) return;
  lat_rep_t = now;
  if (!lat_n) return;
  lat_put("[inlat] n=", lat_n);
  lat_put(" media=", lat_sum / lat_n);
  lat_put(" max=", lat_max);
  lat_put(" ms | <2:", lat_hist[0]);
  lat_put(" <5:", lat_hist[1]);
  lat_put(" <10:", lat_hist[2]);
  lat_put(" <20:", lat_hist[3]);
  lat_put(" <50:", lat_hist[4]);
  lat_put(" <100:", lat_hist[5]);
  lat_put(" <200:", lat_hist[6]);
  lat_put(" >=200:", lat_hist[7]);
  lat_put(" | ticks prog=", st_user);
  lat_put(" kernel=", st_kern);
  lat_put(" win=", st_host);
  lat_put(" | pump=", prof_pump);
  lat_put(" desenho=", prof_draw);
  lat_put(" quadros=", prof_frames);
  lat_put(" poll=", prof_poll);
  serial_puts("\n");
  lat_n = lat_sum = lat_max = 0;
  for (b = 0; b < 8; b++) lat_hist[b] = 0;
  st_user = st_kern = st_host = 0;
  prof_pump = prof_draw = prof_frames = prof_poll = 0;
#else
  (void)now;
  in_pend_t = 0;
#endif
}

static void gui_preempt_tick(uint64_t rip) {
  struct gui_task* t;
  uint8_t* xs;
  int i;
  if (host_on) {
    if (!in_pend_t && input_peek()) {
      uint32_t now = (uint32_t)irq_ticks();
      in_pend_t = now ? now : 1;
    }
#if WIN_INLAT_LOG
    if (task_cur < 0) st_host++;
    else if (mod_rip_preemptible(rip)) st_user++;
    else st_kern++;
#endif
  }
  if (task_cur < 0 || !host_on || !gtasks || sched_frozen()) return;
  t = &gtasks[task_cur];
  if (!t->used || t->wake == WAKE_DEAD || t->wake == WAKE_NONE) return;
  /* Entrada esperando (tecla, mouse): o WIN precisa da CPU ja, sem esperar a
   * fatia acabar. */
  if (!in_pend_t && (uint32_t)irq_ticks() - task_slice_t0 < GUI_SLICE_MS) return;
  if (!mod_rip_preemptible(rip)) return;
  if (xs_mode < 0) xs_probe();
  /* Fora da pilha do programa: a de uma thread do Firefox pode ser pequena.
   * Uma area por tarefa basta: ela so e preemptada uma vez de cada vez. */
  if (!t->xs_raw) t->xs_raw = (uint8_t*)task_mem(XS_MAX + 64);
  if (!t->xs_raw) return;
  xs = (uint8_t*)(((uintptr_t)t->xs_raw + 63) & ~(uintptr_t)63);
  if (xs_mode) {
    for (i = 512; i < 576; i++) xs[i] = 0;      /* cabecalho XSAVE */
    __asm__ volatile ("xsave %0" : "=m"(*(uint8_t(*)[XS_MAX])xs) : "a"(xs_lo), "d"(xs_hi) : "memory");
  } else {
    __asm__ volatile ("fxsave %0" : "=m"(*(uint8_t(*)[512])xs) : : "memory");
  }
  gui_preempts++;
  /* O EOI ja foi: com IF=1 o desktop roda normal. Um IRQ aqui dentro cai no
   * kernel (RIP fora do programa) e so conta o tick. */
  __asm__ volatile ("sti");
  gui_yield(WAKE_BUSY);
  __asm__ volatile ("cli");
  if (xs_mode) {
    __asm__ volatile ("xrstor %0" : : "m"(*(uint8_t(*)[XS_MAX])xs), "a"(xs_lo), "d"(xs_hi) : "memory");
  } else {
    __asm__ volatile ("fxrstor %0" : : "m"(*(uint8_t(*)[512])xs) : "memory");
  }
}

static int gui_event_coop(int* ev, int timeout_ms) {
  uint32_t t0 = net_ms();
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
    if (timeout_ms > 0) {
      /* WAKE_BLOCK so acorda com evento: o prazo nunca venceria */
      if ((int)(net_ms() - t0) >= timeout_ms) return GUI_EV_NONE;
      gui_yield(WAKE_POLL);
    } else {
      gui_yield(WAKE_BLOCK);
    }
    if (!gui_ok()) return -1;
  }
}

/* Espera um evento ate timeout_ms (-1 = sem limite). 0 = nada aconteceu. */
int gui_event(int* ev, int timeout_ms) {
  /* Thread de programa: so tira da fila da janela dela, sem esperar (quem
   * espera e o k_gui_event, fora do gui_thread_enter). O caminho sem WIN
   * abaixo le o teclado e o mouse direto: e so do mundo. */
  if (kt_in_thread()) {
    ev[0] = ev[1] = ev[2] = ev[3] = 0;
    if (!gui_ok()) return -1;
    if (gui.ask_close) {
      gui.ask_close = 0;
      ev[0] = GUI_EV_CLOSE;
      return GUI_EV_CLOSE;
    }
    return gui_q_pop(ev);
  }
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
    if (kind == 0 && gui.raw) {
      ev[0] = GUI_EV_SCAN;
      ev[3] = scb;
      return GUI_EV_SCAN;
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
      if (gui.raw && !gui.drag) {
        int btn = gui.pkt[0] & 7;
        int inside = x >= 0 && y >= 0 && x < gui.cw && y < gui.ch;
        if (inside || gui.raw_btn) {
          gui.raw_btn = btn && (gui.raw_btn || inside);
          ev[0] = GUI_EV_MOVE;
          ev[3] = btn;
          return GUI_EV_MOVE;
        }
      }
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
        tb_direito = right && !left;
        if (chrome_click) hit = chrome_click((int)sys_mx, (int)sys_my);
        tb_direito = 0;
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
      if (gui.raw) gui_q_push(GUI_EV_SCAN, 0, 0, scb);
      else if (gui_key_event(scb, &key)) gui_q_push(GUI_EV_KEY, 0, 0, key);
      continue;
    }
    moved = 1;
    {
      int dz = mouse_wheel();
      if (!from_abs)
        sys_mouse_place(sys_mx + (int8_t)gui.pkt[1], sys_my - (int8_t)gui.pkt[2]);
      if (gui.raw && !gui.drag) {
        int rx = (int)sys_mx - gui.ox, ry = (int)sys_my - (gui.oy + gui.title_h);
        int btn = gui.pkt[0] & 7;
        int inside = rx >= 0 && ry >= 0 && rx < gui.cw && ry < gui.ch;
        if (inside || gui.raw_btn) {
          gui.raw_btn = btn && (gui.raw_btn || inside);
          gq_push_move(&gui, rx, ry, btn);
          continue;
        }
      }
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
        tb_direito = right && !left;
        if (chrome_click) hit = chrome_click((int)sys_mx, (int)sys_my);
        tb_direito = 0;
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
/* janela de programa sendo redimensionada pelo canto (contorno g_rs_*) */
static int g_rs = -1;
static int g_rs_x, g_rs_y, g_rs_w, g_rs_h;
static int g_cap = -1;
static int g_rcap = -1; /* janela de programa que recebeu o botao direito */

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
  if (t->stack) task_free(t->stack);
  t->stack = 0;
  if (t->xs_raw) task_free(t->xs_raw);
  t->xs_raw = 0;
  if (t->tty) {
    if (t->con && t->con->tty == t->tty) t->con->tty = 0;
    task_free(t->tty);
  }
  t->tty = 0;
  t->run = 0;
  t->con = 0;
  t->used = 0;
  t->wake = WAKE_NONE;
  t->gid = -1;
}

/* ---------- encerrar programas a forca (KILL, KILLALL, DEL no TOP) ----------
 * Pedidos de qualquer lugar (comando, k_kill de um programa) vao para uma fila
 * e o laco do WIN atende: so ele pode fechar janelas e soltar a pilha de uma
 * tarefa que nao esta rodando. mod_kill solta a memoria do programa (heap,
 * imagem, DLLs, threads, memoria virtual, soquetes). */
#define KILLQ_MAX 16
static volatile uint32_t killq[KILLQ_MAX];
static volatile int killq_n, killq_all;
static volatile uint32_t killq_keep;   /* quem pediu o KILLALL nao morre */
static volatile uint32_t killq_req, killq_ack;
static volatile int killq_count;       /* tarefas encerradas no ultimo pedido */

static int task_force_kill(int ti) {
  struct gui_task* t = &gtasks[ti];
  uint32_t pid = t->cpu.pid;
  if (!t->used || ti == task_cur) return 0;
  serial_puts("[visual] encerrando a forca a tarefa ");
  serial_puts(t->path);
  serial_puts("\n");
  if (t->wake != WAKE_DEAD && t->wake != WAKE_NONE && pid) mod_kill(pid);
  if (t->run) { /* a janela de saida do programa fecha junto */
    t->run->auto_close = 1;
    run_close_pending = 1;
  }
  task_reap(ti);
  return 1;
}

static void killq_service(void) {
  int i, n = 0, k;
  uint32_t req = killq_req;
  if (!gtasks || req == killq_ack) return;
  if (killq_all) {
    uint32_t mark;
    /* ninguem abriu nada ainda: tudo o que roda e do inicio */
    if (!mod_boot_mark()) mod_mark_boot();
    mark = mod_boot_mark();
    for (i = 0; i < task_n; i++) {
      uint32_t pid, root;
      if (!gtasks[i].used || i == task_cur) continue;
      pid = gtasks[i].cpu.pid;
      if (!pid) continue;                                /* comando interno */
      if (killq_keep && mod_chain_has(killq_keep, pid)) continue; /* quem pediu */
      root = mod_root_pid(pid);
      if (mark && root < mark) continue;                 /* programa de inicio */
      n += task_force_kill(i);
    }
  }
  for (k = 0; k < killq_n; k++) {
    uint32_t want = killq[k];
    for (i = 0; i < task_n; i++)
      if (gtasks[i].used && gtasks[i].cpu.pid && mod_chain_has(gtasks[i].cpu.pid, want))
        n += task_force_kill(i);
  }
  killq_n = 0;
  killq_all = 0;
  killq_keep = 0;
  if (n) {
    (void)wine_reset_if_idle(); /* sem EXE do Windows: WINE.DLL e secoes saem */
    serial_puts("[visual] tarefas encerradas a forca: ");
    serial_hex32((uint32_t)n);
    serial_puts("\n");
  }
  killq_count = n;
  killq_ack = req;
}

/* pids das tarefas do WIN cujo programa tem esse nome (CLAUDE.EXE,
 * Claude.exe ou so CLAUDE). Quantos gravou em out. */
static int name_match(const char* path, const char* name) {
  const char* b = path;
  const char* q;
  int i;
  for (q = path; *q; q++)
    if (*q == '\\' || *q == ':' || *q == '/') b = q + 1;
  for (i = 0; name[i] && b[i]; i++) {
    char x = name[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return !name[i] && (!b[i] || b[i] == '.');
}
int win_task_pids(const char* name, uint32_t* out, int max) {
  int i, n = 0;
  if (!host_on || !gtasks || !name) return -1;
  for (i = 0; i < task_n && n < max; i++)
    if (gtasks[i].used && gtasks[i].cpu.pid && name_match(gtasks[i].path, name)) out[n++] = gtasks[i].cpu.pid;
  return n;
}

/* Tarefas do WIN para o TOP (k_ps): nome do programa e pid. */
int win_task_list(char (*names)[32], uint32_t* pids, int max) {
  int i, n = 0, k;
  if (!host_on || !gtasks) return 0;
  for (i = 0; i < task_n && n < max; i++) {
    const char* b;
    const char* q;
    if (!gtasks[i].used || !gtasks[i].cpu.pid) continue;
    b = gtasks[i].path;
    for (q = b; *q; q++)
      if (*q == '\\' || *q == ':' || *q == '/') b = q + 1;
    for (k = 0; k < 31 && b[k]; k++) names[n][k] = b[k];
    names[n][k] = 0;
    pids[n++] = gtasks[i].cpu.pid;
  }
  return n;
}

/* pid = 0 e all = 1: KILLALL (menos os de inicio e quem pediu).
 * Devolve quantas tarefas encerrou; -1 se o WIN nao esta aberto, -2 se nao
 * houve resposta (o WIN parado em outra tela). */
int win_kill(uint32_t pid, int all) {
  uint32_t me = mod_current_pid(), req, t0;
  if (!host_on || !gtasks) return -1;
  if (all) {
    killq_keep = me;
    killq_all = 1;
  } else if (killq_n < KILLQ_MAX) {
    killq[killq_n++] = pid;
  }
  req = killq_req + 1;
  killq_req = req;
  if (task_cur < 0 && !kt_in_thread()) { /* no laco do WIN (comando do Command) */
    killq_service();
    return killq_count;
  }
  t0 = net_ms();
  while (killq_ack != req) { /* tarefa ou thread (API HTTP): o laco atende */
    if (net_ms() - t0 > 5000) return -2;
    if (in_task()) gui_yield(WAKE_POLL);
    else if (kt_in_thread()) kt_yield_in_thread();
    else return -2; /* outra tela do mundo: o laco do WIN nao roda agora */
  }
  return killq_count;
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
  rc = t->cmd ? t->cmd(t->argc, av) : mod_exec(t->path, t->argc, av);
  console_detach(t);
  if (t->tty) {
    if (t->con && t->con->tty == t->tty) {
      if (t->tty->on) tty_to_log(t->con, t->tty);
      t->con->tty = 0;
    }
  }
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
  if (t->logout && rc != 0) {
    serial_puts("[visual] producao: ");
    serial_puts(t->path);
    serial_puts(" falhou, veja Ctrl+F12\n");
  }
  t->wake = WAKE_DEAD;
  mod_cpu_load(&host_cpu);
  ctx_switch(&t->rsp, host_rsp);
  for (;;) __asm__ volatile ("hlt");
}

/* A tarefa nao herda o desvio do shell do desktop: o Command liga shell_out
 * durante shell_exec_line, e a primeira fatia do programa roda ali dentro.
 * Com ele ligado k_vput achava que a saida ia para um arquivo e recusava. */
static void task_shell_enter(struct task_shell* s, struct gui_task* t) {
  con_state_save(&s->cs);
  s->scr = shell_get_screen_exec();
  if (!t->sh_ok) {
    /* Primeira vez: so o pipe ("type x | prog") passa para o programa. */
    t->sh.cs = s->cs;
    t->sh.cs.out = 0;
    t->sh.cs.exec = 0;
    t->sh.cs.flush = 0;
    t->sh.scr = 0;
    t->sh_ok = 1;
  }
  con_state_load(&t->sh.cs);
  shell_set_screen_exec(t->sh.scr);
}

static void task_shell_leave(const struct task_shell* s, struct gui_task* t) {
  con_state_save(&t->sh.cs);
  t->sh.scr = shell_get_screen_exec();
  con_state_load(&s->cs);
  shell_set_screen_exec(s->scr);
}

static void task_switch_to(int ti) {
  struct task_shell sh;
  struct gui_task* t = &gtasks[ti];
  if (!t->used || t->wake == WAKE_DEAD || t->wake == WAKE_NONE) return;
  mod_cpu_save(&host_cpu);
  mod_cpu_load(&t->cpu);
  task_cur = ti;
  gui_ix = (t->gid >= 0 && t->gid < GUI_MAX) ? t->gid : 0;
  task_slice_t0 = (uint32_t)irq_ticks();
  task_shell_enter(&sh, t);
  ctx_switch(&host_rsp, t->rsp);
  task_shell_leave(&sh, t);
  task_cur = -1;
  /* Volta da preempcao com IF=0 (veio do IRQ0). */
  __asm__ volatile ("sti");
  if (t->wake == WAKE_DEAD) task_reap(ti);
}

static void task_start(int ti) {
  struct gui_task* t = &gtasks[ti];
  struct task_shell sh;
  mod_cpu_save(&host_cpu);
  task_cur = ti;
  gui_ix = 0;
  task_slice_t0 = (uint32_t)irq_ticks();
  task_shell_enter(&sh, t);
  ctx_switch(&host_rsp, t->rsp);
  task_shell_leave(&sh, t);
  task_cur = -1;
  __asm__ volatile ("sti");
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
  for (ti = 0; ti < task_n; ti++)
    if (!gtasks[ti].used) break;
  if (ti >= TASK_MAX) {
    serial_puts("[visual] janelas de programa cheias\n");
    return -1;
  }
  if (ti >= task_n) task_n = ti + 1;
  t = &gtasks[ti];
  t->used = 1;
  t->wake = WAKE_NONE;
  t->poll_t = 0;
  t->gid = -1;
  t->run = 0;
  t->logout = 0;
  t->con = con;
  t->tty = 0;
  t->cmd = task_pending_cmd;
  t->sh_ok = 0;
  t->con_row = 0;
  t->con_col = 0;
  t->rsp = 0;
  t->xs_raw = 0;
  if (con) {
    t->tty = (struct con_tty*)task_mem(sizeof(struct con_tty));
    if (t->tty) {
      gui_zero(t->tty, (uint32_t)sizeof(struct con_tty));
      t->tty->cols = 80;
      t->tty->rows = 25;
      con->tty = t->tty;
    }
  }
  t->stack = (uint8_t*)task_mem(TASK_STACK);
  if (!t->stack) {
    if (t->tty) task_free(t->tty);
    if (con) con->tty = 0;
    t->tty = 0;
    t->used = 0;
    t->con = 0;
    serial_puts("[visual] sem pilha para o programa\n");
    return -1;
  }
  wm_copy_str(t->path, (int)sizeof(t->path), path ? path : "");
  task_set_args(t, argc, argv);
  if (!con && syslog_production()) {
    /* Producao: nada de janela de saida (o WINE abre varios filhos com
     * --wine-spawn=N); o texto e o codigo de saida ficam no Ctrl+F12. */
    t->logout = 1;
  } else if (!con) {
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
  for (i = 0; i < task_n; i++) {
    if (!task_has_event(i)) continue;
    if (gwins[gtasks[i].gid].z >= bz) {
      bz = gwins[gtasks[i].gid].z;
      best = i;
    }
  }
  if (best >= 0) return best;
  for (n = 0; n < task_n; n++) {
    i = (task_rr + n) % task_n;
    if (!gtasks[i].used) continue;
    if (gtasks[i].wake == WAKE_BUSY || gtasks[i].wake == WAKE_POLL) return i;
    if (gtasks[i].wake == WAKE_WAIT && task_wait_ready(&gtasks[i])) return i;
  }
  return -1;
}

/* alguma tarefa do WIN quer CPU ja (evento na fila ou calculando)? */
static int tasks_want_cpu(void) {
  int i;
  if (!gtasks || !gwins) return 0;
  for (i = 0; i < task_n; i++) {
    if (!gtasks[i].used) continue;
    if (task_has_event(i) || gtasks[i].wake == WAKE_BUSY) return 1;
  }
  return 0;
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
  for (i = 0; i < task_n; i++) {
    if (!gtasks[i].used) continue;
    if (gtasks[i].gid >= 0 && gtasks[i].gid < GUI_MAX && gwins[gtasks[i].gid].open)
      gq_push(&gwins[gtasks[i].gid], GUI_EV_CLOSE, 0, 0, 0);
    for (n = 0; n < 256 && gtasks[i].used && gtasks[i].wake != WAKE_DEAD; n++) {
      if (gtasks[i].wake == WAKE_NONE) break;
      task_switch_to(i);
    }
    if (gtasks[i].used && gtasks[i].wake != WAKE_DEAD) {
      /* Nao terminou sozinho (o Firefox demora a fechar): o mod_exec dele
       * nunca volta, entao a memoria do programa sai aqui. */
      if (gtasks[i].wake != WAKE_NONE) mod_kill(gtasks[i].cpu.pid);
      if (gtasks[i].stack) task_free(gtasks[i].stack);
      if (gtasks[i].xs_raw) task_free(gtasks[i].xs_raw);
      gtasks[i].used = 0;
      gtasks[i].stack = 0;
      gtasks[i].xs_raw = 0;
    } else if (gtasks[i].used) {
      task_reap(i);
    }
  }
  gui_focus = -1;
  g_drag = -1;
  g_rs = -1;
  g_cap = -1;
  g_rcap = -1;
  host_on = 0;
}

/* A janela vai sumir: o programa que escrevia nela segue sem saida. */
static void task_forget_win(struct wm_win* w) {
  int i;
  if (!gtasks || !w) return;
  for (i = 0; i < task_n; i++) {
    if (gtasks[i].run == w) gtasks[i].run = 0;
    if (w->kind == WM_KIND_CMD && w->st && gtasks[i].con == (struct cmd_con*)w->st) {
      if (gtasks[i].tty) gtasks[i].tty->gone = 1;
      ((struct cmd_con*)w->st)->tty = 0;
      gtasks[i].con = 0;
    }
  }
}

static int win_ieq(const char* a, const char* b);

/* Borda esquerda do painel de IA docado (0 se ele nao esta aberto). */
static int gui_panel_left(void) {
  int i;
  if (!gwins) return 0;
  for (i = 0; i < GUI_MAX; i++) {
    if (!gwins[i].open || !win_ieq(gwins[i].title, "SODOS IA")) continue;
    /* Arrastado para fora pela direita: o "S" fica como se o painel
     * estivesse encostado na borda (senao cai em cima do relogio). */
    if (gwins[i].ox > (int)gfx_w - gwins[i].cw) return (int)gfx_w - gwins[i].cw;
    return gwins[i].ox;
  }
  return 0;
}

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
  int rc;
  if (syslog_production()) {   /* sem janela: saida e codigo no Ctrl+F12 */
    kexp_set_console_hook(syslog_putc);
    gui_session(1);
    rc = mod_exec(path, argc, argv);
    if (!host_on) gui_session(0);
    kexp_set_console_hook(0);
    (void)rc;
    return;
  }
  i = wm_new_run_window(wins, big, path, owner);
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

/* arg != 0: arquivo passado como argv[1] (abrir com, LINKEXT). */
/* 1 enquanto o WIN abre um programa sozinho (o AIPANEL do inicio) */
static int autostart_launch;

static int desk_ext_is(const char* name, const char* ext);
static void wm_exec_arg(struct wm_win* wins, int* focus, int big, const char* path,
                        const char* arg) {
  uint8_t* bak;
  int n;
  /* primeira abertura que nao e automatica: daqui em diante nao e "inicio"
   * (KILLALL). O pid do programa sai depois, no mod_exec da tarefa. */
  if (!autostart_launch && !mod_boot_mark()) {
    serial_puts("[visual] primeira abertura pelo usuario: ");
    serial_puts(path);
    serial_puts("\n");
    mod_mark_boot();
  }
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
  /* SOX (x32), SOX2 (LP64), ELF dinamico ou EXE do Windows (MZ/PE, que so
   * roda com SET WINE=1): o carregador de modulos decide. */
  if (file_load(path, (char*)magic, 5) == 4 &&
      (magic[0] == SOX_MAGIC || magic[0] == SOX_MAGIC2 || magic[0] == 0x464C457Fu ||
       ((magic[0] & 0xFFFFu) == 0x5A4Du && wine_is_pe(path)))) {
    char* argv[3];
    argv[0] = (char*)path;
    argv[1] = (char*)arg;
    argv[2] = 0;
    wm_exec_sox(wins, focus, big, path, owner, arg && arg[0] ? 2 : 1, argv);
    app_win = owner;
    if (owner && owner->st && gui_focus < 0) app_status_set("executou");
    return;
  }
  /* Nao e programa do SODOS nem do Windows: decide pelo tipo. Antes tudo caia
   * no binario plano abaixo e um .BAT (texto) aberto pelo /win/launch era
   * executado como codigo: #UD no kernel. */
  if (desk_ext_is(path, "BAT")) {
    char bexe[DRV_PATH];
    char* argv[4];
    if (mod_find_exe("BAT", bexe, (int)sizeof(bexe)) != 0) {
      app_status_set("BAT.EXE nao encontrado");
      return;
    }
    argv[0] = bexe;
    argv[1] = (char*)path;
    argv[2] = (char*)arg;
    argv[3] = 0;
    wm_exec_sox(wins, focus, big, bexe, owner, arg && arg[0] ? 3 : 2, argv);
    app_win = owner;
    if (owner && owner->st && gui_focus < 0) app_status_set("executou");
    return;
  }
  if ((magic[0] & 0xFFFFu) == 0x5A4Du) { /* MZ sem PE: DOS de 16 bits */
    serial_puts("[visual] EXE do DOS (16 bits) nao roda: ");
    serial_puts(path);
    serial_puts("\n");
    app_status_set("EXE do DOS: nao suportado");
    return;
  }
  if (!desk_ext_is(path, "BIN")) {
    /* dado (texto, imagem...): o programa associado, se houver */
    if (linkext_fn && linkext_fn(path) == 0) return;
    serial_puts("[visual] nao e programa: ");
    serial_puts(path);
    serial_puts("\n");
    app_status_set("nao e programa");
    return;
  }
  /* .BIN: binario plano antigo, carregado no PROG_SLOT */
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

static void wm_exec(struct wm_win* wins, int* focus, int big, const char* path) {
  wm_exec_arg(wins, focus, big, path, 0);
}

/* Atalho gravado em D:\... e o arquivo ficou no pendrive: procura o mesmo
 * caminho em U: e depois so o nome na raiz de U:. 1 achou (caminho em out). */
static int desk_find(const char* path, char* out, int max) {
  char alt[300]; /* caminho com nome longo */
  const char* leaf;
  char L;
  int i, j;
  if (!path || !path[0] || max < 4) return 0;
  if (path[1] != ':') {
    if (file_size(path) < 0) return 0;
    wm_copy_str(out, max, path);
    return 1;
  }
  L = path[0];
  if (L >= 'a' && L <= 'z') L = (char)(L - 32);
  if (drives_is_mounted(L) && file_size(path) >= 0) {
    wm_copy_str(out, max, path);
    return 1;
  }
  if (!drives_is_mounted('U') && drives_mount_usb() != 0 && L == 'U') return 0;
  if (L != 'U') {
    wm_copy_str(alt, (int)sizeof(alt), path);
    alt[0] = 'U';
    if (file_size(alt) >= 0) {
      wm_copy_str(out, max, alt);
      return 1;
    }
  }
  leaf = path + 2;
  for (i = 2; path[i]; i++)
    if (path[i] == '\\' || path[i] == '/') leaf = path + i + 1;
  if (!leaf[0]) return 0;
  /* Pasta do sistema (PATHSODOS, padrao U:\SODOS): pendrive com tudo em
   * U:\SODOS e arquivos pessoais na raiz. */
  env_sodos_path(leaf, alt, (int)sizeof(alt));
  if (file_size(alt) >= 0) {
    wm_copy_str(out, max, alt);
    return 1;
  }
  if (L == 'U' || leaf == path + 3) return 0;
  j = 0;
  alt[j++] = 'U';
  alt[j++] = ':';
  alt[j++] = '\\';
  for (i = 0; leaf[i] && j < (int)sizeof(alt) - 1; i++) alt[j++] = leaf[i];
  alt[j] = '\0';
  if (file_size(alt) < 0) return 0;
  wm_copy_str(out, max, alt);
  return 1;
}

/* Icones de D:\user\default\desktop (ou da mesma pasta em U:). Pasta termina sem barra.
 * Imagem com miniatura ao lado (NOME.THM) aparece como a propria figura. */
#define DESK_MAX 24
#define DESK_ROOT "D:\\user\\default\\desktop\\"
#define DESK_ROOT_USB "U:\\user\\default\\desktop\\"
/* Nome longo inteiro (FAT LFN, ate 255): antes so 8.3 (nome de mais de 12
 * nem aparecia). O rotulo e encurtado so na hora de desenhar (desk_short). */
#define DESK_NAME 256
#define DESK_LABEL 12 /* letras no rotulo (= label_w do desk_place); o resto vira ".." */
#define DESK_PATH 300
struct desk_item {
  char name[DESK_NAME];
  char path[DESK_PATH];
  int image;
  int dir;
  int lnk;   /* NOME.LNK: atalho, o conteudo e o caminho do programa */
  int tw, th;
  uint8_t* px;
};
static struct desk_item desk_items[DESK_MAX];
static int desk_n;
/* Icone arrastado pelo usuario (2026-11-19): fica onde foi solto, inclusive
 * na segunda tela da tela estendida; os outros seguem a grade, na primeira
 * tela. As posicoes ficam em D:\SODOS\DESKPOS.CFG ("NOME x y" por linha). */
#define DESK_POS_CFG "D:\\SODOS\\DESKPOS.CFG"
#define DESK_POS_MAX 64
static struct { char name[DESK_NAME]; int16_t x, y; } desk_pos[DESK_POS_MAX];
static int desk_pos_n;
static int desk_press = -1;          /* icone com o botao apertado em cima */
static int desk_press_mx, desk_press_my, desk_press_ox, desk_press_oy;
static int desk_arr;                 /* 1 = passou do limiar: e arrasto */
static int desk_drag_x, desk_drag_y; /* canto do icone durante o arrasto */
static int desk_soltou = -1;         /* soltou sem arrastar: abre este */

static int desk_nome_eq(const char* a, const char* b) {
  int i = 0;
  for (;; i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
    if (!x) return 1;
  }
}

/* posicao gravada do item i; 0 = segue a grade */
static int desk_pos_de(int i, int* x, int* y) {
  int k;
  for (k = 0; k < desk_pos_n; k++)
    if (desk_nome_eq(desk_pos[k].name, desk_items[i].name)) {
      *x = desk_pos[k].x;
      *y = desk_pos[k].y;
      return 1;
    }
  return 0;
}

/* Le os dois numeros do fim de b[s..e) (x y); e passa a apontar para o fim
 * do nome. 0 se nao achou os dois. */
static int desk_pos_nums(const char* b, int s, int* e, int v[2]) {
  int c, q = *e;
  for (c = 1; c >= 0; c--) {
    int d = 1, val = 0, any = 0;
    while (q > s && b[q - 1] == ' ') q--;
    while (q > s && b[q - 1] >= '0' && b[q - 1] <= '9') {
      val += (b[q - 1] - '0') * d;
      d *= 10;
      q--;
      any = 1;
    }
    if (!any) return 0;
    if (q > s && b[q - 1] == '-') {
      val = -val;
      q--;
    }
    v[c] = val;
  }
  while (q > s && b[q - 1] == ' ') q--;
  *e = q;
  return 1;
}

static void desk_pos_load(void) {
  static char b[DESK_POS_MAX * (DESK_NAME + 16)];
  int n = file_load(DESK_POS_CFG, b, (int)sizeof(b) - 1), i = 0;
  desk_pos_n = 0;
  if (n <= 0) return;
  b[n] = 0;
  while (i < n && desk_pos_n < DESK_POS_MAX) {
    int s, e, k, v[2] = { 0, 0 };
    while (i < n && (b[i] == '\r' || b[i] == '\n' || b[i] == ' ')) i++;
    s = i;
    while (i < n && b[i] != '\r' && b[i] != '\n') i++;
    e = i;
    if (!desk_pos_nums(b, s, &e, v) || e <= s) continue;
    for (k = 0; s + k < e && k < DESK_NAME - 1; k++) desk_pos[desk_pos_n].name[k] = b[s + k];
    desk_pos[desk_pos_n].name[k] = 0;
    desk_pos[desk_pos_n].x = (int16_t)v[0];
    desk_pos[desk_pos_n].y = (int16_t)v[1];
    desk_pos_n++;
  }
}

static void desk_pos_save(void) {
  static char b[DESK_POS_MAX * (DESK_NAME + 16)];
  int n = 0, k, j;
  for (k = 0; k < desk_pos_n; k++) {
    int c;
    for (j = 0; desk_pos[k].name[j]; j++) b[n++] = desk_pos[k].name[j];
    for (c = 0; c < 2; c++) {
      int v = c ? desk_pos[k].y : desk_pos[k].x, d = 1;
      b[n++] = ' ';
      if (v < 0) {
        b[n++] = '-';
        v = -v;
      }
      while (v / d >= 10) d *= 10;
      for (; d; d /= 10) b[n++] = (char)('0' + (v / d) % 10);
    }
    b[n++] = '\r';
    b[n++] = '\n';
  }
  (void)drives_mkdir("D:\\SODOS");
  if (file_save(DESK_POS_CFG, b, n) != 0) serial_puts("[visual] DESKPOS.CFG nao gravou\n");
}

/* o item i passa a ficar em (x, y) */
static void desk_pos_set(int i, int x, int y) {
  int k;
  for (k = 0; k < desk_pos_n; k++)
    if (desk_nome_eq(desk_pos[k].name, desk_items[i].name)) break;
  if (k == desk_pos_n) {
    if (desk_pos_n >= DESK_POS_MAX) return;
    for (int j = 0; j < DESK_NAME; j++) desk_pos[k].name[j] = desk_items[i].name[j];
    desk_pos_n++;
  }
  desk_pos[k].x = (int16_t)x;
  desk_pos[k].y = (int16_t)y;
  desk_pos_save();
}
static volatile int desk_dirty;   /* win_desk_touch: rele no proximo quadro */
/* Renomear no desktop (depois de Nova pasta): indice, texto e nome antigo */
static int desk_ren = -1;
static char desk_ren_buf[DESK_NAME];
static int desk_ren_len;
static int desk_ren_all;   /* nome todo marcado: digitar substitui */
static char desk_ren_old[DESK_NAME];
/* Icone marcado pelo ultimo clique: so o rotulo dele tem fundo (os outros sao
 * transparentes). -1 nenhum, 0..desk_n-1 itens da pasta (desk_mark_name
 * guarda o nome para achar de novo quando a pasta e relida), DESK_MARK_FIXED
 * + 0..3 os fixos DOS/ARQ/WEB/CMD. */
#define DESK_MARK_FIXED 0x1000
static int desk_mark = -1;
static char desk_mark_name[DESK_NAME];

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
  char thm[DESK_PATH];
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

#define DESK_LIST 8192 /* 24 itens com nome longo */
static char desk_last[DESK_LIST];
static int desk_last_n = -1;

static int desk_read_list(char* list, const char** prefix) {
  int n;
  *prefix = DESK_ROOT;
  n = drives_is_mounted('D') ? file_list_long(DESK_ROOT, list, DESK_LIST) : -1;
  if (n <= 0) {
    /* sem D: (notebook com boot pelo GRUB): a mesma pasta no pendrive */
    *prefix = DESK_ROOT_USB;
    if (!drives_is_mounted('U') && drives_mount_usb() != 0) return -1;
    n = file_list_long(*prefix, list, DESK_LIST);
  }
  return n;
}

static void desk_load(void) {
  static char list[DESK_LIST]; /* 8 KB: fora da pilha */
  int n, p;
  const char* prefix;
  int plen = 0;
  desk_free();
  desk_pos_load();
  desk_press = -1;
  desk_arr = 0;
  n = desk_read_list(list, &prefix);
  desk_last_n = n;
  for (p = 0; p < n && p < DESK_LIST; p++) desk_last[p] = list[p];
  if (n <= 0) return;
  while (prefix[plen]) plen++;
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
    if (k >= DESK_NAME) continue;
    it = &desk_items[desk_n];
    /* nome como esta no disco (com minusculas): o 8.3 ja vem em maiusculas */
    for (i = 0; i < k; i++) it->name[i] = list[s + i];
    it->name[k] = 0;
    if (!dir && desk_ext_is(it->name, "THM")) continue;
    if (plen + k >= (int)sizeof(it->path)) continue;
    for (i = 0; i < plen; i++) it->path[i] = prefix[i];
    for (i = 0; i < k; i++) it->path[plen + i] = it->name[i];
    it->path[plen + k] = 0;
    it->dir = dir;
    it->image = 0;
    it->lnk = !dir && desk_ext_is(it->name, "LNK");
    if (!dir && desk_is_image(it->name)) it->image = 1;
    else if (!dir && desk_is_video(it->name)) it->image = 2;
    it->tw = it->th = 0;
    it->px = 0;
    if (it->image == 1) desk_load_thm(it);
    desk_n++;
  }
  if (desk_ren >= 0) {
    /* a lista mudou durante a edicao: acha o item pelo nome antigo */
    int i, j;
    desk_ren = -1;
    for (i = 0; i < desk_n; i++) {
      (void)j;
      if (desk_nome_eq(desk_items[i].name, desk_ren_old)) desk_ren = i;
    }
  }
  if (desk_mark >= 0 && desk_mark < DESK_MARK_FIXED) {
    /* o item marcado pode ter mudado de posicao (ou sumido) */
    int i, j;
    desk_mark = -1;
    for (i = 0; i < desk_n && desk_mark_name[0]; i++) {
      (void)j;
      if (desk_nome_eq(desk_items[i].name, desk_mark_name)) desk_mark = i;
    }
  }
}

/* Marca o item i da pasta (ou um fixo, i >= DESK_MARK_FIXED; -1 desmarca). */
static void desk_mark_item(int i) {
  int k = 0;
  desk_mark = i;
  if (i >= 0 && i < desk_n)
    for (; desk_items[i].name[k] && k < (int)sizeof(desk_mark_name) - 1; k++)
      desk_mark_name[k] = desk_items[i].name[k];
  desk_mark_name[k] = 0;
}

/* Depois de um comando: rele as miniaturas so se a lista de arquivos mudou.
 * 1 se mudou (o desktop precisa de um quadro inteiro). */
static int desk_refresh(void) {
  static char list[DESK_LIST];
  const char* prefix;
  int n = desk_read_list(list, &prefix);
  int i;
  if (n == desk_last_n) {
    for (i = 0; i < n && i < DESK_LIST && list[i] == desk_last[i]; i++) { }
    if (i >= n || i >= DESK_LIST) return 0;
  }
  desk_load();
  return 1;
}

/* Chamado por k_fsave/k_mkdir/k_fdelete/...: caminho com DESKTOP marca o
 * desktop para reler os icones (o laco do WIN olha desk_dirty). */
void win_desk_touch(const char* path) {
  static const char key[] = "DESKTOP";
  int i, j;
  if (!path) return;
  for (i = 0; path[i]; i++) {
    for (j = 0; key[j]; j++) {
      char c = path[i + j];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      if (c != key[j]) break;
    }
    if (!key[j]) {
      desk_dirty = 1;
      return;
    }
  }
}

static int desk_dir_ok(const char* p) {
  char buf[16];
  return file_list(p, buf, (int)sizeof(buf)) >= 0;
}

/* Pasta do desktop (D:, ou U: sem D:), criando USER\DEFAULT\DESKTOP se
 * faltar. out recebe o caminho com barra no fim. 0 = ok. */
static int desk_ensure_root(char* out, int max) {
  static const char* parts[3] = {"USER", "DEFAULT", "DESKTOP"};
  char p[48];
  int i, k, n;
  char L;
  {
    /* a pasta que o desktop ja mostra (D: com itens, ou a do pendrive) */
    static char list[DESK_LIST];
    const char* pre;
    if (desk_read_list(list, &pre) > 0) {
      wm_copy_str(out, max, pre);
      return 0;
    }
  }
  L = drives_is_mounted('D') ? 'D' : 'U';
  if (L == 'U' && !drives_is_mounted('U') && drives_mount_usb() != 0) return -1;
  n = 0;
  p[n++] = L;
  p[n++] = ':';
  p[n++] = '\\';
  for (i = 0; i < 3; i++) {
    for (k = 0; parts[i][k] && n < (int)sizeof(p) - 2; k++) p[n++] = parts[i][k];
    p[n] = 0;
    if (!desk_dir_ok(p)) drives_mkdir(p);
    p[n++] = '\\';
    p[n] = 0;
  }
  if (!desk_dir_ok(p)) return -1;
  wm_copy_str(out, max, p);
  return 0;
}

/* Nome do icone em edicao: Enter grava (rename no disco), Esc cancela. */
static void desk_ren_start(int i) {
  int k;
  if (i < 0 || i >= desk_n) return;
  for (k = 0; desk_items[i].name[k] && k < DESK_NAME - 1; k++) desk_ren_buf[k] = desk_items[i].name[k];
  desk_ren_buf[k] = 0;
  desk_ren_len = k;
  desk_ren_all = 1;
  wm_copy_str(desk_ren_old, (int)sizeof(desk_ren_old), desk_items[i].name);
  desk_ren = i;
}

static void desk_ren_commit(void) {
  struct desk_item* it;
  char to[DESK_PATH];
  int plen, nlen, i, same = 1;
  if (desk_ren < 0 || desk_ren >= desk_n) {
    desk_ren = -1;
    return;
  }
  it = &desk_items[desk_ren];
  desk_ren = -1;
  while (desk_ren_len > 0 && (desk_ren_buf[desk_ren_len - 1] == ' ' ||
                              desk_ren_buf[desk_ren_len - 1] == '.'))
    desk_ren_len--;
  desk_ren_buf[desk_ren_len] = 0;
  if (!desk_ren_len) return;
  for (i = 0; it->name[i] || desk_ren_buf[i]; i++)
    if (it->name[i] != desk_ren_buf[i]) same = 0;
  if (same) return;
  nlen = 0;
  while (it->name[nlen]) nlen++;
  plen = 0;
  while (it->path[plen]) plen++;
  plen -= nlen;
  if (plen <= 0 || plen + desk_ren_len >= (int)sizeof(to)) return;
  for (i = 0; i < plen; i++) to[i] = it->path[i];
  for (i = 0; i < desk_ren_len; i++) to[plen + i] = desk_ren_buf[i];
  to[plen + desk_ren_len] = 0;
  if (desk_dir_ok(to) || file_size(to) >= 0) {
    app_status_set("ja existe com esse nome");
    return;
  }
  if (drives_rename(it->path, to) != 0) {
    app_status_set("nao renomeou");
    return;
  }
  desk_load();
}

/* Tecla durante a edicao (ASCII; 0 ignora). */
static void desk_ren_key(int c) {
  if (desk_ren_all && c != '\r' && c != '\n' && c != 27) {
    desk_ren_all = 0;
    if (c == 8 || (c >= 32 && c < 127)) {
      desk_ren_len = 0;
      desk_ren_buf[0] = 0;
      if (c == 8) return;
    }
  }
  if (c == '\r' || c == '\n') desk_ren_commit();
  else if (c == 27) desk_ren = -1;
  else if (c == 8) {
    if (desk_ren_len > 0) desk_ren_buf[--desk_ren_len] = 0;
  } else if (c >= 32 && c < 127 && desk_ren_len < DESK_NAME - 1 && c != '\\' && c != '/' &&
             c != ':' && c != '*' && c != '?' && c != '"' && c != '<' && c != '>' && c != '|') {
    /* nome longo: aceita espaco e minusculas (antes so 8.3 em maiusculas) */
    desk_ren_buf[desk_ren_len++] = (char)c;
    desk_ren_buf[desk_ren_len] = 0;
  }
}

/* Botao direito no desktop > Nova pasta: NOVAPASTA, NOVAP001, ... */
static void desk_new_folder(void) {
  char root[48];
  char full[64];
  int i, n;
  if (desk_ensure_root(root, (int)sizeof(root)) != 0) {
    app_status_set("sem pasta do desktop");
    return;
  }
  for (i = 0; i < 100; i++) {
    char name[12];
    if (i == 0) wm_copy_str(name, (int)sizeof(name), "NOVAPASTA");
    else {
      wm_copy_str(name, (int)sizeof(name), "NOVAP000");
      name[5] = (char)('0' + i / 100);
      name[6] = (char)('0' + (i / 10) % 10);
      name[7] = (char)('0' + i % 10);
    }
    wm_copy_str(full, (int)sizeof(full), root);
    n = 0;
    while (full[n]) n++;
    wm_copy_str(full + n, (int)sizeof(full) - n, name);
    if (desk_dir_ok(full) || file_size(full) >= 0) continue;
    if (drives_mkdir(full) == 0) {
      int k;
      desk_load();
      for (k = 0; k < desk_n; k++) {
        int j;
        for (j = 0; name[j] && desk_items[k].name[j] == name[j]; j++) { }
        if (!name[j] && !desk_items[k].name[j]) {
          desk_ren_start(k);
          break;
        }
      }
      return;
    }
  }
  app_status_set("nao criou a pasta");
}

/* Atalho .LNK: primeira linha do arquivo e o caminho do .EXE. */
static int desk_lnk_target(const char* lnk, char* out, int max) {
  char buf[300]; /* alvo com nome longo */
  int n = file_load(lnk, buf, (int)sizeof(buf));
  int i, j = 0;
  if (n <= 0) return 0;
  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
  buf[n] = 0;
  i = 0;
  while (buf[i] == ' ' || buf[i] == '\t') i++;
  for (; buf[i] && buf[i] != '\r' && buf[i] != '\n' && j < max - 1; i++) out[j++] = buf[i];
  while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t')) j--;
  out[j] = 0;
  return j > 0;
}

static int desk_place(int index, int icon, int ch, int scale, int sw, int sh, int bar_h,
                      int* x, int* y) {
  int label_w = 12 * FONT_W * scale;
  int cell_w = icon + 16;
  int cell_h = icon + 6 + ch;
  int x0, cols, rows, col, row, px, py, i, slot;
  if (index == desk_press && desk_arr) {
    *x = desk_drag_x;
    *y = desk_drag_y;
    return 1;
  }
  /* posicao do usuario: so se o icone cabe na area de agora (sem a tela
   * estendida, um icone da segunda tela volta para a grade) */
  if (desk_pos_de(index, &px, &py) && px >= 0 && py >= 0 && px + icon <= sw &&
      py + icon <= sh - bar_h) {
    *x = px;
    *y = py;
    return 1;
  }
  /* a vaga na grade conta so os icones sem posicao propria */
  slot = 0;
  for (i = 0; i < index && i < desk_n; i++)
    if (!desk_pos_de(i, &px, &py) || px < 0 || py < 0 || px + icon > sw || py + icon > sh - bar_h)
      slot++;
  index = slot;
  /* tela estendida: a grade dos icones fica na primeira tela (2026-11-19) */
  if (gfx_wide && sw > (int)gfx_wide_x2) sw = (int)gfx_wide_x2;
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

/* Rotulo de icone do desktop: letras com sombra direto sobre o fundo (cor ou
 * papel de parede aparecem); so o icone marcado ganha a faixa de selecao. */
/* iw = largura do icone: o rotulo fica centralizado embaixo dele. */
static void desk_label(int x, int iw, int y, const char* s, int marked, uint32_t fg,
                       uint32_t shadow, int scale) {
  int n = 0;
  {
    int tw;
    while (s[n]) n++;
    tw = wf_on() ? wfont_width(s) : n * FONT_W * scale;
    n = 0;
    /* prato do tema: o circulo tem o centro em 29/64 do icone */
    if (ui_th.icons && ui_th.plate[0] && icn_get(ui_th.plate)) iw = iw * 58 / 64;
    x += (iw - tw) / 2;
    if (x < 2) x = 2;
  }
  /* cores do tema so sobre o fundo do tema; com papel de parede ou cor do
   * usuario fica o rotulo claro com sombra de sempre */
  if (ui_th.modern && !desk_wall && !desk_rgb) {
    fg = gui_col(marked ? 0xFFFFFFu : ui_th.label);
    shadow = gui_col(ui_th.label_sh);
    if (!ui_th.label_sh_on && !marked) {
      if (wf_on()) {
        wf_text(x, y + 1, s, fg, 0);
        return;
      }
      draw_text_t(x, y, s, fg, scale);
      return;
    }
  }
  if (wf_on()) {
    if (marked)
      fill_round(x - 3, y, wfont_width(s) + 6, wfont_height() + 2, ui_th.modern ? 5 : 0,
                 ui_th.modern ? gui_col(UI_ACCENT) : desk_color(32, 96, 200));
    else
      wf_text(x + 1, y + 2, s, shadow, 0);
    wf_text(x, y + 1, s, fg, 0);
    return;
  }
  while (s[n]) n++;
  if (marked)
    fill_rect(x - 1, y - 1, n * FONT_W * scale + 2, glyph_rows() * scale + 2,
              desk_color(32, 96, 200));
  else
    draw_text_t(x + 1, y + 1, s, shadow, scale);
  draw_text_t(x, y, s, fg, scale);
}

/* Icone do tema para um item do desktop: o do proprio programa (FIREFOX.EXE
 * -> FIREFOX.ICN), senao o do tipo. */
static int desk_theme_icon(const struct desk_item* it, int x, int y, int s) {
  char base[12];
  const char* kind;
  int i;
  for (i = 0; it->name[i] && it->name[i] != '.' && i < 11; i++) base[i] = it->name[i];
  base[i] = 0;
  if (!it->dir && th_desk_icon(base, x, y, s)) return 1;
  if (it->dir) kind = "PASTA";
  else if (it->image == 1) kind = "IMAGEM";
  else if (it->image == 2) kind = "VIDEO";
  else if (it->lnk) kind = "ATALHO";
  else if (desk_ext_is(it->name, "EXE") || desk_ext_is(it->name, "BAT")) kind = "PROGRAMA";
  else kind = "ARQUIVO";
  return th_desk_icon(kind, x, y, s);
}

static void desk_draw(int icon, int ch, int scale, int sw, int sh, int bar_h,
                      uint32_t col_desk, uint32_t col_white, uint32_t col_black) {
  int i;
  (void)col_desk;   /* rotulos sem fundo (desk_label) */
  for (i = 0; i < desk_n; i++) {
    int x, y;
    struct desk_item* it = &desk_items[i];
    if (!desk_place(i, icon, ch, scale, sw, sh, bar_h, &x, &y)) break;
    if (it->px && it->tw > 0) {
      desk_blit(x, y, icon, it->px + 8, it->tw, it->th);
    } else if (ui_th.icons && desk_theme_icon(it, x, y, icon)) {
      /* icone do tema */
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
      if (it->lnk) {
        /* seta de atalho no canto de baixo, a esquerda */
        int a = icon / 3;
        int t = a / 6 > 1 ? a / 6 : 1;
        int k;
        uint32_t blue = desk_color(32, 96, 200);
        if (a < 6) a = 6;
        fill_rect(x + 1, y + icon - 1 - a, a, a, col_white);
        fill_rect(x + 1, y + icon - 1 - a, a, 1, col_black);
        fill_rect(x + a, y + icon - 1 - a, 1, a, col_black);
        for (k = 0; k < a - 4; k++)
          fill_rect(x + 3 + k, y + icon - 4 - k - t, t, t, blue);
        fill_rect(x + a - 1 - a / 2, y + icon - a + 1, a / 2, t, blue);
        fill_rect(x + a - 2, y + icon - a + 1, t, a / 2, blue);
      }
    }
    if (i == desk_ren) {
      int bw = 12 * FONT_W * scale + 8;
      int bh = (vga_bios_font_ok ? FONT_H : 8) * scale + 4;
      /* caixa de 12 letras: nome maior rola, mostrando o fim (onde se digita) */
      const char* vis = desk_ren_len > 12 ? desk_ren_buf + desk_ren_len - 12 : desk_ren_buf;
      int tw = (desk_ren_len > 12 ? 12 : desk_ren_len) * FONT_W * scale;
      fill_rect(x - 2, y + icon, bw, bh, col_black);
      fill_rect(x - 1, y + icon + 1, bw - 2, bh - 2, col_white);
      if (desk_ren_all) {
        uint32_t sel = desk_color(32, 96, 200);
        fill_rect(x + 2, y + icon + 2, tw, bh - 4, sel);
        draw_string_scaled(x + 2, y + icon + 2, vis, col_white, sel, scale);
      } else
        draw_string_scaled(x + 2, y + icon + 2, vis, col_black, col_white, scale);
      fill_rect(x + 2 + tw, y + icon + 2, scale > 1 ? 2 : 1, bh - 4, col_black);
    } else {
      /* rotulo: o .LNK sem a extensao; nome longo encurtado com ".." para
       * nao invadir os vizinhos (o nome inteiro continua no item) */
      char lab[24];
      int k = 0, n = 0, last = -1;
      while (it->name[n]) n++;
      if (it->lnk)
        for (k = 0; k < n; k++)
          if (it->name[k] == '.') last = k;
      if (last > 0) n = last;
      if (n > DESK_LABEL) {
        for (k = 0; k < DESK_LABEL - 2; k++) lab[k] = it->name[k];
        lab[k++] = '.';
        lab[k++] = '.';
      } else {
        for (k = 0; k < n; k++) lab[k] = it->name[k];
      }
      lab[k] = 0;
      desk_label(x, icon, y + icon + 2, lab, i == desk_mark, col_white, col_black, scale);
    }
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
  char exe[48];
  if (gui_raise_title("Navegador")) return;
  if (desk_find("D:\\BROWSER.EXE", exe, (int)sizeof(exe)) && file_size(exe) > 0) {
    wm_exec(wins, focus, big, exe);
    return;
  }
  {
    int nw = wm_new_run_window(wins, big, "Navegador", 0);
    serial_puts("[visual] sem D:BROWSER.EXE nem U:BROWSER.EXE (make disk-files)\n");
    if (nw >= 0) {
      run_puts("BROWSER.EXE nao encontrado em D:\\ nem em U:\\.\n\n"
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
  char exe[48];
  if (file && desk_find("D:\\VIDPLAY.EXE", exe, (int)sizeof(exe)) && file_size(exe) > 0) {
    argv[0] = exe;
    argv[1] = (char*)file;
    argv[2] = 0;
    wm_exec_sox(wins, focus, big, argv[0], app_win, 2, argv);
    return;
  }
  {
    int nw = wm_new_run_window(wins, big, "Video", 0);
    if (nw >= 0) {
      run_puts("VIDPLAY.EXE nao encontrado em D:\\ nem em U:\\.\n\n"
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
  char exe[48];
  if (file && desk_find("D:\\IMAGEVW.EXE", exe, (int)sizeof(exe)) && file_size(exe) > 0) {
    argv[0] = exe;
    argv[1] = (char*)file;
    argv[2] = 0;
    wm_exec_sox(wins, focus, big, argv[0], app_win, 2, argv);
    return;
  }
  {
    int nw = wm_new_run_window(wins, big, "Imagem", 0);
    if (nw >= 0) {
      run_puts("IMAGEVW.EXE nao encontrado em D:\\ nem em U:\\.\n\n"
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

/* Menu Iniciar: EXPLORER.EXE; sem ele no disco, o explorer.asm embutido. */
static void wm_open_explorer(struct wm_win* wins, int* focus, int big) {
  char exe[48];
  if (gui_raise_title("Explorer")) return;
  if (desk_find("D:\\EXPLORER.EXE", exe, (int)sizeof(exe)) && file_size(exe) > 0) {
    wm_exec(wins, focus, big, exe);
    return;
  }
  serial_puts("[visual] sem D:EXPLORER.EXE nem U:EXPLORER.EXE; usando explorer.asm\n");
  wm_open_app(wins, focus, big);
}

/* Clique numa pasta do desktop: Explorer em "visao de pasta" (EXPLORER /P
 * caminho). 0 se nao achou o EXPLORER.EXE (quem chama mostra a listagem). */
static int wm_open_folder(struct wm_win* wins, int* focus, int big, const char* path) {
  char exe[48];
  char* argv[4];
  if (!desk_find("D:\\EXPLORER.EXE", exe, (int)sizeof(exe)) || file_size(exe) <= 0) return 0;
  argv[0] = exe;
  argv[1] = (char*)"/P";
  argv[2] = (char*)path;
  argv[3] = 0;
  wm_exec_sox(wins, focus, big, exe, app_win, 3, argv);
  return 1;
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
    wins[i].w = (gfx_gop ? 21 : 12) * FONT_W * scale + 48;  /* GOP: "(reinicie)" */
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

/* Menu de contexto do desktop: Nova pasta, Console, Configuracoes de tela, Design. */
#define POP_N 4
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

static int ui_logo_x(int w, int lw);   /* definido junto da barra de tarefas */

struct start_menu {
  int x, y, w, h, item_h, n;
  int sub_x, sub_y, sub_w, sub_h;
  int mode_x, mode_y, mode_w, mode_h;
};

static void start_menu_geom(struct start_menu* m, int scr_w, int scr_h, int bar_h,
                            int menu_w, int menu_item_h, int scale) {
  m->n = 7;
  if (ui_th.icons) menu_w += menu_item_h * 6 / 10 + 10;
  {
    int big = scr_h >= 400;
    int lw = (big ? 7 : 4) * FONT_W * scale + (big ? 16 : 8);
    m->x = ui_logo_x(scr_w, lw);
    if (m->x + menu_w > scr_w - 2) m->x = scr_w - menu_w - 2;
    if (m->x < 2) m->x = 2;
  }
  m->w = menu_w;
  m->item_h = menu_item_h;
  m->h = menu_item_h * m->n + 4;
  m->y = scr_h - bar_h - m->h;
  m->sub_w = 18 * FONT_W * scale + 16 + (ui_th.icons ? menu_item_h * 6 / 10 + 10 : 0);
  m->sub_h = menu_item_h * 7 + 4;
  m->sub_x = m->x + m->w - 4;
  m->sub_y = m->y + 3 * menu_item_h;
  if (m->sub_x + m->sub_w > scr_w - 2)
    m->sub_x = m->x - m->sub_w + 4;
  if (m->sub_y + m->sub_h > scr_h - bar_h)
    m->sub_y = scr_h - bar_h - m->sub_h;
  m->mode_w = (gfx_gop ? 21 : 10) * FONT_W * scale + 16;  /* GOP: "(reinicie)" */
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
  int ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  int ty = y + (h - ch) / 2;
  (void)col_win;
  (void)col_black;
  (void)col_title;
  (void)col_white;
  const char* ic = th_menu_icon(label);
  struct icn_ent* ie = ic ? icn_get(ic) : 0;
  int tx = x + 12;
  if (hot) fill_round(x + 2, y, w - 4, h, ui_th.modern ? 8 : 6, gui_col(UI_HOT));
  if (ie) {
    int is = h * 6 / 10;
    icn_draw(ie, x + 12, y + (h - is) / 2, is);
    tx += is + 10;
  }
  draw_text_t(tx, ty, label, gui_col(hot ? UI_HOT_TEXT : UI_TEXT), scale);
  if (arrow)
    draw_text_t(x + w - FONT_W * scale - 14, ty, ">", gui_col(UI_DIM), scale);
}

static void menu_frame(int x, int y, int w, int h, uint32_t edge) {
  (void)edge;
  round_frame(x, y, w, h, UI_RADIUS, gui_col(UI_BORDER));
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
static int at_res_alive(void);
static int at_res_task = -1; /* WINTAB.EXE residente, esperando o proximo Alt+Tab */

static int run_hidden(struct wm_win* w) {
  int i;
  if (!w || !gtasks || !gwins || w->kind != WM_KIND_RUN) return 0;
  for (i = 0; i < task_n; i++) {
    if (!gtasks[i].used || gtasks[i].run != w) continue;
    if (i == at_res_task && at_res_alive()) return 1; /* WINTAB parado: sem janela */
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

/* ---------------------------------------------------------------------------
 * Arraste rapido. Refazer a tela a cada movimento custa dois quadros inteiros
 * (papel de parede e copia para o video). No primeiro movimento um quadro
 * normal guarda a tela sem a janela (bg) e a imagem da janela (win); depois so
 * se escreve a regiao que mudou, uma copia por pixel. O que esta atras fica
 * congelado ate soltar: o quadro inteiro pendente sai quando o arraste termina.
 * ------------------------------------------------------------------------- */
#define DC_CLEAR 0x01010101u   /* canto arredondado: pixel que e do fundo */

static struct {
  int on;        /* bg e win prontos: use dc_present */
  int capture;   /* o proximo quadro inteiro guarda bg e win */
  int moved;     /* ha posicao nova para mostrar */
  int wm, gw;    /* janela arrastada: indice em wins ou em gwins (-1 = nao e) */
  int x, y, w, h;
  int cx, cy;    /* ultima posicao do cursor */
  int ylim;      /* linhas daqui para baixo sao da barra de tarefas: a janela fica atras */
  uint32_t* bg;
  uint32_t* win;
  uint32_t* line; /* linha com cursor ou canto: compoe aqui para nao piscar */
} dc = { .wm = -1, .gw = -1 };

static void dc_copy(volatile uint32_t* dst, const volatile uint32_t* src, uint32_t n) {
  __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
}

static void dc_end(void) {
  if (dc.bg) ufree(dc.bg);
  if (dc.win) ufree(dc.win);
  if (dc.line) ufree(dc.line);
  dc.bg = dc.win = dc.line = 0;
  dc.on = dc.capture = dc.moved = 0;
  dc.wm = dc.gw = -1;
}

/* A janela mudou de lugar. 1: dc_present cuida. 0: precisa do quadro inteiro
 * (o de captura, ou o caminho antigo se nao ha memoria para os buffers). */
static int dc_track(int wm, int gw, int w, int h) {
  uint32_t screen = gfx_w * gfx_h * 4u;
  if (dc.on) {
    dc.moved = 1;
    return 1;
  }
  if (dc.capture || !gfx_bochs || !um_ready()) return 0;
  /* A imagem guardada e a janela inteira (dc_capture_pos a poe toda na tela
   * para tirar a foto); maior que a tela fica no caminho antigo. */
  if (w > (int)gfx_w || h > (int)gfx_h) return 0;
  if (w < 1 || h < 1) return 0;
  dc.bg = (uint32_t*)umalloc_owned(screen, 0);
  dc.win = (uint32_t*)umalloc_owned((uint32_t)w * (uint32_t)h * 4u, 0);
  dc.line = (uint32_t*)umalloc_owned(gfx_w * 4u, 0);
  if (!dc.bg || !dc.win || !dc.line) {
    dc_end();
    return 0;
  }
  dc.wm = wm;
  dc.gw = gw;
  dc.w = w;
  dc.h = h;
  dc.ylim = (int)gfx_h - (c_bar_h > 0 ? c_bar_h : 0);
  dc.capture = 1;
  return 0;
}

/* Onde desenhar a janela para a foto: a posicao mais perto de (x,y) com a
 * janela toda na tela. Fora dela nao ha pixel para guardar. */
static void dc_capture_pos(int x, int y, int* sx, int* sy) {
  if (x + dc.w > (int)gfx_w) x = (int)gfx_w - dc.w;
  if (y + dc.h > (int)gfx_h) y = (int)gfx_h - dc.h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  *sx = x;
  *sy = y;
}

static void dc_snap_bg(void) {
  uint32_t y;
  for (y = 0; y < gfx_h; y++)
    dc_copy(dc.bg + y * gfx_w, (const volatile uint32_t*)gfx_at(0, (int)y), gfx_w);
}

/* A janela ja esta desenhada sobre o fundo guardado em dc_snap_bg. */
static void dc_snap_win(int x, int y, int cx, int cy) {
  int r, c;
  for (r = 0; r < dc.h; r++) {
    int sy = y + r;
    uint32_t* dst = dc.win + r * dc.w;
    for (c = 0; c < dc.w; c++) {
      int sx = x + c;
      if (sx < 0 || sy < 0 || sx >= (int)gfx_w || sy >= (int)gfx_h) {
        dst[c] = DC_CLEAR;
        continue;
      }
      dst[c] = *(const volatile uint32_t*)gfx_at(sx, sy);
      /* A barra de titulo tem cantos redondos: ali o fundo nao anda junto. */
      if (r < UI_RADIUS && dst[c] == dc.bg[(uint32_t)sy * gfx_w + (uint32_t)sx])
        dst[c] = DC_CLEAR;
    }
  }
  dc.x = x;
  dc.y = y;
  dc.cx = cx;
  dc.cy = cy;
  dc.capture = 0;
  dc.on = 1;
}

static void dc_grow(int* x0, int* y0, int* x1, int* y1, int x, int y, int w, int h) {
  if (x < *x0) *x0 = x;
  if (y < *y0) *y0 = y;
  if (x + w > *x1) *x1 = x + w;
  if (y + h > *y1) *y1 = y + h;
}

static void dc_rows(volatile uint8_t* base, uint32_t pitch, int x, int y, int x0, int y0,
                    int x1, int y1, int cx, int cy, int cur_rows);

/* janela arrastada em (x,y) com a sombra em volta */
static void dc_grow_sh(int* x0, int* y0, int* x1, int* y1, int x, int y) {
  dc_grow(x0, y0, x1, y1, x - SH_PL, y - SH_PT, dc.w + SH_PL + SH_PR, dc.h + SH_PT + SH_PB);
}

/* Logo depois de dc_snap_win: a foto foi tirada com a janela toda na tela em
 * (dc.x,dc.y). Refaz, na pagina em montagem, o fundo ali e a janela na posicao
 * de verdade (x,y), meio fora da tela e atras da barra de tarefas se for o
 * caso. O cursor vem depois, com o resto do quadro. */
static void dc_place(int x, int y) {
  int x0 = x, y0 = y, x1 = x + dc.w, y1 = y + dc.h;
  volatile uint8_t* base;
  uint32_t pitch;
  dc_grow(&x0, &y0, &x1, &y1, dc.x, dc.y, dc.w, dc.h);
  dc_grow_sh(&x0, &y0, &x1, &y1, x, y);
  dc_grow_sh(&x0, &y0, &x1, &y1, dc.x, dc.y);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > (int)gfx_w) x1 = (int)gfx_w;
  if (y1 > (int)gfx_h) y1 = (int)gfx_h;
  dc.x = x;
  dc.y = y;
  if (x0 >= x1 || y0 >= y1) return;
  if (gfx_offscreen) {
    base = gfx_back;
    pitch = gfx_w * 4u;
  } else {
    base = gfx_lfb + gfx_draw_y0 * gfx_pitch_bytes;
    pitch = gfx_pitch_bytes;
  }
  dc_rows(base, pitch, x, y, x0, y0, x1, y1, 0, 0, 0);
}

/* Compoe as linhas y0..y1 e colunas x0..x1 (ja dentro da tela) em base: fundo
 * guardado, janela em (x,y) cortada nas bordas e atras da barra de tarefas, e
 * cursor em (cx,cy) se cur_rows > 0. */
static void dc_rows(volatile uint8_t* base, uint32_t pitch, int x, int y, int x0, int y0,
                    int x1, int y1, int cx, int cy, int cur_rows) {
  int scale = cur_scale();
  int wx0 = x < x0 ? x0 : x;
  int wx1 = x + dc.w > x1 ? x1 : x + dc.w;
  uint32_t black = pix32(PIXA(0, 0, 0)), white = pix32(PIXA(255, 255, 255));
  int sh_on = gfx_bochs && dc.w >= 2 * UI_RADIUS && dc.h >= 2 * UI_RADIUS &&
              !(x <= 0 && y <= 0 && x + dc.w >= (int)gfx_w);
  int r;
  if (sh_on && !sh_ready) sh_init();
  for (r = y0; r < y1; r++) {
    const uint32_t* bg = dc.bg + (uint32_t)r * gfx_w;
    volatile uint32_t* out = (volatile uint32_t*)(base + (uint32_t)r * pitch);
    int in_win = r >= y && r < y + dc.h && r < dc.ylim && wx0 < wx1;
    const uint32_t* src = in_win ? dc.win + (r - y) * dc.w : 0;
    int corner = in_win && r - y < UI_RADIUS;
    int in_cur = r >= cy && r < cy + cur_rows;
    uint32_t ay = sh_on && r >= y - SH_PT && r < y + dc.h + SH_PB && r < dc.ylim
                    ? sh_row_a(r, y, dc.h) : 0;
    if (ay) {
      /* sombra: fundo escurecido, depois a janela por cima */
      int s0 = x - SH_PL > x0 ? x - SH_PL : x0;
      int s1 = x + dc.w + SH_PR < x1 ? x + dc.w + SH_PR : x1;
      dc_copy(dc.line + x0, bg + x0, (uint32_t)(x1 - x0));
      if (in_win && !corner) {
        sh_span(dc.line, s0, wx0 > s0 ? wx0 : s0, ay, x, x + dc.w);
        sh_span(dc.line, wx1 < s1 ? wx1 : s1, s1, ay, x, x + dc.w);
      } else if (s0 < s1) {
        sh_span(dc.line, s0, s1, ay, x, x + dc.w);
      }
    } else if (!in_cur && !corner) {
      /* Caso comum: cada pedaco vai direto, sem copia intermediaria. */
      if (!in_win) {
        dc_copy(out + x0, bg + x0, (uint32_t)(x1 - x0));
        continue;
      }
      if (x0 < wx0) dc_copy(out + x0, bg + x0, (uint32_t)(wx0 - x0));
      dc_copy(out + wx0, src + (wx0 - x), (uint32_t)(wx1 - wx0));
      if (wx1 < x1) dc_copy(out + wx1, bg + wx1, (uint32_t)(x1 - wx1));
      continue;
    }
    if (!ay) dc_copy(dc.line + x0, bg + x0, (uint32_t)(x1 - x0));
    if (corner) {
      int c;
      for (c = wx0; c < wx1; c++)
        if (src[c - x] != DC_CLEAR) dc.line[c] = src[c - x];
    } else if (in_win) {
      dc_copy(dc.line + wx0, src + (wx0 - x), (uint32_t)(wx1 - wx0));
    }
    if (in_cur) {
      const char* row = cur_row[(r - cy) / scale];
      int rx, ox;
      for (rx = 0; rx < CUR_W; rx++) {
        if (row[rx] == ' ') continue;
        for (ox = 0; ox < scale; ox++) {
          int px = cx + rx * scale + ox;
          if (px >= x0 && px < x1) dc.line[px] = row[rx] == 'W' ? white : black;
        }
      }
    }
    dc_copy(out + x0, dc.line + x0, (uint32_t)(x1 - x0));
  }
}

/* Escreve fundo, janela e cursor na pagina visivel, so na regiao que mudou. Sem
 * trocar de pagina: a outra volta no quadro inteiro, ao soltar. */
static void dc_present(int x, int y, int cx, int cy) {
  int scale = cur_scale();
  int cw = CUR_W * scale, chh = CUR_H * scale;
  int x0 = x, y0 = y, x1 = x + dc.w, y1 = y + dc.h;
  volatile uint8_t* base = gfx_lfb + gfx_front_y * gfx_pitch_bytes;
  uint32_t pitch = gfx_pitch_bytes;
  int cur_rows = gpu_hwcur ? 0 : chh;   /* cursor da placa: nao entra na imagem */
  dc_grow(&x0, &y0, &x1, &y1, dc.x, dc.y, dc.w, dc.h);
  dc_grow_sh(&x0, &y0, &x1, &y1, x, y);
  dc_grow_sh(&x0, &y0, &x1, &y1, dc.x, dc.y);
  dc_grow(&x0, &y0, &x1, &y1, dc.cx, dc.cy, cw, chh);
  dc_grow(&x0, &y0, &x1, &y1, cx, cy, cw, chh);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > (int)gfx_w) x1 = (int)gfx_w;
  if (y1 > (int)gfx_h) y1 = (int)gfx_h;
  dc.moved = 0;
  cursor_saved = 0;
  dc.x = x;
  dc.y = y;
  dc.cx = cx;
  dc.cy = cy;
  if (x0 >= x1 || y0 >= y1) return;
  /* com driver a tela e o back buffer; sem ele, a pagina visivel do LFB */
  if (gpu_drv && gfx_offscreen) {
    base = gfx_back;
    pitch = gfx_w * 4u;
  }
  dc_rows(base, pitch, x, y, x0, y0, x1, y1, cx, cy, cur_rows);
  if (gpu_drv && gfx_offscreen) {
    struct gpu_rect d;
    d.x = x0;
    d.y = y0;
    d.w = x1 - x0;
    d.h = y1 - y0;
    gfx_cur_shown.x = cx;
    gfx_cur_shown.y = cy;
    gfx_cur_shown.w = cur_rows ? cw : 0;
    gfx_cur_shown.h = cur_rows;
    gfx_gpu_show(&d, 1);
    if (gpu_hwcur) gfx_hwcur_move(cx, cy);
  }
}

/* ---- maximizar / restaurar (botao ao lado do X e duplo clique no titulo) */
static int max_bar_h(int scale) {
  return (vga_bios_font_ok ? FONT_H : 8) * scale + (gfx_h >= 400u ? 8 : 2);
}

/* Botao de maximizar: quadrado vazado; maximizada, dois quadrados (restaurar). */
static void title_max_btn(int x, int y, int s, int maxed) {
  int m = s / 4 > 2 ? s / 4 : 2;
  int t = s >= 20 ? 2 : 1;
  uint32_t fg = gui_col(UI_TTEXT);
  fill_round(x, y, s, s, ui_th.modern ? s / 2 : 5, gui_col(ui_th.tbtn));
  if (!maxed) {
    int bx = x + m, by = y + m, bw = s - 2 * m, bh = s - 2 * m;
    fill_rect(bx, by, bw, t * 2, fg);
    fill_rect(bx, by + bh - t, bw, t, fg);
    fill_rect(bx, by, t, bh, fg);
    fill_rect(bx + bw - t, by, t, bh, fg);
  } else {
    int d = s / 6 > 1 ? s / 6 : 2;
    int bw = s - 2 * m - d, bh = bw;
    int bx = x + m + d, by = y + m;
    /* de tras (so as bordas de cima e da direita aparecem) */
    fill_rect(bx, by, bw, t, fg);
    fill_rect(bx + bw - t, by, t, bh, fg);
    fill_rect(bx, by, t, d, fg);
    fill_rect(bx + bw - d, by + bh - t, d, t, fg);
    /* da frente */
    bx -= d;
    by += d;
    fill_rect(bx, by, bw, t * 2, fg);
    fill_rect(bx, by + bh - t, bw, t, fg);
    fill_rect(bx, by, t, bh, fg);
    fill_rect(bx + bw - t, by, t, bh, fg);
  }
}

/* Retangulo de "restaurar": 80% da largura e da altura da tela, centrado
 * acima da barra de tarefas. */
static void max_restore_rect(int scale, int* x, int* y, int* w, int* h) {
  int bar = max_bar_h(scale), x0, tw;
  gfx_tela_de(*x, &x0, &tw);   /* *x: um ponto da janela (o monitor dela) */
  *w = tw * 8 / 10;
  *h = (int)gfx_h * 8 / 10;
  if (*h > (int)gfx_h - bar) *h = (int)gfx_h - bar;
  *x = x0 + (tw - *w) / 2;
  *y = ((int)gfx_h - bar - *h) / 2;
  if (*y < 0) *y = 0;
}

static int gw_is_max(struct gui_win* g) {
  int x0, tw;
  gfx_tela_de(g->ox + g->cw / 2, &x0, &tw);
  return g->maxed || (g->ox <= x0 && g->oy <= 0 && g->cw >= tw);
}

/* Troca o tamanho da area do programa: novo buffer com o que cabia do
 * antigo; o programa recebe GUI_EV_RESIZE e redesenha no proximo quadro. */
static int gw_set_rect(struct gui_win* g, int x, int y, int cw, int ch) {
  uint32_t* np;
  int yy, cpw, cph;
  if (cw < 160) cw = 160;
  if (ch < 48) ch = 48;
  np = (uint32_t*)umalloc_owned((uint32_t)cw * (uint32_t)ch * 4u, 0);
  if (!np) return -1;
  fill32(np, 0xFFFFFFu, (uint32_t)cw * (uint32_t)ch);
  cpw = cw < g->cw ? cw : g->cw;
  cph = ch < g->ch ? ch : g->ch;
  if (g->pix)
    for (yy = 0; yy < cph; yy++) {
      int xx;
      for (xx = 0; xx < cpw; xx++) np[yy * cw + xx] = g->pix[yy * g->cw + xx];
    }
  if (g->pix) ufree(g->pix);
  g->pix = np;
  g->ox = x;
  g->oy = y;
  g->cw = cw;
  g->ch = ch;
  gq_push(g, GUI_EV_RESIZE, cw, ch, 0);
  win_api_dirty = 1;
  return 0;
}

static void gw_toggle_max(struct gui_win* g) {
  int scale = g->scale > 0 ? g->scale : 1;
  if (gw_is_max(g)) {
    int x = g->ox + g->cw / 2, y, w, h;
    max_restore_rect(scale, &x, &y, &w, &h);
    if (gw_set_rect(g, x, y, w, h - g->title_h) == 0) g->maxed = 0;
  } else {
    int h = (int)gfx_h - max_bar_h(scale), x0, tw;
    gfx_tela_de(g->ox + g->cw / 2, &x0, &tw);
    if (gw_set_rect(g, x0, 0, tw, h - g->title_h) == 0) g->maxed = 1;
  }
}

static int wm_is_max(struct wm_win* w) {
  int x0, tw;
  gfx_tela_de(w->x + w->w / 2, &x0, &tw);
  return w->x <= x0 && w->y <= 0 && w->w >= tw;
}

static void wm_toggle_max(struct wm_win* w, int scale) {
  if (wm_is_max(w)) {
    w->x += w->w / 2;
    max_restore_rect(scale, &w->x, &w->y, &w->w, &w->h);
  } else {
    int x0, tw;
    gfx_tela_de(w->x + w->w / 2, &x0, &tw);
    w->x = x0;
    w->y = 0;
    w->w = tw;
    w->h = (int)gfx_h - max_bar_h(scale);
  }
  win_api_dirty = 1;
}

static void gui_draw_frame(struct gui_win* w, int active, uint32_t col_title, uint32_t col_white,
                           uint32_t col_black, uint32_t col_btn, uint32_t col_inactive) {
  int scale = w->scale > 0 ? w->scale : 1;
  int ch = glyph_rows() * scale;
  int close_s = w->title_h - 4;
  int close_x = w->ox + w->cw - close_s - TB_PAD;
  int ty = w->oy + (w->title_h - ch) / 2;
  int xoff = close_s > FONT_W * scale ? (close_s - FONT_W * scale) / 2 : 0;
  int hh = w->title_h + w->ch;
  (void)col_title;
  (void)col_inactive;
  if (w->panel) {
    gui_blit_win(w);
    return;
  }
  round_top(w->ox, w->oy, w->cw, w->title_h, UI_RADIUS,
            gui_col(active ? UI_TITLE_A : UI_TITLE_I));
  if (wf_on())
    wf_text(w->ox + 12, w->oy + (w->title_h - wfont_height()) / 2, w->title, gui_col(UI_TTEXT),
            close_x - close_s - 10);
  else
    draw_text_t(w->ox + 12, ty, w->title, gui_col(UI_TTEXT), scale);
  fill_round(close_x, w->oy + 3, close_s, close_s, ui_th.modern ? close_s / 2 : 5,
             gui_col(ui_th.close));
  draw_text_t(close_x + xoff, ty, "x", gui_col(ui_th.modern ? 0xFFFFFFu : UI_TEXT), scale);
  title_max_btn(close_x - close_s - 4, w->oy + 3, close_s, gw_is_max(w));
  gui_blit_win(w);
  (void)col_black;
  round_top_frame(w->ox, w->oy, w->cw, hh, UI_RADIUS, gui_col(UI_BORDER));
}

/* Com paint_only, paint_stacked so desenha o que esta da janela paint_z para
 * cima e encosta no retangulo: o resto da tela nao muda. */
static int paint_only, paint_z, paint_x, paint_y, paint_w, paint_h;

static int paint_skip(int z, int x, int y, int w, int h) {
  if (!paint_only) return 0;
  /* a sombra de uma janela da frente pode cair no retangulo sem a janela */
  x -= SH_PL;
  y -= SH_PT;
  w += SH_PL + SH_PR;
  h += SH_PT + SH_PB;
  return z < paint_z || x >= paint_x + paint_w || x + w <= paint_x ||
         y >= paint_y + paint_h || y + h <= paint_y;
}

/* Sombra antes de pintar a janela de nivel z. No quadro inteiro o fundo acabou
 * de ser pintado. Com paint_only so o retangulo da janela paint_z foi refeito
 * (e sem os cantos redondos de cima, que sao fundo velho, ja com sombra):
 * corta ali, senao a sombra escurece de novo a cada redesenho. */
static void paint_shadow(int z, int x, int y, int w, int h) {
  int px0 = paint_x, py0 = paint_y, px1 = paint_x + paint_w, py1 = paint_y + paint_h;
  if (!paint_only) {
    win_shadow(x, y, w, h, 0, 0, (int)gfx_w, (int)gfx_h, (int)gfx_h);
    return;
  }
  if (z == paint_z) return;
  win_shadow(x, y, w, h, px0, py0 + UI_RADIUS, px1, py1, (int)gfx_h);
  win_shadow(x, y, w, h, px0 + UI_RADIUS, py0, px1 - UI_RADIUS, py0 + UI_RADIUS, (int)gfx_h);
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
    if (dc.capture && dc.wm == i) continue;
    if (paint_skip(wins[i].z, wins[i].x, wins[i].y, wins[i].w, wins[i].h)) continue;
    order[n] = i;
    kind[n] = 1;
    n++;
  }
  for (i = 0; gwins && i < GUI_MAX; i++) {
    if (!gwins[i].open) continue;
    if (dc.capture && dc.gw == i) continue;
    if (paint_skip(gwins[i].z, gwins[i].ox, gwins[i].oy, gwins[i].cw,
                   gwins[i].title_h + gwins[i].ch)) continue;
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
    if (kind[i] == 1) {
      struct wm_win* sw = &wins[order[i]];
      paint_shadow(sw->z, sw->x, sw->y, sw->w, sw->h);
    } else {
      struct gui_win* sg = &gwins[order[i]];
      if (!sg->panel) paint_shadow(sg->z, sg->ox, sg->oy, sg->cw, sg->title_h + sg->ch);
    }
    if (kind[i] == 1)
      wm_draw_window(&wins[order[i]], wins[order[i]].z == top && top > 0, title_h, scale,
                     col_win, col_title, col_white, col_black, col_btn, col_inactive);
    else
      gui_draw_frame(&gwins[order[i]], gwins[order[i]].z == top && top > 0,
                     col_title, col_white, col_black, col_btn, col_inactive);
  }
}

/* ---------------------------------------------------------------------------
 * Alt+Tab. O WIN so guarda o pedido e as miniaturas; quem mostra a pilha e
 * escolhe e o WINTAB.EXE (\SODOS), que so roda enquanto o Alt esta apertado.
 * Ao apertar, cada janela e desenhada sozinha no quadro e reduzida para uma
 * miniatura (a de tras aparece inteira, sem o que a cobre); o quadro inteiro
 * sai de novo logo depois. Sem o WINTAB.EXE, Alt+Tab so passa para a proxima.
 * O primeiro Alt+Tab carrega o WINTAB.EXE; depois ele fica na memoria, parado
 * em k_alttab_wait (sem CPU), e os seguintes so o acordam (at_seq).
 * ------------------------------------------------------------------------- */
#define AT_MAX (WM_MAX + GUI_MAX)
#define AT_TW 360
#define AT_TH 240
#define AT_GUI 1000 /* id de janela de programa: AT_GUI + indice em gwins */

struct at_shot {
  int id;
  int x, y, w, h;
  int tw, th;
  char title[64];
  uint32_t* pix; /* 0xRRGGBB, tw x th */
};

static struct at_shot at_list[AT_MAX];
static int at_n;
static int at_on;        /* Alt+Tab em curso */
static int at_req;       /* o laco do WIN tira as miniaturas e chama o WINTAB */
static int at_tabs;      /* Tabs desde o Alt (Shift+Tab desconta) */
static int at_alt;       /* o Alt continua apertado */
static int at_gid = -1;  /* janela (painel) do WINTAB */
static int at_act = -1;  /* janela pedida por k_win_activate */
static uint32_t at_t0;
static int at_held;      /* Alt apertado, pelos bytes que o WIN ja leu */
static volatile int at_seq; /* +1 a cada Alt+Tab com miniaturas prontas */
static int at_res_pid = -1;

/* O WINTAB residente ainda e a mesma tarefa (nao saiu nem foi morto). */
static int at_res_alive(void) {
  struct gui_task* t;
  if (at_res_task < 0 || at_res_task >= TASK_MAX || !gtasks) return 0;
  t = &gtasks[at_res_task];
  if (!t->used || t->wake == WAKE_DEAD || (int)t->cpu.pid != at_res_pid) {
    at_res_task = -1;
    return 0;
  }
  return 1;
}

/* Tela em RAM (gfx_back, 32 bpp) com o quadro inteiro: ler dela e rapido.
 * Ler a memoria de video, como get_pixel faz fora do quadro, e lentissimo na
 * placa de verdade (GOP do notebook): o Alt+Tab levava segundos. */
static int at_back_ok(void) { return gfx_bochs && gfx_back && !gfx_dbuf; }

static void at_copy32(void* dst, const void* src, uint32_t n) {
  __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

/* Pixel do quadro em 0xRRGGBB (get_pixel devolve no formato do PIXA). */
static uint32_t at_rgb(int x, int y) {
  uint32_t v;
  if (at_back_ok()) {
    if (x < 0 || y < 0 || (uint32_t)x >= gfx_w || (uint32_t)y >= gfx_h) return 0;
    return ((const uint32_t*)gfx_back)[(uint32_t)y * gfx_w + (uint32_t)x] & 0xFFFFFFu;
  }
  v = get_pixel(x, y);
  if (gfx_bochs) return (v & 0xFFu) << 16 | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
  return gui_ega[v & 15u];
}

/* Reduz o retangulo (x,y,w,h) do quadro para s->tw x s->th, media de ate 3x3. */
static void at_grab(struct at_shot* s, int x, int y, int w, int h) {
  int tx, ty;
  for (ty = 0; ty < s->th; ty++) {
    int sy0 = y + ty * h / s->th, sy1 = y + (ty + 1) * h / s->th;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (tx = 0; tx < s->tw; tx++) {
      int sx0 = x + tx * w / s->tw, sx1 = x + (tx + 1) * w / s->tw;
      uint32_t r = 0, g = 0, b = 0, n = 0;
      int dx = (sx1 - sx0 + 2) / 3, dy = (sy1 - sy0 + 2) / 3, px, py;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      if (dx < 1) dx = 1;
      if (dy < 1) dy = 1;
      for (py = sy0; py < sy1; py += dy)
        for (px = sx0; px < sx1; px += dx) {
          uint32_t c = at_rgb(px, py);
          r += (c >> 16) & 0xFFu;
          g += (c >> 8) & 0xFFu;
          b += c & 0xFFu;
          n++;
        }
      if (!n) n = 1;
      s->pix[ty * s->tw + tx] = (r / n) << 16 | (g / n) << 8 | (b / n);
    }
  }
}

static void at_free(void) {
  int i;
  for (i = 0; i < at_n; i++) {
    if (at_list[i].pix) ufree(at_list[i].pix);
    at_list[i].pix = 0;
  }
  at_n = 0;
}

static void at_end(void) {
  at_free();
  at_on = 0;
  at_req = 0;
  at_alt = 0;
  at_tabs = 0;
  at_gid = -1;
}

/* Lista da frente para tras e miniaturas. Desenha por cima do quadro atual:
 * quem chama pede o quadro inteiro depois. */
static void at_snap(struct wm_win* wins, int title_h, int scale, uint32_t col_win,
                    uint32_t col_title, uint32_t col_white, uint32_t col_black,
                    uint32_t col_btn, uint32_t col_inactive) {
  int used[AT_MAX];
  int i, top = stack_top_z(wins);
  /* Cada janela e desenhada sozinha na tela em RAM e reduzida; depois a tela
   * em RAM volta como estava (nada disso chega a tela de verdade). */
  uint32_t* save = 0;
  uint32_t npx = gfx_w * gfx_h;
  int old_off = gfx_offscreen;
  uint32_t old_y0 = gfx_draw_y0;
  uint32_t t0 = (uint32_t)irq_ticks();
  if (at_back_ok()) save = (uint32_t*)umalloc_top(npx * 4u);
  if (save) {
    at_copy32(save, gfx_back, npx);
    gfx_offscreen = 1;
    gfx_draw_y0 = 0;
  }
  at_free();
  for (i = 0; i < AT_MAX; i++) used[i] = 0;
  for (;;) {
    int best = -1, bz = -1;
    struct at_shot* s;
    int x, y, w, h;
    for (i = 0; i < AT_MAX; i++) {
      int z;
      if (used[i]) continue;
      if (i < WM_MAX) {
        if (!wins[i].used || run_hidden(&wins[i])) continue;
        z = wins[i].z;
      } else {
        struct gui_win* g = gwins ? &gwins[i - WM_MAX] : 0;
        if (!g || !g->open || g->panel) continue;
        z = g->z;
      }
      if (z > bz) {
        bz = z;
        best = i;
      }
    }
    if (best < 0 || at_n >= AT_MAX) break;
    used[best] = 1;
    s = &at_list[at_n];
    if (best < WM_MAX) {
      struct wm_win* ww = &wins[best];
      s->id = best;
      x = ww->x;
      y = ww->y;
      w = ww->w;
      h = ww->h;
      app_copy(s->title, (int)sizeof(s->title), ww->title);
      wm_draw_window(ww, ww->z == top, title_h, scale, col_win, col_title, col_white, col_black,
                     col_btn, col_inactive);
    } else {
      struct gui_win* g = &gwins[best - WM_MAX];
      s->id = AT_GUI + best - WM_MAX;
      x = g->ox;
      y = g->oy;
      w = g->cw;
      h = g->title_h + g->ch;
      app_copy(s->title, (int)sizeof(s->title), g->title);
      gui_draw_frame(g, g->z == top, col_title, col_white, col_black, col_btn, col_inactive);
    }
    s->x = x;
    s->y = y;
    s->w = w;
    s->h = h;
    /* so o pedaco na tela (a janela pode estar arrastada para fora) */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)gfx_w) w = (int)gfx_w - x;
    if (y + h > (int)gfx_h) h = (int)gfx_h - y;
    if (w < 8 || h < 8) continue;
    s->tw = AT_TW;
    s->th = h * AT_TW / w;
    if (s->th > AT_TH) {
      s->th = AT_TH;
      s->tw = w * AT_TH / h;
    }
    if (s->tw < 1) s->tw = 1;
    if (s->th < 1) s->th = 1;
    s->pix = (uint32_t*)umalloc_owned((uint32_t)(s->tw * s->th * 4), 0);
    if (!s->pix) continue;
    at_grab(s, x, y, w, h);
    at_n++;
  }
  if (save) {
    at_copy32(gfx_back, save, npx);
    ufree(save);
    gfx_offscreen = old_off;
    gfx_draw_y0 = old_y0;
  }
  serial_puts(save ? "[alttab] miniaturas (RAM) ms=" : "[alttab] miniaturas (video) ms=");
  {
    char num[16];
    int32_to_string((int32_t)((uint32_t)irq_ticks() - t0), num, (int)sizeof(num));
    serial_puts(num);
    serial_puts("\n");
  }
}

/* --- chamadas do WINTAB.EXE (k_win_*, k_alttab, k_gui_panel) --- */

int win_list_api(struct k_win_info* out, int max) {
  int i;
  if (!out || max <= 0) return at_n;
  for (i = 0; i < at_n && i < max; i++) {
    out[i].id = at_list[i].id;
    out[i].x = at_list[i].x;
    out[i].y = at_list[i].y;
    out[i].w = at_list[i].w;
    out[i].h = at_list[i].h;
    out[i].tw = at_list[i].tw;
    out[i].th = at_list[i].th;
    app_copy(out[i].title, (int)sizeof(out[i].title), at_list[i].title);
  }
  return i;
}

/* Miniatura i reduzida/ampliada para w x h, RGBA (A = 255). */
int win_thumb_api(int i, void* rgba, int w, int h) {
  struct at_shot* s;
  uint8_t* d = (uint8_t*)rgba;
  int x, y;
  if (i < 0 || i >= at_n || !d || w < 1 || h < 1) return -1;
  s = &at_list[i];
  for (y = 0; y < h; y++) {
    const uint32_t* row = s->pix + (y * s->th / h) * s->tw;
    for (x = 0; x < w; x++) {
      uint32_t c = row[x * s->tw / w];
      d[0] = (uint8_t)(c >> 16);
      d[1] = (uint8_t)(c >> 8);
      d[2] = (uint8_t)c;
      d[3] = 255;
      d += 4;
    }
  }
  return 0;
}

/* O que esta na tela agora (antes do painel abrir), RGBA. */
int win_shot_api(int x, int y, int w, int h, void* rgba) {
  uint8_t* d = (uint8_t*)rgba;
  int px, py;
  if (!gfx_live || !d || w < 1 || h < 1) return -1;
  for (py = y; py < y + h; py++)
    for (px = x; px < x + w; px++) {
      uint32_t c = at_rgb(px, py);
      d[0] = (uint8_t)(c >> 16);
      d[1] = (uint8_t)(c >> 8);
      d[2] = (uint8_t)c;
      d[3] = 255;
      d += 4;
    }
  return 0;
}

/* O laco do WIN poe a janela na frente (ele tem o foco das janelas internas). */
int win_activate_api(int id) {
  at_act = id;
  win_api_dirty = 1;
  return 0;
}

int alttab_api(int* tabs) {
  if (tabs) *tabs = at_tabs;
  return at_on && at_alt;
}

/* WINTAB residente: marca a tarefa e espera at_seq sair de seq (ou ms).
 * Devolve at_seq; -1 fora de uma tarefa do WIN (ai o WINTAB sai). */
int alttab_wait_api(int seq, int ms) {
  if (!gtasks || task_cur < 0 || !host_on || kt_in_thread()) return -1;
  at_res_task = task_cur;
  at_res_pid = (int)gtasks[task_cur].cpu.pid;
  if (seq == at_seq && ms > 0) gui_coop_wait(&at_seq, seq, (uint32_t)ms);
  return at_seq;
}

/* Janela sem barra de titulo nem moldura, no retangulo pedido (painel). */
int gui_panel_open(const char* title, int x, int y, int w, int h) {
  int rc = gui_open(title);
  uint32_t* pix;
  if (rc != 0) return rc;
  if (w < 16) w = 16;
  if (h < 16) h = 16;
  if (w > (int)gfx_w) w = (int)gfx_w;
  if (h > (int)gfx_h) h = (int)gfx_h;
  pix = (uint32_t*)umalloc_owned((uint32_t)w * (uint32_t)h * 4u, 0);
  if (!pix) {
    gui_close();
    return -2;
  }
  ufree(gui.pix);
  gui.pix = pix;
  fill32(gui.pix, 0, (uint32_t)w * (uint32_t)h);
  gui.ox = x;
  gui.oy = y;
  gui.cw = w;
  gui.ch = h;
  gui.title_h = 0;
  gui.panel = 1;
  if (at_on) at_gid = gui_ix;
  win_api_dirty = 1;
  return 0;
}

#define HIT_NONE 0
#define HIT_WM   1
#define HIT_GUI  2

static int z_top(struct wm_win* wins, int x, int y, int* kind) {
  int i, best = -1, bk = HIT_NONE, bz = -1;
  /* A barra de tarefas fica na frente: a parte de uma janela arrastada para
   * baixo dela nao recebe clique nem roda. */
  if (c_wins && c_bar_h > 0 && y >= (int)gfx_h - c_bar_h) {
    if (kind) *kind = HIT_NONE;
    return -1;
  }
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

/* 1 = nenhuma janela no ponto: o clique pode chegar aos icones do desktop. */
static int z_empty(struct wm_win* wins, int x, int y) {
  int hk = HIT_NONE;
  z_top(wins, x, y, &hk);
  return hk == HIT_NONE;
}

static void chrome_unbind(void) {
  dc_end();
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

/* Comando interno de tela cheia (EDIT): tarefa como um .EXE, com a tela da janela. */
static int cmd_spawn_screen(int (*cmd)(int, char**), int argc, char** argv) {
  struct cmd_con* c = cons_cap;
  if (!host_on || task_cur >= 0 || !c_wins || !c) return 0;
  c->busy = 1;
  task_pending_cmd = cmd;
  if (task_spawn(c_wins, c_focus, c_big, argv && argv[0] ? argv[0] : "EDIT", 0, argc, argv, c) != 0) {
    task_pending_cmd = 0;
    c->busy = 0;
    return 0;
  }
  task_pending_cmd = 0;
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
  gfx_desk_clear();
  if (!c_wins) return;
  if (!th_desk_icon("DOS", 12, 12, c_icon)) fill_rect(12, 12, c_icon, c_icon, c_win);
  desk_label(12, c_icon, 12 + c_icon + 2, "DOS", desk_mark == DESK_MARK_FIXED + 0, c_white, c_black,
             c_scale);
  if (!th_desk_icon("ARQ", 12, c_icon2_y, c_icon))
    fill_rect(12, c_icon2_y, c_icon, c_icon, c_title);
  desk_label(12, c_icon, c_icon2_y + c_icon + 2, "ARQ", desk_mark == DESK_MARK_FIXED + 1, c_white,
             c_black, c_scale);
  if (!th_desk_icon("WEB", 12, c_icon3_y, c_icon))
    draw_nav_icon(12, c_icon3_y, c_icon, c_title, c_white);
  desk_label(12, c_icon, c_icon3_y + c_icon + 2, "WEB", desk_mark == DESK_MARK_FIXED + 2, c_white,
             c_black, c_scale);
  if (!th_desk_icon("CMD", 12, c_icon4_y, c_icon))
    draw_cmd_icon(12, c_icon4_y, c_icon, c_white, c_black);
  desk_label(12, c_icon, c_icon4_y + c_icon + 2, "CMD", desk_mark == DESK_MARK_FIXED + 3, c_white,
             c_black, c_scale);
  desk_draw(c_icon, (vga_bios_font_ok ? FONT_H : 8) * c_scale, c_scale, w, h, c_bar_h,
            c_desk, c_white, c_black);
  paint_stacked(c_wins, c_title_h, c_scale, c_win, c_title, c_white, c_black, c_btn, c_inactive);
}

/* Fundo dos menus: opaco ou translucido conforme a opcao "glass". */
static void menu_panel(int x, int y, int w, int h) {
  round_rect(x, y, w, h, UI_RADIUS, gui_col(UI_PANEL), ui_glass ? 215 : 255);
}

/* X do botao "S" na barra: 0 = esquerda, 1 = junto ao painel (lateral direita). */
static int gui_panel_left(void);

static int ui_logo_x(int w, int lw) {
  if (ui_menu_pos == 1) {
    int px = gui_panel_left();
    if (px > lw + 40) return px - lw - 10;
    return w - lw - 8;
  }
  return 6;
}


/* ---------------------------------------------------------------------------
 * Barra de tarefas com programas (2026-11-19). Cada programa aberto ganha um
 * botao com icone; varias janelas do mesmo programa ficam empilhadas no mesmo
 * botao (como no Windows) e o clique passa de uma para a outra. Um tracinho
 * embaixo marca o que esta aberto (comprido no da frente). Botao direito
 * fixa ou desafixa o programa: os fixados ficam na barra mesmo fechados (o
 * clique abre) e vao para D:\SODOS\BARRA.CFG, um por linha. Programas do
 * proprio WIN ficam como "#Nome" (#Command, #Explorer...).
 * ------------------------------------------------------------------------- */
#define TB_MAX 16
#define TB_KEY 80
#define TB_CFG "D:\\SODOS\\BARRA.CFG"
struct tb_item {
  char key[TB_KEY];
  char nome[16];
  int fixo, n, foco;
};
static struct tb_item tb[TB_MAX];
static int tb_n;
static char tb_fix[TB_MAX][TB_KEY];
static int tb_nfix, tb_fix_lido;

static int tb_eq(const char* a, const char* b) {
  int i = 0;
  for (;; i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
    if (!x) return 1;
  }
}

static void tb_cp(char* d, const char* o, int max) {
  int i = 0;
  while (o[i] && i < max - 1) {
    d[i] = o[i];
    i++;
  }
  d[i] = 0;
}

static void tb_fix_load(void) {
  char b[TB_MAX * TB_KEY];
  int n, i = 0;
  tb_fix_lido = 1;
  tb_nfix = 0;
  n = file_load(TB_CFG, b, (int)sizeof(b) - 1);
  if (n <= 0) return;
  b[n] = 0;
  while (i < n && tb_nfix < TB_MAX) {
    int k = 0;
    while (i < n && (b[i] == '\r' || b[i] == '\n')) i++;
    while (i < n && b[i] != '\r' && b[i] != '\n' && k < TB_KEY - 1) tb_fix[tb_nfix][k++] = b[i++];
    while (i < n && b[i] != '\n') i++;
    tb_fix[tb_nfix][k] = 0;
    if (k > 0) tb_nfix++;
  }
}

static void tb_fix_save(void) {
  char b[TB_MAX * (TB_KEY + 2)];
  int n = 0, i, k;
  for (i = 0; i < tb_nfix; i++) {
    for (k = 0; tb_fix[i][k]; k++) b[n++] = tb_fix[i][k];
    b[n++] = '\r';
    b[n++] = '\n';
  }
  (void)drives_mkdir("D:\\SODOS");
  if (file_save(TB_CFG, b, n) != 0) serial_puts("[visual] BARRA.CFG nao gravou\n");
}

/* nome curto: "D:\X\TELAS.EXE" -> "TELAS"; "#Command" -> "Command" */
static void tb_nome(const char* key, char* out) {
  const char* b = key;
  int i, k = 0;
  if (key[0] == '#') {
    tb_cp(out, key + 1, 16);
    return;
  }
  for (i = 0; key[i]; i++)
    if (key[i] == '\\' || key[i] == '/' || key[i] == ':') b = key + i + 1;
  while (b[k] && b[k] != '.' && k < 15) {
    out[k] = b[k];
    k++;
  }
  out[k] = 0;
}

static int tb_add(const char* key, int fixo) {
  int i;
  for (i = 0; i < tb_n; i++)
    if (tb_eq(tb[i].key, key)) return i;
  if (tb_n >= TB_MAX) return -1;
  tb_cp(tb[tb_n].key, key, TB_KEY);
  tb_nome(key, tb[tb_n].nome);
  tb[tb_n].fixo = fixo;
  tb[tb_n].n = 0;
  tb[tb_n].foco = 0;
  return tb_n++;
}

/* chave do programa de uma janela */
static void tb_key_wm(const struct wm_win* w, char* key) {
  const char* t = w->title;
  if (w->kind == WM_KIND_MGR) t = "Gerenciador";
  else if (w->kind == WM_KIND_NOTE) t = "Bloco de notas";
  else if (w->kind == WM_KIND_CMD) t = "Command";
  else if (w->kind == WM_KIND_DISP) t = "Configuracoes de tela";
  else if (w->kind == WM_KIND_FONT) t = "Fontes";
  key[0] = '#';
  tb_cp(key + 1, t, TB_KEY - 1);
}

static void tb_key_gui(int g, char* key) {
  int t;
  for (t = 0; gtasks && t < task_n; t++)
    if (gtasks[t].used && gtasks[t].gid == g && gtasks[t].path[0]) {
      tb_cp(key, gtasks[t].path, TB_KEY);
      return;
    }
  key[0] = '#';
  tb_cp(key + 1, gwins[g].title, TB_KEY - 1);
}

/* fixados primeiro (na ordem do arquivo), depois os abertos */
static void tb_monta(void) {
  char key[TB_KEY];
  int i, j;
  if (!tb_fix_lido) tb_fix_load();
  tb_n = 0;
  for (i = 0; i < tb_nfix; i++) (void)tb_add(tb_fix[i], 1);
  for (i = 0; c_wins && i < WM_MAX; i++) {
    if (!c_wins[i].used || run_hidden(&c_wins[i])) continue;
    tb_key_wm(&c_wins[i], key);
    if ((j = tb_add(key, 0)) < 0) continue;
    tb[j].n++;
    if (gui_focus < 0 && c_focus && *c_focus == i) tb[j].foco = 1;
  }
  for (i = 0; gwins && i < GUI_MAX; i++) {
    if (!gwins[i].open || gwins[i].panel) continue;
    tb_key_gui(i, key);
    if ((j = tb_add(key, 0)) < 0) continue;
    tb[j].n++;
    if (gui_focus == i) tb[j].foco = 1;
  }
}

/* x do primeiro botao e lado do botao */
static int tb_x0(int w, int scale) {
  int lw = c_start_w > 0 ? c_start_w : 44, cw = FONT_W * scale;
  if (ui_menu_pos == 1) return 10;
  return ui_logo_x(w, lw) + lw + 14 + 5 * cw + 24;
}

static int tb_lado(int bar_h) { return bar_h - 12 > 16 ? bar_h - 12 : 16; }

/* botao sob (x, y); -1 = nenhum */
static int tb_em(int x, int y) {
  int w = (int)gfx_w, h = (int)gfx_h, bh = c_bar_h, s, i, x0;
  if (!c_wins || bh <= 0 || y < h - bh || y >= h) return -1;
  tb_monta();
  s = tb_lado(bh);
  x0 = tb_x0(w, c_scale > 0 ? c_scale : 1);
  for (i = 0; i < tb_n; i++) {
    int bx = x0 + i * (s + 12);
    if (x >= bx - 4 && x < bx + s + 4) return i;
  }
  return -1;
}

static void tb_icone(const struct tb_item* t, int x, int y, int s, int scale) {
  static const uint32_t cores[6] = { 0x2F6FB0, 0x7A4FB0, 0x2E8B57, 0xB06A2F, 0xA8324A, 0x3A8C99 };
  char l[3];
  uint32_t hsh = 0;
  int i, cw = FONT_W * scale, ch = (vga_bios_font_ok ? FONT_H : 8) * scale;
  if (th_desk_icon(t->nome, x, y, s)) return;
  for (i = 0; t->nome[i]; i++) hsh = hsh * 31u + (uint8_t)t->nome[i];
  fill_round(x, y, s, s, s / 5, gui_col(cores[hsh % 6u]));
  l[0] = t->nome[0] ? t->nome[0] : '?';
  if (l[0] >= 'a' && l[0] <= 'z') l[0] = (char)(l[0] - 32);
  l[1] = 0;
  draw_text_t(x + (s - cw) / 2, y + (s - ch) / 2, l, 0xFFFFFFu, scale);
}

static void tb_desenha(int w, int h, int bar_h, int scale) {
  int s = tb_lado(bar_h), x = tb_x0(w, scale), by = h - bar_h, i, lim = w - 320;
  if (!c_wins) return;
  tb_monta();
  for (i = 0; i < tb_n && x + s < lim; i++, x += s + 12) {
    struct tb_item* t = &tb[i];
    int y = by + (bar_h - s) / 2 - 1;
    if (t->foco) fill_round(x - 4, by + 3, s + 8, bar_h - 6, 6, gui_col(UI_BORDER));
    if (t->n > 1) {   /* empilhadas: duas folhas atras */
      if (t->n > 2) fill_round(x + 6, y - 6, s, s, s / 5, gui_col(UI_BORDER));
      fill_round(x + 3, y - 3, s, s, s / 5, gui_col(UI_DIM));
    }
    tb_icone(t, x, y, s, scale);
    if (t->n > 0) {
      int lw = t->foco ? s / 2 : 6;
      fill_round(x + (s - lw) / 2, h - 4, lw, 3, 1, gui_col(UI_ACCENT));
    }
  }
}

/* Clique num botao da barra. 1 = tratado. Esquerdo: abre o fixado fechado
 * ou traz para a frente a proxima janela do programa. Direito (tb_direito):
 * fixa ou desafixa. */
static int tb_clique(int x, int y) {
  int i = tb_em(x, y), k, prox = -1, prox_gui = 0, achou_foco = 0, prim = -1, prim_gui = 0;
  char key[TB_KEY];
  struct tb_item* t;
  if (i < 0) return 0;
  t = &tb[i];
  if (tb_direito) {
    for (k = 0; k < tb_nfix; k++)
      if (tb_eq(tb_fix[k], t->key)) break;
    if (k < tb_nfix) {
      for (; k + 1 < tb_nfix; k++) tb_cp(tb_fix[k], tb_fix[k + 1], TB_KEY);
      tb_nfix--;
      app_status_set("removido da barra");
    } else if (tb_nfix < TB_MAX) {
      tb_cp(tb_fix[tb_nfix++], t->key, TB_KEY);
      app_status_set("fixado na barra");
    }
    tb_fix_save();
    win_api_dirty = 1;
    return 1;
  }
  if (t->n == 0) {
    if (t->key[0] != '#') wm_exec(c_wins, c_focus, c_big, t->key);
    else if (tb_eq(t->key, "#Command")) wm_open_command(c_wins, c_focus, c_big);
    else if (tb_eq(t->key, "#Bloco de notas")) wm_open_notepad(c_wins, c_focus, c_big);
    else if (tb_eq(t->key, "#Gerenciador")) wm_open_mgr(c_wins, c_focus, c_big);
    else if (tb_eq(t->key, "#Explorer")) wm_open_explorer(c_wins, c_focus, c_big);
    else if (tb_eq(t->key, "#Navegador")) wm_open_browser(c_wins, c_focus, c_big);
    else if (tb_eq(t->key, "#Configuracoes de tela")) wm_open_display(c_wins, c_focus, c_big);
    else if (tb_eq(t->key, "#Fontes")) wm_open_fonts(c_wins, c_focus, c_big);
    else app_status_set("nao sei abrir este atalho");
    win_api_dirty = 1;
    return 1;
  }
  /* a janela seguinte do programa, depois da que esta na frente */
  for (k = 0; k < WM_MAX + GUI_MAX; k++) {
    int g = k >= WM_MAX, ix = g ? k - WM_MAX : k, eh_foco;
    if (g) {
      if (!gwins || !gwins[ix].open || gwins[ix].panel) continue;
      tb_key_gui(ix, key);
      eh_foco = gui_focus == ix;
    } else {
      if (!c_wins[ix].used || run_hidden(&c_wins[ix])) continue;
      tb_key_wm(&c_wins[ix], key);
      eh_foco = gui_focus < 0 && c_focus && *c_focus == ix;
    }
    if (!tb_eq(key, t->key)) continue;
    if (prim < 0) {
      prim = ix;
      prim_gui = g;
    }
    if (achou_foco && prox < 0) {
      prox = ix;
      prox_gui = g;
    }
    if (eh_foco) achou_foco = 1;
  }
  if (prox < 0) {
    prox = prim;
    prox_gui = prim_gui;
  }
  if (prox < 0) return 1;
  if (prox_gui) {
    gwins[prox].z = ++zseq;
    gui_focus = prox;
    gui_stole = 1;
  } else {
    wm_raise(&c_wins[prox]);
    if (c_focus) *c_focus = prox;
  }
  win_api_dirty = 1;
  return 1;
}

/* Barra de tarefas: logo "S" (abre o menu), marca, programas e data/hora. */
static void draw_taskbar(int w, int h, int bar_h, int ch, int scale, int start_w) {
  static const char* mon[12] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN",
                                "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};
  int by = h - bar_h;
  int ty = by + (bar_h - ch) / 2;
  int lw = start_w > 0 ? start_w : 44;
  int cw = FONT_W * scale;
  int t, d, x;
  char clk[8];
  char dat[12];

  int lx = ui_logo_x(w, lw);
  if (ui_th.modern && gfx_bochs && UI_BAR_A < 255)
    fill_rect_a(0, by, w, bar_h, gui_col(UI_BAR), UI_BAR_A);
  else
    fill_rect(0, by, w, bar_h, gui_col(UI_BAR));
  fill_rect(0, by, w, 1, gui_col(UI_BORDER));
  fill_round(lx, by + 5, lw, bar_h - 10, UI_RADIUS, gui_col(UI_ACCENT));
  draw_text_t(lx + (lw - cw) / 2, by + (bar_h - ch) / 2, "S",
              gui_col(ui_th.modern ? 0xFFFFFFu : UI_TEXT), scale);
  if (ui_menu_pos == 1)
    draw_text_t(lx - 14 - 5 * cw, ty, "SODOS", gui_col(UI_TEXT), scale);
  else
    draw_text_t(lx + lw + 14, ty, "SODOS", gui_col(UI_TEXT), scale);

  t = rtc_time();
  if (t >= 0 && t <= 235959) {
    int hh = t / 10000, mm = (t / 100) % 100;
    clk[0] = (char)('0' + (hh / 10) % 10);
    clk[1] = (char)('0' + hh % 10);
    clk[2] = ':';
    clk[3] = (char)('0' + (mm / 10) % 10);
    clk[4] = (char)('0' + mm % 10);
    clk[5] = 0;
    x = w - 12 - 5 * cw;
    draw_text_t(x, ty, clk, gui_col(UI_TEXT), scale);
    d = rtc_date();
    if (d > 0) {
      int dd = d % 100, mo = (d / 100) % 100, n = 0;
      if (dd >= 10) dat[n++] = (char)('0' + dd / 10);
      dat[n++] = (char)('0' + dd % 10);
      dat[n++] = ' ';
      if (mo >= 1 && mo <= 12) {
        dat[n++] = mon[mo - 1][0];
        dat[n++] = mon[mo - 1][1];
        dat[n++] = mon[mo - 1][2];
      }
      dat[n] = 0;
      x -= 14 + n * cw;
      draw_text_t(x, ty, dat, gui_col(UI_DIM), scale);
    }
  }
  tb_desenha(w, h, bar_h, scale);
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
  draw_taskbar(w, h, c_bar_h, ch, c_scale, c_start_w);
  if (!*chrome_menu) return;
  {
    static const char* labels[] = {
      "Bloco de notas", "Gerenciador", "Explorer", "Configuracoes", "Navegador", "Command", "Sair"
    };
    menu_panel(gm.x, gm.y, gm.w, gm.h);
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
    int design_hot = wm_pt((int)sys_mx, (int)sys_my, gm.sub_x,
                           gm.sub_y + 3 * gm.item_h, gm.sub_w, gm.item_h);
    int menu_hot = wm_pt((int)sys_mx, (int)sys_my, gm.sub_x,
                         gm.sub_y + 4 * gm.item_h, gm.sub_w, gm.item_h);
    int lx_hot = wm_pt((int)sys_mx, (int)sys_my, gm.sub_x,
                       gm.sub_y + 5 * gm.item_h, gm.sub_w, gm.item_h);
    int th_hot = wm_pt((int)sys_mx, (int)sys_my, gm.sub_x,
                       gm.sub_y + 6 * gm.item_h, gm.sub_w, gm.item_h);
    menu_panel(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h);
    menu_row(gm.sub_x + 1, gm.sub_y + 2, gm.sub_w - 2, gm.item_h - 2,
             "Tela", tela_hot, 1, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Fontes", font_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + 2 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Fontes avancadas", adv_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + 3 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Design", design_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + 4 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Menu", menu_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + 5 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Abrir com", lx_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_row(gm.sub_x + 1, gm.sub_y + 2 + 6 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
             "Temas", th_hot, 0, c_win, c_title, c_white, c_black, c_scale);
    menu_frame(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h, c_black);
  }
  if (c_cfg && *c_cfg && c_tela && *c_tela) {
    menu_panel(gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h);
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
  char exe[48];
  if (gui_raise_title("Fontes avancadas")) return;
  if (!desk_find("D:\\SODOS\\FONTADV.EXE", exe, (int)sizeof(exe)))
    wm_copy_str(exe, (int)sizeof(exe), "D:\\SODOS\\FONTADV.EXE");
  wm_exec(wins, focus, big, exe);
}

static void host_open_design(struct wm_win* wins, int* focus, int big) {
  char exe[48];
  if (gui_raise_title("Configurar tela")) return;
  if (!desk_find("D:\\SODOS\\VIDEOCFG.EXE", exe, (int)sizeof(exe)) &&
      !desk_find("D:\\VIDEOCFG.EXE", exe, (int)sizeof(exe)))
    wm_copy_str(exe, (int)sizeof(exe), "D:\\SODOS\\VIDEOCFG.EXE");
  wm_exec(wins, focus, big, exe);
}

static void host_open_ai(struct wm_win* wins, int* focus, int big) {
  char exe[48];
  if (gui_raise_title("SODOS IA")) return;
  if (!desk_find("D:\\SODOS\\AIPANEL.EXE", exe, (int)sizeof(exe)) &&
      !desk_find("D:\\AIPANEL.EXE", exe, (int)sizeof(exe)))
    wm_copy_str(exe, (int)sizeof(exe), "D:\\SODOS\\AIPANEL.EXE");
  wm_exec(wins, focus, big, exe);
}

static void host_open_menu_cfg(struct wm_win* wins, int* focus, int big) {
  char exe[48];
  if (gui_raise_title("Menu")) return;
  if (!desk_find("D:\\SODOS\\MENUCFG.EXE", exe, (int)sizeof(exe)) &&
      !desk_find("D:\\MENUCFG.EXE", exe, (int)sizeof(exe)))
    wm_copy_str(exe, (int)sizeof(exe), "D:\\SODOS\\MENUCFG.EXE");
  wm_exec(wins, focus, big, exe);
}

/* Iniciar > Configuracoes > Abrir com: LINKEXT.EXE (associacoes). */
static void host_open_linkext(struct wm_win* wins, int* focus, int big) {
  char exe[48];
  if (gui_raise_title("Abrir com")) return;
  if (!desk_find("D:\\SODOS\\LINKEXT.EXE", exe, (int)sizeof(exe)))
    wm_copy_str(exe, (int)sizeof(exe), "D:\\SODOS\\LINKEXT.EXE");
  wm_exec(wins, focus, big, exe);
}

/* Iniciar > Configuracoes > Temas: TEMAS.EXE (\SODOS\ICONS\<tema>). */
static void host_open_temas(struct wm_win* wins, int* focus, int big) {
  char exe[48];
  if (gui_raise_title("Temas")) return;
  if (!desk_find("D:\\SODOS\\TEMAS.EXE", exe, (int)sizeof(exe)))
    wm_copy_str(exe, (int)sizeof(exe), "D:\\SODOS\\TEMAS.EXE");
  wm_exec(wins, focus, big, exe);
}

/* Pedido do LINKEXT.DLL (win_launch_arg): programa com o arquivo, ou um dos
 * visores do proprio WIN (@NOTEPAD, @IMAGEM, @VIDEO). */
static void wm_open_with(struct wm_win* wins, int* focus, int big, const char* exe,
                         const char* file) {
  if (exe[0] == '@') {
    const char* k = exe + 1;
    if ((k[0] == 'I' || k[0] == 'i') && (k[1] == 'M' || k[1] == 'm')) {
      wm_open_image(wins, focus, big, file);
    } else if ((k[0] == 'V' || k[0] == 'v') && (k[1] == 'I' || k[1] == 'i')) {
      wm_open_video(wins, focus, big, file);
    } else {
      int ni;
      wm_open_notepad(wins, focus, big);
      note_load(file);
      ni = wm_find_kind(wins, WM_KIND_NOTE);
      if (ni >= 0) note_set_title(&wins[ni]);
    }
    return;
  }
  wm_exec_arg(wins, focus, big, exe, file);
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
    } else if (item == 2) {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      host_open_advanced(c_wins, c_focus, c_big);
    } else if (item == 3) {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      host_open_design(c_wins, c_focus, c_big);
    } else if (item == 4) {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      host_open_menu_cfg(c_wins, c_focus, c_big);
    } else if (item == 5) {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      host_open_linkext(c_wins, c_focus, c_big);
    } else {
      *chrome_menu = 0;
      *c_cfg = 0;
      *c_tela = 0;
      host_open_temas(c_wins, c_focus, c_big);
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
      else if (item == 2) wm_open_explorer(c_wins, c_focus, c_big);
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
  if (wm_pt(x, y, ui_logo_x(w, c_start_w), h - c_bar_h + 2, c_start_w, c_bar_h - 4)) {
    *chrome_menu = !*chrome_menu;
    *c_cfg = 0;
    *c_tela = 0;
    return 1;
  }
  if (!*chrome_menu && tb_clique(x, y)) return 1;
  if (*chrome_menu) {
    *chrome_menu = 0;
    *c_cfg = 0;
    *c_tela = 0;
    return 1;
  }
  if (z_empty(c_wins, x, y)) {
    if (wm_pt(x, y, 12, c_icon4_y, c_icon, c_icon)) {
      desk_mark_item(DESK_MARK_FIXED + 3);
      wm_open_command(c_wins, c_focus, c_big);
      return 1;
    }
    if (wm_pt(x, y, 12, c_icon3_y, c_icon, c_icon)) {
      desk_mark_item(DESK_MARK_FIXED + 2);
      wm_open_browser(c_wins, c_focus, c_big);
      return 1;
    }
    if (wm_pt(x, y, 12, c_icon2_y, c_icon, c_icon)) {
      desk_mark_item(DESK_MARK_FIXED + 1);
      wm_open_notepad(c_wins, c_focus, c_big);
      return 1;
    }
    if (wm_pt(x, y, 12, 12, c_icon, c_icon)) {
      desk_mark_item(DESK_MARK_FIXED + 0);
      wm_open_mgr(c_wins, c_focus, c_big);
      return 1;
    }
  }
  if (!chrome_in_browser(x, y)) return 1;
  return 0;
}

static int wm_no_partial; /* menu ou dialogo aberto: ficam sobre a janela */

/* Refaz so a janela w e o que esta na frente dela, e copia so esse retangulo
 * para a pagina visivel. 0: nao da, precisa do quadro inteiro. O quadro de tras
 * fica com o retangulo novo, sem problema: o proximo quadro inteiro refaz tudo. */
static int wm_present_win(struct wm_win* w) {
  int x0 = w->x, y0 = w->y, x1 = w->x + w->w, y1 = w->y + w->h;
  int over, r;
  if (!gfx_bochs || !c_wins || wm_no_partial || dc.on || dc.capture) return 0;
  if (!gfx_dbuf && !gfx_back) return 0;
  if (chrome_menu && *chrome_menu) return 0;
  /* Fora da area livre (sobre a barra de tarefas, por exemplo). */
  if (x0 < 0 || y0 < 0 || x1 > (int)gfx_w || y1 > (int)gfx_h - c_bar_h) return 0;
  over = cursor_saved && old_mx < x1 && old_mx + cur_bw > x0 &&
         old_my < y1 && old_my + cur_bh > y0;
  if (over) cursor_restore();
  gfx_frame_begin();
  paint_only = 1;
  paint_z = w->z;
  paint_x = x0;
  paint_y = y0;
  paint_w = w->w;
  paint_h = w->h;
  paint_stacked(c_wins, c_title_h, c_scale, c_win, c_title, c_white, c_black, c_btn, c_inactive);
  paint_only = 0;
  for (r = y0; r < y1; r++)
    dc_copy((volatile uint32_t*)(gfx_lfb + ((uint32_t)r + gfx_front_y) * gfx_pitch_bytes + (uint32_t)x0 * 4u),
            (const volatile uint32_t*)gfx_at(x0, r), (uint32_t)(x1 - x0));
  gfx_offscreen = 0;
  gfx_draw_y0 = gfx_front_y;
  if (over) cursor_redraw_at(old_mx, old_my, sys_cursor_col());
  return 1;
}

static int cons_present(struct cmd_con* c) {
  int i;
  for (i = 0; c_wins && i < WM_MAX; i++)
    if (c_wins[i].used && c_wins[i].kind == WM_KIND_CMD && c_wins[i].st == (struct app_state*)c)
      return wm_present_win(&c_wins[i]);
  return 0;
}

static void cons_redraw(void) {
  if (!c_wins || cons_painting) return;
  cons_painting = 1;
  if (!cons_cap || !cons_present(cons_cap)) {
    gfx_frame_begin();
    cursor_saved = 0;
    chrome_paint_under();
    chrome_paint_over();
    cursor_redraw_at((int)sys_mx, (int)sys_my, sys_cursor_col());
    gfx_frame_end();
  }
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
/* Ctrl+F1..F12 no WIN: a tela volta ao modo texto da tela do SO pedida.
 * O WIN fica parado ate alguem voltar a ele (Ctrl+F7); ai o modo grafico
 * e reprogramado e o quadro inteiro e desenhado de novo. */
static void win_vt_away(int to) {
  serial_puts("[visual] WIN sai para a tela de texto\n");
  cursor_restore();
  gfx_live = 0;
  gpu_suspend();
  gfx_tela_uma();
  if (gfx_gop) {
    fbcon_suspend(0);
  } else {
    if (gfx_bochs) {
      bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
      bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
      bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    }
    vga_mode_text();
    vga_dac_restore_std16();
  }

  vt_select(to);

  serial_puts("[visual] WIN volta a tela\n");
  if (gfx_gop) {
    fbcon_suspend(1);
    gfx_gpu_try();
  } else if (gfx_bochs) {
    if (!bochs_program((uint16_t)gfx_w, (uint16_t)gfx_h))
      serial_puts("[visual] modo grafico nao voltou\n");
    gfx_buf_init();
    gfx_gpu_try();
  } else {
    vga_mode13h();
    set_palette_color(0, 0, 0, 0);
    set_palette_color(1, 0, 0, 42);
    set_palette_color(2, 0, 42, 0);
    set_palette_color(3, 0, 32, 32);
    set_palette_color(7, 42, 42, 42);
    set_palette_color(8, 21, 21, 21);
    set_palette_color(14, 63, 63, 0);
    set_palette_color(15, 63, 63, 63);
  }
  gfx_live = 1;
  cursor_saved = 0;
  win_api_dirty = 1;
}

/* Fim do WIN (Esc, menu Sair ou Ctrl+Backspace): desfaz o ambiente grafico e
 * devolve a memoria. As janelas, as tarefas, o papel de parede, o driver da
 * placa e os buffers do desktop saem agora; a tarefa do WIN em vt.c fica
 * marcada como fechada e o Ctrl+F7 cria outra. */
static void win_session_free(struct wm_win* wins) {
  int i;
  if (wins) {
    for (i = 0; i < WM_MAX; i++) {
      if (!wins[i].used) continue;
      task_forget_win(&wins[i]);   /* ninguem mais aponta para esta janela */
    }
  }
  chrome_unbind();   /* fecha janelas canvas e encerra as tarefas */
  if (wins) {
    for (i = 0; i < WM_MAX; i++) {
      if (wins[i].st) kfree(wins[i].st);
      wins[i].st = 0;
      wins[i].used = 0;
    }
  }
  desk_free();
  if (desk_wall) {
    ufree(desk_wall);
    desk_wall = 0;
  }
  /* As tabelas do desktop sao pequenas, mas tambem sao do ambiente grafico. */
  if (gwins) {
    kfree(gwins);
    gwins = 0;
  }
  if (gtasks) {
    kfree(gtasks);
    gtasks = 0;
  }
  run_out = 0;
  cons_cap = 0;
  app_win = 0;
  gui_focus = -1;
  win_api_dirty = 1;
}

/* Tecla que pertence ao SO e nao pode chegar a janela (nem ao guest do
 * QEMU): Ctrl+F1..F12 troca de tela e Ctrl+Backspace fecha o WIN. */
static int win_os_key(uint8_t sc) {
  uint8_t k = sc & 0x7F;
  if (!kbd_ctrl_down()) return 0;
  if (k == 0x0E) return 1;                       /* Backspace */
  return (k >= 0x3B && k <= 0x44) || k == 0x57 || k == 0x58;  /* F1..F12 */
}

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
  int sel_drag = -1; /* Bloco de notas ou Command em que o botao marca texto */
  int pop_open = 0;
  int pop_x = 0;
  int pop_y = 0;
  int pop_hot = 0;
  int menu_row_hot = -1; /* linha do menu Iniciar sob o mouse (ver menu_hover_row) */
  int shift_on = 0;
  int kbd_e0 = 0;
  int draw_skip = 0;
  int pkt_idx = 0;
  uint32_t in_ms = 0;   /* ultimo evento de entrada */
  uint32_t pump_ms = 0; /* ultima vez que um programa ganhou a CPU */
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
  int icon = big ? gfx_sx(ui_th.icon_px) : 14;
  int icon_gap = big ? gfx_sy(36) : 18;
  int icon2_y = 12 + icon + icon_gap;
  int icon3_y = icon2_y + icon + icon_gap;
  int icon4_y = icon3_y + icon + icon_gap;
  uint32_t col_desk, col_win, col_title, col_bar, col_white, col_black, col_btn, col_cursor, col_inactive;
  uint32_t th_gen_seen;
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
  irq_set_tick_hook(gui_preempt_tick);
  desk_load();
  wf_init(scale);   /* fonte TrueType dos titulos e rotulos (ou fica a de bitmap) */
  chrome_leave = 0;
  gui_ensure();
  chrome_bind(wins, &focus, &menu_open, &menu_cfg, &menu_tela, big, scale, bar_h, title_h,
              start_w, menu_w, menu_item_h, icon, icon2_y, icon3_y, icon4_y,
              col_desk, col_win, col_title, col_bar, col_white, col_black, col_btn, col_inactive);

  th_gen_seen = ui_th.gen;
  for (;;) {
    uint8_t data = 0;

    kt_bkl_breathe();
    if (th_gen_seen != ui_th.gen) {
      /* tema novo (TEMAS.EXE): tamanho dos icones do desktop pode mudar */
      th_gen_seen = ui_th.gen;
      icon = big ? gfx_sx(ui_th.icon_px) : 14;
      icon2_y = 12 + icon + icon_gap;
      icon3_y = icon2_y + icon + icon_gap;
      icon4_y = icon3_y + icon + icon_gap;
      chrome_bind(wins, &focus, &menu_open, &menu_cfg, &menu_tela, big, scale, bar_h, title_h,
                  start_w, menu_w, menu_item_h, icon, icon2_y, icon3_y, icon4_y, col_desk,
                  col_win, col_title, col_bar, col_white, col_black, col_btn, col_inactive);
      need_draw = 1;
    }   /* threads de programas esperando o kernel */
    if (gui_stole) {
      focus = -1;
      gui_stole = 0;
      need_draw = 1;
    }
    {
      /* Entrada primeiro: um pacote de mouse sao 3 bytes e cada volta do
       * laco dava uma fatia inteira ao programa (Firefox) antes do proximo
       * byte. Com entrada chegando, so cede a cada 30 ms. */
      uint32_t now = (uint32_t)irq_ticks();
      int pend = pkt_idx > 0 || ps2in_pending() || (now - in_ms) < 3;
      if ((!pend || now - pump_ms >= 30) && task_pick() >= 0) {
        pump_ms = now;
        tasks_pump();
        prof_pump += (uint32_t)irq_ticks() - now;
      }
    }
    gui_paste_pump();
    if (win_api_dirty) {
      /* 2: so janelas de programa mudaram; vira need_draw 2 se nada mais pediu */
      need_draw = win_api_dirty == 2 && need_draw != 1 ? 2 : 1;
      win_api_dirty = 0;
    }
    if (chrome_leave) {
      int leave = chrome_leave;
      chrome_leave = 0;
      if (leave == 1) {
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE && note_dirty)
          note_save();
        win_session_free(wins);
        return 0;
      }
      chrome_unbind();
      desk_free();
      return 1;
    }
    mx = sys_mx;
    my = sys_my;
    if (launch_pending) {
      launch_pending = 0;
      if (launch_arg[0]) wm_open_with(wins, &focus, big, launch_path, launch_arg);
      else wm_exec(wins, &focus, big, launch_path);
      need_draw = 1;
    }
    killq_service();
    if (run_close_pending) {
      int k;
      run_close_pending = 0;
      for (k = 0; k < WM_MAX; k++) {
        if (!wins[k].used || !wins[k].auto_close) continue;
        task_forget_win(&wins[k]);
        if (wins[k].st) {
          kfree(wins[k].st);
          wins[k].st = 0;
        }
        if (app_win == &wins[k]) app_win = 0;
        wins[k].auto_close = 0;
        wins[k].used = 0;
        if (focus == k) {
          int j;
          focus = -1;
          for (j = 0; j < WM_MAX; j++)
            if (wins[j].used) { focus = j; break; }
        }
      }
      need_draw = 1;
    }
    if (desk_dirty) {
      desk_dirty = 0;
      if (desk_refresh()) need_draw = 1;
    }
    if (ui_ia_launch) {
      ui_ia_launch = 0;
      if (!gui_raise_title("SODOS IA")) {
        autostart_launch = 1;
        host_open_ai(wins, &focus, big);
        autostart_launch = 0;
        need_draw = 1;
      }
    }
    /* Alt+Tab: miniaturas com o quadro de agora, depois o WINTAB.EXE. */
    if (at_req) {
      char exe[48];
      at_req = 0;
      at_snap(wins, title_h, scale, col_win, col_title, col_white, col_black, col_btn,
              col_inactive);
      need_draw = 1;
      if (at_n < 2) {
        at_end();
      } else if (at_res_alive()) {
        at_seq++; /* WINTAB ja na memoria: so acorda */
      } else if (desk_find("D:\\SODOS\\WINTAB.EXE", exe, (int)sizeof(exe))) {
        at_seq++;
        autostart_launch = 1; /* nao conta como abertura do usuario (KILLALL) */
        wm_exec(wins, &focus, big, exe);
        autostart_launch = 0;
      } else {
        /* sem o WINTAB: so passa para a janela de tras */
        at_act = at_list[1].id;
      }
    }
    if (at_act >= 0) {
      int id = at_act;
      at_act = -1;
      if (id >= AT_GUI && id < AT_GUI + GUI_MAX && gwins && gwins[id - AT_GUI].open) {
        gwins[id - AT_GUI].z = ++zseq;
        gui_focus = id - AT_GUI;
        focus = -1;
      } else if (id >= 0 && id < WM_MAX && wins[id].used) {
        wm_raise(&wins[id]);
        focus = id;
      }
      at_end();
      need_draw = 1;
    }
    if (at_on && !at_req) {
      /* painel fechado (Esc ou fim do WINTAB), ou o WINTAB nunca abriu */
      if (at_gid >= 0 ? !(gwins && gwins[at_gid].open)
                      : (uint32_t)irq_ticks() - at_t0 > 1500u) {
        at_end();
        need_draw = 1;
      }
    }
    wm_no_partial = menu_open || pop_open || note_dlg || note_file_menu;
    /* Programa do Command passou a usar a tela 80x25: a janela cresce. */
    if (cons_dirty && cons_dirty->tty && cons_dirty->tty->on && cons_dirty->tty->fit) {
      int fi;
      cons_dirty->tty->fit = 0;
      for (fi = 0; fi < WM_MAX; fi++) {
        struct wm_win* fw = &wins[fi];
        int need_w = 80 * FONT_W * scale + 12;
        int need_h = title_h + 25 * glyph_rows() * scale + 6;
        if (!fw->used || fw->kind != WM_KIND_CMD || fw->st != (struct app_state*)cons_dirty) continue;
        if (wm_is_max(fw)) break;
        if (fw->w < need_w) fw->w = need_w;
        if (fw->h < need_h) fw->h = need_h;
        wm_clamp_rect(&fw->x, &fw->y, &fw->w, &fw->h, big);
        need_draw = 1;
        break;
      }
    }
    if (need_draw) {
      cons_dirty = 0; /* o quadro inteiro ja mostra */
    } else if (cons_dirty && !dc.on) {
      struct cmd_con* cd = cons_dirty;
      cons_dirty = 0;
      if (!cons_present(cd)) need_draw = 1;
    }
    /* A janela arrastada sumiu (programa fechou, por exemplo). */
    if ((dc.on || dc.capture) &&
        !(dc.wm >= 0 ? (drag == dc.wm && wins[dc.wm].used)
                     : (g_drag == dc.gw && gwins && gwins[dc.gw].open))) {
      dc_end();
      need_draw = 1;
    }
    /* Com entrada na fila, um quadro agora so mostraria uma posicao velha.
     * O limite garante um quadro mesmo com o mouse sem parar. */
    if ((need_draw || dc.moved) && ps2in_pending() && ++draw_skip < 64) {
      /* desenha depois de ler */
    } else if (dc.on) {
      draw_skip = 0;
      if (dc.moved) {
        if (dc.wm >= 0) dc_present(wins[dc.wm].x, wins[dc.wm].y, (int)mx, (int)my);
        else dc_present(gwins[dc.gw].ox, gwins[dc.gw].oy, (int)mx, (int)my);
      }
    } else if (need_draw) {
      uint32_t prof_t0 = (uint32_t)irq_ticks();
      int bar_ty = h - bar_h + (bar_h - ch) / 2;
      struct start_menu gm;
      int mi;
      start_menu_geom(&gm, w, h, bar_h, menu_w, menu_item_h, scale);
      gfx_frame_dmg = need_draw == 2;
      gfx_frame_begin();
      cursor_saved = 0;
      gfx_desk_clear();
      if (!th_desk_icon("DOS", 12, 12, icon)) fill_rect(12, 12, icon, icon, col_win);
      desk_label(12, icon, 12 + icon + 2, "DOS", desk_mark == DESK_MARK_FIXED + 0, col_white,
                 col_black, scale);
      if (!th_desk_icon("ARQ", 12, icon2_y, icon))
        fill_rect(12, icon2_y, icon, icon, col_title);
      desk_label(12, icon, icon2_y + icon + 2, "ARQ", desk_mark == DESK_MARK_FIXED + 1, col_white,
                 col_black, scale);
      if (!th_desk_icon("WEB", 12, icon3_y, icon))
        draw_nav_icon(12, icon3_y, icon, col_title, col_white);
      desk_label(12, icon, icon3_y + icon + 2, "WEB", desk_mark == DESK_MARK_FIXED + 2, col_white,
                 col_black, scale);
      if (!th_desk_icon("CMD", 12, icon4_y, icon))
        draw_cmd_icon(12, icon4_y, icon, col_white, col_black);
      desk_label(12, icon, icon4_y + icon + 2, "CMD", desk_mark == DESK_MARK_FIXED + 3, col_white,
                 col_black, scale);
      desk_draw(icon, ch, scale, w, h, bar_h, col_desk, col_white, col_black);
      paint_stacked(wins, title_h, scale, col_win, col_title, col_white, col_black,
                    col_btn, col_inactive);
      draw_taskbar(w, h, bar_h, ch, scale, start_w);
      if (gwins && g_rs >= 0) {
        /* contorno do novo tamanho enquanto arrasta o canto */
        uint32_t oc = gui_col(UI_ACCENT);
        fill_rect(g_rs_x, g_rs_y, g_rs_w, 2, oc);
        fill_rect(g_rs_x, g_rs_y + g_rs_h - 2, g_rs_w, 2, oc);
        fill_rect(g_rs_x, g_rs_y, 2, g_rs_h, oc);
        fill_rect(g_rs_x + g_rs_w - 2, g_rs_y, 2, g_rs_h, oc);
      }
      if (menu_open) {
        static const char* labels[] = {
          "Bloco de notas", "Gerenciador", "Explorer", "Configuracoes", "Navegador", "Command", "Sair"
        };
        menu_panel(gm.x, gm.y, gm.w, gm.h);
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
        int design_hot = wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + 3 * gm.item_h,
                               gm.sub_w, gm.item_h);
        int menu_hot = wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + 4 * gm.item_h,
                             gm.sub_w, gm.item_h);
        int lx_hot = wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + 5 * gm.item_h,
                           gm.sub_w, gm.item_h);
        int th_hot = wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y + 6 * gm.item_h,
                           gm.sub_w, gm.item_h);
        menu_panel(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h);
        menu_row(gm.sub_x + 1, gm.sub_y + 2, gm.sub_w - 2, gm.item_h - 2,
                 "Tela", tela_hot, 1, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Fontes", font_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + 2 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Fontes avancadas", adv_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + 3 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Design", design_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + 4 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Menu", menu_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + 5 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Abrir com", lx_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_row(gm.sub_x + 1, gm.sub_y + 2 + 6 * gm.item_h, gm.sub_w - 2, gm.item_h - 2,
                 "Temas", th_hot, 0, col_win, col_title, col_white, col_black, scale);
        menu_frame(gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h, col_black);
      }
      if (menu_open && menu_cfg && menu_tela) {
        menu_panel(gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h);
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
        menu_panel(px, py, pw, ph);
        {
          int ih = (ph - 4) / POP_N;
          menu_row(px + 1, py + 2, pw - 2, ih, "Nova pasta", pop_hot == 0, 0,
                   col_win, col_title, col_white, col_black, scale);
          menu_row(px + 1, py + 2 + ih, pw - 2, ih, "Console", pop_hot == 1, 0,
                   col_win, col_title, col_white, col_black, scale);
          menu_row(px + 1, py + 2 + 2 * ih, pw - 2, ih, "Configuracoes de tela", pop_hot == 2, 0,
                   col_win, col_title, col_white, col_black, scale);
          menu_row(px + 1, py + 2 + 3 * ih, pw - 2, ih, "Design", pop_hot == 3, 0,
                   col_win, col_title, col_white, col_black, scale);
        }
        menu_frame(px, py, pw, ph, col_black);
      }
      if (dc.capture) {
        /* A janela pode estar meio fora da tela: a foto e tirada com ela
         * toda dentro (sx,sy) e depois dc_place a poe no lugar certo. */
        int rx, ry, sx, sy;
        dc_snap_bg();
        if (dc.wm >= 0) {
          rx = wins[dc.wm].x;
          ry = wins[dc.wm].y;
          dc_capture_pos(rx, ry, &sx, &sy);
          wins[dc.wm].x = sx;
          wins[dc.wm].y = sy;
          wm_draw_window(&wins[dc.wm], 1, title_h, scale, col_win, col_title, col_white,
                         col_black, col_btn, col_inactive);
          wins[dc.wm].x = rx;
          wins[dc.wm].y = ry;
        } else {
          rx = gwins[dc.gw].ox;
          ry = gwins[dc.gw].oy;
          dc_capture_pos(rx, ry, &sx, &sy);
          gwins[dc.gw].ox = sx;
          gwins[dc.gw].oy = sy;
          gui_draw_frame(&gwins[dc.gw], 1, col_title, col_white, col_black, col_btn, col_inactive);
          gwins[dc.gw].ox = rx;
          gwins[dc.gw].oy = ry;
        }
        dc_snap_win(sx, sy, (int)mx, (int)my);
        dc_place(rx, ry);
      }
      cursor_redraw_at((int)mx, (int)my, col_cursor);
      gfx_frame_end();
      prof_draw += (uint32_t)irq_ticks() - prof_t0;
      prof_frames++;
      need_draw = 0;
      draw_skip = 0;
    }

    {
      uint8_t scb = 0;
      int from_abs = 0;
      uint32_t prof_p0 = (uint32_t)irq_ticks();
      int kind;
      if (win_pend) {               /* a letra que sobrou do acento */
        win_syn = win_pend;
        win_pend = 0;
        kbd_e0 = 0;
        data = 0x39;                /* scancode qualquer de caractere */
        goto wm_key;
      }
      kind = input_poll(packet, &pkt_idx, &scb, &from_abs);
      prof_poll += (uint32_t)irq_ticks() - prof_p0;
      if (kind < 0) {
        vt_idle();   /* as telas do DOS ao fundo andam */
        /* Nada a fazer: empresta a CPU0 e o BKL as threads por 1 ms (o tique
         * do PIT acorda). Antes girava aqui com o BKL, e as threads dos
         * programas (o Claude no WINE: ~150) esperavam no bkl_take. */
        if (!need_draw && !win_api_dirty && !tasks_want_cpu()) kt_world_idle(1);
        else __asm__ volatile ("pause");
        continue;
      }
      in_ms = (uint32_t)irq_ticks();
      if (kind == 0) {
        int rf = gui_raw_focus();
        /* Janela crua (emulador): o byte vai inteiro, exceto as teclas do SO. */
        if (rf >= 0 && !win_os_key(scb)) {
          gq_push(&gwins[rf], GUI_EV_SCAN, 0, 0, scb);
          win_api_dirty = 1;
          continue;
        }
        data = scb;
        goto wm_key;
      }
      {
        int left;
        if (!from_abs)
          sys_mouse_place(mx + (int8_t)packet[1], my - (int8_t)packet[2]);
        mx = sys_mx;
        my = sys_my;
        {
          int rf = gui_raw_focus();
          if (rf >= 0) {
            struct gui_win* g = &gwins[rf];
            int rx = (int)mx - g->ox, ry = (int)my - (g->oy + g->title_h);
            int btn = packet[0] & 7;
            int inside = rx >= 0 && ry >= 0 && rx < g->cw && ry < g->ch;
            if (inside || g->raw_btn) {
              gq_push_move(g, rx, ry, btn);
              win_api_dirty = 1;
            }
            g->raw_btn = btn && (g->raw_btn || inside);
          }
        }
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
          {
            /* O destaque e calculado no desenho a partir do mouse: trocar de
             * linha precisa de um quadro inteiro. Sem isso so um redesenho
             * de janela de programa (need_draw 2, que com driver de placa so
             * mostra o retangulo dela) atualizava, e o menu ficava pela metade. */
            int row = -1;
            if (menu_tela && wm_pt((int)mx, (int)my, gm.mode_x, gm.mode_y, gm.mode_w, gm.mode_h))
              row = 200 + ((int)my - (gm.mode_y + 2)) / (gm.item_h > 0 ? gm.item_h : 1);
            else if (menu_cfg && wm_pt((int)mx, (int)my, gm.sub_x, gm.sub_y, gm.sub_w, gm.sub_h))
              row = 100 + ((int)my - gm.sub_y) / (gm.item_h > 0 ? gm.item_h : 1);
            else if (over_main)
              row = ((int)my - gm.y) / (gm.item_h > 0 ? gm.item_h : 1);
            if (row != menu_row_hot) {
              menu_row_hot = row;
              need_draw = 1;
            }
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
              } else if (wins[wh].kind == WM_KIND_CMD && wins[wh].st) {
                cmd_scroll((struct cmd_con*)wins[wh].st, dz * 3);
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
        if (!left && prev_left && desk_press >= 0 && !desk_arr) {
          desk_soltou = desk_press;   /* clique simples: abre como antes */
          desk_press = -1;
        }
        if (left && gwins && g_rs >= 0 && g_rs < GUI_MAX && gwins[g_rs].open) {
          rs_apply((int)mx, (int)my, 200, gwins[g_rs].title_h + 60, w, h - bar_h, &g_rs_x,
                   &g_rs_y, &g_rs_w, &g_rs_h);
          need_draw = 1;
        } else if (left && gwins && g_drag >= 0 && g_drag < GUI_MAX && gwins[g_drag].open) {
          struct gui_win* gd = &gwins[g_drag];
          int gcs = gd->title_h - 4;
          gd->ox = (int)mx - g_drag_ox;
          gd->oy = (int)my - g_drag_oy;
          /* pode sair da tela; a barra de titulo continua alcancavel */
          win_keep_title(&gd->ox, &gd->oy, gd->cw, gd->title_h, bar_h, TB_PAD + 2 * gcs + 4);
          if (!dc_track(-1, g_drag, gwins[g_drag].cw, gwins[g_drag].title_h + gwins[g_drag].ch))
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
            rs_apply((int)mx, (int)my, min_w, min_h, w, h - bar_h, &wins[drag].x,
                     &wins[drag].y, &wins[drag].w, &wins[drag].h);
          }
          /* Pode sair pela esquerda, direita e por baixo (o desenho e cortado
           * nas bordas); a barra de titulo fica alcancavel. */
          win_keep_title(&wins[drag].x, &wins[drag].y, wins[drag].w, title_h, bar_h,
                         TB_PAD + 2 * (title_h - 4) + 4);
          if (drag_mode != 0 || !dc_track(drag, -1, wins[drag].w, wins[drag].h))
            need_draw = 1;
        } else if (left && prev_left && sel_drag >= 0 && wins[sel_drag].used) {
          /* arrastando: a ponta da marcacao segue o mouse */
          struct wm_win* sw = &wins[sel_drag];
          if (sw->kind == WM_KIND_NOTE) {
            int top = title_h + note_menubar_h(ch);
            int k = note_hit(sw, top, scale, (int)mx, (int)my);
            if (k != note_caret) {
              note_caret = k;
              note_follow_caret(sw, top, scale);
              need_draw = 1;
            }
          } else if (sw->kind == WM_KIND_CMD && sw->st) {
            struct cmd_con* cc = (struct cmd_con*)sw->st;
            int r, k;
            cmd_hit(sw, cc, title_h, scale, (int)mx, (int)my, &r, &k);
            if (r != cc->br || k != cc->bc) {
              cc->br = r;
              cc->bc = k;
              need_draw = 1;
            }
          }
        } else if (left && prev_left && desk_press >= 0 && desk_press < desk_n) {
          int dx = (int)mx - desk_press_mx, dy = (int)my - desk_press_my;
          if (!desk_arr && (dx > 6 || dx < -6 || dy > 6 || dy < -6)) desk_arr = 1;
          if (desk_arr) {
            int nx = (int)mx - desk_press_ox, ny = (int)my - desk_press_oy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > (int)w - icon) nx = (int)w - icon;
            if (ny > (int)h - bar_h - icon - ch - 6) ny = (int)h - bar_h - icon - ch - 6;
            if (nx != desk_drag_x || ny != desk_drag_y) {
              desk_drag_x = nx;
              desk_drag_y = ny;
              need_draw = 1;
            }
          }
        } else if ((left && !prev_left) || desk_soltou >= 0) {
          int handled = 0;
          int icons_on;
          if (desk_ren >= 0) {
            desk_ren_commit();
            handled = 1;
            need_draw = 1;
          } else if (pop_open) {
            int px, py, pw, ph;
            pop_geom(pop_x, pop_y, scale, w, h, bar_h, &px, &py, &pw, &ph);
            pop_open = 0;
            if (wm_pt((int)mx, (int)my, px, py, pw, ph)) {
              int ih = (ph - 4) / POP_N;
              int item = ih > 0 ? ((int)my - (py + 2)) / ih : 0;
              if (item <= 0) desk_new_folder();
              else if (item == 1) wm_open_command(wins, &focus, big);
              else if (item == 2) wm_open_display(wins, &focus, big);
              else host_open_design(wins, &focus, big);
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
          } else if (wm_pt((int)mx, (int)my, ui_logo_x(w, start_w), h - bar_h + 2, start_w, bar_h - 4)) {
            menu_open = !menu_open;
            menu_cfg = 0;
            menu_tela = 0;
            note_file_menu = 0;
            handled = 1;
            need_draw = 1;
          } else if (!menu_open && left && tb_clique((int)mx, (int)my)) {
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
              } else if (item == 2) {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                host_open_advanced(wins, &focus, big);
              } else if (item == 3) {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                host_open_design(wins, &focus, big);
              } else if (item == 4) {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                host_open_menu_cfg(wins, &focus, big);
              } else if (item == 5) {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                host_open_linkext(wins, &focus, big);
              } else {
                menu_open = 0;
                menu_cfg = 0;
                menu_tela = 0;
                host_open_temas(wins, &focus, big);
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
                else if (item == 2) wm_open_explorer(wins, &focus, big);
                else if (item == 4) wm_open_browser(wins, &focus, big);
                else if (item == 5) wm_open_command(wins, &focus, big);
                else {
                  serial_puts("[visual] Sair (menu)\n");
                  win_session_free(wins);
                  return 0;
                }
              }
              handled = 1;
              need_draw = 1;
            }
          }
          /* janela na frente: os icones do fundo nao recebem o clique */
          icons_on = z_empty(wins, (int)mx, (int)my);
          if (handled) {
            /* ja tratado */
          } else if (icons_on && wm_pt((int)mx, (int)my, 12, icon4_y, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            desk_mark_item(DESK_MARK_FIXED + 3);
            wm_open_command(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if (icons_on && wm_pt((int)mx, (int)my, 12, icon3_y, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            desk_mark_item(DESK_MARK_FIXED + 2);
            wm_open_browser(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if (icons_on && wm_pt((int)mx, (int)my, 12, icon2_y, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            desk_mark_item(DESK_MARK_FIXED + 1);
            wm_open_notepad(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if (icons_on && wm_pt((int)mx, (int)my, 12, 12, icon, icon)) {
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            desk_mark_item(DESK_MARK_FIXED + 0);
            wm_open_mgr(wins, &focus, big);
            handled = 1;
            need_draw = 1;
          } else if (icons_on && desk_soltou < 0 &&
                     (desk_i = desk_hit((int)mx, (int)my, icon, ch, scale, w, h, bar_h)) >= 0) {
            int ix = 0, iy = 0;
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            desk_mark_item(desk_i);
            (void)desk_place(desk_i, icon, ch, scale, w, h, bar_h, &ix, &iy);
            desk_press = desk_i;
            desk_press_mx = (int)mx;
            desk_press_my = (int)my;
            desk_press_ox = (int)mx - ix;
            desk_press_oy = (int)my - iy;
            desk_drag_x = ix;
            desk_drag_y = iy;
            desk_arr = 0;
            handled = 1;
            need_draw = 1;
          } else if (icons_on && desk_soltou >= 0 && desk_soltou < desk_n &&
                     (desk_i = desk_soltou) >= 0) {
            desk_soltou = -1;
            menu_open = 0;
            menu_cfg = 0;
            menu_tela = 0;
            desk_mark_item(desk_i);
            if (desk_items[desk_i].dir &&
                wm_open_folder(wins, &focus, big, desk_items[desk_i].path)) {
              /* aberta no Explorer (visao de pasta) */
            } else if (desk_items[desk_i].dir) {
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
            } else if (desk_items[desk_i].lnk) {
              char tgt[DESK_PATH];
              char exe[DESK_PATH];
              if (desk_lnk_target(desk_items[desk_i].path, tgt, (int)sizeof(tgt))) {
                if (!desk_find(tgt, exe, (int)sizeof(exe)))
                  wm_copy_str(exe, (int)sizeof(exe), tgt);
                wm_exec(wins, &focus, big, exe);
              } else
                app_status_set("atalho vazio");
            } else if (linkext_fn && !app_is_prog(desk_items[desk_i].name) &&
                       linkext_fn(desk_items[desk_i].path) == 0) {
              /* LINKEXT.DLL achou o programa da extensao (abre no proximo quadro) */
            } else if (desk_items[desk_i].image == 1)
              wm_open_image(wins, &focus, big, desk_items[desk_i].path);
            else if (desk_items[desk_i].image == 2)
              wm_open_video(wins, &focus, big, desk_items[desk_i].path);
            else if (app_is_prog(desk_items[desk_i].name)) {
              char exe[DESK_PATH];
              if (!desk_find(desk_items[desk_i].path, exe, (int)sizeof(exe)))
                wm_copy_str(exe, (int)sizeof(exe), desk_items[desk_i].path);
              wm_exec(wins, &focus, big, exe);
            }
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
            if (hk == HIT_NONE && (int)my < h - bar_h && desk_mark != -1) {
              /* clique no desktop vazio: nenhum icone fica marcado */
              desk_mark_item(-1);
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
              int rc;
              if (gw->panel) {
                gq_push(gw, GUI_EV_DOWN, relx, rely, 1);
                g_cap = id;
              } else if (wm_pt((int)mx, (int)my, gw->ox + gw->cw - close_s - TB_PAD, gw->oy + 2,
                        close_s, close_s)) {
                gq_push(gw, GUI_EV_CLOSE, 0, 0, 0);
              } else if (wm_pt((int)mx, (int)my, gw->ox + gw->cw - 2 * close_s - TB_PAD - 4,
                               gw->oy + 2, close_s, close_s)) {
                gw_toggle_max(gw);
              } else if ((rc = win_corner((int)mx, (int)my, gw->ox, gw->oy, gw->cw,
                                          gw->title_h + gw->ch, big ? 12 : 8)) != 0) {
                /* canto: o contorno segue o rato; o buffer muda ao soltar */
                rs_begin(rc, (int)mx, (int)my, gw->ox, gw->oy, gw->cw, gw->title_h + gw->ch);
                g_rs = id;
                g_rs_x = gw->ox;
                g_rs_y = gw->oy;
                g_rs_w = gw->cw;
                g_rs_h = gw->title_h + gw->ch;
              } else if (rely < 0 && dbl_click(1000 + id, (int)mx, (int)my)) {
                gw_toggle_max(gw);
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
              int close_x = wins[hit].x + wins[hit].w - close_s - TB_PAD;
              int rc = win_corner((int)mx, (int)my, wins[hit].x, wins[hit].y, wins[hit].w,
                                  wins[hit].h, big ? 12 : 8);
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
              } else if (wm_pt((int)mx, (int)my, close_x - close_s - 4, wins[hit].y + 2, close_s,
                               close_s) ||
                         (wm_pt((int)mx, (int)my, wins[hit].x, wins[hit].y, wins[hit].w, title_h) &&
                          dbl_click(hit, (int)mx, (int)my))) {
                wm_toggle_max(&wins[hit], scale);
              } else if (rc) {
                drag = hit;
                drag_mode = rc;
                rs_begin(rc, (int)mx, (int)my, wins[hit].x, wins[hit].y, wins[hit].w,
                         wins[hit].h);
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
                    if (linkext_fn && linkext_fn(app_open_path) == 0) {
                      /* LINKEXT.DLL abriu */
                    } else if (desk_is_video(app_open_path))
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
                drag_mode = RS_R | RS_B;
                rs_begin(drag_mode, (int)mx, (int)my, wins[hit].x, wins[hit].y, wins[hit].w,
                         wins[hit].h);
              } else if (wm_pt((int)mx, (int)my, wins[hit].x, wins[hit].y, wins[hit].w, title_h)) {
                drag = hit;
                drag_mode = 0;
                drag_ox = (int)mx - wins[hit].x;
                drag_oy = (int)my - wins[hit].y;
              } else if (wins[hit].kind == WM_KIND_NOTE && !note_missing &&
                         wm_pt((int)mx, (int)my, wins[hit].x, wins[hit].y + title_h + mb_h,
                               wins[hit].w, wins[hit].h - title_h - mb_h)) {
                /* clicar poe o cursor e comeca a marcar; com Shift estende */
                int k = note_hit(&wins[hit], title_h + mb_h, scale, (int)mx, (int)my);
                int wa, wb;
                if (!keymap_shift()) note_anc = k;
                else if (note_anc < 0) note_anc = note_caret;
                note_caret = k;
                sel_drag = hit;
                if (dbl_click(hit, (int)mx, (int)my) &&
                    word_at(note_buf, note_len, note_dbl_pos(&wins[hit], title_h + mb_h, scale,
                                                             (int)mx, (int)my), &wa, &wb)) {
                  note_anc = wa;
                  note_caret = wb;
                  sel_drag = -1; /* arrastar nao desfaz a palavra */
                }
              } else if (wins[hit].kind == WM_KIND_CMD && wins[hit].st &&
                         wm_pt((int)mx, (int)my, wins[hit].x, wins[hit].y + title_h,
                               wins[hit].w, wins[hit].h - title_h)) {
                struct cmd_con* cc = (struct cmd_con*)wins[hit].st;
                int r, k;
                cmd_hit(&wins[hit], cc, title_h, scale, (int)mx, (int)my, &r, &k);
                if (!(keymap_shift() && cc->sel)) {
                  cc->ar = r;
                  cc->ac = k;
                }
                cc->br = r;
                cc->bc = k;
                cc->sel = 1;
                sel_drag = hit;
                if (dbl_click(hit, (int)mx, (int)my)) {
                  cmd_hit_cell(&wins[hit], cc, title_h, scale, (int)mx, (int)my, &r, &k);
                  cmd_word(cc, r, k);
                  sel_drag = -1;
                }
              }
              need_draw = 1;
              handled = 1;
            }
          }
          (void)handled;
        }
        if (!left) {
          if (desk_press >= 0 && desk_press < desk_n && desk_arr) {
            desk_pos_set(desk_press, desk_drag_x, desk_drag_y);
            need_draw = 1;
          }
          desk_press = -1;
          desk_arr = 0;
          desk_soltou = -1;
          if (gwins && g_rs >= 0 && g_rs < GUI_MAX && gwins[g_rs].open) {
            struct gui_win* rg = &gwins[g_rs];
            if (g_rs_x != rg->ox || g_rs_y != rg->oy || g_rs_w != rg->cw ||
                g_rs_h != rg->title_h + rg->ch) {
              if (gw_set_rect(rg, g_rs_x, g_rs_y, g_rs_w, g_rs_h - rg->title_h) == 0)
                rg->maxed = 0;
            }
            need_draw = 1;
          }
          g_rs = -1;
          if (gwins && g_cap >= 0 && g_cap < GUI_MAX && gwins[g_cap].open)
            gq_push(&gwins[g_cap], GUI_EV_UP, (int)mx - gwins[g_cap].ox,
                    (int)my - (gwins[g_cap].oy + gwins[g_cap].title_h), 1);
          g_cap = -1;
          g_drag = -1;
          drag = -1;
          sel_drag = -1;
          if (dc.on || dc.capture) {
            dc_end();
            need_draw = 1;
          }
        }
        prev_left = left;
        {
          int right = (packet[0] >> 1) & 1;
          /* Botao direito numa janela de programa (Firefox: menu de contexto)
           * vai para ela como DOWN/UP com botao 2, como o esquerdo. */
          if (gwins && right && !prev_right && !note_dlg && !menu_open) {
            int hk = HIT_NONE;
            int id = z_top(wins, (int)mx, (int)my, &hk);
            if (hk == HIT_GUI && id >= 0 && id < GUI_MAX && gwins[id].open) {
              struct gui_win* gw = &gwins[id];
              int relx = (int)mx - gw->ox;
              int rely = (int)my - (gw->oy + gw->title_h);
              if (rely >= 0 && relx >= 0 && relx < gw->cw && rely < gw->ch) {
                gw->z = ++zseq;
                gui_focus = id;
                focus = -1;
                pop_open = 0;
                gq_push(gw, GUI_EV_DOWN, relx, rely, 2);
                g_rcap = id;
                need_draw = 1;
              }
            }
          } else if (!right && prev_right && g_rcap >= 0) {
            if (gwins && g_rcap < GUI_MAX && gwins[g_rcap].open)
              gq_push(&gwins[g_rcap], GUI_EV_UP, (int)mx - gwins[g_rcap].ox,
                      (int)my - (gwins[g_rcap].oy + gwins[g_rcap].title_h), 2);
            g_rcap = -1;
          }
          if (right && !prev_right && !note_dlg && !menu_open && tb_em((int)mx, (int)my) >= 0) {
            tb_direito = 1;
            (void)tb_clique((int)mx, (int)my);
            tb_direito = 0;
            need_draw = 1;
          } else if (right && !prev_right && !note_dlg) {
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
        if (!need_draw && !dc.on) cursor_draw((int)mx, (int)my, col_cursor);
      }
      continue;
    }

    wm_key:
    {
      uint8_t sc = data;
      int kp = 0; /* Enter e / do teclado numerico: seguem como tecla comum */
      int syn = win_syn; /* != 0: tecla que sobrou do acento (win_pend) */
      win_syn = 0;
      int vt_to;
      /* Ctrl+F1..F12: telas do SO. Ctrl+F7 e esta. */
      vt_to = (!(sc & 0x80) && !kbd_e0 && kbd_ctrl_down()) ? vt_hotkey(sc) : -1;
      if (vt_to >= 0) {
        if (vt_to != VT_WIN) {
          win_vt_away(vt_to);
          pkt_idx = 0;
          shift_on = 0;
          need_draw = 1;
        }
        continue;
      }
      /* Ctrl+Backspace: sai do WIN e devolve a memoria do ambiente grafico.
       * Fica antes de qualquer janela, dialogo ou janela crua para nenhum
       * programa receber a combinacao. */
      if (!(sc & 0x80) && sc == 0x0E && kbd_ctrl_down()) {
        serial_puts("[visual] Ctrl+Backspace: fim do WIN\n");
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE && note_dirty)
          note_save();
        win_session_free(wins);
        return 0;
      }
      /* PrintScreen (E0 2A E0 37; Alt+PrtSc manda 54): a tela inteira vai para
       * Z:\SAVE\SCREENSHOT em PNG. Nenhuma janela recebe a tecla. */
      if (!(sc & 0x80) && ((kbd_e0 && sc == 0x37) || sc == 0x54)) {
        kbd_e0 = 0;
        shift_on = 0;   /* o E0 2A da sequencia e um Shift falso */
        prtscr_save(0, 0);
        continue;
      }
      /* Alt+Tab: troca de janela. O WINTAB.EXE mostra a pilha enquanto o Alt
       * esta apertado e soltar o Alt escolhe; nenhuma janela recebe o Tab. */
      if ((sc & 0x7F) == 0x38) {
        at_held = !(sc & 0x80);
        if (!at_held && at_on) at_alt = 0;
      }
      if (sc == 0x0F && !kbd_e0 && at_held) {
        if (!at_on) {
          at_on = 1;
          at_req = 1;
          at_tabs = 0;
          at_gid = -1;
          at_t0 = (uint32_t)irq_ticks();
        }
        at_alt = 1;
        at_tabs += shift_on ? -1 : 1;
        continue;
      }
      if (keymap_kp_nav(sc)) kbd_e0 = 1; /* NumLock desligado: vira seta, Home... */
      if (kbd_e0 && !(sc & 0x80)) kp = keymap_ext_char(sc);
      if (desk_ren >= 0) {
        /* nome de icone do desktop em edicao: o teclado e dele */
        if (!(sc & 0x80) && (!kbd_e0 || kp)) {
          desk_ren_key(syn ? syn : kp ? kp : win_keychar(sc));
          need_draw = 1;
        }
        kbd_e0 = 0;
        continue;
      }
      /* Programa rodando no Command em foco: as teclas sao dele (k_getc). */
      if (!(gwins && gui_focus >= 0 && gui_focus < GUI_MAX && gwins[gui_focus].open) &&
          focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_CMD && wins[focus].st &&
          ((struct cmd_con*)wins[focus].st)->tty) {
        struct con_tty* ty = ((struct cmd_con*)wins[focus].st)->tty;
        if (sc == 0xE0) {
          kbd_e0 = 1;
          continue;
        }
        if (!(sc & 0x80) && sc != SCANCODE_LSHIFT && sc != SCANCODE_RSHIFT)
          tty_push(ty, syn ? syn : tty_key_of(sc, kbd_e0, kp));
        kbd_e0 = 0;
        continue;
      }
      if (kbd_e0 && !kp) {
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
        } else if (!(sc & 0x80) && focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_CMD) {
          struct cmd_con* cc = (struct cmd_con*)wins[focus].st;
          struct cmd_geo cg;
          int pg = 1;
          if (cc) {
            cmd_geom(&wins[focus], cc, title_h, scale, &cg);
            pg = cg.vis > 2 ? cg.vis - 1 : 1;
          }
          if (sc == 0x48) cmd_hist(&wins[focus], 1);
          else if (sc == 0x50) cmd_hist(&wins[focus], -1);
          else if (sc == 0x53) cmd_key(&wins[focus], 0x7f);
          else if (cc && sc == 0x49) cmd_scroll(cc, -pg);            /* PgUp */
          else if (cc && sc == 0x51) cmd_scroll(cc, pg);             /* PgDn */
          else if (cc && kbd_ctrl_down() && sc == 0x47) cmd_scroll(cc, -CMD_ROWS); /* Ctrl+Home */
          else if (cc && kbd_ctrl_down() && sc == 0x4F) cmd_scroll(cc, CMD_ROWS);  /* Ctrl+End */
          else if (cc && (sc == 0x4B || sc == 0x4D || sc == 0x47 || sc == 0x4F))
            cmd_move(cc, sc, keymap_shift());
          if (!wm_present_win(&wins[focus])) need_draw = 1;
        } else if (!(sc & 0x80) && focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE) {
          int top = title_h + note_menubar_h(ch);
          if (sc == 0x53) {
            /* Del: apaga a marcacao ou o caractere sob o cursor */
            if (!note_del_sel() && note_caret < note_len) {
              note_anc = note_caret + 1;
              note_del_sel();
            }
          } else if (sc == 0x4B || sc == 0x4D || sc == 0x48 || sc == 0x50 ||
                     sc == 0x47 || sc == 0x4F) {
            note_move(&wins[focus], scale, sc, keymap_shift());
          }
          note_follow_caret(&wins[focus], top, scale);
          need_draw = 1;
        }
        continue;
      }
      kbd_e0 = 0;
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
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_CMD) {
          cmd_set_input((struct cmd_con*)wins[focus].st, "");
          if (!wm_present_win(&wins[focus])) need_draw = 1;
          continue;
        }
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE && note_dirty)
          note_save();
        serial_puts("[visual] ESC\n");
        win_session_free(wins);
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
        char c = syn ? (char)syn : kp ? (char)kp : (char)win_keychar(sc);
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
          win_session_free(wins);
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
          } else if ((unsigned char)c >= 32 && (unsigned char)c != 127) {
            note_dlg_insert(c);
            need_draw = 1;
          }
          continue;
        }
        if (gwins && gui_focus >= 0 && gui_focus < GUI_MAX && gwins[gui_focus].open) {
          int k = (c == '\n') ? '\r' : (unsigned char)c;
          /* Ctrl+letra chega como caractere de controle (Ctrl+C = 3), como no
           * terminal. Ctrl+V cola a area de transferencia do kernel como
           * teclas, a nao ser que o programa a trate (k_gui_clip). Ctrl+H, I,
           * J e M seriam Backspace, Tab e Enter: nao vao. */
          if (kbd_ctrl_down()) {
            int l = (unsigned char)c | 0x20;
            if (l >= 'a' && l <= 'z') {
              if (l == 'v' && !gwins[gui_focus].clip) {
                gui_paste_start(gui_focus);
                continue;
              }
              if (l == 'h' || l == 'i' || l == 'j' || l == 'm') continue;
              k = l - 'a' + 1;
            }
          }
          gq_push(&gwins[gui_focus], GUI_EV_KEY, 0, 0, k);
          continue;
        }
        /* Ctrl+C copia, Ctrl+X recorta, Ctrl+V cola e Ctrl+A marca tudo, no
         * Command e no Bloco de notas. A area de transferencia e a do kernel,
         * a mesma do prompt do DOS. */
        if (kbd_ctrl_down() && focus >= 0 && wins[focus].used &&
            (wins[focus].kind == WM_KIND_CMD || wins[focus].kind == WM_KIND_NOTE)) {
          int l = (unsigned char)c | 0x20;
          if (l == 'c' || l == 'x' || l == 'v' || l == 'a') {
            if (wins[focus].kind == WM_KIND_CMD && wins[focus].st) {
              struct cmd_con* cc = (struct cmd_con*)wins[focus].st;
              if (l == 'c' || l == 'x') cmd_clip_copy(cc);
              if (l == 'x' && !cc->busy) cmd_del_sel(cc);
              if (l == 'v') cmd_paste(cc);
              if (l == 'a' && !cc->busy) {
                cc->sel = 1;
                cc->ar = cc->n;
                cc->ac = cmd_plen();
                cc->br = cc->n;
                cc->bc = cc->ac + cc->inlen;
                cc->cur = cc->inlen;
              }
            } else if (wins[focus].kind == WM_KIND_NOTE) {
              int a, b;
              if (note_missing) {
                note_buf[0] = '\0';
                note_len = 0;
                note_caret = 0;
                note_anc = -1;
                note_missing = 0;
              }
              if ((l == 'c' || l == 'x') && note_sel(&a, &b)) clip_set(note_buf + a, b - a);
              if (l == 'x') note_del_sel();
              if (l == 'v') {
                static char t[CLIP_MAX];
                int n = clip_get(t, (int)sizeof(t));
                note_del_sel();
                note_insert(t, n);
              }
              if (l == 'a') {
                note_anc = 0;
                note_caret = note_len;
              }
              note_follow_caret(&wins[focus], title_h + note_menubar_h(ch), scale);
            }
            need_draw = 1;
            continue;
          }
        }
        if (focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_CMD) {
          if (cmd_key(&wins[focus], c)) {
            if (wins[focus].st) kfree(wins[focus].st);
            wins[focus].st = 0;
            wins[focus].used = 0;
            focus = wm_find_kind(wins, WM_KIND_MGR);
            if (focus < 0) focus = 0;
            need_draw = 1;
          } else if (c != '\n' && c != '\r' && !wm_present_win(&wins[focus])) {
            need_draw = 1; /* Enter: cmd_enter ja redesenhou */
          }
          continue;
        }
        if (!(focus >= 0 && wins[focus].used && wins[focus].kind == WM_KIND_NOTE))
          continue;
        if (note_missing) {
          note_buf[0] = '\0';
          note_len = 0;
          note_caret = 0;
          note_anc = -1;
          note_missing = 0;
        }
        if (c == '\b') {
          /* apaga a marcacao ou o caractere antes do cursor */
          if (!note_del_sel() && note_caret > 0) {
            note_anc = note_caret - 1;
            note_del_sel();
          }
          note_follow_caret(&wins[focus], title_h + note_menubar_h(ch), scale);
          need_draw = 1;
          continue;
        }
        if (c == '\n' || c == '\r') c = '\n';
        note_del_sel(); /* digitar troca o que esta marcado */
        note_insert(&c, 1);
        note_follow_caret(&wins[focus], title_h + note_menubar_h(ch), scale);
        need_draw = 1;
      }
    }
  }
}

/* ---------------------------------------------------------------------------
 * API de configuracao de video (exportada como k_gui_quality, k_win_bg e
 * k_win_wallpaper). Usada pelo VIDEOCFG.EXE.
 * ------------------------------------------------------------------------- */
int gui_quality_get(void) { return gfx_quality; }

int gui_quality_set(int mode) {
  int old = gfx_quality;
  gfx_quality = mode ? 1 : 0;
  win_api_dirty = 1;
  return old;
}

int win_bg_set(uint32_t rgb) {
  if (desk_wall) {
    ufree(desk_wall);
    desk_wall = 0;
  }
  desk_rgb = rgb & 0xFFFFFFu;
  win_api_dirty = 1;
  return 0;
}

/* rgba: w*h*4 (R,G,B,A). Escala para a tela e guarda como fundo. */
int ui_menu_pos_set(int pos) {
  int old = ui_menu_pos;
  ui_menu_pos = pos ? 1 : 0;
  win_api_dirty = 1;
  return old;
}

int ui_glass_set(int on) {
  int old = ui_glass;
  ui_glass = on ? 1 : 0;
  win_api_dirty = 1;
  return old;
}

/* Pedido de abrir um programa no WIN (o laco executa no proximo quadro). */
int win_launch(const char* path) {
  int i = 0;
  if (!path || !path[0]) return -1;
  while (path[i] && i < (int)sizeof(launch_path) - 1) {
    launch_path[i] = path[i];
    i++;
  }
  launch_path[i] = 0;
  launch_arg[0] = 0;
  launch_pending = 1;
  return 0;
}

/* LINKEXT.DLL: abrir exe com o arquivo (o laco do WIN executa no proximo quadro). */
int win_launch_arg(const char* exe, const char* arg) {
  int i;
  if (!exe || !exe[0] || !arg) return -1;
  if (launch_pending) return -1;
  for (i = 0; exe[i] && i < (int)sizeof(launch_path) - 1; i++) launch_path[i] = exe[i];
  launch_path[i] = 0;
  for (i = 0; arg[i] && i < (int)sizeof(launch_arg) - 1; i++) launch_arg[i] = arg[i];
  launch_arg[i] = 0;
  launch_pending = 1;
  return 0;
}

int ui_ia_set(int on) {
  int old = ui_ia;
  ui_ia = on ? 1 : 0;
  if (ui_ia) ui_ia_launch = 1;
  win_api_dirty = 1;
  return old;
}

int ui_menu_pos_get(void) { return ui_menu_pos; }

int ui_theme_api(const char* set, struct ui_theme_info* out) {
  int rc = 0;
  if (set) rc = ui_theme_load(set);
  if (out) {
    int i;
    out->gen = ui_th.gen;
    out->modern = ui_th.modern;
    out->icons = ui_th.icons;
    for (i = 0; i < 15 && ui_th.name[i]; i++) out->name[i] = ui_th.name[i];
    out->name[i] = 0;
    for (i = 0; i < 39 && ui_th.dir[i]; i++) out->dir[i] = ui_th.dir[i];
    out->dir[i] = 0;
    out->face = ui_th.w_face;
    out->bg = ui_th.w_bg;
    out->panel = ui_th.w_panel;
    out->text = ui_th.w_text;
    out->text_dim = ui_th.w_dim;
    out->text_dis = ui_th.w_dis;
    out->sel_bg = ui_th.w_sel;
    out->sel_text = ui_th.w_sel_text;
    out->field_bg = ui_th.w_field;
    out->border = ui_th.w_border;
    out->accent = ui_th.w_accent;
    out->radius = ui_th.w_radius;
    out->btn_h = ui_th.w_btn_h;
    out->row_h = ui_th.w_row_h;
  }
  return set ? rc : (int)ui_th.gen;
}
int ui_glass_get(void) { return ui_glass; }
int ui_ia_get(void) { return ui_ia; }

int win_wall_set(const uint8_t* rgba, int sw, int sh) {
  uint32_t* buf;
  int x, y;
  if (desk_wall) {
    ufree(desk_wall);
    desk_wall = 0;
  }
  if (!rgba || sw <= 0 || sh <= 0 || sw > 8192 || sh > 8192 || !gfx_bochs)
    return -1;
  buf = (uint32_t*)umalloc_owned((uint32_t)(gfx_w * gfx_h * 4u), 0);
  if (!buf) return -1;
  /* escala por passo fixo (16.16): sem divisao por pixel */
  for (y = 0; y < (int)gfx_h; y++) {
    uint32_t sy = (uint32_t)(((uint64_t)(uint32_t)y * (uint32_t)sh) / gfx_h);
    const uint8_t* row = rgba + (size_t)sy * (size_t)sw * 4;
    uint32_t acc = 0;
    uint32_t step = (uint32_t)(((uint64_t)(uint32_t)sw << 16) / gfx_w);
    uint32_t* out = buf + (size_t)y * (size_t)gfx_w;
    for (x = 0; x < (int)gfx_w; x++) {
      const uint8_t* p = row + (size_t)(acc >> 16) * 4u;
      out[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      acc += step;
    }
  }
  desk_wall = buf;
  win_api_dirty = 1;
  return 0;
}

/* Le D:\SODOS\VIDEO.CFG (mode=0|1, bg=RRGGBB, wall=1) e aplica.
 * O papel de parede vem de D:\SODOS\WALL.RAW: "SODW", u16 w, u16 h, RGB. */
static int cfg_hex(const char* s) {
  int v = 0, i;
  for (i = 0; i < 6; i++) {
    char c = s[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    v = v * 16 + d;
  }
  return v;
}

static void win_load_wall(void) {
  int sz;
  uint8_t* raw;
  unsigned w, h;
  uint8_t* rgba;
  unsigned k;
  sz = file_size("D:\\SODOS\\WALL.RAW");
  if (sz < 16 || sz > (24 << 20)) return;
  /* file_load escreve um NUL no fim e le no maximo max-1: reserva +1 */
  raw = (uint8_t*)umalloc_owned((uint32_t)sz + 1u, 0);
  if (!raw) return;
  if (file_load("D:\\SODOS\\WALL.RAW", (char*)raw, sz + 1) != sz) {
    ufree(raw);
    return;
  }
  if (!(raw[0] == 'S' && raw[1] == 'O' && raw[2] == 'D' && raw[3] == 'W')) {
    ufree(raw);
    return;
  }
  w = (unsigned)raw[4] | ((unsigned)raw[5] << 8);
  h = (unsigned)raw[6] | ((unsigned)raw[7] << 8);
  if (!w || !h || w > 4096 || h > 4096 ||
      8u + w * h * 3u > (unsigned)sz) {
    ufree(raw);
    return;
  }
  rgba = (uint8_t*)umalloc_owned(w * h * 4u, 0);
  if (rgba) {
    for (k = 0; k < w * h; k++) {
      rgba[k * 4] = raw[8 + k * 3];
      rgba[k * 4 + 1] = raw[9 + k * 3];
      rgba[k * 4 + 2] = raw[10 + k * 3];
      rgba[k * 4 + 3] = 255;
    }
    win_wall_set(rgba, (int)w, (int)h);
    ufree(rgba);
  }
  ufree(raw);
}

static void win_load_cfg(void) {
  static char cfg[1024]; /* wall= com nome longo (ate 255) */
  int n, i, mode = 0, wall = 0;
  uint32_t bg = 0;
  gpu_cfg_line(0, 0);
  n = file_load("D:\\SODOS\\VIDEO.CFG", cfg, (int)sizeof(cfg) - 1);
  if (n <= 0) return;
  cfg[n] = 0;
  i = 0;
  while (i < n) {
    int j = i, k;
    while (j < n && cfg[j] != '\n' && cfg[j] != '\r') j++;
    k = i;
    if (k + 5 <= j && cfg[k] == 'm' && cfg[k + 1] == 'o' && cfg[k + 2] == 'd' &&
        cfg[k + 3] == 'e' && cfg[k + 4] == '=') {
      mode = cfg[k + 5] == '1' ? 1 : 0;
    } else if (k + 3 <= j && cfg[k] == 'b' && cfg[k + 1] == 'g' &&
               cfg[k + 2] == '=') {
      bg = (uint32_t)cfg_hex(cfg + k + 3);
    } else if (k + 5 <= j && cfg[k] == 'w' && cfg[k + 1] == 'a' &&
               cfg[k + 2] == 'l' && cfg[k + 3] == 'l' && cfg[k + 4] == '=') {
      wall = (cfg[k + 5] != 0 && cfg[k + 5] != '\r' && cfg[k + 5] != '\n');
    } else if (k + 8 <= j && cfg[k] == 'm' && cfg[k + 1] == 'e' && cfg[k + 2] == 'n' &&
               cfg[k + 3] == 'u' && cfg[k + 4] == 'p' && cfg[k + 5] == 'o' &&
               cfg[k + 6] == 's' && cfg[k + 7] == '=') {
      ui_menu_pos = (cfg[k + 8] == '1' || cfg[k + 8] == '2') ? 1 : 0;
    } else if (k + 6 <= j && cfg[k] == 'g' && cfg[k + 1] == 'l' && cfg[k + 2] == 'a' &&
               cfg[k + 3] == 's' && cfg[k + 4] == 's' && cfg[k + 5] == '=') {
      ui_glass = cfg[k + 6] == '1';
    } else if (k + 3 <= j && cfg[k] == 'i' && cfg[k + 1] == 'a' && cfg[k + 2] == '=') {
      ui_ia = cfg[k + 3] == '1';
      if (ui_ia) ui_ia_launch = 1;
    } else if (k + 4 <= j && cfg[k] == 'g' && cfg[k + 1] == 'p' && cfg[k + 2] == 'u') {
      gpu_cfg_line(cfg + k, j - k);
    }
    while (j < n && (cfg[j] == '\n' || cfg[j] == '\r')) j++;
    i = j;
  }
  gfx_quality = mode ? 1 : 0;
  desk_rgb = bg;
  if (wall) win_load_wall();
  ui_theme_boot();
  serial_puts("[visual] cfg mode=");
  serial_u16((uint16_t)(mode + 1));
  serial_puts(" bg=");
  serial_hex32(bg);
  serial_puts("\n");
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
  int vt_to = -1;   /* tela pedida com Ctrl+Fn durante o modo grafico */

  serial_puts("[visual] enter\n");
  (void)serial_log_save();   /* PC sem serial: D:\LOG\SERIAL.LOG ate aqui */
  if (!vga_bios_font_ok)
    vga_capture_bios_font();
  shift_on = 0;
  kbd_e0 = 0;
  pkt_idx = 0;
  cursor_saved = 0;
gfx_restart:
  gfx_bochs = 0;
  gfx_gop = 0;

  if (!gop_try_mode() && !bochs_try_mode()) {
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
  gfx_session_on = 1;
  if (session == GFX_SESSION_WIN) {
    win_load_cfg();
    gfx_gpu_try();
  }
  fbcon_suspend(1);
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

  {
    int ps2_ok;
    ps2in_raw(1);      /* o ACK do F4 e nosso, nao da IRQ12 */
    ps2_ok = ps2_mouse_init();
    ps2_enable_keyboard_port();
    ps2in_raw(0);
    if (tablet_hw_init() != 0) (void)xhci_mouse_init(ps2_ok);
  }
  ps2in_flush();       /* sobras de antes do WIN (eco do mouse, teclas velhas) */
  delay_loop();

  serial_puts("[visual] entrada no loop (ESC sai)\n");

  /* IRQ0 (timer), IRQ1 (teclado) e IRQ12 (mouse) ligadas: teclado e mouse
   * chegam pela fila de kernel/ps2in.c. */

  if (session == GFX_SESSION_WIN) {
    if (win_wm_run()) {
      bochs_disable_if_needed();
      serial_puts("[visual] troca de tela\n");
      (void)serial_log_save();
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
      int kp = 0;

      /* Ctrl+F1..F12 tambem troca de tela com o "visual" aberto: sai do modo
       * grafico e so entao seleciona a tela pedida. */
      if (!(sc & 0x80) && kbd_ctrl_down() && vt_hotkey(sc) >= 0) {
        vt_to = vt_hotkey(sc);
        break;
      }
      if (kbd_e0) {
        kbd_e0 = 0;
        if (!(sc & 0x80)) kp = keymap_ext_char(sc);
        if (!kp) continue;
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
        char c = kp ? (char)kp : (char)keymap_char(sc);
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
  gfx_session_on = 0;
  gpu_close();
  gfx_tela_uma();
  gfx_offscreen = 0;   /* com driver ele ficava 1 entre os quadros */

  cursor_restore();
  gfx_mode_sel = -1;
  serial_puts("[visual] modo texto 80x25 + DAC\n");
  bochs_disable_if_needed();
  if (vga_legacy()) {
    vga_mode_text();
    vga_dac_restore_std16();
  } else {
    fbcon_suspend(0);
  }
  clear_screen();
  if (vt_to >= 0 && session != GFX_SESSION_WIN) {
    int to = vt_to;
    vt_to = -1;
    vt_select(to);
  }
  serial_puts("[visual] fim\n");
}

void visual_run(void) {
  gfx_session_run(GFX_SESSION_VISUAL);
}

void win_run(void) {
  gfx_session_run(GFX_SESSION_WIN);
}

static void vram_name(char* d, const char* s) {
  int i;
  for (i = 0; i < 23 && s[i]; i++) d[i] = s[i];
  for (; i < 24; i++) d[i] = 0;
}

/* Para o TOP: a placa, a memoria de video e o que o modo grafico ocupa.
 * Le so o estado que ja existe; nao reprograma nada. */
int win_vram_info(struct k_vram* v) {
  uint32_t vram = 0, frame, n = 0, kb = 0;
  int i;
  if (!v) return -1;
  for (i = 0; i < (int)sizeof(*v); i++) ((uint8_t*)v)[i] = 0;
  v->lfb = pci_find_display(&v->pci_id);
  /* a placa pode estar atras de uma ponte (notebook: barramento 4) */
  if (!v->pci_id) v->pci_id = gpu_find_display();
  if (gfx_gop || (!gfx_session_on && !vga_legacy())) {
    const struct BootInfo* bi = bootinfo_get();
    vram_name(v->dev, "GOP (UEFI)");
    if (bi && bi->fb_addr) {
      v->lfb = (uint32_t)bi->fb_addr;
      /* o GOP so conta o quadro visivel; o resto da placa nao aparece */
      v->vram_kb = (uint32_t)(((uint64_t)bi->fb_pitch * bi->fb_height + 1023u) >> 10);
    }
  } else if (bochs_present()) {
    vram_name(v->dev, "Bochs VBE");
    if (bochs_vram_bytes(&vram)) v->vram_kb = vram >> 10;
  } else {
    vram_name(v->dev, "VGA");
    v->vram_kb = 256;
  }
  if (gpu_name()) {
    vram_name(v->drv, gpu_name());
    v->gpu_kb = gpu_mem_kb();
  }
  if (!gfx_session_on) {
    v->state = 0;
    /* tela de texto 80x25 (4 KB) + fonte no plano 2 (8 KB); no GOP o fbcon
     * desenha o texto no proprio framebuffer */
    if (vga_legacy()) {
      v->w = 80;
      v->h = 25;
      v->bpp = 4;
      v->pitch = 160;
      v->text_kb = 12;
    } else {
      v->front_kb = v->vram_kb;
    }
    return 0;
  }
  v->state = gfx_live ? 1 : 2;
  v->w = gfx_w;
  v->h = gfx_h;
  v->bpp = gfx_bochs ? 32 : 8;
  v->pitch = gfx_bochs ? gfx_pitch_bytes : FB13_W;
  frame = gfx_bochs ? gfx_pitch_bytes * gfx_h : FB13_W * FB13_H;
  v->front_kb = (frame + 1023u) >> 10;
  if (gfx_dbuf) v->page2_kb = v->front_kb;
  if (gfx_back) {
    v->back_addr = (uint32_t)(uintptr_t)gfx_back;
    v->back_kb = (gfx_w * gfx_h * (gfx_bochs ? 4u : 1u) + 1023u) >> 10;
  }
  if (desk_wall) v->wall_kb = (gfx_w * gfx_h * 4u + 1023u) >> 10;
  if (gwins) {
    for (i = 0; i < GUI_MAX; i++) {
      if (!gwins[i].open || !gwins[i].pix) continue;
      n++;
      kb += ((uint32_t)gwins[i].cw * (uint32_t)gwins[i].ch * 4u + 1023u) >> 10;
    }
  }
  v->win_n = n;
  v->win_kb = kb;
  return 0;
}
