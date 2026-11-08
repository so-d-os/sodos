/*
 * gpu.c - Carrega o driver da placa de video (um .DLL em D:\SODOS\DRV) e
 * desliga ao primeiro erro. Contrato e tabela de custo em docs/GPU.md.
 *
 * Sem placa conhecida e sem gpudrv= no VIDEO.CFG nada daqui roda alem de
 * gpu_wanted() na troca de modo; gpu_drv fica 0 e o quadro sai pela CPU.
 */
#include <stdint.h>
#include "gpu.h"
#include "serial.h"
#include "mm.h"
#include "module.h"
#include "net.h"

const struct gpu_drv* gpu_drv;

/* vendor << 16 | device -> DLL. So placas com driver escrito entram aqui. */
static const struct {
  uint32_t id;
  const char* dll;
} gpu_table[] = {
  { 0x1AF41050u, "VIRTGPU.DLL" },   /* virtio-gpu (QEMU -device virtio-vga) */
};

#define GPU_DIR "D:\\SODOS\\DRV\\"

/* VIDEO.CFG */
static int cfg_off;          /* gpu=0 */
static char cfg_dll[16];     /* gpudrv=X.DLL: forca um driver (testes) */

/* placa achada por gpu_wanted() */
static uint32_t pci_id, pci_dev;
static const char* want_dll;

static int dll_handle;
static uint32_t mem_bytes;
/* driver carregado mas parado (WIN ao fundo); gpu_drv fica 0 enquanto isso */
static const struct gpu_drv* parked;
/* falhou nesta sessao do WIN: nao tenta de novo ate o WIN abrir outra vez */
static int failed;

static inline void outl(uint16_t port, uint32_t v) {
  __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t v;
  __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static uint32_t k_pci_read32(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fun << 8 | (off & 0xFCu));
  return inl(0xCFC);
}

static void k_pci_write32(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off, uint32_t v) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fun << 8 | (off & 0xFCu));
  outl(0xCFC, v);
}

/* Bloco alinhado: o ponteiro original e o tamanho ficam nos 16 bytes antes. */
static void* k_alloc(uint32_t bytes, uint32_t align) {
  uint8_t* raw;
  uintptr_t p;
  if (align < 16u) align = 16u;
  raw = (uint8_t*)umalloc_owned(bytes + align + 16u, 0);
  if (!raw) return 0;
  p = ((uintptr_t)raw + 16u + align - 1u) & ~(uintptr_t)(align - 1u);
  ((uintptr_t*)p)[-1] = (uintptr_t)raw;
  ((uint32_t*)p)[-4] = bytes;
  mem_bytes += bytes;
  return (void*)p;
}

static void k_free(void* q) {
  if (!q) return;
  mem_bytes -= ((uint32_t*)q)[-4];
  ufree((void*)((uintptr_t*)q)[-1]);
}

static void k_log(const char* s) { serial_puts(s); }

static const struct gpu_kapi kapi = {
  GPU_ABI, k_log, serial_hex32, k_pci_read32, k_pci_write32, k_alloc, k_free, net_ms
};

static int cfg_is(const char* line, int n, const char* key) {
  int i;
  for (i = 0; key[i]; i++)
    if (i >= n || line[i] != key[i]) return 0;
  return i;
}

void gpu_cfg_line(const char* line, int n) {
  int k, i;
  if (!line) {
    cfg_off = 0;
    cfg_dll[0] = 0;
    failed = 0;
    return;
  }
  if ((k = cfg_is(line, n, "gpudrv=")) > 0) {
    for (i = 0; i < (int)sizeof(cfg_dll) - 1 && k + i < n; i++) cfg_dll[i] = line[k + i];
    cfg_dll[i] = 0;
  } else if ((k = cfg_is(line, n, "gpu=")) > 0) {
    cfg_off = k < n && line[k] == '0';
  }
}

int gpu_wanted(void) {
  uint32_t dev, i;
  want_dll = 0;
  if (cfg_off || failed) return 0;
  for (dev = 0; dev < 32 && !want_dll; dev++) {
    uint32_t id = k_pci_read32(0, dev, 0, 0);
    if (id == 0xFFFFFFFFu) continue;
    if (((k_pci_read32(0, dev, 0, 0x08) >> 24) & 0xFFu) != 0x03u) continue;
    pci_id = (id & 0xFFFFu) << 16 | id >> 16;
    pci_dev = dev;
    for (i = 0; i < sizeof(gpu_table) / sizeof(gpu_table[0]); i++)
      if (gpu_table[i].id == pci_id) want_dll = gpu_table[i].dll;
    if (cfg_dll[0]) want_dll = cfg_dll;
  }
  return want_dll != 0;
}

