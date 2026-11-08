/*
 * gpu.c - Carrega o driver da placa de video (um .DLL em X:\SODOS\DRV) e
 * desliga ao primeiro erro. Contrato e tabela de custo em docs/GPU.md.
 *
 * Placas conhecidas: a tabela abaixo e as entradas [video] do SODOS.CFG
 * (D:\SODOS.CFG ou, no notebook, U:\SODOS.CFG). Sem placa conhecida e sem
 * gpudrv= no VIDEO.CFG nada daqui roda alem de gpu_wanted() na troca de modo;
 * gpu_drv fica 0 e o quadro sai pela CPU.
 *
 * Quando ha driver para tentar, o que acontece vai para X:\SODOS\GPU.LOG
 * (o notebook nao tem COM1), alem do log serial.
 */
#include <stdint.h>
#include "gpu.h"
#include "serial.h"
#include "mm.h"
#include "module.h"
#include "net.h"
#include "drives.h"

const struct gpu_drv* gpu_drv;
int gpu_hwcur;

/* vendor << 16 | device -> DLL. So placas com driver escrito entram aqui. */
static const struct {
  uint32_t id;
  const char* dll;
} gpu_table[] = {
  { 0x1AF41050u, "VIRTGPU.DLL" },   /* virtio-gpu (QEMU -device virtio-vga) */
};

/* VIDEO.CFG */
static int cfg_off;          /* gpu=0 */
static char cfg_dll[16];     /* gpudrv=X.DLL: forca um driver (testes) */

/* SODOS.CFG, secao [video]: uma entrada por placa= */
#define CFG_MAX 4
static struct {
  uint32_t id;
  int on;                    /* ativo=0 desliga so esta placa */
  char nome[48];
  char dll[16];
} cfg_ent[CFG_MAX];
static int cfg_n;
static int cfg_read;         /* ja leu nesta sessao do WIN */
static char cfg_drive;       /* unidade onde achou o SODOS.CFG; 0 = nenhuma */
static int usb_tried;        /* U: so e montado para isto uma vez por boot */

/* placa achada por gpu_wanted() */
static uint32_t pci_id, pci_bus, pci_dev, pci_fun;
static const char* want_dll;
static const char* card_name;

/* GPU.LOG: so existe enquanto ha driver para tentar */
#define LOG_MAX 8192
static char* log_buf;
static int log_len;

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

static void log_put(const char* s) {
  serial_puts(s);
  if (!log_buf) return;
  while (*s && log_len < LOG_MAX - 1) log_buf[log_len++] = *s++;
}

static void log_hex(uint32_t v) {
  char t[9];
  int i;
  for (i = 7; i >= 0; i--, v >>= 4) t[i] = "0123456789ABCDEF"[v & 15u];
  t[8] = 0;
  log_put(t);
}

/* Grava o GPU.LOG ao lado do SODOS.CFG (ou em D:\SODOS). */
static void log_save(void) {
  char path[24] = "D:\\SODOS\\GPU.LOG";
  if (!log_buf) return;
  if (cfg_drive) path[0] = cfg_drive;
  if (!drives_is_mounted(path[0])) return;
  file_save(path, log_buf, log_len);
}

static void k_log(const char* s) { log_put(s); }

static const struct gpu_kapi kapi = {
  GPU_ABI, k_log, log_hex, k_pci_read32, k_pci_write32, k_alloc, k_free, net_ms
};

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* "1002:15E7" -> 0x100215E7; 0 se nao for isso. */
static uint32_t parse_id(const char* p, int n) {
  uint32_t v = 0;
  int i, d = 0;
  for (i = 0; i < n && d < 8; i++) {
    int h = hexval(p[i]);
    if (p[i] == ':' && d == 4) continue;
    if (h < 0) break;
    v = v << 4 | (uint32_t)h;
    d++;
  }
  return d == 8 ? v : 0;
}

static void copy_val(char* dst, int max, const char* src, int n) {
  int i;
  for (i = 0; i < n && i < max - 1; i++) dst[i] = src[i];
  dst[i] = 0;
}

