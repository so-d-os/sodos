/* vhdx.c - Leitor VHDX (MS-VHDX v1). Ver vhdx.h.
 *
 *   0       identificador "vhdxfile"
 *   64 KB   cabecalho 1 ("head"), 128 KB cabecalho 2: vale o de maior sequencia
 *   192 KB  tabela de regioes ("regi"): onde ficam a BAT e os metadados
 *   metadados: tamanho do bloco, tamanho do disco, setor logico
 *   BAT: uma entrada de 64 bits por bloco de dados, com uma entrada de bitmap
 *        de setores intercalada a cada chunk_ratio blocos.
 * Estado do bloco: 6 = presente (offset em MB nos bits 20..63); 0/1/2/3 = le
 * zeros; 7 (parcial) so existe em disco diferencial, que nao lemos. */
#include <stdlib.h>
#include <string.h>
#include "vhdx.h"

static const uint8_t G_BAT[16] = {0x66, 0x77, 0xC2, 0x2D, 0x23, 0xF6, 0x00, 0x42,
                                  0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08};
static const uint8_t G_META[16] = {0x06, 0xA2, 0x7C, 0x8B, 0x90, 0x47, 0x9A, 0x4B,
                                   0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E};
static const uint8_t G_FILEPARAM[16] = {0x37, 0x67, 0xA1, 0xCA, 0x36, 0xFA, 0x43, 0x4D,
                                        0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B};
static const uint8_t G_VDSIZE[16] = {0x24, 0x42, 0xA5, 0x2F, 0x1B, 0xCD, 0x76, 0x48,
                                     0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8};
static const uint8_t G_LSS[16] = {0x1D, 0xBF, 0x41, 0x81, 0x6F, 0xA9, 0x09, 0x47,
                                  0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F};

struct vhdx {
  wsl_io io;
  uint64_t size;
  uint32_t block;
  uint32_t lss;
  uint64_t chunk_ratio;
  uint64_t* bat;
  uint64_t bat_n;
  int dirty;
};

int vhdx_probe(const uint8_t* b) { return memcmp(b, "vhdxfile", 8) == 0; }

static int zero16(const uint8_t* g) {
  int i;
  for (i = 0; i < 16; i++)
    if (g[i]) return 0;
  return 1;
}

