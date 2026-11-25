/*
 * audio.c - Lado do kernel do som (ver audio.h e docs/AUDIO.md).
 *
 * O driver atual (au.drv) nunca e 0: desligado, e o driver nulo daqui, que
 * aceita tudo e joga fora. SET AUDIO=1 (env.c chama audio_env_changed)
 * carrega o AUDIO.DLL, que acha a placa no init; sem placa, o DLL sai da
 * memoria e o nulo continua. SET AUDIO=0 para o som e descarrega na hora.
 *
 * Quem chama o driver incrementa au.busy: o IRQ0 (audio_irq_tick, com o BKL
 * do mundo) nao entra no DLL enquanto outro trecho do kernel esta nele (na
 * mesma CPU, interrompido no meio, por exemplo).
 *
 * Os programas mandam 8 ou 16 bits, mono ou estereo, em qualquer taxa de 8 a
 * 192 kHz; o driver recebe sempre 16 bits estereo. Se a placa nao tem a taxa,
 * vai a 48 kHz com interpolacao linear (sem filtro: serve para voz, avisos e
 * jogos; musica em 44,1 ou 48 kHz passa direto na maioria das placas).
 */
#include <stdint.h>
#include "audio.h"
#include "drives.h"
#include "env.h"
#include "kthread.h"
#include "mm.h"
#include "module.h"
#include "net.h"
#include "serial.h"
#include "shell.h"
#include "visual.h"
#include "xhci.h"   /* pci_mmio_map */

const char* mod_pid_name(uint32_t pid);  /* module.c */

/* ---------- driver nulo ---------- */
static int n_init(const struct audio_host* h) { (void)h; return 0; }
static void n_fini(void) {}
static int n_start(uint32_t rate) { (void)rate; return 0; }
static int n_write(const void* buf, int bytes) { (void)buf; return bytes; }
static int n_zero(void) { return 0; }
static void n_stop(void) {}
static int n_volume(int pct) { (void)pct; return -1; }
static int n_info(char* out, int max) { (void)out; (void)max; return 0; }

static const struct audio_ops audio_null = {
  AUDIO_ABI, "nulo", n_init, n_fini, n_start, n_write, n_zero, n_zero, n_stop, n_volume, n_info,
};

/* ---------- estado ---------- */
#define CV_FRAMES 512          /* conversao do k_audio_write (quadros estereo) */
#define BP_FRAMES 256          /* tom do IRQ0 */
#define LOG_MAX 3072           /* log do ultimo carregamento (comando AUDIO) */
#define BEEP_RATE 48000u
#define BEEP_AHEAD (BEEP_RATE / 10u * 4u)   /* 100 ms na fila */
#define BEEP_FADE 240u         /* 5 ms de subida e descida: sem estalo */
#define TICK_MS 8u
#define STUCK_MS 3000u         /* fila cheia sem andar: a placa parou */

volatile int audio_tick_on;

/* Guardas nas pontas: o TROCA KERNEL acha a variavel inteira (docs/TROCA.md). */
static struct {
  uint32_t guarda0;
  const struct audio_ops* drv;  /* nunca 0 */
  int handle;                   /* mod_dll_* do DLL; 0 = driver nulo */
  int retry;                    /* ligado, mas o DLL nao foi achado ainda */
  int busy;                     /* dentro do driver agora */
  char path[AUDIO_PATH_MAX];    /* de onde veio o DLL */
  char* log;                    /* so existe com AUDIO ligado */
  int log_n, log_bol;
  /* fluxo de um programa */
  int opened;
  uint32_t owner;               /* pid */
  int rate, ch, bits;
  uint32_t out_rate;            /* taxa da placa */
  uint32_t step, frac;          /* conversao de taxa, 16.16 */
  int have_prev;
  int prev_l, prev_r;
  /* tom (k_audio_beep, AUDIO TESTE) */
  int beep;                     /* o fluxo da placa e do tom */
  uint32_t beep_left, beep_pos, beep_total, beep_ph, beep_inc;
  uint64_t tick_last;
  volatile int wait_word;       /* k_audio_write dorme nele */
  int16_t cv[CV_FRAMES * 2];
  int16_t bp[BP_FRAMES * 2];
  uint32_t guarda1;
} au = { .guarda0 = 0x41554449u, .drv = &audio_null, .guarda1 = 0x4F444E45u };