static int key_is(const char* line, int n, const char* key) {
  int i;
  for (i = 0; key[i]; i++)
    if (i >= n || line[i] != key[i]) return 0;
  return i;
}

/* Le a secao [video] do SODOS.CFG: placa=, nome=, driver=, ativo=. */
static void cfg_parse(const char* t, int n) {
  int i = 0, in_video = 0;
  cfg_n = 0;
  while (i < n) {
    int j = i, k, len;
    const char* l;
    while (j < n && t[j] != '\n' && t[j] != '\r') j++;
    l = t + i;
    len = j - i;
    while (len > 0 && (l[len - 1] == ' ' || l[len - 1] == '\t')) len--;
    if (len > 0 && l[0] == '[') {
      in_video = key_is(l, len, "[video]") > 0;
    } else if (in_video && (k = key_is(l, len, "placa=")) > 0) {
      if (cfg_n < CFG_MAX) {
        cfg_ent[cfg_n].id = parse_id(l + k, len - k);
        cfg_ent[cfg_n].on = 1;
        cfg_ent[cfg_n].nome[0] = 0;
        cfg_ent[cfg_n].dll[0] = 0;
        cfg_n++;
      }
    } else if (in_video && cfg_n > 0 && (k = key_is(l, len, "nome=")) > 0) {
      copy_val(cfg_ent[cfg_n - 1].nome, (int)sizeof(cfg_ent[0].nome), l + k, len - k);
    } else if (in_video && cfg_n > 0 && (k = key_is(l, len, "driver=")) > 0) {
      copy_val(cfg_ent[cfg_n - 1].dll, (int)sizeof(cfg_ent[0].dll), l + k, len - k);
    } else if (in_video && cfg_n > 0 && (k = key_is(l, len, "ativo=")) > 0) {
      cfg_ent[cfg_n - 1].on = !(k < len && l[k] == '0');
    }
    while (j < n && (t[j] == '\n' || t[j] == '\r')) j++;
    i = j;
  }
}

/* Uma vez por sessao do WIN: D:\SODOS.CFG, senao U:\SODOS.CFG. */
static void cfg_load(void) {
  static const char drv[2] = { 'D', 'U' };
  char path[16] = "D:\\SODOS.CFG";
  char* t;
  int i, n;
  if (cfg_read) return;
  cfg_read = 1;
  cfg_n = 0;
  cfg_drive = 0;
  for (i = 0; i < 2 && !cfg_drive; i++) {
    path[0] = drv[i];
    if (!drives_is_mounted(drv[i])) {
      /* o pendrive monta sozinho no primeiro uso; tenta isso uma vez */
      if (drv[i] != 'U' || usb_tried) continue;
      usb_tried = 1;
    }
    if (file_size(path) <= 0) continue;
    t = (char*)umalloc_owned(4096, 0);
    if (!t) return;
    n = file_load(path, t, 4095);
    if (n > 0) {
      cfg_parse(t, n);
      cfg_drive = drv[i];
    }
    ufree(t);
  }
}

/* Placas de video no PCI, seguindo as pontes (no notebook a GPU esta no
 * barramento 4). Para na primeira com driver (tabela ou SODOS.CFG); se
 * nenhuma tiver, fica a primeira achada, para o TOP mostrar. */
