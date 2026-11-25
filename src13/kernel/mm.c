/* mm.c - First-fit. Cada bloco guarda o tamanho total, inclusive o cabecalho.
 * Duas arenas: a do kernel (kmalloc, 2 MB em 0xA80000) e a alta (umalloc, a
 * maior regiao E820 abaixo de 4 GB, coberta pela identidade de kernel_entry)
 * para programas, DLLs e buffers grandes.
 *
 * Desde o boot em dois estagios o kernel fica em 0x01000000.  A arena alta e
 * escolhida a partir do mapa E820 entregue no BootInfo: nunca testamos (nem
 * escrevemos) memoria reservada e nunca comecamos antes de __bss_end.
 */
#include "kthread.h"
#include "mm.h"
#include "bootinfo.h"
#include "bootpak.h"
#include "serial.h"

/* O kernel fica em 0x01000000.  A FAT ocupa 0xA00000-0xA59000 e o heap do
 * kernel fica em 0xA80000-0xC7FFFF, abaixo do kernel. */
#define MM_HEAP_START 0x00A80000u
#define MM_HEAP_END   0x00C80000u
#define MM_ALIGN      16u

/* Limite da identidade montada por kernel_entry (4 GB menos uma pagina de
 * 2 MB, para que fim de arena e ponteiros caibam em 32 bits).  Sem E820 o
 * teste de RAM para em 1 GB: acima disso pode haver MMIO. */
#define UM_MAP_MAX    0xFFE00000u
#define UM_PROBE_MAX  0x40000000u
#define UM_STEP       0x00100000u
#define UM_START      0x01000000u
#define UM_MIN_SIZE   0x00400000u
#define UM_ALIGN_BIG  0x00200000u

extern char __bss_end[];

/* Blocos em sequencia fisica (mm_walk, ufree_owner e o TOP dependem disso).
 * prev = tamanho do bloco anterior (0 no primeiro): junta vizinhos livres nos
 * dois sentidos na hora do free, entao nunca ha dois livres seguidos. */
struct mm_hdr {
  uint32_t size;
  uint32_t free;
  uint32_t owner;
  uint32_t prev;
};

/* Bloco livre: os elos da lista ficam na area util (minimo 16 bytes). */
struct mm_free {
  struct mm_hdr h;
  uint32_t next;
  uint32_t prv;
};

/* Listas de livres por tamanho (potencias de 2 a partir de 32 bytes). Antes o
 * first-fit percorria a arena inteira a cada pedido: com o Firefox eram
 * milhares de blocos e ~12% da CPU carregando uma pagina. */
#define MM_NBIN 27
#define MM_MIN_BLK 32u

struct arena {
  uint32_t start;
  uint32_t end;
  int ready;
  uint32_t bin[MM_NBIN];
};

static struct arena low;
static struct arena high;
static uint32_t cur_owner;

static uint32_t align_up(uint32_t n) {
  return (n + (MM_ALIGN - 1u)) & ~(MM_ALIGN - 1u);
}

static struct mm_hdr* blk_at(uint32_t addr) {
  return (struct mm_hdr*)addr;
}

static int range_ok(const struct arena* a, struct mm_hdr* h) {
  uint32_t p = (uint32_t)h;
  if (p < a->start || p >= a->end) return 0;
  if (h->size < sizeof(struct mm_hdr)) return 0;
  if (h->size > a->end - p) return 0;
  return 1;
}

static void fl_insert(struct arena* a, struct mm_hdr* h);

static void arena_init(struct arena* a, uint32_t start, uint32_t end) {
  struct mm_hdr* h = blk_at(start);
  int i;
  a->start = start;
  a->end = end;
  for (i = 0; i < MM_NBIN; i++) a->bin[i] = 0;
  h->size = end - start;
  h->free = 1;
  h->owner = 0;
  h->prev = 0;
  fl_insert(a, h);
  a->ready = 1;
}

/* Fallback sem E820: testa cada MB acima do kernel.  Nunca comeca em UM_START
 * puro, porque o kernel ocupa 0x01000000 em diante. */
static uint32_t probe_ram_end(uint32_t start) {
  uint32_t a;
  if (start < UM_START) start = UM_START;
  start = (start + UM_STEP - 1u) & ~(UM_STEP - 1u);
  for (a = start; a < UM_PROBE_MAX; a += UM_STEP) {
    volatile uint32_t* p = (volatile uint32_t*)(a + UM_STEP - 4u);
    uint32_t old = *p;
    *p = 0x5A3CA55Au;
    if (*p != 0x5A3CA55Au) return a;
    *p = ~0x5A3CA55Au;
    if (*p != ~0x5A3CA55Au) return a;
    *p = old;
  }
  return UM_PROBE_MAX;
}

/* Verdadeiro se [start,end) cabe inteiro em uma entrada E820 utilizavel. */
static int range_usable(const struct BootInfo* bi, uint32_t start, uint32_t end) {
  const struct BootMemEntry* map;
  uint32_t i;
  if (!bi || bi->mem_count == 0) return 0;
  map = bootinfo_mem_map(bi);
  if (!map) return 0;
  for (i = 0; i < bi->mem_count; ++i) {
    uint64_t b = map[i].base;
    uint64_t len = map[i].length;
    uint64_t e;
    if (map[i].type != BOOTINFO_MEM_USABLE || len == 0) continue;
    if (b > 0xFFFFFFFFULL) continue;
    e = b + len;
    if (e < b) e = 0x100000000ULL;
    if (b <= start && e >= end) return 1;
  }
  return 0;
}

/* Fim da memoria que o kernel ocupa: __bss_end e a verdade do link; o tamanho
 * declarado no BootInfo cobre carregadores que informem um initrd/arquivo. */
static uint32_t kernel_reserved_end(const struct BootInfo* bi) {
  uint32_t end = (uint32_t)(uintptr_t)__bss_end;
  if (bi && bi->kernel_phys <= 0xFFFFFFFFULL) {
    uint64_t kend = bi->kernel_phys + bi->kernel_size;
    if (kend > end && kend <= 0xFFFFFFFFULL) end = (uint32_t)kend;
  }
  /* SODOS.PAK do GRUB fica logo depois do kernel (bootpak_early). */
  if (bootpak_reserved_end() > end) end = bootpak_reserved_end();
  return end;
}

/* ---------------- RAM acima de 4 GB ----------------
 * kernel_entry so monta identidade ate 4 GB.  Aqui completamos o mesmo PDPT
 * (GB 4..HM_MAX_GB-1) com paginas de 2 MB, so onde o E820 diz que ha RAM, e
 * entregamos essas regioes a um alocador de 64 bits (hmalloc).  Programas
 * x32 nao enxergam esses enderecos; quem usa e o kernel. */
#define HM_MAX_GB     64u
#define HM_FIRST_GB   4u
#define HM_MAX_ARENAS 8u
#define HM_PAGE       0x200000ULL
#define HM_GB         0x40000000ULL
#define HM_USER_PCT_DEFAULT 40u

struct hm_hdr {
  uint64_t size;
  uint32_t free;
  uint32_t owner;
};

struct harena {
  uint64_t start;
  uint64_t end;
};

static uint64_t hm_pd[HM_MAX_GB - HM_FIRST_GB][512] __attribute__((aligned(4096)));
static struct harena hm_arenas[HM_MAX_ARENAS];
static uint32_t hm_count;

/* Onde a proxima busca comeca em cada arena (next-fit). Com o Firefox pondo
 * os chunks do GC aqui sao milhares de blocos; recomecar do inicio a cada
 * pedido custaria uma passada pela arena inteira. */
static struct hm_hdr* hm_rover[HM_MAX_ARENAS];
/* Bytes acima de 4 GB entregues a programas (k_hmalloc) e o teto deles. */
static uint64_t hm_user_used;
static uint64_t hm_user_quota;
static uint32_t hm_user_pct = HM_USER_PCT_DEFAULT;

/* ---- Detector de cabecalho estragado acima de 4 GB ----
 * Em 25/11/2026 a cadeia quebrou duas vezes com o Chrome aberto: o cabecalho
 * logo depois de um bloco livre virou FF (uma vez) ou zeros (outra) e a RAM
 * dali ate o fim (23 GB) sumiu ate o reboot. O alocador sozinho esta certo:
 * alguem escreve na memoria do hm depois de solta ou alem do fim. Aqui fica
 * um historico das ultimas operacoes (quem pegou e soltou o que, e de onde
 * veio a chamada) e a cadeia e conferida a cada soltura, busca, juncao, fim
 * de programa e conta (o [temp] conta a cada 10 s). Na primeira quebra o
 * serial ganha um relatorio "[mm] hm estragado" com o historico daquela
 * regiao. */