/* ---------- texto ---------- */
struct sb {
  char* p;
  int n, max;
};
static void sb_c(struct sb* b, char c) {
  if (b->n < b->max - 1) b->p[b->n++] = c;
}
static void sb_s(struct sb* b, const char* s) {
  while (s && *s) sb_c(b, *s++);
}
static void sb_d(struct sb* b, uint32_t v) {
  char t[12];
  int i = 0;
  do t[i++] = (char)('0' + v % 10u); while ((v /= 10u) && i < 11);
  while (i) sb_c(b, t[--i]);
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void scopy(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

/* ---------- host do DLL ---------- */
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) {
  uint32_t v;
  __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static uint32_t h_pci_rd(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fun << 8 | (off & 0xFCu));
  return inl(0xCFC);
}

static void h_pci_wr(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off, uint32_t v) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fun << 8 | (off & 0xFCu));
  outl(0xCFC, v);
}

static int h_mmio_map(uint64_t phys, uint64_t len) { return pci_mmio_map(phys, len); }

/* Bloco alinhado e zerado: o ponteiro original fica nos 8 bytes antes. */
static void* h_alloc(uint32_t bytes, uint32_t align) {
  uint8_t* raw;
  uintptr_t p;
  uint32_t i;
  if (align < 16u) align = 16u;
  raw = (uint8_t*)umalloc_owned(bytes + align + 16u, 0);
  if (!raw) return 0;
  p = ((uintptr_t)raw + 16u + align - 1u) & ~(uintptr_t)(align - 1u);
  ((uintptr_t*)p)[-1] = (uintptr_t)raw;
  for (i = 0; i < bytes; i++) ((uint8_t*)p)[i] = 0;
  return (void*)p;
}

static void h_free(void* q) {
  if (q) ufree((void*)((uintptr_t*)q)[-1]);
}

static void log_c(char c) {
  if (au.log && au.log_n < LOG_MAX - 1) au.log[au.log_n++] = c;
}

static void h_log(const char* s) {
  for (; s && *s; s++) {
    if (au.log_bol) serial_puts("[audio] ");
    au.log_bol = (*s == '\n');
    serial_putc(*s);
    log_c(*s);
  }
}

static void h_log_hex(uint32_t v) {
  char t[9];
  int i;
  for (i = 7; i >= 0; i--, v >>= 4) t[i] = "0123456789ABCDEF"[v & 15u];
  t[8] = 0;
  h_log(t);
}

static const struct audio_host host = {
  AUDIO_ABI, h_log, h_log_hex, h_pci_rd, h_pci_wr, h_mmio_map, h_alloc, h_free, net_ms,
};

static void log_reset(void) {
  if (!au.log) au.log = (char*)umalloc_owned(LOG_MAX, 0);
  au.log_n = 0;
  au.log_bol = 1;
}

static void log_free(void) {
  if (au.log) ufree(au.log);
  au.log = 0;
  au.log_n = 0;
}

/* ---------- carregar e descarregar ---------- */
static int wanted(void) {
  const char* v = env_get("AUDIO");
  return v && v[0] && !(v[0] == '0' && v[1] == 0);
}

/* AUDIO=1 (ou qualquer valor que nao seja um .DLL) -> AUDIO.DLL. */
static void dll_name(char* out, int max) {
  const char* v = env_get("AUDIO");
  int n = 0;
  while (v && v[n]) n++;
  if (n > 4 && v[n - 4] == '.' && up(v[n - 3]) == 'D' && up(v[n - 2]) == 'L' && up(v[n - 1]) == 'L')
    scopy(out, v, max);
  else
    scopy(out, "AUDIO.DLL", max);
}

static void unload_now(void) {
  audio_tick_on = 0;
  if (au.handle) {
    au.busy++;
    au.drv->stop();
    au.drv->fini();
    au.busy--;
    mod_dll_free(au.handle);
    serial_puts("[audio] descarregado\n");
  }
  au.handle = 0;
  au.drv = &audio_null;
  au.beep = 0;
  au.path[0] = 0;
  /* um programa com o som aberto continua escrevendo, agora no nulo */
}

