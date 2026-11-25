/*
 * troca.c - Troca do kernel e dos DLLs do sistema sem reiniciar (troca.h,
 * docs/TROCA.md).
 *
 * Aqui fica so o que precisa ser do kernel: memoria abaixo de 2 GB, parar as
 * outras CPUs, conferir onde elas e as threads interrompidas estao, escrever
 * os jmps e guardar o estado (geracao, blocos de codigo, mapa de simbolos).
 * Ler o ELF novo e relocar o codigo e trabalho do TROCA.EXE (utils/troca.c),
 * que pode ser corrigido sem reiniciar nada.
 *
 * Como as CPUs param: IPI TROCA_VETOR para todas as outras. Ela so entra com
 * IF=1, entao em cada CPU ha um quadro de interrupcao so (o da IPI): o RIP
 * dele e onde a CPU volta. Threads fora da CPU interrompidas pelo tique
 * guardam o RIP em task.irq_rip (kthread.c, kt_irq_rips). Quem estiver no
 * meio dos 5 primeiros bytes de uma funcao trocada impede a troca
 * (TROCA_EOCUPADO); o TROCA.EXE tenta de novo logo depois.
 *
 * As variaveis daqui tem tamanho fixo: o kernel novo usa as mesmas.
 */
#include <stdint.h>
#include "troca.h"
#include "mm.h"
#include "smp.h"
#include "kthread.h"
#include "serial.h"
#include "irq.h"
#include "wine.h"
#include "fsdrv.h"
#include "linkext.h"

void shell_register_all(void);   /* kernel.c */

extern char __bss_start[];
extern char __bss_end[];
int sodos_build(void);

#define KERNEL_LOAD 0x01000000ull
#define TROCA_MAGIC 0x41434F5254ull    /* "TROCA" antes do ponteiro cru */

static uint32_t geracao;
static uint32_t build_boot;            /* 0 ate a primeira troca */
static uint32_t nblocos;
static struct {
  uint64_t lo, hi;
  uint32_t build;
  uint32_t pad;
} blocos[TROCA_BLOCOS_MAX];
static void* mapa;
static uint32_t mapa_size;
static char origem[96];
static uint32_t ultimo_us;

static volatile uint32_t pedido;       /* 1: as outras CPUs ficam na IPI */
static volatile uint32_t parados;
static volatile uint64_t rip_cpu[KT_CPU_MAX];
static volatile uint8_t chegou[KT_CPU_MAX];
static uint64_t rips[KT_MAX + 8];

static inline uint64_t tsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* cpuid serializa: depois de outra CPU escrever codigo, esta busca de novo. */
static inline void serializa(void) {
  uint32_t a = 0, b, c = 0, d;
  __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d) :: "memory");
}

