/*
 * wine.c - Lado do kernel dos EXEs do Windows (ver wine.h).
 *
 * Desligado (padrao): nenhum DLL carregado; wine_entry recusa com a mensagem
 * "SET WINE=1". Ligado: o WINE.DLL vem no primeiro EXE do Windows e fica.
 * SET WINE=0 descarrega na hora se nenhum EXE do Windows esta rodando, senao
 * quando o ultimo terminar (wine_exec_done).
 *
 * Copias: normalmente uma so. TROCA WINE (TROCA.EXE, troca.c) carrega outra
 * sem reiniciar: os EXEs do Windows abertos dali em diante usam a nova, os
 * que ja rodam continuam com a deles. O estado de processos, handles e
 * pedidos de spawn e de cada copia, entao um filho de um EXE do Windows nasce
 * na copia do pai: aninhado (o pai esta na cadeia do mod_exec) ou em paralelo
 * (k_win_launch_arg "--wine-spawn=N": wine_note_spawn guarda a copia de quem
 * pediu). Uma copia que deixou de ser a atual sai da memoria com o ultimo EXE.
 */
#include <stdint.h>
#include "wine.h"
#include "drives.h"
#include "env.h"
#include "module.h"
#include "serial.h"
#include "mm.h"
#include "irq.h"

void mod_set_err(const char* a, const char* b);
int kexp_date(void);
int kexp_time(void);

volatile uint8_t* wine_kuser;
static uint64_t kuser_epoch;   /* FILETIME quando irq_ticks era 0 */

/* KSYSTEM_TIME {low, high1, high2}: quem le repete ate high1 == high2. */
static void ksystime(volatile uint8_t* at, uint64_t v) {
  volatile uint32_t* k = (volatile uint32_t*)at;
  k[2] = (uint32_t)(v >> 32);
  k[0] = (uint32_t)v;
  k[1] = (uint32_t)(v >> 32);
}

void wine_tick(uint64_t ms) {
  volatile uint8_t* k = wine_kuser;
  uint64_t it = ms * 10000ull;   /* unidades de 100 ns */
  if (!k) return;
  ksystime(k + 0x08, it);                /* InterruptTime */
  ksystime(k + 0x14, kuser_epoch + it);  /* SystemTime */
  ksystime(k + 0x320, ms);               /* TickCount (multiplicador 1) */
}

static uint64_t days_from_civil(int y, unsigned m, unsigned d) {
  int era;
  unsigned yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint64_t)(era * 146097 + (int)doe - 719468);
}

static void* kuser_raw;        /* bloco do heap quando a pagina e um alias */

static void kuser_on(void) {
  uint8_t* k;
  int d, t, i;
  uint64_t secs;
  const char* force;
  if (wine_kuser) return;
  force = env_get("WINE_KUSERMAP"); /* teste: forca o alias mesmo com a RAM livre */
  k = (force && force[0] == '1') ? 0 : (uint8_t*)umalloc_at(WINE_KUSER_ADDR, 0x1000, 0);
  if (!k) {
    /* Num PC com UEFI o endereco pode ser do firmware ou estar fora do heap:
     * uma pagina nossa passa a aparecer em 0x7FFE0000 (tabela de paginas). */
    uint8_t* raw = (uint8_t*)umalloc_owned(0x2000, 0); /* do kernel: dura ate o kuser_off */
    uint8_t* pg = raw ? (uint8_t*)(((uintptr_t)raw + 0xFFFu) & ~(uintptr_t)0xFFFu) : 0;
    if (!pg || !mm_alias_page(WINE_KUSER_ADDR, (uint64_t)(uintptr_t)pg)) {
      if (raw) ufree(raw);
      serial_puts("[wine] sem a pagina 0x7FFE0000 (KUSER_SHARED_DATA)\n");
      return;
    }
    kuser_raw = raw;
    k = pg;
    serial_puts("[wine] KUSER_SHARED_DATA mapeada em 0x7FFE0000 (pagina do heap)\n");
  }
  for (i = 0; i < 0x1000; i++) k[i] = 0;
  d = kexp_date();
  t = kexp_time();
  if (d <= 0) d = 20260101;
  if (t < 0) t = 0;
  secs = days_from_civil(d / 10000, (unsigned)(d / 100 % 100), (unsigned)(d % 100)) * 86400u +
         (uint64_t)(t / 10000) * 3600u + (uint64_t)(t / 100 % 100) * 60u + (uint64_t)(t % 100);
  kuser_epoch = (secs + 11644473600ull) * 10000000ull - irq_ticks() * 10000ull;
  *(uint32_t*)(k + 0x04) = 1u << 24;     /* TickCountMultiplier */
  *(uint32_t*)(k + 0x260) = 19045;       /* NtBuildNumber */
  *(uint32_t*)(k + 0x264) = 1;           /* NtProductType: WinNt */
  *(uint32_t*)(k + 0x26C) = 10;          /* NtMajorVersion */
  *(uint32_t*)(k + 0x270) = 0;           /* NtMinorVersion */
  *(uint32_t*)(k + 0x3C0) = 1;           /* ActiveProcessorCount */
  wine_kuser = k;
  mm_null_guard(1); /* NULL falha, como no Windows */
  wine_tick(irq_ticks());
}