/* 1 carregou e achou placa; 0 o arquivo nao esta ali; -1 estava ali mas nao
 * serviu (nao e driver de som, ou nao achou placa). */
static int try_load(const char* path) {
  int h, rc;
  audio_entry_fn fn;
  const struct audio_ops* ops;
  if (file_size(path) <= 0) return 0;
  h = mod_dll_load_sys(path);
  if (!h) {
    h_log(path);
    h_log(": ");
    h_log(mod_last_error());
    h_log("\n");
    return -1;
  }
  fn = (audio_entry_fn)mod_dll_sym(h, AUDIO_SYM);
  ops = fn ? fn() : 0;
  if (!ops || ops->abi != AUDIO_ABI || !ops->init || !ops->fini || !ops->start || !ops->write ||
      !ops->pending || !ops->service || !ops->stop || !ops->volume || !ops->info) {
    h_log(path);
    h_log(": nao e um driver de som (ABI ");
    h_log_hex(AUDIO_ABI);
    h_log(")\n");
    mod_dll_free(h);
    return -1;
  }
  h_log(path);
  h_log(" carregado\n");
  au.busy++;
  rc = ops->init(&host);
  au.busy--;
  if (rc != 0) {
    h_log("o driver nao achou placa de som; fica o driver nulo\n");
    au.busy++;
    ops->fini();
    au.busy--;
    mod_dll_free(h);
    return -1;
  }
  au.handle = h;
  au.drv = ops;
  scopy(au.path, path, (int)sizeof(au.path));
  return 1;
}

/* PATHSODOS\DRV, depois as copias do disco e do SODOS.PAK. */
static int load(void) {
  char name[AUDIO_PATH_MAX], p[AUDIO_PATH_MAX];
  struct sb b;
  int r, i;
  static const char* const dirs[] = { "D:\\SODOS\\DRV\\", "Z:\\SODOS\\DRV\\" };
  dll_name(name, (int)sizeof(name));
  for (i = 0; name[i]; i++)
    if (name[i] == ':' || name[i] == '\\') return try_load(name);   /* caminho completo */
  b.p = p;
  b.max = (int)sizeof(p);
  b.n = 0;
  env_sodos_path("DRV\\", p, (int)sizeof(p));
  while (p[b.n]) b.n++;
  sb_s(&b, name);
  p[b.n] = 0;
  if ((r = try_load(p)) != 0) return r;
  for (i = 0; i < 2; i++) {
    if (!drives_is_mounted(dirs[i][0])) continue;
    b.n = 0;
    sb_s(&b, dirs[i]);
    sb_s(&b, name);
    p[b.n] = 0;
    if ((r = try_load(p)) != 0) return r;
  }
  return 0;
}

static void load_now(int loud) {
  int r;
  log_reset();
  r = load();
  au.retry = (r == 0);
  if (r > 0) return;
  if (r == 0) {
    char name[AUDIO_PATH_MAX];
    dll_name(name, (int)sizeof(name));
    h_log(name);
    h_log(" nao encontrado (PATHSODOS\\DRV, D:\\SODOS\\DRV, Z:\\SODOS\\DRV); tenta de novo no proximo som\n");
  }
  /* Aviso so na hora do SET e so em modo texto (prompt, AUTOEXEC), como o
   * LINKEXT: dentro do WIN fica na serial e no comando AUDIO. */
  if (loud && !win_screen_size(0, 0)) puts_k("AUDIO: sem som por enquanto (driver nulo). Detalhes: AUDIO\n");
}

static void retry_if_due(void) {
  if (!au.handle && au.retry && wanted()) load_now(0);
}

void audio_env_changed(void) {
  char name[AUDIO_PATH_MAX];
  int n, k;
  if (!wanted()) {
    unload_now();
    au.retry = 0;
    log_free();
    return;
  }
  if (au.handle) {
    /* Ja carregado (SET AUDIO=1 de novo, PATHSODOS mudou): so troca se o
     * nome do DLL mudou. Para ler o arquivo de novo: SET AUDIO=0 e =1. */
    dll_name(name, (int)sizeof(name));
    for (n = 0; au.path[n]; n++) {
    }
    for (k = 0; name[k]; k++) {
    }
    if (k <= n) {
      int i, same = 1;
      for (i = 0; i < k; i++)
        if (up(au.path[n - k + i]) != up(name[i])) same = 0;
      if (same && (k == n || au.path[n - k - 1] == '\\' || au.path[n - k - 1] == ':')) return;
    }
    unload_now();
  }
  load_now(1);
}