static int pci_walk(uint32_t bus, int depth) {
  uint32_t dev, fun, i;
  for (dev = 0; dev < 32; dev++) {
    uint32_t nfun = 1;
    for (fun = 0; fun < nfun; fun++) {
      uint32_t id = k_pci_read32(bus, dev, fun, 0), cls, hdr, pid;
      if (id == 0xFFFFFFFFu || (id & 0xFFFFu) == 0xFFFFu) continue;
      hdr = k_pci_read32(bus, dev, fun, 0x0C) >> 16 & 0xFFu;
      if (fun == 0 && (hdr & 0x80u)) nfun = 8;
      cls = k_pci_read32(bus, dev, fun, 0x08) >> 24 & 0xFFu;
      if ((hdr & 0x7Fu) == 1u && depth < 8) {
        uint32_t sec = k_pci_read32(bus, dev, fun, 0x18) >> 8 & 0xFFu;
        if (sec > bus && pci_walk(sec, depth + 1)) return 1;
        continue;
      }
      if (cls != 0x03u) continue;
      pid = (id & 0xFFFFu) << 16 | id >> 16;
      if (!pci_id) {
        pci_id = pid;
        pci_bus = bus;
        pci_dev = dev;
        pci_fun = fun;
      }
      for (i = 0; i < sizeof(gpu_table) / sizeof(gpu_table[0]); i++)
        if (gpu_table[i].id == pid) want_dll = gpu_table[i].dll;
      for (i = 0; i < (uint32_t)cfg_n; i++)
        if (cfg_ent[i].id == pid) {
          card_name = cfg_ent[i].nome[0] ? cfg_ent[i].nome : 0;
          want_dll = cfg_ent[i].on && cfg_ent[i].dll[0] ? cfg_ent[i].dll : 0;
          if (!cfg_ent[i].on) return 0;   /* ativo=0: esta placa fica com a CPU */
        }
      if (want_dll) {
        pci_id = pid;
        pci_bus = bus;
        pci_dev = dev;
        pci_fun = fun;
        return 1;
      }
    }
  }
  return 0;
}

void gpu_cfg_line(const char* line, int n) {
  int k, i;
  if (!line) {
    cfg_off = 0;
    cfg_dll[0] = 0;
    failed = 0;
    cfg_read = 0;   /* o SODOS.CFG e lido de novo nesta sessao */
    return;
  }
  if ((k = key_is(line, n, "gpudrv=")) > 0) {
    for (i = 0; i < (int)sizeof(cfg_dll) - 1 && k + i < n; i++) cfg_dll[i] = line[k + i];
    cfg_dll[i] = 0;
  } else if ((k = key_is(line, n, "gpu=")) > 0) {
    cfg_off = k < n && line[k] == '0';
  }
}

int gpu_wanted(void) {
  want_dll = 0;
  if (cfg_off || failed) return 0;
  cfg_load();
  pci_id = 0;
  card_name = 0;
  pci_walk(0, 0);
  if (pci_id && cfg_dll[0]) want_dll = cfg_dll;
  return want_dll != 0;
}

uint32_t gpu_find_display(void) {
  if (!pci_id) {
    pci_walk(0, 0);
    want_dll = 0;
  }
  return pci_id;
}

const char* gpu_card_name(void) { return card_name; }

static void unload(const struct gpu_drv* d) {
  failed = 1;
  if (d && d->fini) d->fini();
  if (dll_handle) mod_dll_free(dll_handle);
  dll_handle = 0;
  mem_bytes = 0;
  log_save();
}

/* O kernel nao sabe a placa; quem achou foi gpu_wanted(). */
static void with_pci(struct gpu_mode* mm, const struct gpu_mode* m) {
  *mm = *m;
  mm->pci_id = pci_id;
  mm->pci_bus = pci_bus;
  mm->pci_dev = pci_dev;
  mm->pci_fun = pci_fun;
}

static void say(const char* a, const char* b) {
  log_put("[gpu] ");
  log_put(a);
  if (b) log_put(b);
  log_put("\n");
}