#define HM_HIST 1024u
struct hm_ev {
  uint64_t addr;  /* cabecalho do bloco */
  uint64_t size;
  uint64_t quem;  /* retorno de quem chamou hmalloc/hm_user_alloc/hfree/hm_user_free */
  uint32_t owner;
  uint32_t op;    /* 'A' pegou, 'L' soltou, 'D' solto no fim do dono */
};
static struct hm_ev hm_hist[HM_HIST];
static uint32_t hm_hist_n;       /* eventos desde o boot (indice = n % HM_HIST) */
static uint64_t hm_quem;         /* quem chamou a operacao em curso (sob o BKL) */
static uint64_t hm_bad_seen;     /* quebra ja relatada (uma vez por endereco) */
static uint32_t hm_bad_frees;    /* hfree de bloco invalido ja relatados */
const char* mod_pid_name(uint32_t pid); /* module.c */

#define HM_QUEM() (hm_quem = (uint64_t)(uintptr_t)__builtin_return_address(0))

static void hm_hex64(uint64_t v) {
  serial_hex32((uint32_t)(v >> 32));
  serial_hex32((uint32_t)v);
}

static void hm_owner_name(uint32_t owner) {
  serial_puts(owner == 0 ? "kernel" : owner == 0xFFFFFF00u ? "WINE-compartilhado" : mod_pid_name(owner));
}

static void hm_note(uint32_t op, const struct hm_hdr* h, uint64_t size, uint32_t owner) {
  struct hm_ev* e = &hm_hist[hm_hist_n % HM_HIST];
  e->addr = (uint64_t)(uintptr_t)h;
  e->size = size;
  e->quem = hm_quem;
  e->owner = owner;
  e->op = op;
  hm_hist_n++;
}

/* Operacoes do historico cujo bloco cobria [lo, hi), da mais nova para a
 * mais velha. */
static void hm_hist_near(uint64_t lo, uint64_t hi) {
  uint32_t k, shown = 0, n = hm_hist_n < HM_HIST ? hm_hist_n : HM_HIST;
  for (k = 0; k < n && shown < 40u; k++) {
    uint32_t seq = hm_hist_n - 1u - k;
    const struct hm_ev* e = &hm_hist[seq % HM_HIST];
    char op[2];
    if (e->addr >= hi || e->addr + e->size <= lo) continue;
    op[0] = (char)e->op;
    op[1] = 0;
    serial_puts("[mm]   #");
    serial_hex32(seq);
    serial_puts(" ");
    serial_puts(op);
    serial_puts(" bloco ");
    hm_hex64(e->addr);
    serial_puts(" tam ");
    hm_hex64(e->size);
    serial_puts(" dono ");
    serial_hex32(e->owner);
    serial_puts(" ");
    hm_owner_name(e->owner);
    serial_puts(" quem ");
    hm_hex64(e->quem);
    serial_puts("\n");
    shown++;
  }
  if (!shown) serial_puts("[mm]   (nada no historico perto dali)\n");
}

/* bad: onde devia haver um cabecalho e nao ha; prev: o ultimo bloco bom. */
static void hm_broken(const struct harena* a, const struct hm_hdr* prev, const struct hm_hdr* bad,
                      const char* onde) {
  uint64_t b = (uint64_t)(uintptr_t)bad, room = a->end - b;
  const uint8_t* raw = (const uint8_t*)bad;
  int i;
  if (b >= a->end || b == hm_bad_seen) return;
  hm_bad_seen = b;
  serial_puts("[mm] hm estragado (");
  serial_puts(onde);
  serial_puts(") em ");
  hm_hex64(b);
  serial_puts(", operacao ");
  serial_hex32(hm_hist_n);
  serial_puts(", quem ");
  hm_hex64(hm_quem);
  serial_puts("\n[mm]   bytes:");
  for (i = 0; i < 16 && (uint64_t)i < room; i++) {
    static const char hx[] = "0123456789ABCDEF";
    serial_putc(' ');
    serial_putc(hx[raw[i] >> 4]);
    serial_putc(hx[raw[i] & 15u]);
  }
  serial_puts("\n");
  if (prev) {
    serial_puts("[mm]   bloco de antes ");
    hm_hex64((uint64_t)(uintptr_t)prev);
    serial_puts(" tam ");
    hm_hex64(prev->size);
    serial_puts(prev->free ? " livre" : " usado, dono ");
    if (!prev->free) {
      serial_hex32(prev->owner);
      serial_puts(" ");
      hm_owner_name(prev->owner);
    }
    serial_puts("\n");
  }
  /* quem morou do bloco de antes ate o cabecalho estragado: o bloco de antes
   * costuma ser livre, juntado de varios soltos, e quem escreveu depois de
   * soltar (ou alem do fim) esta entre eles */
  serial_puts("[mm]   historico da regiao (bloco de antes ate o cabecalho estragado):\n");
  hm_hist_near(prev ? (uint64_t)(uintptr_t)prev : (b > 16u ? b - 16u : 0), b + 16u);
}

static void hm_add_arena(uint64_t s, uint64_t e) {
  uint32_t i;
  if (e <= s + UM_MIN_SIZE) return;
  /* Regioes vizinhas no E820 viram uma so. */
  for (i = 0; i < hm_count; ++i) {
    if (hm_arenas[i].end == s) {
      hm_arenas[i].end = e;
      return;
    }
  }
  if (hm_count >= HM_MAX_ARENAS) return;
  hm_arenas[hm_count].start = s;
  hm_arenas[hm_count].end = e;
  hm_count++;
}

/* Mapeia [s,e) (alinhados a 2 MB) na identidade; devolve 0 se nao conseguiu. */
static int hm_map(uint64_t* pdpt, uint64_t s, uint64_t e) {
  uint64_t a;
  for (a = s; a < e; a += HM_PAGE) {
    uint32_t gb = (uint32_t)(a / HM_GB);
    uint32_t idx = (uint32_t)((a % HM_GB) / HM_PAGE);
    uint64_t* pd;
    if (gb < HM_FIRST_GB || gb >= HM_MAX_GB) return 0;
    if (pdpt[gb] & 1u) {
      if (pdpt[gb] & 0x80u) continue;  /* pagina de 1 GB ja montada */
      pd = (uint64_t*)(uintptr_t)(pdpt[gb] & 0x000FFFFFFFFFF000ULL);
    } else {
      pd = hm_pd[gb - HM_FIRST_GB];
      pdpt[gb] = (uint64_t)(uintptr_t)pd | 3u;
    }
    if (!(pd[idx] & 1u)) pd[idx] = a | 0x83u;  /* P | RW | PS, write-back */
  }
  return 1;
}


/* ---- Tipo de cache da RAM acima de 4 GB ----
 * O PAT do kernel e WB, mas o tipo efetivo e o "pior" entre PAT e MTRR. Em
 * VM e sempre WB; num notebook o firmware pode deixar parte (ou tudo) acima
 * de 4 GB sem cobertura WB. Memoria UC/WT para o heap do Firefox (codigo JIT,
 * GC, wasm) deixa tudo dezenas de vezes mais lento: CPU0 a 100% e teclado e
 * mouse sem resposta. Aqui calculamos o tipo efetivo por pagina de 2 MB,
 * corrigimos pelo TOM2 da AMD quando o problema e o tipo padrao, e o que
 * continuar nao-WB fica fora das arenas. */
#define MT_UC 0u
#define MT_WC 1u
#define MT_WT 4u
#define MT_WP 5u
#define MT_WB 6u