static void scopy(char* d, const char* s, int max) {
  int i = 0;
  while (s && s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static void sdec(uint32_t v) {
  char b[12];
  int i = 0;
  do b[i++] = (char)('0' + v % 10u); while ((v /= 10u) && i < 11);
  while (i) serial_putc(b[--i]);
}

/* ---------- consultas ---------- */
int troca_bloco(uint64_t a) {
  uint32_t i;
  for (i = 0; i < nblocos && i < TROCA_BLOCOS_MAX; i++)
    if (a >= blocos[i].lo && a < blocos[i].hi) return (int)i + 1;
  return 0;
}

uint64_t troca_bloco_lo(int n) {
  if (n < 1 || (uint32_t)n > nblocos || n > TROCA_BLOCOS_MAX) return 0;
  return blocos[n - 1].lo;
}

int troca_texto(uint64_t a) {
  if (a >= KERNEL_LOAD && a < (uint64_t)(uintptr_t)__bss_end) return 1;
  return troca_bloco(a) != 0;
}

int troca_info(struct k_troca_info* o) {
  if (!o) return TROCA_EINVAL;
  o->abi = TROCA_ABI;
  o->geracao = geracao;
  o->build = (uint32_t)sodos_build();       /* codigo atual: o ultimo aplicado */
  o->build_boot = build_boot ? build_boot : o->build;
  o->boot_lo = KERNEL_LOAD;
  o->boot_hi = (uint64_t)(uintptr_t)__bss_end;
  o->mapa = mapa;
  o->mapa_size = mapa_size;
  o->cpus = (uint32_t)smp_cpu_count();
  scopy(o->origem, origem, (int)sizeof(o->origem));
  return 0;
}

/* ---------- memoria ---------- */
static void* alinha(uint8_t* raw, uint32_t n) {
  uint8_t* p = (uint8_t*)(((uintptr_t)raw + 16u + 4095u) & ~(uintptr_t)4095u);
  uint32_t i;
  ((uint64_t*)p)[-2] = TROCA_MAGIC;
  ((uint64_t*)p)[-1] = (uint64_t)(uintptr_t)raw;
  for (i = 0; i < n; i++) p[i] = 0;
  return p;
}

void* troca_alloc(uint32_t n) {
  uint32_t need, a;
  uint8_t* raw;
  if (!n || n > 0x40000000u) return 0;
  need = n + 8192u;
  raw = (uint8_t*)umalloc_owned(need, 0);
  if (raw && (uint64_t)(uintptr_t)raw + need <= TROCA_TETO) return alinha(raw, n);
  if (raw) ufree(raw);
  /* A arena passa de 2 GB: procura um buraco baixo, de 1 em 1 MB. */
  for (a = (um_start() + 0xFFFFFu) & ~0xFFFFFu; (uint64_t)a + need <= TROCA_TETO && a < um_end();
       a += 0x100000u) {
    raw = (uint8_t*)umalloc_at(a, need, 0);
    if (raw) return alinha(raw, n);
  }
  return 0;
}

void troca_free(void* p) {
  uint64_t* q = (uint64_t*)p;
  if (!p || ((uintptr_t)p & 4095u) || q[-2] != TROCA_MAGIC) return;
  q[-2] = 0;
  ufree((void*)(uintptr_t)q[-1]);
}

/* ---------- parada das CPUs ---------- */
void troca_ipi(uint64_t rip) {
  int c;
  if (!__atomic_load_n(&pedido, __ATOMIC_ACQUIRE)) return;
  c = smp_cpu_self();
  if (c >= 0 && c < KT_CPU_MAX) {
    rip_cpu[c] = rip;
    chegou[c] = 1;
  }
  __atomic_add_fetch(&parados, 1, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&pedido, __ATOMIC_ACQUIRE)) __asm__ volatile ("pause" ::: "memory");
  serializa();
  __atomic_sub_fetch(&parados, 1, __ATOMIC_SEQ_CST);
}

/* 1 se rip cai no meio dos 5 bytes de algum jmp (rip == at e seguro: a CPU
 * so vai executar o jmp inteiro). */
static int no_meio(const struct k_troca_req* r, uint64_t rip) {
  uint32_t i;
  if (!troca_texto(rip)) return 0;
  for (i = 0; i < r->npatch; i++)
    if (rip > r->patch[i].at && rip < r->patch[i].at + 5) return 1;
  return 0;
}

static int confere(const struct k_troca_req* r) {
  uint32_t i;
  if (r->abi != TROCA_ABI || !r->patch || !r->npatch || r->npatch > 200000u) return 0;
  if (r->text_lo >= r->text_hi || r->text_hi > TROCA_TETO || r->text_lo < KERNEL_LOAD) return 0;
  if (r->ncopy && (!r->copy || r->ncopy > 4096u)) return 0;
  if (r->depois && (r->depois < r->text_lo || r->depois >= r->text_hi)) return 0;
  for (i = 0; i < r->npatch; i++) {
    uint64_t at = r->patch[i].at, to = r->patch[i].to;
    int64_t rel = (int64_t)to - (int64_t)(at + 5);
    if (to < r->text_lo || to >= r->text_hi) return 0;
    if (!(at >= KERNEL_LOAD && at + 5 <= (uint64_t)(uintptr_t)__bss_start) && !troca_bloco(at)) return 0;
    if (rel != (int64_t)(int32_t)rel) return 0;
  }
  for (i = 0; i < r->ncopy; i++) {
    const struct k_troca_copy* c = &r->copy[i];
    if (!c->len || c->len > 0x10000000u || c->dst + c->len > TROCA_TETO || c->src + c->len > 0x100000000ull)
      return 0;
  }
  return 1;
}

static void jmp_em(uint64_t at, uint64_t to) {
  volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)at;
  uint32_t rel = (uint32_t)(int32_t)((int64_t)to - (int64_t)(at + 5));
  p[1] = (uint8_t)rel;
  p[2] = (uint8_t)(rel >> 8);
  p[3] = (uint8_t)(rel >> 16);
  p[4] = (uint8_t)(rel >> 24);
  p[0] = 0xE9;
}

