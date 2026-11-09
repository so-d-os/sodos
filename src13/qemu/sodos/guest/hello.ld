/* O cabecalho multiboot precisa estar nos primeiros 8 KB do arquivo. */
ENTRY(_start)
SECTIONS {
  . = 0x100000;
  .text : { KEEP(*(.multiboot)) *(.text) *(.rodata*) }
  .data : { *(.data) *(.bss) }
}