static inline uint64_t mm_rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}
static inline void mm_wrmsr(uint32_t msr, uint64_t v) {
  __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void mm_cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
  __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static int mt_ok;            /* CPU tem MTRR */
static int mt_amd;           /* AMD: SYSCFG/TOM2 */
static uint64_t mt_physmask; /* bits de endereco fisico */
static uint32_t mt_vcnt;
static uint64_t mt_def;
static uint64_t mt_base[32], mt_mask[32];
static uint64_t mt_syscfg, mt_tom2;
/* Aplicado na BSP; os APs repetem em ap_cpu_setup (mm_mtrr_ap). */
static int mt_fix_tom2;
static uint64_t mt_fix_tom2_val;
static uint64_t mt_bytes[8];  /* bytes de RAM alta por tipo, antes da correcao */

static void mt_read(void) {
  uint32_t a, b, c, d, i;
  mt_ok = 0;
  mm_cpuid(0, &a, &b, &c, &d);
  mt_amd = (b == 0x68747541u); /* "Auth"enticAMD */
  mm_cpuid(1, &a, &b, &c, &d);
  if (!(d & (1u << 12))) return;
  /* Em VM o tipo efetivo e o do host (WB) e o KVM pode dar #GP nas MSRs da
   * AMD (SYSCFG/TOM2) - aqui ainda nao ha IDT: seria reset. */
  if (c & (1u << 31)) return;
  mm_cpuid(0x80000000u, &a, &b, &c, &d);
  if (a >= 0x80000008u) {
    mm_cpuid(0x80000008u, &a, &b, &c, &d);
    mt_physmask = ((a & 0xFFu) >= 64u) ? ~0ULL : ((1ULL << (a & 0xFFu)) - 1u);
  } else {
    mt_physmask = (1ULL << 36) - 1u;
  }
  mt_vcnt = (uint32_t)(mm_rdmsr(0xFE) & 0xFFu);
  if (mt_vcnt > 32u) mt_vcnt = 32u;
  mt_def = mm_rdmsr(0x2FF);
  for (i = 0; i < mt_vcnt; ++i) {
    mt_base[i] = mm_rdmsr(0x200u + 2u * i);
    mt_mask[i] = mm_rdmsr(0x201u + 2u * i);
  }
  if (mt_amd) {
    mt_syscfg = mm_rdmsr(0xC0010010u);
    mt_tom2 = (mt_syscfg & (1ULL << 21)) ? (mm_rdmsr(0xC001001Du) & 0x0000FFFFFF800000ULL) : 0;
  }
  mt_ok = 1;
}

/* Tipo efetivo do MTRR para o endereco a (>= 1 MB). */
static uint32_t mt_type(uint64_t a) {
  uint32_t i, t = 0xFFu;
  if (!mt_ok) return MT_WB;
  if (!(mt_def & (1u << 11))) return MT_UC; /* MTRRs desligados = UC */
  for (i = 0; i < mt_vcnt; ++i) {
    uint64_t m = mt_mask[i];
    uint32_t ty;
    if (!(m & (1u << 11))) continue;
    m &= mt_physmask & ~0xFFFULL;
    if ((a & m) != (mt_base[i] & m)) continue;
    ty = (uint32_t)(mt_base[i] & 0xFFu);
    if (t == 0xFFu) t = ty;
    else if (ty == MT_UC || t == MT_UC) t = MT_UC;
    else if ((ty == MT_WT && t == MT_WB) || (ty == MT_WB && t == MT_WT)) t = MT_WT;
    else if (ty != t) t = MT_UC; /* combinacao indefinida: trata como UC */
  }
  if (t != 0xFFu) return t;
  if (mt_amd && (mt_syscfg & (1ULL << 21)) && (mt_syscfg & (1ULL << 22)) && a >= 0x100000000ULL &&
      a < mt_tom2)
    return MT_WB;
  return (uint32_t)(mt_def & 0xFFu);
}

/* Pior tipo dentro de [a, a + HM_PAGE): confere inicio, fim e as bordas de
 * cada MTRR variavel (paginas de 2 MB; MTRRs tem granularidade de 4 KB). */
static uint32_t mt_type_2m(uint64_t a) {
  uint32_t t = mt_type(a), u = mt_type(a + HM_PAGE - 0x1000u);
  if (t != u) return MT_UC;
  return t;
}

/* Desliga o cache, troca, invalida (sequencia do manual para MTRR). */
static void mt_apply_tom2(uint64_t tom2) {
  uint64_t cr0, f;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %0, %%cr0" : : "r"((cr0 | (1ULL << 30)) & ~(1ULL << 29)) : "memory");
  __asm__ volatile("wbinvd" ::: "memory");
  mm_wrmsr(0xC001001Du, tom2);
  mm_wrmsr(0xC0010010u, mm_rdmsr(0xC0010010u) | (1ULL << 21) | (1ULL << 22));
  __asm__ volatile("wbinvd" ::: "memory");
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
  if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

void mm_mtrr_ap(void) {
  if (mt_fix_tom2) mt_apply_tom2(mt_fix_tom2_val);
}

static void mt_count(uint64_t s, uint64_t e, uint64_t* by) {
  uint64_t a;
  for (a = s; a < e; a += HM_PAGE) by[mt_type_2m(a) & 7u] += HM_PAGE;
}

/* Chamado por hm_init antes de criar as arenas: le os MTRRs, registra na
 * serial quanto da RAM alta e WB/UC e, se o problema for o tipo padrao (AMD),
 * liga o TOM2 com WB. O que ainda nao for WB fica fora (hm_add_arena_wb). */
static void mt_prepare(const uint64_t* rs, const uint64_t* re, uint32_t n) {
  uint32_t i;
  uint64_t top = 0, by[8];
  int bad_default = 0, any_bad = 0;
  for (i = 0; i < 8; ++i) mt_bytes[i] = by[i] = 0;
  mt_read();
  if (!mt_ok) return;
  for (i = 0; i < n; ++i) {
    uint64_t a;
    if (re[i] > top) top = re[i];
    for (a = rs[i]; a < re[i]; a += HM_PAGE) {
      uint32_t t = mt_type_2m(a);
      mt_bytes[t & 7u] += HM_PAGE;
      if (t != MT_WB) {
        uint32_t k, hit = 0;
        any_bad = 1;
        for (k = 0; k < mt_vcnt; ++k) {
          uint64_t m = mt_mask[k];
          if (!(m & (1u << 11))) continue;
          m &= mt_physmask & ~0xFFFULL;
          if ((a & m) == (mt_base[k] & m)) hit = 1;
        }
        if (!hit) bad_default = 1; /* vem do tipo padrao, nao de um MTRR */
      }
    }
  }
  serial_puts("[mm] MTRR def=");
  serial_hex32((uint32_t)mt_def);
  serial_puts(" vcnt=");
  serial_hex32(mt_vcnt);
  if (mt_amd) {
    serial_puts(" SYSCFG=");
    serial_hex32((uint32_t)mt_syscfg);
    serial_puts(" TOM2=");
    serial_hex32((uint32_t)(mt_tom2 >> 20));
    serial_puts("MB");
  }
  serial_puts("\n[mm] RAM alta WB=");
  serial_hex32((uint32_t)(mt_bytes[MT_WB] >> 20));
  serial_puts("MB UC=");
  serial_hex32((uint32_t)(mt_bytes[MT_UC] >> 20));
  serial_puts("MB outros=");
  serial_hex32((uint32_t)((mt_bytes[MT_WC] + mt_bytes[MT_WT] + mt_bytes[MT_WP]) >> 20));
  serial_puts("MB\n");
  if (!any_bad) return;
  if (bad_default && mt_amd && top) {
    /* TOM2 cobre [4 GB, TOM2) com WB por padrao (granularidade 8 MB). */
    uint64_t t2 = (top + 0x7FFFFFULL) & ~0x7FFFFFULL;
    if (t2 < mt_tom2) t2 = mt_tom2;
    mt_fix_tom2_val = t2;
    mt_fix_tom2 = 1;
    mt_apply_tom2(t2);
    mt_read();
    for (i = 0; i < n; ++i) mt_count(rs[i], re[i], by);
    serial_puts("[mm] TOM2 -> WB: agora WB=");
    serial_hex32((uint32_t)(by[MT_WB] >> 20));
    serial_puts("MB\n");
  }
}

void mm_mtrr_info(uint64_t* wb, uint64_t* uc, uint64_t* other, int* fixed) {
  if (wb) *wb = mt_bytes[MT_WB];
  if (uc) *uc = mt_bytes[MT_UC];
  if (other) *other = mt_bytes[MT_WC] + mt_bytes[MT_WT] + mt_bytes[MT_WP];
  if (fixed) *fixed = mt_fix_tom2;
}

static void hm_add_arena_wb(uint64_t s, uint64_t e) {
  /* So as partes WB viram arena (o que sobrar fora de WB nao e usado). */
  uint64_t a = s, run = s;
  for (; a < e; a += HM_PAGE) {
    if (mt_type_2m(a) != MT_WB) {
      if (a > run) hm_add_arena(run, a);
      run = a + HM_PAGE;
    }
  }
  if (e > run) hm_add_arena(run, e);
}

static void hm_init(const struct BootInfo* bi) {
  const struct BootMemEntry* map;
  uint64_t cr3;
  uint64_t* pml4;
  uint64_t* pdpt;
  uint32_t i;
  uint64_t total = 0;

  hm_count = 0;
  map = bootinfo_mem_map(bi);
  if (!bi || !map || bi->mem_count == 0) return;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  pml4 = (uint64_t*)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ULL);
  if (!(pml4[0] & 1u)) return;
  pdpt = (uint64_t*)(uintptr_t)(pml4[0] & 0x000FFFFFFFFFF000ULL);

  {
    uint64_t rs[16], re[16];
    uint32_t n = 0;
    for (i = 0; i < bi->mem_count && n < 16; ++i) {
      uint64_t b = map[i].base;
      uint64_t e = b + map[i].length;
      if (map[i].type != BOOTINFO_MEM_USABLE || map[i].length == 0) continue;
      if (e < b) continue;
      if (b < 0x100000000ULL) b = 0x100000000ULL;
      if (e > (uint64_t)HM_MAX_GB * HM_GB) e = (uint64_t)HM_MAX_GB * HM_GB;
      b = (b + HM_PAGE - 1u) & ~(HM_PAGE - 1u);
      e &= ~(HM_PAGE - 1u);
      if (e <= b) continue;
      if (!hm_map(pdpt, b, e)) continue;
      rs[n] = b;
      re[n] = e;
      n++;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    mt_prepare(rs, re, n);
    for (i = 0; i < n; ++i) hm_add_arena_wb(rs[i], re[i]);
  }

  for (i = 0; i < hm_count; ++i) {
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)hm_arenas[i].start;
    h->size = hm_arenas[i].end - hm_arenas[i].start;
    h->free = 1;
    h->owner = 0;
    total += h->size;
  }
  for (i = 0; i < HM_MAX_ARENAS; ++i) hm_rover[i] = 0;
  serial_puts("[mm] RAM acima de 4 GB: ");
  serial_hex32((uint32_t)(total >> 20));
  serial_puts(" MB em ");
  serial_hex32(hm_count);
  serial_puts(" regioes\n");
}

static int hm_ok(const struct harena* a, const struct hm_hdr* h) {
  uint64_t p = (uint64_t)(uintptr_t)h;
  if (p < a->start || p >= a->end) return 0;
  if (h->size < sizeof(struct hm_hdr)) return 0;
  if (h->size > a->end - p) return 0;
  return 1;
}

static void hm_merge(uint32_t ai, struct hm_hdr* h) {
  const struct harena* a = &hm_arenas[ai];
  for (;;) {
    struct hm_hdr* nxt = (struct hm_hdr*)((uint8_t*)h + h->size);
    if ((uint64_t)(uintptr_t)nxt >= a->end) return;
    if (!hm_ok(a, nxt)) {
      hm_broken(a, h, nxt, "juntar");
      return;
    }
    if (!nxt->free) return;
    if (hm_rover[ai] == nxt) hm_rover[ai] = h;
    h->size += nxt->size;
  }
}

int hm_ready(void) { return hm_count != 0; }

/* Tenta o bloco livre h: payload alinhado a align. A sobra da frente (se
 * houver) vira um bloco livre proprio; a de tras tambem. */
static void* hm_take(uint32_t ai, struct hm_hdr* h, uint64_t nbytes, uint64_t align,
                     uint32_t owner) {
  uint64_t base = (uint64_t)(uintptr_t)h;
  uint64_t pay = base + sizeof(struct hm_hdr);
  uint64_t need;
  uint64_t lead;
  pay = (pay + align - 1u) & ~(align - 1u);
  lead = pay - sizeof(struct hm_hdr) - base;
  /* Folga pequena demais para um bloco livre: empurra para o proximo
   * alinhamento. */
  while (lead != 0 && lead < sizeof(struct hm_hdr) + MM_ALIGN) {
    pay += align;
    lead += align;
  }
  need = (nbytes + sizeof(struct hm_hdr) + (MM_ALIGN - 1u)) & ~(uint64_t)(MM_ALIGN - 1u);
  if (need < nbytes || lead + need < lead || h->size < lead + need) return 0;
  if (lead) {
    struct hm_hdr* nh = (struct hm_hdr*)(uintptr_t)(base + lead);
    nh->size = h->size - lead;
    nh->free = 1;
    nh->owner = 0;
    h->size = lead; /* continua livre */
    h = nh;
  }
  {
    uint64_t rest = h->size - need;
    if (rest >= sizeof(struct hm_hdr) + MM_ALIGN) {
      struct hm_hdr* nxt = (struct hm_hdr*)((uint8_t*)h + need);
      nxt->size = rest;
      nxt->free = 1;
      nxt->owner = 0;
      h->size = need;
      hm_rover[ai] = nxt;
    } else {
      hm_rover[ai] = h;
    }
  }
  h->free = 0;
  h->owner = owner;
  hm_note('A', h, h->size, owner);
  return (uint8_t*)h + sizeof(struct hm_hdr);
}

static void* hm_alloc(uint64_t nbytes, uint64_t align, uint32_t owner) {
  uint32_t i;
  if (nbytes == 0) return 0;
  if (align < MM_ALIGN) align = MM_ALIGN;
  if (align & (align - 1u)) return 0;
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* first = (struct hm_hdr*)(uintptr_t)a->start;
    struct hm_hdr* r = hm_rover[i];
    struct hm_hdr* h;
    struct hm_hdr* prev = 0;
    void* p;
    if (!r || !hm_ok(a, r)) r = first;
    /* Do rover ate o fim, depois do inicio ate o rover. */
    for (h = r; hm_ok(a, h); prev = h, h = (struct hm_hdr*)((uint8_t*)h + h->size)) {
      if (!h->free) continue;
      hm_merge(i, h);
      if ((p = hm_take(i, h, nbytes, align, owner)) != 0) return p;
    }
    if (prev) hm_broken(a, prev, h, "busca"); /* parou antes do fim (no fim nao relata) */
    if (r == first) continue;
    prev = 0;
    for (h = first; hm_ok(a, h) && h != r; prev = h, h = (struct hm_hdr*)((uint8_t*)h + h->size)) {
      if (!h->free) continue;
      hm_merge(i, h);
      if ((p = hm_take(i, h, nbytes, align, owner)) != 0) return p;
    }
    if (h != r) hm_broken(a, prev, h, "busca");
  }
  return 0;
}