int troca_apply(struct k_troca_req* r) {
  uint64_t fl, cr0, t0, t1, hz, lim;
  uint32_t i, esperados = 0, antes;
  int eu, ncpu, n, rc = 0;
  void* mapa_velho;
  if (!r) return TROCA_EINVAL;
  r->rip = 0;
  r->cpu = -2;
  r->us = 0;
  if (!confere(r)) return TROCA_EINVAL;
  if (nblocos >= TROCA_BLOCOS_MAX) return TROCA_ENOMEM;
  /* antes dos jmps: depois deles o sodos_build ja e o do kernel novo */
  antes = nblocos ? blocos[nblocos - 1].build : (uint32_t)sodos_build();
  hz = irq_clock_hz();
  lim = hz ? hz / 20u : 0;                 /* 50 ms */
  ncpu = smp_cpu_count();
  __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
  eu = smp_cpu_self();
  if (eu < 0) eu = 0;
  for (i = 0; i < KT_CPU_MAX; i++) {
    rip_cpu[i] = 0;
    chegou[i] = 0;
  }
  for (n = 0; n < ncpu && n < KT_CPU_MAX; n++)
    if (n != eu && smp_apic_id(n) != 0xFFFFFFFFu) esperados++;
  __atomic_store_n(&parados, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&pedido, 1, __ATOMIC_SEQ_CST);
  if (esperados) smp_ipi_others(TROCA_VETOR);
  t0 = tsc();
  for (n = 0; __atomic_load_n(&parados, __ATOMIC_SEQ_CST) < esperados; n++) {
    if (hz ? (tsc() - t0 > lim) : (n > 30000000)) {
      rc = TROCA_ETEMPO;
      for (i = 0; i < KT_CPU_MAX && i < (uint32_t)ncpu; i++)
        if ((int)i != eu && smp_apic_id((int)i) != 0xFFFFFFFFu && !chegou[i]) {
          r->cpu = (int32_t)i;
          break;
        }
      break;
    }
    __asm__ volatile ("pause" ::: "memory");
  }
  t1 = tsc();
  if (!rc) {
    /* onde as CPUs pararam */
    for (i = 0; i < KT_CPU_MAX && !rc; i++)
      if ((int)i != eu && chegou[i] && no_meio(r, rip_cpu[i])) {
        rc = TROCA_EOCUPADO;
        r->rip = rip_cpu[i];
        r->cpu = (int32_t)i;
      }
    /* threads fora da CPU interrompidas pelo tique */
    if (!rc) {
      n = kt_irq_rips(rips, (int)(sizeof(rips) / sizeof(rips[0])));
      for (i = 0; i < (uint32_t)n && !rc; i++)
        if (no_meio(r, rips[i])) {
          rc = TROCA_EOCUPADO;
          r->rip = rips[i];
          r->cpu = -1;
        }
    }
  }
  if (!rc) {
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    if (cr0 & 0x10000u) __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0 & ~(uint64_t)0x10000u) : "memory");
    for (i = 0; i < r->ncopy; i++) {
      volatile uint8_t* d = (volatile uint8_t*)(uintptr_t)r->copy[i].dst;
      const volatile uint8_t* s = (const volatile uint8_t*)(uintptr_t)r->copy[i].src;
      uint64_t k;
      for (k = 0; k < r->copy[i].len; k++) d[k] = s[k];
    }
    for (i = 0; i < r->npatch; i++) jmp_em(r->patch[i].at, r->patch[i].to);
    if (cr0 & 0x10000u) __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0) : "memory");
    serializa();
    if (!geracao) build_boot = antes;
    blocos[nblocos].lo = r->text_lo;
    blocos[nblocos].hi = r->text_hi;
    blocos[nblocos].build = r->build;
    nblocos++;
    geracao++;
  }
  __atomic_store_n(&pedido, 0, __ATOMIC_SEQ_CST);
  for (n = 0; __atomic_load_n(&parados, __ATOMIC_SEQ_CST) && n < 30000000; n++)
    __asm__ volatile ("pause" ::: "memory");
  if (fl & 0x200u) __asm__ volatile ("sti" ::: "memory");
  if (hz) r->us = (uint32_t)((tsc() - t0) * 1000000u / hz);
  (void)t1;
  if (rc) return rc;
  ultimo_us = r->us;
  mapa_velho = mapa;
  mapa = r->mapa;
  mapa_size = r->mapa_size;
  scopy(origem, r->origem, (int)sizeof(origem));
  if (mapa_velho && mapa_velho != mapa) troca_free(mapa_velho);
  serial_puts("[troca] kernel trocado: ");
  sdec(r->npatch);
  serial_puts(" funcoes, ");
  sdec(r->ncopy);
  serial_puts(" copias, CPUs paradas ");
  sdec(r->us);
  serial_puts(" us, de ");
  serial_puts(origem);
  serial_puts("\n");
  /* daqui em diante quem roda e o kernel novo */
  if (r->depois) ((void (*)(uint32_t, uint32_t))(uintptr_t)r->depois)(antes, geracao);
  return 0;
}

