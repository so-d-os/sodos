#include "shell.h"
#include "mm.h"
#include "serial.h"

static void put_u32(uint32_t n) {
  char num[16];
  uint32_to_string(n, num, (int)sizeof(num));
  puts_k(num);
}

static void put_hex(uint32_t n) {
  static const char hex[] = "0123456789ABCDEF";
  int i;
  puts_k("0x");
  for (i = 7; i >= 0; i--)
    putc(hex[(n >> (i * 4)) & 0xF]);
}

static void put_hex64(uint64_t n) {
  static const char hex[] = "0123456789ABCDEF";
  int i;
  puts_k("0x");
  for (i = 15; i >= 0; i--)
    putc(hex[(n >> (i * 4)) & 0xF]);
}

static void put_mb(uint64_t n) {
  put_u32((uint32_t)(n >> 20));
  puts_k(" MB");
}

/* "free teste": escreve e confere uma palavra de 64 bits a cada 4 KB do maior
 * bloco acima de 4 GB.  Valida mapeamento e RAM sem levar minutos. */
static int high_test(void) {
  uint64_t total, used, free_b, largest, n, i, bad = 0;
  volatile uint64_t* p;
  hm_stats(&total, &used, &free_b, &largest);
  if (largest <= 64u) {
    puts_k("Sem RAM livre acima de 4 GB.\n");
    return 1;
  }
  n = largest - 64u;
  p = (volatile uint64_t*)hmalloc(n);
  if (!p) {
    puts_k("hmalloc falhou.\n");
    return 1;
  }
  puts_k("Testando ");
  put_mb(n);
  puts_k(" em ");
  put_hex64((uint64_t)(uintptr_t)p);
  puts_k("...\n");
  n /= 8u;
  for (i = 0; i < n; i += 512u) p[i] = (uint64_t)(uintptr_t)&p[i] ^ 0x5A3CA55AC33C0FF0ULL;
  for (i = 0; i < n; i += 512u)
    if (p[i] != ((uint64_t)(uintptr_t)&p[i] ^ 0x5A3CA55AC33C0FF0ULL)) bad++;
  hfree((void*)p);
  if (bad) {
    puts_k("ERRO: ");
    put_u32((uint32_t)bad);
    puts_k(" paginas com valor errado.\n");
    return 1;
  }
  puts_k("OK.\n");
  return 0;
}

/* "free blocos": blocos usados do heap dos programas (endereco, KB, dono).
 * Mostra o que ficou preso depois que um programa terminou. */
static int blk_n;

static void blk_put(void* arg, int area, uint64_t addr, uint64_t size, int used, uint32_t owner) {
  (void)arg;
  if (area != 1 || !used) return;
  {
    char n[16];
    serial_puts("[blk] ");
    uint32_to_string((uint32_t)addr, n, (int)sizeof(n));
    serial_hex32((uint32_t)addr);
    serial_puts(" ");
    uint32_to_string((uint32_t)size, n, (int)sizeof(n));
    serial_puts(n);
    serial_puts(" ");
    uint32_to_string(owner, n, (int)sizeof(n));
    serial_puts(n);
    serial_puts("\n");
  }
  if (blk_n++ >= 60) return;
  put_hex((uint32_t)addr);
  puts_k("  ");
  put_u32((uint32_t)(size >> 10));
  puts_k(" KB  dono ");
  put_u32(owner);
  puts_k("\n");
}


/* "free velocidade": mesma escrita+leitura de 64 MB na RAM baixa (heap dos
 * programas) e acima de 4 GB. Se a alta estiver uncached (MTRR do firmware),
 * ela sai dezenas de vezes mais lenta. Mostra tambem o que o boot achou. */
static uint64_t rd_tsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static uint64_t mem_pass(volatile uint64_t* p, uint64_t n) {
  uint64_t t0 = rd_tsc(), i, sum = 0;
  for (i = 0; i < n; i++) p[i] = i;
  for (i = 0; i < n; i++) sum += p[i];
  if (sum == 1) puts_k(" ");
  return rd_tsc() - t0;
}