/* ---------- tom ---------- */
/* Quarto de onda do seno, 0..90 graus em 64 passos, escala 32767. */
static const int16_t sin_q[65] = {
  0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179,
  7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732,
  15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
  22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
  27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571,
  30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
  32609, 32678, 32728, 32757, 32767,
};

/* fase de 32 bits = uma volta */
static int sine(uint32_t ph) {
  uint32_t q = ph >> 30, i = (ph >> 24) & 63u, f = (ph >> 8) & 0xFFFFu;
  int a, b, v;
  if (q & 1u) {
    a = sin_q[64u - i];
    b = sin_q[63u - i];
  } else {
    a = sin_q[i];
    b = sin_q[i + 1u];
  }
  v = a + (int)(((int64_t)(b - a) * (int64_t)f) >> 16);
  return (q & 2u) ? -v : v;
}

/* Poe tom na fila ate BEEP_AHEAD. Com au.busy ja contado. */
static void beep_feed(void) {
  int guard = 0;
  while (au.beep_left > 0 && au.drv->pending() < (int)BEEP_AHEAD && guard++ < 64) {
    uint32_t n = au.beep_left < BP_FRAMES ? au.beep_left : BP_FRAMES, i, ph = au.beep_ph;
    int w;
    for (i = 0; i < n; i++) {
      uint32_t pos = au.beep_pos + i, left = au.beep_total - pos, env = BEEP_FADE;
      int v;
      if (pos < env) env = pos;
      if (left < env) env = left;
      v = sine(ph) / 4;                       /* -12 dB */
      v = (int)((int64_t)v * (int64_t)env / (int64_t)BEEP_FADE);
      au.bp[2 * i] = au.bp[2 * i + 1] = (int16_t)v;
      ph += au.beep_inc;
    }
    w = au.drv->write(au.bp, (int)(n * 4u)) / 4;
    if (w <= 0) break;
    au.beep_ph += au.beep_inc * (uint32_t)w;
    au.beep_pos += (uint32_t)w;
    au.beep_left -= (uint32_t)w;
  }
}

/* IRQ0, com o BKL do mundo (irq.c). Zera o que ja tocou, alimenta o tom e
 * desliga o tick quando a placa para. */
void audio_irq_tick(uint64_t ms) {
  int r;
  if (au.busy || !au.handle) return;
  if (ms - au.tick_last < TICK_MS) return;
  au.tick_last = ms;
  au.busy++;
  if (au.beep) beep_feed();
  r = au.drv->service();
  au.busy--;
  if (!r) {
    audio_tick_on = 0;
    au.beep = 0;
  }
}

/* ---------- programas ---------- */
static int frame_in(void) { return au.ch * (au.bits / 8); }

static void get_frame(const uint8_t* p, int* l, int* r) {
  if (au.bits == 8) {
    *l = ((int)p[0] - 128) << 8;
    *r = au.ch == 2 ? ((int)p[1] - 128) << 8 : *l;
  } else {
    *l = (int16_t)(uint16_t)(p[0] | p[1] << 8);
    *r = au.ch == 2 ? (int16_t)(uint16_t)(p[2] | p[3] << 8) : *l;
  }
}

/* Converte ate in_frames quadros do programa em au.cv (16 bits estereo na
 * taxa da placa). Devolve os quadros de saida; *used = bytes consumidos. */
static int convert(const uint8_t* in, int in_frames, int* used) {
  int fi = frame_in(), n = 0, k, l, r;
  if (au.step == 65536u) {
    for (k = 0; k < in_frames && n < CV_FRAMES; k++, n++) {
      get_frame(in + k * fi, &l, &r);
      au.cv[2 * n] = (int16_t)l;
      au.cv[2 * n + 1] = (int16_t)r;
    }
    *used = k * fi;
    return n;
  }
  /* 8 kHz -> 48 kHz da no maximo 7 quadros por quadro de entrada */
  for (k = 0; k < in_frames && n <= CV_FRAMES - 8; k++) {
    get_frame(in + k * fi, &l, &r);
    if (!au.have_prev) {
      au.prev_l = l;
      au.prev_r = r;
      au.have_prev = 1;
      au.frac = 0;
      continue;
    }
    while (au.frac < 65536u) {
      au.cv[2 * n] = (int16_t)(au.prev_l + (int)(((int64_t)(l - au.prev_l) * au.frac) >> 16));
      au.cv[2 * n + 1] = (int16_t)(au.prev_r + (int)(((int64_t)(r - au.prev_r) * au.frac) >> 16));
      n++;
      au.frac += au.step;
    }
    au.frac -= 65536u;
    au.prev_l = l;
    au.prev_r = r;
  }
  *used = k * fi;
  return n;
}