vhdx* vhdx_open(const wsl_io* file) {
  uint8_t b[4096], h1[4096], h2[4096];
  const uint8_t* h;
  uint64_t bat_off = 0, meta_off = 0;
  uint32_t bat_len = 0, n, i, has_parent = 0;
  vhdx* d;
  if (wio_read(file, 0, b, 8) || !vhdx_probe(b)) {
    wsl_seterr("vhdx: nao e VHDX", 0);
    return 0;
  }
  if (wio_read(file, 64 * 1024, h1, 4096) || wio_read(file, 128 * 1024, h2, 4096)) return 0;
  {
    int ok1 = memcmp(h1, "head", 4) == 0, ok2 = memcmp(h2, "head", 4) == 0;
    if (!ok1 && !ok2) {
      wsl_seterr("vhdx: sem cabecalho", 0);
      return 0;
    }
    h = (ok1 && (!ok2 || rd64(h1 + 8) >= rd64(h2 + 8))) ? h1 : h2;
  }
  d = (vhdx*)calloc(1, sizeof *d);
  if (!d) return 0;
  d->io = *file;
  d->dirty = !zero16(h + 48); /* LogGuid (depois de FileWrite/DataWrite) */
  /* tabela de regioes */
  if (wio_read(file, 192 * 1024, b, 4096) || memcmp(b, "regi", 4)) {
    wsl_seterr("vhdx: tabela de regioes invalida", 0);
    goto fail;
  }
  n = rd32(b + 8);
  for (i = 0; i < n && 16 + i * 32 + 32 <= 4096; i++) {
    const uint8_t* e = b + 16 + i * 32;
    if (!memcmp(e, G_BAT, 16)) {
      bat_off = rd64(e + 16);
      bat_len = rd32(e + 24);
    } else if (!memcmp(e, G_META, 16)) {
      meta_off = rd64(e + 16);
    } else if (rd32(e + 28) & 1) {
      wsl_seterr("vhdx: regiao obrigatoria desconhecida", 0);
      goto fail;
    }
  }
  if (!bat_off || !meta_off) {
    wsl_seterr("vhdx: sem BAT ou metadados", 0);
    goto fail;
  }
  /* metadados */
  if (wio_read(file, meta_off, b, 4096) || memcmp(b, "metadata", 8)) {
    wsl_seterr("vhdx: metadados invalidos", 0);
    goto fail;
  }
  n = rd16(b + 10);
  for (i = 0; i < n && 32 + i * 32 + 32 <= 4096; i++) {
    const uint8_t* e = b + 32 + i * 32;
    uint32_t off = rd32(e + 16);
    uint8_t v[8];
    if (wio_read(file, meta_off + off, v, 8)) goto fail;
    if (!memcmp(e, G_FILEPARAM, 16)) {
      d->block = rd32(v);
      has_parent = (rd32(v + 4) >> 1) & 1;
    } else if (!memcmp(e, G_VDSIZE, 16)) {
      d->size = rd64(v);
    } else if (!memcmp(e, G_LSS, 16)) {
      d->lss = rd32(v);
    }
  }
  if (has_parent) {
    wsl_seterr("vhdx: disco diferencial nao suportado", 0);
    goto fail;
  }
  if (d->block < (1u << 20) || (d->block & (d->block - 1)) || !d->size ||
      (d->lss != 512 && d->lss != 4096)) {
    wsl_seterr("vhdx: parametros invalidos", 0);
    goto fail;
  }
  d->chunk_ratio = ((uint64_t)1 << 23) * d->lss / d->block;
  {
    uint64_t blocks = (d->size + d->block - 1) / d->block;
    uint64_t need = blocks + (blocks - 1) / d->chunk_ratio;
    d->bat_n = need;
    if (need * 8 > bat_len) {
      wsl_seterr("vhdx: BAT pequena", 0);
      goto fail;
    }
    d->bat = (uint64_t*)malloc((size_t)(need * 8));
    if (!d->bat) goto fail;
    /* A BAT de um disco de 1 TB com blocos de 32 MB tem ~260 KB: le de uma vez
     * em pedacos (o wsl_io aceita ate 4 GB, mas o NVMe prefere pedacos). */
    for (i = 0; (uint64_t)i * 65536 < need * 8; i++) {
      uint64_t o = (uint64_t)i * 65536, l = need * 8 - o;
      if (l > 65536) l = 65536;
      if (wio_read(file, bat_off + o, (uint8_t*)d->bat + o, (uint32_t)l)) goto fail;
    }
  }
  return d;
fail:
  vhdx_close(d);
  return 0;
}

void vhdx_close(vhdx* d) {
  if (!d) return;
  free(d->bat);
  free(d);
}

uint64_t vhdx_size(const vhdx* d) { return d->size; }
uint32_t vhdx_block_size(const vhdx* d) { return d->block; }
int vhdx_dirty(const vhdx* d) { return d->dirty; }

int vhdx_pread(vhdx* d, uint64_t off, void* buf, uint32_t len) {
  uint8_t* p = (uint8_t*)buf;
  if (off > d->size || len > d->size - off) {
    wsl_seterr("vhdx: leitura alem do fim", 0);
    return -1;
  }
  while (len) {
    uint64_t blk = off / d->block, in = off % d->block;
    uint64_t take = d->block - in;
    uint64_t e;
    if (take > len) take = len;
    e = d->bat[blk + blk / d->chunk_ratio];
    switch (e & 7) {
    case 6: {
      uint64_t fo = (e >> 20) << 20;
      if (wio_read(&d->io, fo + in, p, (uint32_t)take)) {
        wsl_seterr("vhdx: erro lendo bloco", 0);
        return -1;
      }
      break;
    }
    case 7:
      wsl_seterr("vhdx: bloco parcial (diferencial)", 0);
      return -1;
    default:
      memset(p, 0, (size_t)take);
    }
    p += take;
    off += take;
    len -= (uint32_t)take;
  }
  return 0;
}

static int vio_read(void* ctx, uint64_t off, void* buf, uint32_t len) {
  return vhdx_pread((vhdx*)ctx, off, buf, len);
}

void vhdx_io(vhdx* d, wsl_io* out) {
  out->read = vio_read;
  out->ctx = d;
  out->size = d->size;
}