static void kuser_off(void) {
  uint8_t* k = (uint8_t*)wine_kuser;
  if (!k) return;
  wine_kuser = 0;
  mm_null_guard(0);
  if (kuser_raw) {
    mm_alias_undo();
    ufree(kuser_raw);
    kuser_raw = 0;
  } else {
    ufree(k);
  }
}

/* ---------- faixa baixa para imagens de base fixa ---------- */
#define LOW_MAX 8
static struct {
  uint32_t addr, size, pid;
} low[LOW_MAX];
static int low_n;

void* wine_low_alloc(uint32_t addr, uint32_t size) {
  uint32_t pid = mod_current_pid();
  uint32_t i;
  int slot = -1, k;
  if (!pid || !size || (addr & 0xFFFu)) return 0;
  size = (size + 0xFFFu) & ~0xFFFu;
  if (addr < WINE_LOW_START || addr >= WINE_LOW_END || size > WINE_LOW_END - addr) return 0;
  for (k = 0; k < LOW_MAX; k++) {
    if (!low[k].size) {
      if (slot < 0) slot = k;
      continue;
    }
    if (addr < low[k].addr + low[k].size && low[k].addr < addr + size) return 0;   /* ocupado */
  }
  if (slot < 0) return 0;
  if (!low_n && gfx_back_move(1) != 0) return 0;
  low[slot].addr = addr;
  low[slot].size = size;
  low[slot].pid = pid;
  low_n++;
  for (i = 0; i < size; i++) ((uint8_t*)(uintptr_t)addr)[i] = 0;
  return (void*)(uintptr_t)addr;
}

void wine_low_release_pid(uint32_t pid) {
  int k;
  if (!low_n) return;
  for (k = 0; k < LOW_MAX; k++)
    if (low[k].size && low[k].pid == pid) {
      low[k].size = 0;
      low_n--;
    }
  if (!low_n) gfx_back_move(0);
}

/* ---------- copias do WINE.DLL ---------- */
#define WI_MAX 8
static struct {
  int handle;                          /* mod_dll_* (0 = slot livre) */
  const struct wine_ops* ops;
  int busy;                            /* EXEs do Windows rodando nesta copia */
  uint32_t num;                        /* 1, 2, ... na ordem de carga desde o boot */
  char path[WINE_PATH_MAX];
} wi[WI_MAX];
static int wi_cur = -1;                /* copia dos proximos EXEs de fora */
static uint32_t wi_num;
static int w_busy;                     /* EXEs do Windows rodando agora (todas as copias) */

/* pid do SODOS -> copia: o fim do processo e os filhos dele */
#define WP_MAX 1024
static struct {
  uint32_t pid;
  int inst;
} wp_tab[WP_MAX];

/* Filho pedido em paralelo: sobe como tarefa nova do WIN, sem o pai na cadeia
 * do mod_exec. A copia de quem pediu fica aqui ate ele comecar (60 s). */
#define WS_MAX 32
static struct {
  char arg[32];
  int inst;                            /* -1 = livre */
  uint32_t ms;
} ws_tab[WS_MAX];
static int ws_init;

static int wanted(void) {
  const char* v = env_get("WINE");
  return v && v[0] && !(v[0] == '0' && v[1] == 0);
}

int wine_loaded(void) {
  int k;
  for (k = 0; k < WI_MAX; k++)
    if (wi[k].ops) return 1;
  return 0;
}

/* REBOOT (version.c): o WINE.DLL lista na serial o que cada thread do Windows
 * esta esperando ("[wine-dump]"), para achar travamentos depois do boot. */