/* Dorme um pouco sem girar a CPU (thread: futex; mundo: o WIN anda) e
 * deixa o driver zerar o que tocou. */
static void wait_a_bit(uint32_t ms) {
  (void)kt_futex_wait(&au.wait_word, 0, (int)ms);
  if (au.handle && !au.busy) {
    au.busy++;
    (void)au.drv->service();
    au.busy--;
  }
}

static void beep_stop(void) {
  if (!au.beep) return;
  au.beep = 0;
  au.beep_left = 0;
  if (au.handle) {
    au.busy++;
    au.drv->stop();
    au.busy--;
  }
  audio_tick_on = 0;
}

int k_audio_open(int rate, int channels, int bits) {
  uint32_t me = mod_current_pid();
  int r;
  if (rate < 8000 || rate > 192000 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16)) return -1;
  if (au.opened && au.owner != me) return -2;
  retry_if_due();
  beep_stop();
  if (au.opened && au.handle) {
    /* o mesmo programa abrindo de novo: outro formato */
    au.busy++;
    au.drv->stop();
    au.busy--;
    audio_tick_on = 0;
  }
  au.out_rate = (uint32_t)rate;
  if (au.handle) {
    au.busy++;
    r = au.drv->start((uint32_t)rate);
    if (r == -1) {
      au.out_rate = 48000u;
      r = au.drv->start(48000u);
    }
    au.busy--;
    if (r != 0) {
      h_log("a placa recusou o fluxo\n");
      au.opened = 0;
      return -3;
    }
  }
  au.rate = rate;
  au.ch = channels;
  au.bits = bits;
  au.step = (uint32_t)(((uint64_t)(uint32_t)rate << 16) / au.out_rate);
  au.frac = 0;
  au.have_prev = 0;
  au.owner = me;
  au.opened = 1;
  return 0;
}

int k_audio_write(const void* buf, int bytes) {
  const uint8_t* in = (const uint8_t*)buf;
  int fi, left;
  uint32_t t_prog;
  if (!au.opened || au.owner != mod_current_pid()) return -1;
  if (!buf || bytes <= 0) return 0;
  fi = frame_in();
  bytes -= bytes % fi;
  if (!au.handle) return bytes;              /* driver nulo: joga fora */
  left = bytes;
  t_prog = net_ms();
  while (left > 0) {
    int used, n, done = 0;
    n = convert(in, left / fi, &used);
    in += used;
    left -= used;
    while (done < n) {
      int w;
      if (!au.handle || !au.opened) return bytes;   /* SET AUDIO=0 no meio */
      au.busy++;
      w = au.drv->write(au.cv + done * 2, (n - done) * 4);
      au.busy--;
      if (w < 0) return -2;
      if (w > 0) {
        done += w / 4;
        t_prog = net_ms();
        audio_tick_on = 1;
        continue;
      }
      if (net_ms() - t_prog > STUCK_MS) {
        h_log("a fila nao anda: a placa parou de tocar\n");
        return -2;
      }
      wait_a_bit(5);
    }
  }
  return bytes;
}

int k_audio_pending(void) {
  int p;
  uint64_t f;
  if (!au.opened || !au.handle || au.owner != mod_current_pid()) return 0;
  au.busy++;
  p = au.drv->pending();
  au.busy--;
  if (p <= 0) return 0;
  f = (uint64_t)(uint32_t)(p / 4) * (uint32_t)au.rate / au.out_rate;
  return (int)f * frame_in();
}

