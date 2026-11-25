#include "shell.h"
#include "mm.h"
#include "serial.h"
#include "visual.h"
#include "wine.h"

const char* mod_pid_name(uint32_t pid);   /* module.c */

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

/* ---- cadeia de blocos (mm_check) ----
 * O TOP, o FREE e os alocadores andam de cabecalho em cabecalho. Um cabecalho
 * estragado (alguem escreveu alem do fim do bloco de antes) para o percurso:
 * a RAM dali ate o fim da area some ate o proximo boot. No mapa do TOP ela
 * aparece branca, como se fosse do kernel. */
static void put_dono(uint32_t owner) {
  char names[24][32];
  uint32_t pids[24];
  const char* nm = 0;
  int i, n;
  if (owner == 0) {
    puts_k("kernel");
    return;
  }
  if (owner == WINE_SHARED_OWNER) {
    puts_k("WINE (memoria compartilhada)");
    return;
  }
  puts_k("pid ");
  put_u32(owner);
  n = win_task_list(names, pids, 24);
  for (i = 0; i < n && !nm; i++)
    if (pids[i] == owner) nm = names[i];
  if (!nm) {
    nm = mod_pid_name(owner);
    if (nm && strcmp(nm, "pid?") == 0) nm = 0;
  }
  puts_k(" (");
  puts_k(nm ? nm : "nao roda mais");
  puts_k(")");
}

static void put_hex8(uint8_t v) {
  static const char hex[] = "0123456789ABCDEF";
  putc(hex[v >> 4]);
  putc(hex[v & 0xF]);
}

static void serial_hex64(uint64_t v) {
  serial_hex32((uint32_t)(v >> 32));
  serial_hex32((uint32_t)v);
}

static void put_tam(uint64_t n) {
  if (n >= (1u << 20)) {
    put_mb(n);
  } else if (n >= 1024u) {
    put_u32((uint32_t)(n >> 10));
    puts_k(" KB");
  } else {
    put_u32((uint32_t)n);
    puts_k(" bytes");
  }
}

static void chk_show(int area) {
  struct mm_chk c;
  int r = mm_check(area, &c), i;
  if (r == 0) return;
  if (r > 0) {
    puts_k("Cadeia de blocos inteira (");
    put_u32((uint32_t)c.blocks);
    puts_k(" blocos)\n");
    return;
  }
  puts_k("QUEBRA na cadeia de blocos em ");
  put_hex64(c.stop);
  puts_k(":\n  dali ate o fim (");
  put_hex64(c.end);
  puts_k("), ");
  put_tam(c.end - c.stop);
  puts_k(" fora de tudo:\n  nem livres, nem usados; so o reboot devolve.\n  cabecalho: ");
  if (c.bad_len < (int)sizeof(c.bad_raw)) {
    puts_k("cortado pelo fim da area\n");
  } else {
    puts_k("tamanho ");
    put_hex64(c.bad_size);
    puts_k(c.bad_size < sizeof(c.bad_raw) ? " (menor que o cabecalho)\n"
                                          : " (passa do fim da area)\n");
    puts_k("             livre ");
    put_u32(c.bad_free);
    puts_k(", dono ");
    put_u32(c.bad_owner);
    puts_k("\n");
  }
  puts_k("  bytes:");
  for (i = 0; i < c.bad_len; i++) {
    putc(' ');
    put_hex8(c.bad_raw[i]);
  }
  puts_k("  |");
  for (i = 0; i < c.bad_len; i++)
    putc(c.bad_raw[i] >= 32 && c.bad_raw[i] < 127 ? (char)c.bad_raw[i] : '.');
  puts_k("|\n");
  if (c.last) {
    puts_k("  bloco de antes: ");
    put_hex64(c.last);
    puts_k(", ");
    put_tam(c.last_size);
    if (c.last_free) {
      puts_k(", livre\n");
    } else {
      puts_k(", usado\n  dono: ");
      put_dono(c.last_owner);
      puts_k("\n  (se esse bloco escreveu alem do proprio fim, estragou o cabecalho)\n");
    }
  } else {
    puts_k("  a quebra e no primeiro bloco da area\n");
  }
  serial_puts("[mm] cadeia quebrada: area ");
  serial_hex32((uint32_t)area);
  serial_puts(" em ");
  serial_hex64(c.stop);
  serial_puts(", fora ");
  serial_hex32((uint32_t)((c.end - c.stop) >> 20));
  serial_puts(" MB, cabecalho ");
  serial_hex64(c.bad_size);
  serial_puts(" ");
  serial_hex32(c.bad_owner);
  serial_puts(", antes ");
  serial_hex64(c.last);
  serial_puts(" tam ");
  serial_hex64(c.last_size);
  serial_puts(c.last_free ? " livre\n" : " dono ");
  if (!c.last_free) {
    serial_hex32(c.last_owner);
    serial_puts("\n");
  }
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
  chk_show(0);
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
    chk_show(1);
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
    for (i = 0; i < hm_regions(); i++) {
      if (hm_regions() > 1) {
        puts_k("Regiao ");
        put_u32(i);
        puts_k(": ");
      }
      chk_show(2 + (int)i);
    }
  }
  return 0;
}
