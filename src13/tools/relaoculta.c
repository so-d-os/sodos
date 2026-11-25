/*
 * relaoculta.c - deixa o kernel ligado com ld -q carregavel pelo GRUB.
 *
 *   tools/relaoculta kernel_multiboot.elf [...]
 *
 * O TROCA.EXE (utils/troca.c) precisa das relocacoes do kernel novo, por isso
 * o Makefile liga com -q (--emit-relocs). So que o multiboot2 do GRUB recusa
 * qualquer ELF que tenha uma secao SHT_REL ou SHT_RELA: "ELF files with relocs
 * are not supported yet" e depois "voce precisa carregar o kernel primeiro"
 * (build 54, 2026-11-25: o notebook nao subiu).
 *
 * Aqui so muda o sh_type dessas secoes para SHT_LOUSER + tipo (0x80000004 no
 * RELA, 0x80000009 no REL). Os bytes das relocacoes, os nomes (.rela.text...)
 * e os indices da tabela de simbolos ficam iguais: o GRUB carrega o kernel e
 * o TROCA continua achando .rela.text/.rela.rodata/.rela.data pelo nome.
 * Rodar de novo no mesmo arquivo nao muda nada.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SHT_RELA 4u
#define SHT_REL 9u
#define SHT_LOUSER 0x80000000u

static uint64_t le(const unsigned char* p, int n) {
  uint64_t v = 0;
  while (n--) v = (v << 8) | p[n];
  return v;
}

static int oculta(const char* path) {
  unsigned char eh[64], t[4];
  uint64_t shoff;
  unsigned shentsize, shnum, i, n = 0;
  FILE* f = fopen(path, "r+b");
  if (!f) {
    perror(path);
    return 1;
  }
  if (fread(eh, 1, sizeof eh, f) != sizeof eh || memcmp(eh, "\177ELF", 4) || eh[4] != 2 || eh[5] != 1) {
    fprintf(stderr, "relaoculta: %s nao e um ELF de 64 bits little-endian\n", path);
    fclose(f);
    return 1;
  }
  shoff = le(eh + 0x28, 8);
  shentsize = (unsigned)le(eh + 0x3A, 2);
  shnum = (unsigned)le(eh + 0x3C, 2);
  if (shnum && shentsize != 64) {
    fprintf(stderr, "relaoculta: %s: e_shentsize %u (esperado 64)\n", path, shentsize);
    fclose(f);
    return 1;
  }
  for (i = 0; i < shnum; i++) {
    uint32_t tipo;
    long pos = (long)(shoff + (uint64_t)i * 64 + 4); /* sh_name (4), sh_type (4) */
    if (fseek(f, pos, SEEK_SET) || fread(t, 1, 4, f) != 4) {
      fprintf(stderr, "relaoculta: %s: cabecalho de secao %u fora do arquivo\n", path, i);
      fclose(f);
      return 1;
    }
    tipo = (uint32_t)le(t, 4);
    if (tipo != SHT_RELA && tipo != SHT_REL) continue;
    tipo |= SHT_LOUSER;
    t[0] = (unsigned char)tipo;
    t[1] = (unsigned char)(tipo >> 8);
    t[2] = (unsigned char)(tipo >> 16);
    t[3] = (unsigned char)(tipo >> 24);
    if (fseek(f, pos, SEEK_SET) || fwrite(t, 1, 4, f) != 4) {
      perror(path);
      fclose(f);
      return 1;
    }
    n++;
  }
  if (fclose(f)) {
    perror(path);
    return 1;
  }
  printf("relaoculta: %s: %u secao(oes) de relocacao escondida(s) do GRUB\n", path, n);
  return 0;
}

int main(int argc, char** argv) {
  int i, err = 0;
  if (argc < 2) {
    fprintf(stderr, "uso: %s kernel.elf [...]\n", argv[0]);
    return 2;
  }
  for (i = 1; i < argc; i++) err |= oculta(argv[i]);
  return err;
}
