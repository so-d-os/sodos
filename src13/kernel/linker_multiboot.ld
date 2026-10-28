/* linker_multiboot.ld - caminho legado para ISO com GRUB.
 * O kernel Multiboot tambem fica em 16 MiB; o buffer de formatacao continua
 * absoluto em 0x80800 e nao entra na imagem carregada. */
ENTRY(multiboot_entry)

SECTIONS
{
  . = 0x01000000;

  .multiboot : { *(.multiboot) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
}

format_buf_region = 0x00080800;
