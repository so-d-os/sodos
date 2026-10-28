/* linker.ld - kernel 64 bits, identidade, carga em 16 MiB.
 *
 * O kernel NAO ocupa mais a memoria baixa (0x10000-0x58000).  Ele e carregado
 * pelo stage 2 em 0x01000000, deixando livres a pilha (0x9FC00), a tela em
 * memoria (0x100000), a FAT (0xA00000) e o heap do kernel (0xA80000).
 *
 * FORMAT_BUF_ADDR e um simbolo absoluto: o buffer de 512 bytes usado por
 * fat_format.c continua em memoria baixa (o driver de disco so aceita buffers
 * em [0x60000,0x100000)), mas nao entra no kernel.bin.  Antes ele era uma
 * secao carregavel em 0x80800, o que fazia o objcopy gerar uma imagem que
 * atravessava o slot de aplicacao.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

KERNEL_LOAD    = 0x01000000;
FORMAT_BUF_ADDR = 0x00080800;

SECTIONS
{
  . = KERNEL_LOAD;

  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : {
    __bss_start = .;
    *(.bss*) *(COMMON)
    __bss_end = .;
  }

  /* Sem excecoes: o kernel freestanding nao as usa. */
  /DISCARD/ : {
    *(.eh_frame)
    *(.eh_frame_hdr)
    *(.note.GNU-stack)
    *(.note.gnu.property)
  }
}

/* 512 bytes reservados em 0x80800 para fat_format (fora da imagem carregada). */
format_buf_region = FORMAT_BUF_ADDR;