void wine_dump_waits(void) {
  int k;
  for (k = 0; k < WI_MAX; k++) {
    void (*fn)(void);
    if (!wi[k].handle || !wi[k].ops) continue;
    fn = (void (*)(void))mod_dll_sym(wi[k].handle, "wine_dump");
    if (!fn) {
      serial_puts("[wine-dump] este WINE.DLL nao tem o wine_dump\n");
      continue;
    }
    fn();
  }
}

/* Fim de um processo (normal ou KILL/Ctrl+Backspace, module.c exec_release):
 * o WINE.DLL esquece as threads dele. Sem isso o registro de esperas guardava
 * ponteiros para o wproc ja solto e o wine_dump do REBOOT dava #GP. Vai para
 * todas as copias: cada uma ignora o pid que nao e dela. */
void wine_pid_gone(uint32_t pid) {
  int k;
  for (k = 0; k < WI_MAX; k++) {
    void (*fn)(uint32_t);
    if (!wi[k].handle || !wi[k].ops) continue;
    fn = (void (*)(uint32_t))mod_dll_sym(wi[k].handle, "wine_pid_gone");
    if (fn) fn(pid);
  }
}

static void sdec(uint32_t v) {
  char b[12];
  int i = 0;
  do b[i++] = (char)('0' + v % 10u); while ((v /= 10u) && i < 11);
  while (i) serial_putc(b[--i]);
}

static void unload_inst(int k) {
  if (k < 0 || k >= WI_MAX || !wi[k].handle) return;
  mod_dll_free(wi[k].handle);
  serial_puts("[wine] WINE.DLL descarregado (copia ");
  sdec(wi[k].num);
  serial_puts(")\n");
  wi[k].handle = 0;
  wi[k].ops = 0;
  wi[k].busy = 0;
  wi[k].path[0] = 0;
  if (wi_cur == k) wi_cur = -1;
}

static void unload_all(void) {
  int k;
  for (k = 0; k < WI_MAX; k++) unload_inst(k);
  wi_cur = -1;
}