static void unload(const struct gpu_drv* d) {
  failed = 1;
  if (d && d->fini) d->fini();
  if (dll_handle) mod_dll_free(dll_handle);
  dll_handle = 0;
  mem_bytes = 0;
}

static void say(const char* a, const char* b) {
  serial_puts("[gpu] ");
  serial_puts(a);
  if (b) serial_puts(b);
  serial_puts("\n");
}

int gpu_start(const struct gpu_mode* m) {
  char path[48];
  const struct gpu_drv* d = 0;
  struct gpu_mode mm;
  gpu_driver_fn fn;
  int i, j;
  if (gpu_drv || parked || !want_dll || !m || !m->back) return -1;
  for (i = 0; GPU_DIR[i]; i++) path[i] = GPU_DIR[i];
  for (j = 0; want_dll[j] && i < (int)sizeof(path) - 1; j++) path[i++] = want_dll[j];
  path[i] = 0;
  failed = 1;   /* so volta a 0 se tudo der certo */
  serial_puts("[gpu] ");
  serial_hex32(pci_id);
  serial_puts(" -> ");
  serial_puts(path);
  serial_puts("\n");
  mem_bytes = 0;
  dll_handle = mod_dll_load(path);
  if (!dll_handle) {
    say("nao carregou: ", mod_last_error());
    return -1;
  }
  fn = (gpu_driver_fn)mod_dll_sym(dll_handle, GPU_DRIVER_SYM);
  if (fn) d = fn();
  if (!d || d->abi != GPU_ABI || !d->init || !d->present || !d->fini ||
      !(d->caps & GPU_CAP_PRESENT)) {
    say("DLL nao e driver de placa (ABI ou funcoes): ", path);
    unload(0);
    return -1;
  }
  mm = *m;
  mm.pci_id = pci_id;
  mm.pci_bus = 0;
  mm.pci_dev = pci_dev;
  mm.pci_fun = 0;
  if (d->init(&kapi, &mm) != 0) {
    say("init falhou: ", d->name);
    unload(d);
    return -1;
  }
  if (d->selftest && d->selftest() != 0) {
    say("teste falhou: ", d->name);
    unload(d);
    return -1;
  }
  gpu_drv = d;
  failed = 0;
  say("ativo: ", d->name);
  return 0;
}

void gpu_stop(const char* why) {
  const struct gpu_drv* d = gpu_drv ? gpu_drv : parked;
  if (!d) return;
  /* zera antes: nada mais passa pelo driver, nem se fini falhar */
  gpu_drv = 0;
  parked = 0;
  say("desativado: ", why);
  unload(d);
}

/* Fim do WIN: descarrega sem contar como falha. */
void gpu_close(void) {
  gpu_stop("fim do WIN");
  failed = 0;
}

void gpu_suspend(void) {
  const struct gpu_drv* d = gpu_drv;
  if (!d) return;
  gpu_drv = 0;
  if (!d->suspend || !d->resume || d->suspend() != 0) {
    say("desativado: ", "tela de texto");
    unload(d);
    return;
  }
  parked = d;
  say("suspenso: ", d->name);
}

int gpu_resume(const struct gpu_mode* m) {
  const struct gpu_drv* d = parked;
  if (!d) return -1;
  parked = 0;
  if (d->resume(m) != 0) {
    say("desativado: ", "resume falhou");
    unload(d);
    return -1;
  }
  gpu_drv = d;
  say("ativo de novo: ", d->name);
  return 0;
}

int gpu_suspended(void) { return parked != 0; }

int gpu_present(void) {
  if (!gpu_drv) return -1;
  if (gpu_drv->present(0, 0) == 0) return 0;
  gpu_stop("present falhou");
  return -1;
}

const char* gpu_name(void) {
  const struct gpu_drv* d = gpu_drv ? gpu_drv : parked;
  return d ? d->name : 0;
}

uint32_t gpu_mem_kb(void) { return gpu_name() ? (mem_bytes + 1023u) >> 10 : 0; }
