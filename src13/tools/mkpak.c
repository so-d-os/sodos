/*
 * mkpak.c - gera o pacote de boot SODOS.PAK (kernel/bootpak.c).
 *
 *   tools/mkpak sodos.pak origem=SODOS/DRV/FAT32.DLL [origem=destino ...]
 *
 * Formato (little-endian): pak_hdr {"SODOSPAK", versao 1, quantos, tamanho
 * total, 0}, depois uma pak_ent por arquivo {caminho[64] com '\', offset,
 * tamanho, FNV-1a, 0} e os dados, cada um alinhado em 16 bytes. Origem que
 * nao existe fica fora (aviso), para o os.img sair mesmo sem o driver.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAK_PATH 64
#define MAXF 64

struct pak_hdr {
  char magic[8];
  uint32_t version, count, total, pad;
};

struct pak_ent {
  char path[PAK_PATH];
  uint32_t off, size, fnv, pad;
};

static uint32_t fnv(const unsigned char* p, size_t n) {
  uint32_t h = 2166136261u;
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

int main(int argc, char** argv) {
  struct pak_hdr h;
  struct pak_ent e[MAXF];
  unsigned char* data[MAXF];
  uint32_t off;
  int n = 0, i;
  FILE* f;
  if (argc < 2) {
    fprintf(stderr, "uso: mkpak saida.pak origem=destino ...\n");
    return 2;
  }
  memset(e, 0, sizeof(e));
  for (i = 2; i < argc && n < MAXF; i++) {
    char* eq = strchr(argv[i], '=');
    long sz;
    size_t k;
    FILE* in;
    if (!eq || strlen(eq + 1) >= PAK_PATH) {
      fprintf(stderr, "mkpak: item invalido: %s\n", argv[i]);
      return 2;
    }
    *eq = 0;
    in = fopen(argv[i], "rb");
    if (!in) {
      fprintf(stderr, "mkpak: aviso: %s nao existe, fica fora do pacote\n", argv[i]);
      continue;
    }
    fseek(in, 0, SEEK_END);
    sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    data[n] = malloc(sz ? (size_t)sz : 1);
    if (!data[n] || fread(data[n], 1, (size_t)sz, in) != (size_t)sz) {
      fprintf(stderr, "mkpak: erro lendo %s\n", argv[i]);
      return 1;
    }
    fclose(in);
    for (k = 0; eq[1 + k]; k++) e[n].path[k] = eq[1 + k] == '/' ? '\\' : eq[1 + k];
    e[n].size = (uint32_t)sz;
    e[n].fnv = fnv(data[n], (size_t)sz);
    n++;
  }
  off = (uint32_t)(sizeof(h) + n * sizeof(struct pak_ent));
  for (i = 0; i < n; i++) {
    off = (off + 15u) & ~15u;
    e[i].off = off;
    off += e[i].size;
  }
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, "SODOSPAK", 8);
  h.version = 1;
  h.count = (uint32_t)n;
  h.total = off;
  f = fopen(argv[1], "wb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  fwrite(&h, sizeof(h), 1, f);
  fwrite(e, sizeof(struct pak_ent), (size_t)n, f);
  for (i = 0; i < n; i++) {
    static const unsigned char zero[16];
    long pos = ftell(f);
    fwrite(zero, 1, (size_t)(e[i].off - (uint32_t)pos), f);
    fwrite(data[i], 1, e[i].size, f);
  }
  fclose(f);
  printf("mkpak: %s com %d arquivo(s), %u bytes\n", argv[1], n, off);
  return 0;
}