static void scopy(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

/* Carrega path numa copia nova. Devolve o slot ou -1. */
static int try_load(const char* path) {
  int h, k;
  wine_entry_fn fn;
  const struct wine_ops* ops;
  if (file_size(path) <= 0) return -1;
  for (k = 0; k < WI_MAX; k++)
    if (!wi[k].handle) break;
  if (k >= WI_MAX) {
    mod_set_err("copias do WINE.DLL demais (feche os EXEs do Windows antigos)", 0);
    return -1;
  }
  h = mod_dll_load_sys(path);
  if (!h) {
    serial_puts("[wine] ");
    serial_puts(path);
    serial_puts(": ");
    serial_puts(mod_last_error());
    serial_puts("\n");
    return -1;
  }
  fn = (wine_entry_fn)mod_dll_sym(h, WINE_SYM);
  ops = fn ? fn() : 0;
  if (!ops || ops->abi != WINE_ABI || !ops->start) {
    serial_puts("[wine] ");
    serial_puts(path);
    serial_puts(": nao e o WINE.DLL desta versao\n");
    mod_set_err("nao e o WINE.DLL desta versao: ", path);
    mod_dll_free(h);
    return -1;
  }
  wi[k].handle = h;
  wi[k].ops = ops;
  wi[k].busy = 0;
  wi[k].num = ++wi_num;
  scopy(wi[k].path, path, (int)sizeof(wi[k].path));
  serial_puts("[wine] ");
  serial_puts(path);
  serial_puts(" carregado (copia ");
  sdec(wi[k].num);
  serial_puts(")\n");
  return k;
}

static int load(const char* path) {
  char p[WINE_PATH_MAX];
  int k;
  if (path) return try_load(path);
  env_sodos_path("DRV\\WINE.DLL", p, (int)sizeof(p));
  if ((k = try_load(p)) >= 0) return k;
  if (drives_is_mounted('D') && (k = try_load("D:\\SODOS\\DRV\\WINE.DLL")) >= 0) return k;
  if (drives_is_mounted('Z') && (k = try_load("Z:\\SODOS\\DRV\\WINE.DLL")) >= 0) return k;
  return -1;
}

static int pid_inst(uint32_t pid) {
  int i;
  if (!pid) return -1;
  for (i = 0; i < WP_MAX; i++)
    if (wp_tab[i].pid == pid) return wp_tab[i].inst;
  return -1;
}

static int pid_add(uint32_t pid, int k) {
  int i;
  for (i = 0; i < WP_MAX; i++)
    if (!wp_tab[i].pid) {
      wp_tab[i].pid = pid;
      wp_tab[i].inst = k;
      return 1;
    }
  return 0;
}

static int pid_take(uint32_t pid) {
  int i, k;
  if (!pid) return -1;
  for (i = 0; i < WP_MAX; i++)
    if (wp_tab[i].pid == pid) {
      k = wp_tab[i].inst;
      wp_tab[i].pid = 0;
      return k;
    }
  return -1;
}

static int str_eq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void ws_clear(void) {
  int i;
  for (i = 0; i < WS_MAX; i++) ws_tab[i].inst = -1;
  ws_init = 1;
}

/* kexports.c, k_win_launch_arg: um EXE do Windows pediu um filho em paralelo. */
void wine_note_spawn(const char* arg) {
  static const char pre[] = "--wine-spawn=";
  uint32_t now = (uint32_t)irq_ticks();
  int k = pid_inst(mod_current_pid()), i, slot = -1;
  if (k < 0 || !arg) return;
  for (i = 0; pre[i]; i++)
    if (arg[i] != pre[i]) return;
  if (!ws_init) ws_clear();
  for (i = 0; i < WS_MAX; i++) {
    if (ws_tab[i].inst < 0 || now - ws_tab[i].ms > 60000u) {
      slot = i;
      break;
    }
    if (slot < 0 || (int32_t)(ws_tab[i].ms - ws_tab[slot].ms) < 0) slot = i;   /* o mais velho */
  }
  scopy(ws_tab[slot].arg, arg, (int)sizeof(ws_tab[slot].arg));
  ws_tab[slot].inst = k;
  ws_tab[slot].ms = now;
}

static int spawn_take(const char* arg) {
  uint32_t now = (uint32_t)irq_ticks();
  int i, k;
  if (!ws_init || !arg || arg[0] != '-') return -1;
  for (i = 0; i < WS_MAX; i++)
    if (ws_tab[i].inst >= 0 && now - ws_tab[i].ms <= 60000u && str_eq(ws_tab[i].arg, arg)) {
      k = ws_tab[i].inst;
      ws_tab[i].inst = -1;
      return (k >= 0 && k < WI_MAX && wi[k].ops) ? k : -1;
    }
  return -1;
}

void* wine_entry(void) { return wine_entry_from(0, 0, 0); }

void* wine_entry_from(uint32_t parent, int argc, char** argv) {
  uint32_t pid = mod_current_pid();
  int k = -1;
  if (!wanted()) {
    mod_set_err("programa do Windows: ligue com SET WINE=1", 0);
    return 0;
  }
  if (argc >= 2 && argv && argv[1]) k = spawn_take(argv[1]);
  if (k < 0 && parent) k = pid_inst(parent);
  if (k < 0 || k >= WI_MAX || !wi[k].ops) {
    if (wi_cur < 0 || !wi[wi_cur].ops) wi_cur = load(0);
    if (wi_cur < 0) {
      char p[WINE_PATH_MAX];
      env_sodos_path("DRV", p, (int)sizeof(p));
      mod_set_err("WINE.DLL nao encontrado em ", p);
      return 0;
    }
    k = wi_cur;
  }
  if (!pid_add(pid, k)) {
    mod_set_err("EXEs do Windows demais", 0);
    return 0;
  }
  if (!w_busy) kuser_on();
  w_busy++;
  wi[k].busy++;
  return (void*)wi[k].ops->start;
}

void wine_exec_done(void) { wine_exec_done_pid(mod_current_pid()); }

void wine_exec_done_pid(uint32_t pid) {
  int k = pid_take(pid);
  if (k >= 0 && k < WI_MAX && wi[k].busy > 0) wi[k].busy--;
  if (w_busy > 0) w_busy--;
  if (!w_busy) kuser_off();
  /* copia aposentada (TROCA WINE): sai com o ultimo EXE dela */
  if (k >= 0 && k < WI_MAX && k != wi_cur && wi[k].handle && !wi[k].busy) unload_inst(k);
  if (!w_busy && !wanted()) unload_all();
}

int wine_reset_if_idle(void) {
  void ufree_owner(uint32_t owner);
  if (w_busy) return 0;
  unload_all();
  ufree_owner(WINE_SHARED_OWNER);
  hm_free_owner(WINE_SHARED_OWNER); /* o compartilhado acima de 4 GB (k_hmalloc_shared) */
  kuser_off();
  serial_puts("[wine] sem EXE do Windows: WINE.DLL e memoria compartilhada soltos\n");
  return 1;
}

void wine_env_changed(void) {
  /* So descarrega: carregar fica para o primeiro EXE do Windows. */
  if (!wanted() && !w_busy) unload_all();
}

/* ---------- TROCA WINE ---------- */
struct wb {
  char* p;
  int n, max;
};
static void bc(struct wb* b, char c) {
  if (b->n < b->max - 1) b->p[b->n++] = c;
}
static void bs(struct wb* b, const char* s) {
  while (s && *s) bc(b, *s++);
}
static void bd(struct wb* b, uint32_t v) {
  char t[12];
  int i = 0;
  do t[i++] = (char)('0' + v % 10u); while ((v /= 10u) && i < 11);
  while (i) bc(b, t[--i]);
}

int wine_swap(const char* path, char* msg, int max) {
  struct wb b;
  int k, old = wi_cur, rodando;
  b.p = msg;
  b.n = 0;
  b.max = max;
  if (!wine_loaded()) {
    bs(&b, "Nenhum WINE.DLL carregado: o proximo EXE do Windows ja le o arquivo do disco.\n");
    msg[b.n] = 0;
    return 0;
  }
  k = load(path);
  if (k < 0) {
    bs(&b, "Nao carreguei o WINE.DLL novo");
    if (path) {
      bs(&b, " (");
      bs(&b, path);
      bs(&b, ")");
    }
    bs(&b, ": ");
    bs(&b, mod_last_error());
    bs(&b, "\nOs EXEs do Windows continuam com o WINE.DLL de antes.\n");
    msg[b.n] = 0;
    return -1;
  }
  wi_cur = k;
  rodando = (old >= 0 && old < WI_MAX && wi[old].handle) ? wi[old].busy : 0;
  bs(&b, "WINE.DLL novo carregado de ");
  bs(&b, wi[k].path);
  bs(&b, " (copia ");
  bd(&b, wi[k].num);
  bs(&b, ").\nOs EXEs do Windows abertos daqui em diante usam esta copia.\n");
  if (old >= 0 && old < WI_MAX && wi[old].handle) {
    if (rodando) {
      bs(&b, "Os ");
      bd(&b, (uint32_t)rodando);
      bs(&b, " que ja rodam (e os filhos deles) continuam na copia ");
      bd(&b, wi[old].num);
      bs(&b, ", que sai da memoria quando o ultimo fechar.\n");
    } else {
      unload_inst(old);
    }
  }
  msg[b.n] = 0;
  return 0;
}

int wine_estado(char* out, int max) {
  struct wb b;
  int k, any = 0;
  if (!out || max <= 0) return 0;
  b.p = out;
  b.n = 0;
  b.max = max;
  for (k = 0; k < WI_MAX; k++) {
    if (!wi[k].handle) continue;
    if (!any) bs(&b, "WINE.DLL:\n");
    any = 1;
    bs(&b, "  copia ");
    bd(&b, wi[k].num);
    bs(&b, ": ");
    bs(&b, wi[k].path);
    bs(&b, ", ");
    bd(&b, (uint32_t)wi[k].busy);
    bs(&b, wi[k].busy == 1 ? " EXE do Windows" : " EXEs do Windows");
    bs(&b, k == wi_cur ? " (atual)\n" : " (aposentada: sai com o ultimo)\n");
  }
  if (!any) bs(&b, "WINE.DLL: nao carregado\n");
  out[b.n] = 0;
  return b.n;
}

/* ---------- reconhecimento de PE ---------- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int wine_is_pe(const char* path) {
  uint8_t h[64];
  uint8_t sig[4];
  uint32_t lfanew;
  if (!path || file_read_at(path, 0, (char*)h, (int)sizeof(h)) != (int)sizeof(h)) return 0;
  if (h[0] != 'M' || h[1] != 'Z') return 0;
  lfanew = rd32(h + 0x3C);
  if (lfanew < 0x40 || lfanew > 0x10000) return 0;
  if (file_read_at(path, lfanew, (char*)sig, 4) != 4) return 0;
  (void)rd16;
  return sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
}
