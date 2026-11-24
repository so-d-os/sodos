/* reboot.c - Ctrl+Alt+Del, Ctrl+C e reinicio do PC (freestanding) */
#include "reboot.h"
#include "serial.h"
#include "visual.h"
#include "keymap.h"
#include "hang.h"

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static void reboot_delay(void) {
  for (volatile int i = 0; i < 1000000; i++) { }
}

/* O FAT grava direto no disco (sem cache sujo), entao nada a descarregar. */
void sys_reboot(void) {
  serial_puts("[reboot] reiniciando\n");
  __asm__ volatile ("cli");

  /* 1) 8042: comando 0xFE pulsa a linha de reset da CPU. */
  for (int n = 0; n < 0x10000 && (inb(0x64) & 2); n++) { }
  outb(0x64, 0xFE);
  reboot_delay();

  /* 2) Reset Control Register do chipset (PIIX/ICH/Q35): hard reset. */
  outb(0xCF9, 0x02);
  reboot_delay();
  outb(0xCF9, 0x06);
  reboot_delay();

  /* 3) Triple fault: IDT vazia e qualquer excecao derruba a CPU. */
  {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(idtr));
  }
  for (;;) {
    __asm__ volatile ("hlt");
  }
}

/* Estado proprio: cada leitor de 0x60 (shell, WIN, mouse) tem o seu, mas
 * todos passam os bytes por aqui. Esquerdo e direito em bits separados
 * para soltar um nao zerar o outro. */
static int kw_e0;
static int kw_skip;   /* Pause: E1 1D 45 E1 9D C5 nao e Ctrl */
static uint8_t kw_ctrl;
static uint8_t kw_alt;
static int kw_break;
static int kw_armed;   /* comando do prompt rodando: no WIN Ctrl+C tambem para */
static int kw_raw;     /* k_kbd_raw: Ctrl+C e tecla comum */

void kbd_raw_set(int on) {
  kw_raw = on;
  kw_break = 0;
}

int kbd_raw(void) {
  return kw_raw;
}

void kbd_break_arm(int on) {
  kw_armed = on;
}

int kbd_ctrl_down(void) {
  return kw_ctrl != 0;
}

int kbd_alt_down(void) {
  return kw_alt != 0;
}

int kbd_break_take(void) {
  int b = kw_break;
  kw_break = 0;
  return b;
}

/* Del do Ctrl+Alt+Del ja tratado: a repeticao automatica (tecla segurada) e
 * a soltura tambem sao engolidas. */
static int kw_cad;

int kbd_watch(uint8_t sc) {
  int ext = kw_e0;
  int up = sc & 0x80;
  uint8_t key = sc & 0x7F;
  uint8_t side;

  if (kw_skip) {
    kw_skip--;
    return KBW_PASS;
  }
  if (sc == 0xE0) {
    kw_e0 = 1;
    return KBW_E0;
  }
  kw_e0 = 0;
  if (sc == 0xE1) {
    kw_skip = 2;
    return KBW_PASS;
  }
  if (sc == 0xFA || sc == 0xFE)   /* ACK / resend do controlador */
    return KBW_PASS;

  side = ext ? 2 : 1;
  /* 0x53: Delete (E0 53) ou o Del do teclado numerico. Ctrl+Alt+Del nao
   * reinicia mais na hora (hang.c): fora da tela 1 leva a ela (com o HANG.LOG
   * se o sistema estava parado), na tela 1 reinicia. O Del nunca chega ao
   * programa: no Explorer ele apagaria o arquivo selecionado. */
  if (key == 0x53) {
    if (!up && kw_ctrl && kw_alt) {
      if (!kw_cad) {
        kw_cad = 1;
        hang_ctrl_alt_del();
      }
      return KBW_SWALLOW;
    }
    if (up && kw_cad) {
      kw_cad = 0;
      return KBW_SWALLOW;
    }
  }
  keymap_watch(ext, key, up);
  if (key == 0x1D) {
    if (up) kw_ctrl &= (uint8_t)~side;
    else kw_ctrl |= side;
    return KBW_PASS;
  }
  if (key == 0x38) {
    if (up) kw_alt &= (uint8_t)~side;
    else kw_alt |= side;
    return KBW_PASS;
  }
  /* 0x2E: C. E0 46: Break (Ctrl+Pause). O WIN trata Ctrl+C como tecla. */
  if (!up && kw_ctrl && ((key == 0x2E && !ext) || (key == 0x46 && ext))) {
    if (!kw_raw && (kw_armed || !win_screen_size(0, 0))) kw_break = 1;
    return KBW_PASS;
  }
  /* Ctrl+F1: troca de tela pelo mundo; se o mundo estiver parado, o
   * guardiao (ps2in.c) usa o aviso para destravar ou reiniciar. */
  if (key == 0x3B && !up && !ext && kw_ctrl) kbd_emergency_note();
  return KBW_PASS;
}
