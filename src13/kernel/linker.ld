/* linker.ld - kernel 64 bits, identidade, carga em 0x10000 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

/* Endereços fixos para I/O (partition.c / fat_format.c): 0x80000-0x80620 em uso; 0x80800 = buffer de formatação (512 bytes) */
FORMAT_BUF_ADDR = 0x00080800;

SECTIONS
{
  . = 0x00010000;

  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : {
    __bss_start = .;
    *(.bss*) *(COMMON)
    __bss_end = .;
  }

  /* Sem excecoes: estas tabelas so empurravam o nucleo para cima de 0x58000. */
  /DISCARD/ : {
    *(.eh_frame)
    *(.eh_frame_hdr)
    *(.note.GNU-stack)
    *(.note.gnu.property)
  }

  /* Reserva 512 bytes em 0x80800 para fat_format (evita sobrepor .rodata/.bss do kernel) */
  . = FORMAT_BUF_ADDR;
  .format_sector_buf : {
    format_buf_region = .;
    . = . + 512;
  }
}