void* hmalloc(uint64_t nbytes) {
  HM_QUEM();
  return hm_alloc(nbytes, MM_ALIGN, 0);
}

/* Devolve o tamanho do bloco solto (0 se ptr nao e um bloco usado daqui). */
static uint64_t hm_release(void* ptr) {
  uint32_t i;
  uint64_t p;
  if (!ptr) return 0;
  p = (uint64_t)(uintptr_t)ptr - sizeof(struct hm_hdr);
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)p;
    struct hm_hdr* nxt;
    uint64_t sz;
    if (p < a->start || p >= a->end) continue;
    if (!hm_ok(a, h) || h->free) {
      /* soltura dupla ou ponteiro que nao e de bloco: o proprio cabecalho
       * pode ter sido pisado. Os primeiros vao para o serial. */
      if (hm_bad_frees++ < 20u) {
        serial_puts("[mm] hfree de bloco invalido ou ja solto ");
        hm_hex64(p);
        serial_puts(" quem ");
        hm_hex64(hm_quem);
        serial_puts(hm_ok(a, h) ? " (ja livre)\n" : " (cabecalho estragado)\n");
      }
      return 0;
    }
    /* o vizinho de depois ainda e um cabecalho? Aqui o bloco ainda tem dono:
     * se ele escreveu alem do fim, o relatorio diz quem. */
    nxt = (struct hm_hdr*)((uint8_t*)h + h->size);
    if ((uint64_t)(uintptr_t)nxt < a->end && !hm_ok(a, nxt)) hm_broken(a, h, nxt, "hfree, vizinho");
    sz = h->size;
    hm_note('L', h, sz, h->owner);
    h->free = 1;
    h->owner = 0;
    hm_merge(i, h);
    return sz;
  }
  return 0;
}

void hfree(void* ptr) {
  HM_QUEM();
  (void)hm_release(ptr);
}

/* ---- RAM acima de 4 GB para programas (64 bits: Firefox) ----
 * Teto = hm_user_pct% de toda a RAM do PC (abaixo + acima de 4 GB), limitado
 * ao que existe acima de 4 GB. O kernel quase nao usa essa RAM (so o Z:), entao
 * o teto fica de fato disponivel para o programa. SET HIMEM=<pct> muda. */
static uint64_t hm_total_ram(void) {
  uint64_t t = 0;
  uint32_t i;
  for (i = 0; i < hm_count; ++i) t += hm_arenas[i].end - hm_arenas[i].start;
  if (high.ready) t += high.end - high.start;
  return t;
}

static void hm_quota_update(void) {
  uint64_t q = 0;
  uint32_t i;
  uint64_t hi = 0;
  for (i = 0; i < hm_count; ++i) hi += hm_arenas[i].end - hm_arenas[i].start;
  if (hm_user_pct) q = hm_total_ram() / 100u * hm_user_pct;
  if (q > hi) q = hi;
  hm_user_quota = q;
}

void hm_set_user_pct(uint32_t pct) {
  if (pct > 95u) pct = 95u;
  hm_user_pct = pct;
  hm_quota_update();
}

void* hm_user_alloc(uint64_t nbytes, uint64_t align, uint32_t owner) {
  void* p;
  uint64_t need = (nbytes + sizeof(struct hm_hdr) + (MM_ALIGN - 1u)) & ~(uint64_t)(MM_ALIGN - 1u);
  HM_QUEM();
  if (!hm_count || need < nbytes) return 0;
  if (hm_user_used + need > hm_user_quota) return 0;
  p = hm_alloc(nbytes, align, owner ? owner : 1u);
  if (p) hm_user_used += ((struct hm_hdr*)((uint8_t*)p - sizeof(struct hm_hdr)))->size;
  return p;
}

