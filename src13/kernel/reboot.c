/* reboot.c - Ctrl+Alt+Del e reinicio do PC (freestanding) */
#include "reboot.h"
#include "serial.h"

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

void kbd_watch(uint8_t sc) {
  int ext = kw_e0;
  int up = sc & 0x80;
  uint8_t key = sc & 0x7F;
  uint8_t side;

  if (kw_skip) {
    kw_skip--;
    return;
  }
  if (sc == 0xE0) {
    kw_e0 = 1;
    return;
  }
  kw_e0 = 0;
  if (sc == 0xE1) {
    kw_skip = 2;
    return;
  }
  if (sc == 0xFA || sc == 0xFE)   /* ACK / resend do controlador */
    return;

  side = ext ? 2 : 1;
  if (key == 0x1D) {
    if (up) kw_ctrl &= (uint8_t)~side;
    else kw_ctrl |= side;
    return;
  }
  if (key == 0x38) {
    if (up) kw_alt &= (uint8_t)~side;
    else kw_alt |= side;
    return;
  }
  /* 0x53: Delete (E0 53) ou o Del do teclado numerico. */
  if (key == 0x53 && !up && kw_ctrl && kw_alt) {
    serial_puts("[reboot] Ctrl+Alt+Del\n");
    sys_reboot();
  }
}
