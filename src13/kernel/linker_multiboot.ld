/* linker_multiboot.ld - para ISO com GRUB */
ENTRY(multiboot_entry)

SECTIONS
{
  . = 0x00100000;

  .multiboot : { *(.multiboot) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
}
