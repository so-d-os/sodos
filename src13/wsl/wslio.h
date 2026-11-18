/* wslio.h - Fonte de bytes comum aos leitores do WSL (NTFS, VHDX, ext4).
 *
 * Cada camada le a de baixo por um wsl_io: o SSD cru (ou uma imagem no host
 * de teste) -> particao NTFS -> arquivo ext4.vhdx -> disco virtual -> ext4.
 * Tudo so leitura: nada aqui grava no disco do Windows. */
#ifndef WSLIO_H
#define WSLIO_H
#include <stdint.h>

typedef struct wsl_io {
  /* Le len bytes em off. 0 ok; <0 erro. Le alem do fim = erro. */
  int (*read)(void* ctx, uint64_t off, void* buf, uint32_t len);
  void* ctx;
  uint64_t size; /* bytes (0 = desconhecido) */
} wsl_io;

static inline int wio_read(const wsl_io* io, uint64_t off, void* buf, uint32_t len) {
  return io->read(io->ctx, off, buf, len);
}

static inline uint16_t rd16(const void* p) {
  const uint8_t* b = (const uint8_t*)p;
  return (uint16_t)(b[0] | (b[1] << 8));
}
static inline uint32_t rd32(const void* p) {
  const uint8_t* b = (const uint8_t*)p;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline uint64_t rd64(const void* p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32((const uint8_t*)p + 4) << 32);
}

/* Mensagem de erro da ultima falha (estatica). */
extern char wsl_err[160];
void wsl_seterr(const char* a, const char* b);

#endif