void troca_depois(uint32_t build_antes, uint32_t ger) {
  serial_puts("[troca] kernel novo no ar: build ");
  sdec(build_antes);
  serial_puts(" -> ");
  sdec((uint32_t)sodos_build());
  serial_puts(" (troca ");
  sdec(ger);
  serial_puts(" desde o boot)\n");
  /* comandos internos novos (LIMPA...) e os antigos apontando para o codigo
   * novo: o kmain que os registra so roda no boot */
  shell_register_all();
}

/* ---------- texto do estado ---------- */
struct tb {
  char* p;
  int n, max;
};
static void tc(struct tb* b, char c) {
  if (b->n < b->max - 1) b->p[b->n++] = c;
}
static void ts(struct tb* b, const char* s) {
  while (s && *s) tc(b, *s++);
}
static void td(struct tb* b, uint64_t v) {
  char t[21];
  int i = 0;
  do t[i++] = (char)('0' + v % 10u); while ((v /= 10u) && i < 20);
  while (i) tc(b, t[--i]);
}
static void tx(struct tb* b, uint64_t v) {
  static const char hx[] = "0123456789ABCDEF";
  int d;
  ts(b, "0x");
  for (d = 7; d >= 0; d--) tc(b, hx[(v >> (d * 4)) & 15u]);
}