void hm_user_free(void* ptr) {
  uint64_t sz;
  HM_QUEM();
  sz = hm_release(ptr);
  if (sz <= hm_user_used) hm_user_used -= sz;
  else hm_user_used = 0;
}

/* Fim do programa: solta o que ele pegou acima de 4 GB. */
void hm_free_owner(uint32_t owner) {
  uint32_t i;
  if (owner == 0) return;
  HM_QUEM();
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)a->start;
    struct hm_hdr* prev = 0;
    while (hm_ok(a, h)) {
      if (!h->free && h->owner == owner) {
        if (h->size <= hm_user_used) hm_user_used -= h->size;
        else hm_user_used = 0;
        hm_note('D', h, h->size, owner);
        h->free = 1;
        h->owner = 0;
        hm_merge(i, h);
      }
      prev = h;
      h = (struct hm_hdr*)((uint8_t*)h + h->size);
    }
    hm_broken(a, prev, h, "fim do dono"); /* no fim da arena nao relata */
  }
}

void hm_user_info(uint64_t* quota, uint64_t* used, uint64_t* total_ram) {
  if (quota) *quota = hm_user_quota;
  if (used) *used = hm_user_used;
  if (total_ram) *total_ram = hm_total_ram();
}

void hm_stats(uint64_t* total, uint64_t* used, uint64_t* free_bytes, uint64_t* largest) {
  uint64_t t = 0, u = 0, f = 0, big = 0;
  uint32_t i;
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)a->start;
    struct hm_hdr* prev = 0;
    t += a->end - a->start;
    while (hm_ok(a, h)) {
      if (h->free) {
        f += h->size;
        if (h->size > big) big = h->size;
      } else {
        u += h->size;
      }
      prev = h;
      h = (struct hm_hdr*)((uint8_t*)h + h->size);
    }
    hm_broken(a, prev, h, "conta"); /* o [temp] conta a cada 10 s */
  }
  if (total) *total = t;
  if (used) *used = u;
  if (free_bytes) *free_bytes = f;
  if (largest) *largest = big;
}

uint32_t hm_regions(void) { return hm_count; }
uint64_t hm_region_start(uint32_t i) { return i < hm_count ? hm_arenas[i].start : 0; }
uint64_t hm_region_end(uint32_t i) { return i < hm_count ? hm_arenas[i].end : 0; }

void mm_init(void) {
  const struct BootInfo* bi = bootinfo_get();
  const struct BootMemEntry* map;
  uint32_t kend = kernel_reserved_end(bi);
  uint32_t start;
  uint32_t i;

  low.ready = 0;
  high.ready = 0;

  if (kend < UM_START) kend = UM_START;
  start = (kend + UM_ALIGN_BIG - 1u) & ~(UM_ALIGN_BIG - 1u);

  map = bootinfo_mem_map(bi);
  if (bi && map && bi->mem_count != 0) {
    /* A maior regiao livre ate UM_MAP_MAX: firmwares de notebook deixam
     * buracos reservados que cortariam a primeira regiao. */
    uint64_t best_s = 0, best_e = 0;
    for (i = 0; i < bi->mem_count; ++i) {
      uint64_t b = map[i].base;
      uint64_t len = map[i].length;
      uint64_t e;
      uint64_t s;
      if (map[i].type != BOOTINFO_MEM_USABLE || len == 0) continue;
      if (b > 0xFFFFFFFFULL) continue;
      e = b + len;
      if (e < b) e = 0x100000000ULL;
      if (e > UM_MAP_MAX) e = UM_MAP_MAX;
      if (e <= start) continue;
      s = (b > start) ? b : start;
      s = (s + UM_ALIGN_BIG - 1u) & ~(uint64_t)(UM_ALIGN_BIG - 1u);
      if (e > s + UM_MIN_SIZE && e - s > best_e - best_s) {
        best_s = s;
        best_e = e;
      }
    }
    if (best_e) arena_init(&high, (uint32_t)best_s, (uint32_t)best_e);
    /* A arena baixa e fixa; so a habilitamos se o E820 disser que e RAM. */
    if (kend >= MM_HEAP_END && range_usable(bi, MM_HEAP_START, MM_HEAP_END))
      arena_init(&low, MM_HEAP_START, MM_HEAP_END);
    hm_init(bi);
    hm_quota_update();
  } else {
    uint32_t top = probe_ram_end(start);
    if (top >= start + UM_MIN_SIZE)
      arena_init(&high, start, top);
    if (top >= MM_HEAP_END && kend >= MM_HEAP_END)
      arena_init(&low, MM_HEAP_START, MM_HEAP_END);
  }
}

int mm_ready(void) { return low.ready; }
uint32_t mm_heap_start(void) { return MM_HEAP_START; }
uint32_t mm_heap_end(void) { return MM_HEAP_END; }

static int bin_of(uint32_t size) {
  int b = 31 - __builtin_clz(size) - 5;
  if (b < 0) b = 0;
  if (b >= MM_NBIN) b = MM_NBIN - 1;
  return b;
}

static struct mm_free* fb(uint32_t addr) { return (struct mm_free*)addr; }

static void fl_insert(struct arena* a, struct mm_hdr* h) {
  int b = bin_of(h->size);
  struct mm_free* f = (struct mm_free*)h;
  f->prv = 0;
  f->next = a->bin[b];
  if (f->next) fb(f->next)->prv = (uint32_t)h;
  a->bin[b] = (uint32_t)h;
}

/* Antes de mudar h->size: a lista e escolhida pelo tamanho. */
static void fl_remove(struct arena* a, struct mm_hdr* h) {
  struct mm_free* f = (struct mm_free*)h;
  if (f->prv) fb(f->prv)->next = f->next;
  else a->bin[bin_of(h->size)] = f->next;
  if (f->next) fb(f->next)->prv = f->prv;
}

static struct mm_hdr* next_blk(const struct arena* a, struct mm_hdr* h) {
  uint32_t n = (uint32_t)h + h->size;
  return n < a->end ? blk_at(n) : 0;
}

static struct mm_hdr* prev_blk(const struct arena* a, struct mm_hdr* h) {
  uint32_t p;
  if (!h->prev || h->prev > (uint32_t)h - a->start) return 0;
  p = (uint32_t)h - h->prev;
  return blk_at(p);
}

static void fix_next_prev(const struct arena* a, struct mm_hdr* h) {
  struct mm_hdr* n = next_blk(a, h);
  if (n) n->prev = h->size;
}

/* Refaz prev e as listas a partir da sequencia fisica, juntando livres
 * vizinhos. Para ufree_owner e para quando um elo de lista sai da arena (um
 * programa escreveu alem do bloco dele, dentro de um bloco livre). */
static void arena_rebuild(struct arena* a) {
  struct mm_hdr* h;
  uint32_t prev = 0;
  int i;
  for (i = 0; i < MM_NBIN; i++) a->bin[i] = 0;
  h = blk_at(a->start);
  while (range_ok(a, h)) {
    h->prev = prev;
    if (h->free) {
      struct mm_hdr* n;
      while ((n = next_blk(a, h)) && range_ok(a, n) && n->free) h->size += n->size;
      h->free = 1;
      h->owner = 0;
      fl_insert(a, h);
    }
    prev = h->size;
    h = blk_at((uint32_t)h + h->size);
  }
}

static int link_ok(const struct arena* a, uint32_t x) {
  struct mm_hdr* h = blk_at(x);
  return x >= a->start && x < a->end && !(x & (MM_ALIGN - 1u)) && range_ok(a, h) && h->free == 1;
}

/* Bloco livre (fora da lista): junta com os vizinhos e entra na lista. */
static struct mm_hdr* coalesce(struct arena* a, struct mm_hdr* h) {
  struct mm_hdr* n = next_blk(a, h);
  struct mm_hdr* p;
  if (n && range_ok(a, n) && n->free) {
    fl_remove(a, n);
    h->size += n->size;
  }
  p = prev_blk(a, h);
  if (p && range_ok(a, p) && p->free) {
    fl_remove(a, p);
    p->size += h->size;
    h = p;
  }
  h->free = 1;
  h->owner = 0;
  fix_next_prev(a, h);
  fl_insert(a, h);
  return h;
}

/* h ocupado com size >= need: a sobra (se der um bloco) volta livre. O vizinho
 * seguinte de um bloco que estava livre nunca e livre. */
static void split(struct arena* a, struct mm_hdr* h, uint32_t need) {
  uint32_t rest = h->size - need;
  if (rest >= MM_MIN_BLK) {
    struct mm_hdr* t = blk_at((uint32_t)h + need);
    t->size = rest;
    t->free = 1;
    t->owner = 0;
    t->prev = need;
    h->size = need;
    fix_next_prev(a, t);
    fl_insert(a, t);
  } else {
    fix_next_prev(a, h);
  }
}

