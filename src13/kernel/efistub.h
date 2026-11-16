/* efistub.h - video pelo firmware antes do ExitBootServices (efistub.c).
 * So existe no kernel_multiboot.elf; o kernel.elf (BIOS) nao tem efistub.o,
 * por isso bootinfo.c e visual.c usam estes simbolos por referencia fraca. */
#pragma once
#include <stdint.h>

#define EFISTUB_MAX_MODES 32

/* Saida GOP que o kernel recebeu: modos de 32 bpp que a placa aceita, o
 * modo ligado, a resolucao nativa do monitor (EDID; 0 = sem EDID) e se o
 * modo veio do \SODOS\TELA.CFG (tela=LxA) ou do padrao (nativa). */
struct efistub_video {
  uint32_t n;
  uint16_t w[EFISTUB_MAX_MODES];
  uint16_t h[EFISTUB_MAX_MODES];
  uint16_t cur_w, cur_h;
  uint16_t native_w, native_h;
  uint8_t from_cfg;
};

/* Chamado por efi_entry (multiboot_header.asm) com os boot services vivos.
 * 0 = ExitBootServices feito. */
int efistub_main(const uint8_t* mbi);

/* Manda o texto do diagnostico, linha a linha, para puts_fn (serial_puts). */
void efistub_report(void (*puts_fn)(const char*));

/* 0 se o boot nao passou pelo stub (BIOS, GRUB sem UEFI). */
const struct efistub_video* efistub_video(void);
