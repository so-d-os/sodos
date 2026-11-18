/* vhdx.h - Disco virtual VHDX (dinamico ou fixo; sem diferencial) so leitura.
 * E o formato do ext4.vhdx do WSL2. */
#ifndef WSL_VHDX_H
#define WSL_VHDX_H
#include "wslio.h"

typedef struct vhdx vhdx;

/* 1 se os 8 primeiros bytes sao "vhdxfile". */
int vhdx_probe(const uint8_t* b8);
vhdx* vhdx_open(const wsl_io* file);
void vhdx_close(vhdx* d);
uint64_t vhdx_size(const vhdx* d);          /* bytes do disco virtual */
uint32_t vhdx_block_size(const vhdx* d);
/* 1 se o log nao foi reaplicado (Windows nao desligou o WSL direito). */
int vhdx_dirty(const vhdx* d);
int vhdx_pread(vhdx* d, uint64_t off, void* buf, uint32_t len);
/* wsl_io do disco virtual. */
void vhdx_io(vhdx* d, wsl_io* out);

#endif