static uint32_t blk_need(uint32_t nbytes) {
  uint32_t need = align_up(nbytes + (uint32_t)sizeof(struct mm_hdr));
  if (need < nbytes) return 0;
  return need < MM_MIN_BLK ? MM_MIN_BLK : need;
}

/* Primeiro livre que cabe, da lista do tamanho para cima. */
static struct mm_hdr* find_fit(struct arena* a, uint32_t need) {
  int b, tries;
  for (tries = 0; tries < 2; tries++) {
    int bad = 0;
    uint32_t holder = 0, badx = 0;
    for (b = bin_of(need); b < MM_NBIN && !bad; b++) {
      uint32_t x, px = 0;
      for (x = a->bin[b]; x; px = x, x = fb(x)->next) {
        if (!link_ok(a, x)) {
          bad = 1;
          holder = px;
          badx = x;
          break;
        }
        if (blk_at(x)->size >= need) return blk_at(x);
      }
    }
    if (!bad) return 0;
    serial_puts("[mm] lista de livres corrompida: refazendo\n");
    /* Quem estragou: o bloco livre cujo elo ficou ruim (escrita depois do
     * free) ou o bloco ocupado logo antes dele (escrita alem do fim). */
    serial_puts("[mm]   elo ruim ");
    serial_hex32(badx);
    if (holder) {
      struct mm_hdr* hh = blk_at(holder);
      struct mm_hdr* pv = prev_blk(a, hh);
      const uint32_t* w = (const uint32_t*)(uintptr_t)(holder + (uint32_t)sizeof(struct mm_hdr));
      serial_puts(" no livre ");
      serial_hex32(holder);
      serial_puts(" tam ");
      serial_hex32(hh->size);
      serial_puts(" dados ");
      serial_hex32(w[0]);
      serial_puts(" ");
      serial_hex32(w[1]);
      serial_puts(" ");
      serial_hex32(w[2]);
      serial_puts(" ");
      serial_hex32(w[3]);
      if (pv && range_ok(a, pv)) {
        serial_puts(" | antes: bloco ");
        serial_hex32((uint32_t)(uintptr_t)pv);
        serial_puts(" tam ");
        serial_hex32(pv->size);
        serial_puts(" dono ");
        serial_hex32(pv->owner);
        serial_puts(pv->free ? " livre" : " ocupado");
      }
    } else {
      serial_puts(" (cabeca da lista)");
    }
    /* cabecalho em x e o bloco que fisicamente cobre x-1 (andando a arena
     * desde o inicio: o prev de x pode estar estragado) */
    if (badx >= a->start && badx < a->end) {
      const uint32_t* hw = (const uint32_t*)(uintptr_t)badx;
      struct mm_hdr* h = blk_at(a->start);
      struct mm_hdr* last = 0;
      serial_puts(" | em x: ");
      serial_hex32(hw[0]);
      serial_puts(" ");
      serial_hex32(hw[1]);
      serial_puts(" ");
      serial_hex32(hw[2]);
      serial_puts(" ");
      serial_hex32(hw[3]);
      while (range_ok(a, h) && (uint32_t)(uintptr_t)h < badx) {
        last = h;
        h = blk_at((uint32_t)(uintptr_t)h + h->size);
      }
      if (last) {
        serial_puts(" | bloco antes ");
        serial_hex32((uint32_t)(uintptr_t)last);
        serial_puts(" tam ");
        serial_hex32(last->size);
        serial_puts(" dono ");
        serial_hex32(last->owner);
        serial_puts(last->free ? " livre" : " ocupado");
        serial_puts((uint32_t)(uintptr_t)last + last->size == badx ? " (encostado)" : " (NAO encosta: x no meio)");
      }
    }
    serial_puts("\n");
    arena_rebuild(a);
  }
  return 0;
}

static void* arena_alloc(struct arena* a, uint32_t nbytes, uint32_t owner) {
  struct mm_hdr* h;
  uint32_t need;
  if (!a->ready || nbytes == 0) return 0;
  need = blk_need(nbytes);
  if (!need) return 0;
  h = find_fit(a, need);
  if (!h) return 0;
  fl_remove(a, h);
  h->free = 0;
  h->owner = owner;
  split(a, h, need);
  return (uint8_t*)h + sizeof(struct mm_hdr);
}

static struct mm_hdr* arena_hdr(const struct arena* a, void* ptr) {
  uint32_t p;
  struct mm_hdr* h;
  if (!a->ready || !ptr) return 0;
  p = (uint32_t)ptr - (uint32_t)sizeof(struct mm_hdr);
  if (p < a->start || p >= a->end) return 0;
  h = blk_at(p);
  if (!range_ok(a, h) || h->free) return 0;
  return h;
}

static void arena_free(struct arena* a, void* ptr) {
  struct mm_hdr* h = arena_hdr(a, ptr);
  if (!h) return;
  coalesce(a, h);
}

static void arena_stats(const struct arena* a, uint32_t* total, uint32_t* used,
                        uint32_t* free_bytes, uint32_t* largest) {
  struct mm_hdr* h;
  uint32_t t = 0, u = 0, f = 0, big = 0;
  if (a->ready) {
    t = a->end - a->start;
    h = blk_at(a->start);
    while (range_ok(a, h)) {
      if (h->free) {
        f += h->size;
        if (h->size > big) big = h->size;
      } else {
        u += h->size;
      }
      h = blk_at((uint32_t)h + h->size);
    }
  }
  if (total) *total = t;
  if (used) *used = u;
  if (free_bytes) *free_bytes = f;
  if (largest) *largest = big;
}

void* kmalloc(uint32_t nbytes) { return arena_alloc(&low, nbytes, 0); }
void kfree(void* ptr) { arena_free(&low, ptr); }

void mm_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes, uint32_t* largest) {
  arena_stats(&low, total, used, free_bytes, largest);
}

int um_ready(void) { return high.ready; }
uint32_t um_start(void) { return high.start; }
uint32_t um_end(void) { return high.end; }

void um_set_owner(uint32_t owner) { cur_owner = owner; }
uint32_t um_owner(void) { return cur_owner; }

void* umalloc(uint32_t nbytes) { return arena_alloc(&high, nbytes, cur_owner); }
void* umalloc_owned(uint32_t nbytes, uint32_t owner) { return arena_alloc(&high, nbytes, owner); }

/* Do fim do ultimo bloco livre que cabe: caches do kernel que ficam (buffers
 * de soquete, cache de pastas do FAT) nao picam o comeco da arena, onde os
 * programas pedem blocos grandes e contiguos (o JIT do Firefox reserva 2 GB). */
void* umalloc_top(uint32_t nbytes) {
  struct arena* a = &high;
  struct mm_hdr* best = 0;
  uint32_t need;
  int b;
  if (!a->ready || nbytes == 0) return 0;
  need = blk_need(nbytes);
  if (!need) return 0;
  if (!find_fit(a, need)) return 0;  /* valida as listas */
  for (b = bin_of(need); b < MM_NBIN; b++) {
    uint32_t x;
    for (x = a->bin[b]; x; x = fb(x)->next)
      if (blk_at(x)->size >= need && blk_at(x) > best) best = blk_at(x);
  }
  if (!best) return 0;
  fl_remove(a, best);
  if (best->size - need >= MM_MIN_BLK) {
    struct mm_hdr* top = blk_at((uint32_t)best + best->size - need);
    best->size -= need;
    top->size = need;
    top->free = 0;
    top->owner = 0;
    top->prev = best->size;
    fix_next_prev(a, top);
    fl_insert(a, best);
    return (uint8_t*)top + sizeof(struct mm_hdr);
  }
  best->free = 0;
  best->owner = 0;
  return (uint8_t*)best + sizeof(struct mm_hdr);
}
/* Bloco cujo endereco util e multiplo de align (potencia de 2, >= 16). O
 * mmap anonimo do port do Firefox pedia len + 2 MB para alinhar sozinho: um
 * chunk de 1 MB do GC custava 3 MB e o heap acabava (OOM no Nursery). Aqui a
 * folga antes do endereco alinhado volta a ser um bloco livre. */
