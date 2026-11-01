/* linker_multiboot.ld - kernel atual carregado pelo GRUB via Multiboot2.
 * Mesmo layout de linker.ld (0x01000000). O cabecalho Multiboot2 fica no
 * inicio da imagem, dentro dos primeiros 32 KiB exigidos pela especificacao.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(multiboot_entry)

KERNEL_LOAD     = 0x01000000;
FORMAT_BUF_ADDR = 0x00080800;

SECTIONS
{
  . = KERNEL_LOAD;

  .multiboot : ALIGN(8) { *(.multiboot) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : {
    __bss_start = .;
    *(.bss*) *(COMMON)
    __bss_end = .;
  }

  /DISCARD/ : {
    *(.eh_frame)
    *(.eh_frame_hdr)
    *(.note.GNU-stack)
    *(.note.gnu.property)
  }
}

format_buf_region = FORMAT_BUF_ADDR;