int gpu_start(const struct gpu_mode* m) {
  char path[48];
  const struct gpu_drv* d = 0;
  struct gpu_mode mm;
  gpu_driver_fn fn;
  int i, j;
  static const char dir[] = "D:\\SODOS\\DRV\\";
  if (gpu_drv || parked || !want_dll || !m || !m->back) return -1;
  for (i = 0; dir[i]; i++) path[i] = dir[i];
  if (cfg_drive) path[0] = cfg_drive;   /* drivers ao lado do SODOS.CFG */
  for (j = 0; want_dll[j] && i < (int)sizeof(path) - 1; j++) path[i++] = want_dll[j];
  path[i] = 0;
  failed = 1;   /* so volta a 0 se tudo der certo */
  if (!log_buf) log_buf = (char*)umalloc_owned(LOG_MAX, 0);
  log_len = 0;
  log_put("[gpu] placa ");
  log_hex(pci_id);
  log_put(" em ");
  log_hex(pci_bus << 16 | pci_dev << 8 | pci_fun);
  if (card_name) {
    log_put(" (");
    log_put(card_name);
    log_put(")");
  }
  log_put(cfg_drive ? ", SODOS.CFG em " : ", sem SODOS.CFG");
  if (cfg_drive) {
    char d[3] = { cfg_drive, ':', 0 };
    log_put(d);
  }
  log_put("\n[gpu] modo ");
  log_hex(m->w);
  log_put("x");
  log_hex(m->h);
  log_put(" fb ");
  log_hex((uint32_t)(uintptr_t)m->fb);
  log_put(" pitch ");
  log_hex(m->fb_pitch);
  log_put("\n[gpu] -> ");
  log_put(path);
  log_put("\n");
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
  with_pci(&mm, m);
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
  log_save();
  return 0;
}

void gpu_stop(const char* why) {
  const struct gpu_drv* d = gpu_drv ? gpu_drv : parked;
  if (!d) return;
  /* zera antes: nada mais passa pelo driver, nem se fini falhar */
  gpu_drv = 0;
  gpu_hwcur = 0;
  parked = 0;
  say("desativado: ", why);
  unload(d);
}

/* Fim do WIN: descarrega sem contar como falha e libera o log. */
void gpu_close(void) {
  gpu_stop("fim do WIN");
  failed = 0;
  if (log_buf) ufree(log_buf);
  log_buf = 0;
  log_len = 0;
}

void gpu_suspend(void) {
  const struct gpu_drv* d = gpu_drv;
  if (!d) return;
  gpu_drv = 0;
  gpu_hwcur = 0;   /* o resume entrega a imagem de novo */
  if (!d->suspend || !d->resume || d->suspend() != 0) {
    say("desativado: ", "tela de texto");
    unload(d);
    return;
  }
  parked = d;
  say("suspenso: ", d->name);
  log_save();   /* o usuario pode estar indo ler o log numa tela do DOS */
}

int gpu_resume(const struct gpu_mode* m) {
  const struct gpu_drv* d = parked;
  struct gpu_mode mm;
  if (!d || !m) return -1;
  parked = 0;
  with_pci(&mm, m);
  if (d->resume(&mm) != 0) {
    say("desativado: ", "resume falhou");
    unload(d);
    return -1;
  }
  gpu_drv = d;
  say("ativo de novo: ", d->name);
  log_save();
  return 0;
}

int gpu_suspended(void) { return parked != 0; }

int gpu_present(const struct gpu_rect* rects, int n) {
  if (!gpu_drv) return -1;
  if (n <= 0 || !(gpu_drv->caps & GPU_CAP_DAMAGE)) {
    rects = 0;
    n = 0;
  }
  if (gpu_drv->present(rects, n) == 0) return 0;
  gpu_stop("present falhou");
  return -1;
}

void gpu_cursor_set(const uint32_t* argb, int w, int h, int hot_x, int hot_y) {
  const struct gpu_drv* d = gpu_drv;
  gpu_hwcur = 0;
  if (!d || !(d->caps & GPU_CAP_CURSOR) || !d->cursor_set || !d->cursor_move || !argb) return;
  if (d->cursor_set(argb, w, h, hot_x, hot_y) != 0) {
    say("cursor da placa recusado: ", d->name);
    return;
  }
  gpu_hwcur = 1;
  say("cursor pela placa: ", d->name);
}

int gpu_cursor_move(int x, int y) {
  if (!gpu_hwcur || !gpu_drv) return -1;
  if (gpu_drv->cursor_move(x, y) == 0) return 0;
  gpu_stop("cursor falhou");
  return -1;
}

const char* gpu_name(void) {
  const struct gpu_drv* d = gpu_drv ? gpu_drv : parked;
  return d ? d->name : 0;
}

uint32_t gpu_mem_kb(void) { return gpu_name() ? (mem_bytes + 1023u) >> 10 : 0; }
