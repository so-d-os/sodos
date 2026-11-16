/* efistub.h - diagnostico de video antes do ExitBootServices (efistub.c).
 * So existe no kernel_multiboot.elf; o kernel.elf (BIOS) nao tem efistub.o,
 * por isso o bootinfo.c chama efistub_report por referencia fraca. */
#pragma once
#include <stdint.h>

/* Chamado por efi_entry (multiboot_header.asm) com os boot services vivos.
 * 0 = ExitBootServices feito. */
int efistub_main(const uint8_t* mbi);

/* Manda o texto do diagnostico, linha a linha, para puts_fn (serial_puts). */
void efistub_report(void (*puts_fn)(const char*));