void* umalloc_aligned(uint32_t nbytes, uint32_t align) {
  struct arena* a = &high;
  uint32_t need;
  const uint32_t hs = (uint32_t)sizeof(struct mm_hdr);
  int b;
  if (align <= MM_ALIGN) return umalloc(nbytes);
  if (!a->ready || nbytes == 0 || (align & (align - 1u))) return 0;
  need = blk_need(nbytes);
  if (!need) return 0;
  if (!find_fit(a, need)) return 0;  /* valida as listas */
  for (b = bin_of(need); b < MM_NBIN; b++) {
    uint32_t x;
    for (x = a->bin[b]; x; x = fb(x)->next) {
      struct mm_hdr* h = blk_at(x);
      uint32_t start = x;
      uint32_t end = start + h->size;
      uint32_t p;
      struct mm_hdr* n;
      if (h->size < need) continue;
      p = (start + hs + align - 1u) & ~(align - 1u);
      /* A folga da frente precisa caber como bloco livre valido. */
      if (p - hs != start && p - hs - start < MM_MIN_BLK) p += align;
      if (!(p > start && p - hs >= start && p - hs <= end && end - (p - hs) >= need)) continue;
      fl_remove(a, h);
      n = blk_at(p - hs);
      if ((uint32_t)n != start) {
        h->size = (uint32_t)n - start; /* cabeca continua livre */
        n->size = end - (uint32_t)n;
        n->prev = h->size;
        fl_insert(a, h);
      }
      n->free = 0;
      n->owner = cur_owner;
      split(a, n, need);
      return (uint8_t*)n + hs;
    }
  }
  return 0;
}

/* Bloco num endereco escolhido (addr multiplo de 16), se a faixa estiver
 * livre. Para a pagina KUSER_SHARED_DATA dos programas Windows (wine.c). */
void* umalloc_at(uint32_t addr, uint32_t nbytes, uint32_t owner) {
  struct arena* a = &high;
  const uint32_t hs = (uint32_t)sizeof(struct mm_hdr);
  uint32_t need;
  int b;
  if (!a->ready || nbytes == 0 || (addr & (MM_ALIGN - 1u))) return 0;
  if (addr - hs < a->start || addr >= a->end) return 0;
  need = blk_need(nbytes);
  if (!need) return 0;
  if (!find_fit(a, MM_MIN_BLK)) return 0;  /* valida as listas */
  for (b = 0; b < MM_NBIN; b++) {
    uint32_t x;
    for (x = a->bin[b]; x; x = fb(x)->next) {
      struct mm_hdr* h = blk_at(x);
      uint32_t start = x, end = start + h->size;
      struct mm_hdr* n;
      if (addr - hs < start || addr - hs + need > end) continue;
      /* a folga da frente precisa caber como bloco livre valido */
      if (addr - hs != start && addr - hs - start < MM_MIN_BLK) return 0;
      fl_remove(a, h);
      n = blk_at(addr - hs);
      if ((uint32_t)n != start) {
        h->size = (uint32_t)n - start;
        n->size = end - (uint32_t)n;
        n->prev = h->size;
        fl_insert(a, h);
      }
      n->free = 0;
      n->owner = owner;
      split(a, n, need);
      return (uint8_t*)n + hs;
    }
  }
  return 0;
}

void ufree(void* ptr) { arena_free(&high, ptr); }

uint32_t usize(void* ptr) {
  struct mm_hdr* h = arena_hdr(&high, ptr);
  if (!h) return 0;
  return h->size - (uint32_t)sizeof(struct mm_hdr);
}

void* urealloc(void* ptr, uint32_t nbytes) {
  uint32_t old, need;
  uint8_t* n;
  uint32_t i;
  struct mm_hdr* h;
  struct mm_hdr* nx;
  if (!ptr) return umalloc(nbytes);
  if (nbytes == 0) {
    ufree(ptr);
    return 0;
  }
  h = arena_hdr(&high, ptr);
  if (!h) return 0;
  old = h->size - (uint32_t)sizeof(struct mm_hdr);
  if (old >= nbytes) return ptr;
  need = blk_need(nbytes);
  if (!need) return 0;
  /* cresce no lugar se o vizinho livre cobre */
  nx = next_blk(&high, h);
  if (nx && range_ok(&high, nx) && nx->free && h->size + nx->size >= need) {
    fl_remove(&high, nx);
    h->size += nx->size;
    split(&high, h, need);
    return ptr;
  }
  n = (uint8_t*)arena_alloc(&high, nbytes, h->owner);
  if (!n) return 0;
  for (i = 0; i < old; i++) n[i] = ((uint8_t*)ptr)[i];
  arena_free(&high, ptr);
  return n;
}

void ufree_owner(uint32_t owner) {
  struct mm_hdr* h;
  int any = 0;
  if (!high.ready || owner == 0) return;
  h = blk_at(high.start);
  while (range_ok(&high, h)) {
    if (!h->free && h->owner == owner) {
      h->free = 1;
      h->owner = 0;
      any = 1;
    }
    h = blk_at((uint32_t)h + h->size);
  }
  if (any) arena_rebuild(&high);
}

void um_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes, uint32_t* largest) {
  arena_stats(&high, total, used, free_bytes, largest);
}

static void arena_walk(const struct arena* a, int area, mm_walk_fn fn, void* arg) {
  struct mm_hdr* h;
  if (!a->ready) return;
  h = blk_at(a->start);
  while (range_ok(a, h)) {
    fn(arg, area, (uint32_t)h, h->size, !h->free, h->free ? 0 : h->owner);
    h = blk_at((uint32_t)h + h->size);
  }
}

void mm_walk(mm_walk_fn fn, void* arg) {
  uint32_t i;
  if (!fn) return;
  arena_walk(&low, 0, fn, arg);
  arena_walk(&high, 1, fn, arg);
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)a->start;
    while (hm_ok(a, h)) {
      fn(arg, 2, (uint64_t)(uintptr_t)h, h->size, !h->free, 0);
      h = (struct hm_hdr*)((uint8_t*)h + h->size);
    }
  }
}

/* Copia o que ha em stop (o cabecalho que parou o percurso), sem passar do fim. */
static void chk_raw(struct mm_chk* c) {
  uint64_t room = c->end - c->stop;
  int i;
  c->bad_len = room < sizeof(c->bad_raw) ? (int)room : (int)sizeof(c->bad_raw);
  for (i = 0; i < c->bad_len; i++) c->bad_raw[i] = ((const uint8_t*)(uintptr_t)c->stop)[i];
}

int mm_check(int area, struct mm_chk* c) {
  uint32_t i;
  if (!c) return 0;
  for (i = 0; i < sizeof(*c); i++) ((uint8_t*)c)[i] = 0;
  if (area == 0 || area == 1) {
    const struct arena* a = area ? &high : &low;
    struct mm_hdr* h;
    if (!a->ready) return 0;
    c->start = a->start;
    c->end = a->end;
    h = blk_at(a->start);
    while (range_ok(a, h)) {
      c->blocks++;
      c->last = (uint32_t)(uintptr_t)h;
      c->last_size = h->size;
      c->last_free = h->free;
      c->last_owner = h->owner;
      h = blk_at((uint32_t)(uintptr_t)h + h->size);
    }
    c->stop = (uint32_t)(uintptr_t)h;
    if (c->stop >= c->end) return 1;
    chk_raw(c);
    if (c->bad_len >= (int)sizeof(struct mm_hdr)) {
      c->bad_size = h->size;
      c->bad_free = h->free;
      c->bad_owner = h->owner;
    }
    return -1;
  }
  if (area < 2 || (uint32_t)(area - 2) >= hm_count) return 0;
  {
    const struct harena* a = &hm_arenas[area - 2];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)a->start;
    c->start = a->start;
    c->end = a->end;
    while (hm_ok(a, h)) {
      c->blocks++;
      c->last = (uint64_t)(uintptr_t)h;
      c->last_size = h->size;
      c->last_free = h->free;
      c->last_owner = h->owner;
      h = (struct hm_hdr*)((uint8_t*)h + h->size);
    }
    c->stop = (uint64_t)(uintptr_t)h;
    if (c->stop >= c->end) return 1;
    chk_raw(c);
    if (c->bad_len >= (int)sizeof(struct hm_hdr)) {
      c->bad_size = h->size;
      c->bad_free = h->free;
      c->bad_owner = h->owner;
    }
    return -1;
  }
}

uint32_t mm_kernel_end(void) { return kernel_reserved_end(bootinfo_get()); }

/* Marca [phys, phys+size) como write-combining (PAT 1: PWT=1, PCD=0, que o
 * kernel_entry programa no MSR do PAT). Para o LFB que o WIN acha depois do
 * boot (Bochs VBE do QEMU/VirtualBox em 0xFD000000): o mapeamento padrao e
 * write-back, mas o MTRR do SeaBIOS deixa o buraco PCI como UC, e UC vence WB.
 * Com o PAT WC, a copia do quadro deixa de ser uncached (era 40-200 MB/s).
 * So mexe em paginas de 2 MB ja presentes; devolve quantas mudou. */
