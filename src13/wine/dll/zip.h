/* zip.h - leitor de ZIP/inflate do WINE.DLL (zip.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

struct zip {
  const uint8_t* data;
  uint64_t size;
  uint64_t count, cd_off, cd_size;
  uint64_t pos, index;
};
struct zip_entry {
  char name[260];
  int method;
  int is_dir;
  uint32_t crc;
  uint64_t csize, usize, local;
};

int zip_open(struct zip* z, const uint8_t* data, uint64_t size);
int zip_next(struct zip* z, struct zip_entry* en);
uint8_t* zip_extract(struct zip* z, const struct zip_entry* en);
long zip_inflate(const uint8_t* in, size_t inlen, uint8_t* out, size_t outcap);