int troca_estado(char* out, int max) {
  struct tb b;
  uint32_t i;
  if (!out || max <= 0) return 0;
  b.p = out;
  b.n = 0;
  b.max = max;
  ts(&b, "Kernel: build ");
  td(&b, (uint32_t)sodos_build());
  if (geracao) {
    ts(&b, " (boot: build ");
    td(&b, build_boot);
    ts(&b, "; ");
    td(&b, geracao);
    ts(&b, geracao == 1 ? " troca a quente)\n" : " trocas a quente)\n");
    for (i = 0; i < nblocos && i < TROCA_BLOCOS_MAX; i++) {
      ts(&b, "  troca ");
      td(&b, i + 1);
      ts(&b, ": build ");
      td(&b, blocos[i].build);
      ts(&b, ", codigo em ");
      tx(&b, blocos[i].lo);
      ts(&b, " (");
      td(&b, (blocos[i].hi - blocos[i].lo + 1023u) / 1024u);
      ts(&b, " KB)\n");
    }
    ts(&b, "  ultima: ");
    ts(&b, origem);
    ts(&b, ", CPUs paradas ");
    td(&b, ultimo_us);
    ts(&b, " us\n");
  } else {
    ts(&b, ", o do boot (nenhuma troca a quente)\n");
  }
  b.n += wine_estado(b.p + b.n, b.max - b.n);
  ts(&b, "FAT32.DLL: ");
  ts(&b, fsdrv_path()[0] ? fsdrv_path() : "nao carregado");
  ts(&b, "\nLINKEXT.DLL: ");
  ts(&b, linkext_loaded() ? "carregado\n" : "nao carregado\n");
  b.p[b.n] = 0;
  return b.n;
}

/* ---------- DLLs ---------- */
static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Nome do arquivo de um caminho, em maiusculas ("D:\X\wine.dll" -> "WINE.DLL"). */
static void base_up(const char* p, char* out, int max) {
  const char* b = p;
  const char* q;
  int i = 0;
  for (q = p; *q; q++)
    if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
  while (*b && i < max - 1) out[i++] = upc(*b++);
  out[i] = 0;
}

static int seq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int tem_caminho(const char* s) {
  for (; *s; s++)
    if (*s == '\\' || *s == '/' || *s == ':') return 1;
  return 0;
}

int troca_dll(const char* nome, char* msg, int max) {
  char b[32];
  struct tb t;
  if (!msg || max <= 0) return TROCA_EINVAL;
  t.p = msg;
  t.n = 0;
  t.max = max;
  msg[0] = 0;
  if (!nome || !nome[0]) return TROCA_EINVAL;
  base_up(nome, b, (int)sizeof(b));
  if (seq(b, "WINE") || seq(b, "WINE.DLL"))
    return wine_swap(tem_caminho(nome) ? nome : 0, msg, max);
  if (seq(b, "FAT32.DLL") || seq(b, "FAT32")) {
    int n = fsdrv_retire("FAT32.DLL");
    if (tem_caminho(nome)) {
      ts(&t, "FAT32.DLL: o caminho vem do SET FAT32DLL= (ou de \\SODOS\\DRV); ");
    }
    ts(&t, n ? "a copia carregada foi aposentada: a proxima montagem FAT32 le o DLL de novo.\n"
                "Os volumes ja montados continuam com o driver antigo ate serem remontados.\n"
             : "nenhum FAT32.DLL carregado: a proxima montagem ja le o arquivo novo.\n");
    t.p[t.n] = 0;
    return 0;
  }
  if (seq(b, "LINKEXT.DLL") || seq(b, "LINKEXT")) {
    int r = linkext_reload();
    ts(&t, r == 0 ? "LINKEXT.DLL recarregado.\n"
         : r == 1 ? "LINKEXT desligado (SET LINKEXT=1 carrega o arquivo novo).\n"
                  : "LINKEXT.DLL em uso agora; tente de novo.\n");
    t.p[t.n] = 0;
    return r < 0 ? -1 : 0;
  }
  if (seq(b, "VIRTGPU.DLL") || seq(b, "AMDGOP.DLL") || seq(b, "AMDCUR.DLL") || seq(b, "AMDSMC.DLL") ||
      seq(b, "AMDSDMA.DLL") || seq(b, "AMDDUAL.DLL") || seq(b, "GPUTEST.DLL") || seq(b, "GPUFAIL.DLL")) {
    ts(&t, b);
    ts(&t, ": o driver de video e escolhido no boot e a tela depende dele; para trocar, reinicie.\n");
    t.p[t.n] = 0;
    return -1;
  }
  ts(&t, b);
  ts(&t, ": nao e um DLL do sistema que o kernel mantem (os DLLs dos programas sao lidos de novo\n"
         "a cada programa que abre).\n");
  t.p[t.n] = 0;
  return -1;
}