int mm_map_wc(uint64_t phys, uint64_t size) {
  uint64_t cr3, a, end = phys + size;
  uint64_t* pml4;
  uint64_t* pdpt;
  int n = 0;
  if (!size) return 0;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  pml4 = (uint64_t*)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ULL);
  if (!(pml4[0] & 1u)) return 0;
  pdpt = (uint64_t*)(uintptr_t)(pml4[0] & 0x000FFFFFFFFFF000ULL);
  for (a = phys & ~(HM_PAGE - 1u); a < end; a += HM_PAGE) {
    uint32_t gb = (uint32_t)(a / HM_GB);
    uint64_t* pd;
    uint64_t* e;
    if (gb >= 512 || !(pdpt[gb] & 1u) || (pdpt[gb] & 0x80u)) continue;
    pd = (uint64_t*)(uintptr_t)(pdpt[gb] & 0x000FFFFFFFFFF000ULL);
    e = &pd[(a % HM_GB) / HM_PAGE];
    if ((*e & 0x81u) != 0x81u) continue;           /* presente e de 2 MB */
    *e = (*e & ~(0x10ULL | 0x1000ULL)) | 0x08ULL;  /* PCD=0, PAT=0, PWT=1 */
    n++;
  }
  if (n) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    kt_tlb_shootdown();   /* as outras CPUs tambem tinham o mapeamento velho */
  }
  return n;
}

/* ---- Uma pagina de 4 KB "emprestada" num endereco fixo ----
 * O kernel mapeia tudo na identidade com paginas de 2 MB/1 GB. A pagina
 * KUSER_SHARED_DATA dos programas Windows tem endereco fixo (0x7FFE0000) e,
 * num PC com UEFI, esse endereco pode ser do firmware ou estar fora do heap.
 * Aqui a pagina de 2 MB (ou 1 GB) que o contem vira uma tabela de paginas de
 * 4 KB, igual a identidade, menos `va`, que passa a apontar para `pa`.
 * Um alias por vez; mm_alias_undo devolve o mapeamento original. */
static uint64_t alias_pt[512] __attribute__((aligned(4096)));
static uint64_t alias_pd[512] __attribute__((aligned(4096)));
static uint64_t* alias_pdpt_e;   /* entrada do PDPT trocada (pagina de 1 GB) */
static uint64_t alias_pdpt_old;
static uint64_t* alias_pd_e;     /* entrada do PD trocada */
static uint64_t alias_pd_old;
static uint64_t* alias_pt_e;     /* entrada de 4 KB trocada (ja havia tabela) */
static uint64_t alias_pt_old;
#define PG_ADDR 0x000FFFFFFFFFF000ULL
#define PG_NX (1ULL << 63)

static void alias_flush(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
  kt_tlb_shootdown();
}

int mm_alias_page(uint64_t va, uint64_t pa) {
  uint64_t cr3, *pml4, *pdpt, *pd, pde;
  uint32_t gi = (uint32_t)(va >> 30), pi = (uint32_t)((va >> 21) & 511), ti = (uint32_t)((va >> 12) & 511);
  uint32_t i;
  if (alias_pd_e || alias_pt_e || va >= (512ULL << 30) || (va & 0xFFF) || (pa & 0xFFF)) return 0;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  pml4 = (uint64_t*)(uintptr_t)(cr3 & PG_ADDR);
  if (!(pml4[0] & 1u)) return 0;
  pdpt = (uint64_t*)(uintptr_t)(pml4[0] & PG_ADDR);
  if (!(pdpt[gi] & 1u)) return 0;
  if (pdpt[gi] & 0x80u) { /* 1 GB: vira 512 paginas de 2 MB */
    uint64_t e = pdpt[gi], base = e & 0x000FFFFFC0000000ULL;
    uint64_t fl = (e & 0x11Fu) | (e & 0x1000u) | (e & PG_NX) | 0x80u;
    for (i = 0; i < 512; i++) alias_pd[i] = (base + ((uint64_t)i << 21)) | fl;
    alias_pdpt_e = &pdpt[gi];
    alias_pdpt_old = e;
    pdpt[gi] = (uint64_t)(uintptr_t)alias_pd | (e & 7u);
  }
  pd = (uint64_t*)(uintptr_t)(pdpt[gi] & PG_ADDR);
  pde = pd[pi];
  if (!(pde & 1u)) {
    if (alias_pdpt_e) {
      *alias_pdpt_e = alias_pdpt_old;
      alias_pdpt_e = 0;
    }
    return 0;
  }
  if (!(pde & 0x80u)) { /* ja e tabela de 4 KB: troca so a entrada */
    uint64_t* pt = (uint64_t*)(uintptr_t)(pde & PG_ADDR);
    alias_pt_e = &pt[ti];
    alias_pt_old = pt[ti];
    pt[ti] = pa | 3u | (pt[ti] & (0x100u | PG_NX));
  } else {
    uint64_t base = pde & 0x000FFFFFFFE00000ULL;
    uint64_t fl = (pde & 0x11Fu) | ((pde & 0x1000u) ? 0x80u : 0) | (pde & PG_NX);
    for (i = 0; i < 512; i++) alias_pt[i] = (base + ((uint64_t)i << 12)) | fl;
    alias_pt[ti] = pa | 3u | (pde & PG_NX); /* write-back, presente, escrita */
    alias_pd_e = &pd[pi];
    alias_pd_old = pde;
    pd[pi] = (uint64_t)(uintptr_t)alias_pt | (pde & 7u);
  }
  alias_flush();
  return 1;
}

void mm_alias_undo(void) {
  if (alias_pt_e) {
    *alias_pt_e = alias_pt_old;
    alias_pt_e = 0;
  }
  if (alias_pd_e) {
    *alias_pd_e = alias_pd_old;
    alias_pd_e = 0;
  }
  if (alias_pdpt_e) {
    *alias_pdpt_e = alias_pdpt_old;
    alias_pdpt_e = 0;
  }
  alias_flush();
}

/* ---- Pagina 0 sem mapeamento enquanto roda programa do Windows ----
 * No Windows os primeiros 64 KB nao existem: ponteiro NULL falha na hora. Aqui
 * o endereco 0 e RAM (tabela de interrupcoes da BIOS) e um NULL lido ou
 * escrito por engano vira lixo ou estraga memoria. Com o guarda ligado, os
 * primeiros 4 KB ficam fora do mapa (a pagina de 2 MB/1 GB que os contem vira
 * tabela de 4 KB) e o acesso vira falta, entregue ao programa. O kernel so le
 * essa area durante o boot (EBDA em smp.c). */
static uint64_t null_pt[512] __attribute__((aligned(4096)));
static uint64_t null_pd[512] __attribute__((aligned(4096)));
static uint64_t null_pdpt_old, null_pd_old, null_pt_old;
static uint64_t* null_pt_e;
static int null_on, null_split_pdpt, null_split_pd;

void mm_null_guard(int on) {
  uint64_t cr3, *pml4, *pdpt, *pd;
  uint32_t i;
  if (on == null_on) return;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  pml4 = (uint64_t*)(uintptr_t)(cr3 & PG_ADDR);
  if (!(pml4[0] & 1u)) return;
  pdpt = (uint64_t*)(uintptr_t)(pml4[0] & PG_ADDR);
  if (on) {
    if (!(pdpt[0] & 1u)) return;
    null_split_pdpt = null_split_pd = 0;
    null_pt_e = 0;
    if (pdpt[0] & 0x80u) { /* 1 GB -> 512 x 2 MB */
      uint64_t e = pdpt[0], fl = (e & 0x11Fu) | (e & 0x1000u) | (e & PG_NX) | 0x80u;
      for (i = 0; i < 512; i++) null_pd[i] = ((e & 0x000FFFFFC0000000ULL) + ((uint64_t)i << 21)) | fl;
      null_pdpt_old = e;
      pdpt[0] = (uint64_t)(uintptr_t)null_pd | (e & 7u);
      null_split_pdpt = 1;
    }
    pd = (uint64_t*)(uintptr_t)(pdpt[0] & PG_ADDR);
    if (pd[0] & 0x80u) { /* 2 MB -> 512 x 4 KB, a primeira ausente */
      uint64_t e = pd[0], fl = (e & 0x11Fu) | ((e & 0x1000u) ? 0x80u : 0) | (e & PG_NX);
      for (i = 0; i < 512; i++) null_pt[i] = ((e & 0x000FFFFFFFE00000ULL) + ((uint64_t)i << 12)) | fl;
      null_pt[0] = 0;
      null_pd_old = e;
      pd[0] = (uint64_t)(uintptr_t)null_pt | (e & 7u);
      null_split_pd = 1;
    } else if (pd[0] & 1u) { /* ja e tabela de 4 KB */
      uint64_t* pt = (uint64_t*)(uintptr_t)(pd[0] & PG_ADDR);
      null_pt_e = &pt[0];
      null_pt_old = pt[0];
      pt[0] = 0;
    }
  } else {
    pd = (uint64_t*)(uintptr_t)(pdpt[0] & PG_ADDR);
    if (null_pt_e) *null_pt_e = null_pt_old;
    if (null_split_pd) pd[0] = null_pd_old;
    if (null_split_pdpt) pdpt[0] = null_pdpt_old;
    null_pt_e = 0;
    null_split_pd = null_split_pdpt = 0;
  }
  null_on = on;
  alias_flush();
}