static int speed_test(void) {
  const uint64_t bytes = 64ull << 20;
  volatile uint64_t* lo = (volatile uint64_t*)umalloc((uint32_t)bytes);
  volatile uint64_t* hi = (volatile uint64_t*)hmalloc(bytes);
  uint64_t cl = 0, ch = 0, wb, uc, other;
  int fixed;
  mm_mtrr_info(&wb, &uc, &other, &fixed);
  if (!wb && !uc && !other) puts_k("MTRR nao lido (VM ou CPU sem MTRR).\n");
  else {
  puts_k("MTRR no boot: RAM alta WB ");
  put_mb(wb);
  puts_k(", UC ");
  put_mb(uc);
  puts_k(", outros ");
  put_mb(other);
  puts_k(fixed ? " (TOM2 corrigido para WB)\n" : "\n");
  }
  if (lo) {
    (void)mem_pass(lo, bytes / 8u);
    cl = mem_pass(lo, bytes / 8u);
    ufree((void*)lo);
  }
  if (hi) {
    (void)mem_pass(hi, bytes / 8u);
    ch = mem_pass(hi, bytes / 8u);
    hfree((void*)hi);
  }
  puts_k("64 MB escrita+leitura (ciclos/MB): baixa ");
  put_u32((uint32_t)(cl / 64u));
  puts_k("  alta ");
  put_u32((uint32_t)(ch / 64u));
  puts_k("\n");
  if (cl && ch) {
    uint32_t r = (uint32_t)(ch * 10u / cl);
    puts_k("alta/baixa = ");
    put_u32(r / 10u);
    puts_k(".");
    put_u32(r % 10u);
    puts_k(r > 30u ? "x  <- RAM alta LENTA (cache errado)\n" : "x  (ok)\n");
  }
  return 0;
}

int cmd_free(int argc, char** argv) {
  if (argc > 1 && (strcmp(argv[1], "blocos") == 0 || strcmp(argv[1], "BLOCOS") == 0)) {
    uint32_t t, u, f, big;
    blk_n = 0;
    mm_walk(blk_put, 0);
    if (blk_n > 60) {
      put_u32((uint32_t)(blk_n - 60));
      puts_k(" blocos a mais.\n");
    }
    um_stats(&t, &u, &f, &big);
    puts_k("Maior bloco livre ");
    put_u32(big >> 10);
    puts_k(" KB\n");
    return 0;
  }
  uint32_t total, used, free_b, largest;
  if (argc > 1 && (strcmp(argv[1], "velocidade") == 0 || strcmp(argv[1], "VELOCIDADE") == 0))
    return speed_test();
  if (argc > 1 && (strcmp(argv[1], "teste") == 0 || strcmp(argv[1], "TESTE") == 0))
    return high_test();
  mm_stats(&total, &used, &free_b, &largest);
  puts_k("Heap ");
  put_hex(mm_heap_start());
  puts_k("-");
  put_hex(mm_heap_end());
  puts_k("\n");
  if (!mm_ready()) {
    puts_k("Heap indisponivel: o kernel passou do inicio da area livre.\n");
    return 1;
  }
  puts_k("Total  ");
  put_u32(total);
  puts_k(" bytes\nUsado  ");
  put_u32(used);
  puts_k(" bytes\nLivre  ");
  put_u32(free_b);
  puts_k(" bytes\nMaior bloco livre  ");
  put_u32(largest);
  puts_k(" bytes\n");
  if (um_ready()) {
    um_stats(&total, &used, &free_b, &largest);
    puts_k("\nHeap alto (programas) ");
    put_hex(um_start());
    puts_k("-");
    put_hex(um_end());
    puts_k("\nTotal  ");
    put_u32(total / 1024u);
    puts_k(" KB\nUsado  ");
    put_u32(used / 1024u);
    puts_k(" KB\nLivre  ");
    put_u32(free_b / 1024u);
    puts_k(" KB\n");
  } else {
    puts_k("\nHeap alto indisponivel (pouca RAM).\n");
  }
  if (hm_ready()) {
    uint64_t t, u, f, big;
    uint32_t i;
    hm_stats(&t, &u, &f, &big);
    puts_k("\nAcima de 4 GB (kernel + programas 64 bits)\n");
    for (i = 0; i < hm_regions(); i++) {
      puts_k("  ");
      put_hex64(hm_region_start(i));
      puts_k("-");
      put_hex64(hm_region_end(i));
      puts_k("\n");
    }
    puts_k("Total  ");
    put_mb(t);
    puts_k("\nUsado  ");
    put_mb(u);
    puts_k("\nLivre  ");
    put_mb(f);
    {
      uint64_t q, uu, ram;
      hm_user_info(&q, &uu, &ram);
      puts_k("\nProgramas 64 bits (Firefox): ");
      put_mb(uu);
      puts_k(" de ");
      put_mb(q);
      puts_k(" (SET HIMEM=pct; RAM total ");
      put_mb(ram);
      puts_k(")");
    }
    puts_k("\n");
  }
  return 0;
}