int k_audio_close(int wait) {
  if (!au.opened || au.owner != mod_current_pid()) return -1;
  if (wait && au.handle) {
    uint32_t t0 = net_ms(), lim;
    int p;
    au.busy++;
    p = au.drv->pending();
    au.busy--;
    lim = (uint32_t)((uint64_t)(uint32_t)(p > 0 ? p : 0) * 1000u / (au.out_rate * 4u)) + 1000u;
    while (au.handle && au.opened && net_ms() - t0 < lim) {
      au.busy++;
      p = au.drv->pending();
      au.busy--;
      if (p <= 0) break;
      wait_a_bit(10);
    }
  }
  if (au.handle && au.opened) {
    au.busy++;
    au.drv->stop();
    au.busy--;
    audio_tick_on = 0;
  }
  au.opened = 0;
  au.owner = 0;
  return 0;
}

int k_audio_volume(int pct) {
  int r;
  retry_if_due();
  if (!au.handle) return -1;
  if (pct > 100) pct = 100;
  au.busy++;
  r = au.drv->volume(pct);
  au.busy--;
  return r;
}

int k_audio_beep(int hz, int ms) {
  int r;
  if (hz < 20 || hz > 20000 || ms <= 0) return -1;
  if (ms > 10000) ms = 10000;
  retry_if_due();
  if (!au.handle || au.opened) return -1;
  if (!au.beep) {
    au.busy++;
    r = au.drv->start(BEEP_RATE);
    au.busy--;
    if (r != 0) return -1;
    au.beep = 1;
  }
  au.beep_total = BEEP_RATE / 1000u * (uint32_t)ms;
  au.beep_left = au.beep_total;
  au.beep_pos = 0;
  au.beep_ph = 0;
  au.beep_inc = (uint32_t)(((uint64_t)(uint32_t)hz << 32) / BEEP_RATE);
  au.busy++;
  beep_feed();
  au.busy--;
  audio_tick_on = 1;
  return 0;
}

void audio_pid_gone(uint32_t pid) {
  if (!au.opened || au.owner != pid) return;
  if (au.handle) {
    au.busy++;
    au.drv->stop();
    au.busy--;
    audio_tick_on = 0;
  }
  au.opened = 0;
  au.owner = 0;
}

int k_audio_info(char* out, int max) {
  struct sb b;
  const char* v = env_get("AUDIO");
  if (!out || max <= 0) return 0;
  b.p = out;
  b.n = 0;
  b.max = max;
  if (!wanted()) {
    sb_s(&b, "AUDIO desligado (SET AUDIO=1 liga): driver nulo, nada carregado.\n");
  } else {
    sb_s(&b, "AUDIO=");
    sb_s(&b, v);
    if (au.handle) {
      sb_s(&b, ": driver ");
      sb_s(&b, au.drv->name);
      sb_s(&b, " (");
      sb_s(&b, au.path);
      sb_s(&b, ")\n");
      au.busy++;
      b.n += au.drv->info(b.p + b.n, b.max - b.n);
      au.busy--;
      if (b.n > b.max - 1) b.n = b.max - 1;
    } else {
      sb_s(&b, ", mas sem placa: driver nulo.\n");
    }
  }
  if (au.opened) {
    sb_s(&b, "Aberto por ");
    sb_s(&b, mod_pid_name(au.owner));
    sb_s(&b, " (pid ");
    sb_d(&b, au.owner);
    sb_s(&b, "): ");
    sb_d(&b, (uint32_t)au.rate);
    sb_s(&b, " Hz, ");
    sb_d(&b, (uint32_t)au.ch);
    sb_s(&b, au.ch == 1 ? " canal, " : " canais, ");
    sb_d(&b, (uint32_t)au.bits);
    sb_s(&b, " bits");
    if (au.handle && au.out_rate != (uint32_t)au.rate) {
      sb_s(&b, " (convertido para ");
      sb_d(&b, au.out_rate);
      sb_s(&b, " Hz)");
    }
    sb_s(&b, "\n");
  } else if (au.beep) {
    sb_s(&b, "Tocando o tom de teste.\n");
  }
  if (wanted() && au.log && au.log_n) {
    int i;
    sb_s(&b, "--- log do carregamento ---\n");
    for (i = 0; i < au.log_n; i++) sb_c(&b, au.log[i]);
  }
  out[b.n] = 0;
  return b.n;
}
