/* dados.c - Le do kernel o que ocupa a memoria (k_ps, k_memmap) e formata. */
#include "top.h"

struct proc procs[MAX_PROC];
int n_procs;
int ordem = ORD_MEM;
struct resumo res;
unsigned mapa_kb[MAPA_MAX * KM_CATS];
int mapa_n;
unsigned mapa_cel_kb;

/* Tamanhos de celula que dao numeros redondos na legenda, em MB. */
static const unsigned redondos[] = {
  1, 2, 4, 5, 8, 10, 16, 20, 25, 32, 40, 50, 64, 100, 128, 200, 256, 500, 512, 1024, 2048, 4096
};

void mapa_le(int cells, unsigned pid) {
  unsigned cel = 0, span_mb;
  int i;
  if (cells > MAPA_MAX) cells = MAPA_MAX;
  /* Primeiro em potencia de 2, so para saber ate onde vai a RAM. */
  mapa_n = k_memmap(mapa_kb, cells, &cel, pid);
  if (mapa_n <= 0) {
    mapa_n = 0;
    mapa_cel_kb = 0;
    return;
  }
  span_mb = (unsigned)mapa_n * (cel >> 10);
  for (i = 0; i < (int)(sizeof redondos / sizeof redondos[0]); i++)
    if ((span_mb + redondos[i] - 1) / redondos[i] <= (unsigned)cells) break;
  if (i < (int)(sizeof redondos / sizeof redondos[0]) && (redondos[i] << 10) < cel) {
    cel = redondos[i] << 10;
    mapa_n = k_memmap(mapa_kb, cells, &cel, pid);
  }
  mapa_cel_kb = cel;
}

static void resume(void) {
  int c, k;
  memset(&res, 0, sizeof res);
  for (c = 0; c < mapa_n; c++)
    for (k = 0; k < KM_CATS; k++) res.cat_kb[k] += mapa_kb[c * KM_CATS + k];
  for (k = 0; k < KM_CATS; k++)
    if (k != KM_RESERV && k != KM_DONO) res.ram_kb += res.cat_kb[k];
  res.livre_kb = res.cat_kb[KM_LIVRE];
  res.kernel_kb = res.cat_kb[KM_KERNEL] + res.cat_kb[KM_BUFFERS];
  res.progs_kb = res.cat_kb[KM_EXE] + res.cat_kb[KM_DLL] + res.cat_kb[KM_DADOS];
  for (c = 0; c < n_procs; c++) {
    if (procs[c].k.kind == KP_EXE) res.n_exe++;
    else if (procs[c].k.kind == KP_DLL) res.n_dll++;
    else if (procs[c].k.kind == KP_SVC && procs[c].k.state == 'A') res.n_svc++;
  }
}

void dados_le(void) {
  static struct k_proc lidos[MAX_PROC];
  int i;
  n_procs = k_ps(lidos, MAX_PROC);
  if (n_procs < 0) n_procs = 0;
  for (i = 0; i < n_procs; i++) {
    procs[i].k = lidos[i];
    procs[i].total_kb = lidos[i].image_kb + lidos[i].data_kb;
  }
  /* O resumo sai do mapa: ele cobre a RAM toda, nao so o que tem dono. */
  mapa_le(1024, 0);
  resume();
  dados_ordena();
}

static int compara(const struct proc* a, const struct proc* b) {
  int d;
  switch (ordem) {
  case ORD_NOME:
    return strcmp(a->k.name, b->k.name);
  case ORD_PID:
    if (a->k.pid != b->k.pid) return a->k.pid < b->k.pid ? -1 : 1;
    return strcmp(a->k.name, b->k.name);
  case ORD_TIPO:
    d = (int)a->k.kind - (int)b->k.kind;
    if (d) return d;
    /* no mesmo tipo, maior primeiro */
  default:
    if (a->total_kb != b->total_kb) return a->total_kb > b->total_kb ? -1 : 1;
    return strcmp(a->k.name, b->k.name);
  }
}

/* Insercao: a lista e curta. */
void dados_ordena(void) {
  int i, j;
  for (i = 1; i < n_procs; i++) {
    struct proc t = procs[i];
    for (j = i; j > 0 && compara(&t, &procs[j - 1]) < 0; j--) procs[j] = procs[j - 1];
    procs[j] = t;
  }
}

const char* nome_tipo(unsigned kind) {
  switch (kind) {
  case KP_EXE: return "EXE";
  case KP_DLL: return "DLL";
  case KP_SYS: return "sistema";
  case KP_SVC: return "servico";
  }
  return "?";
}

const char* nome_estado(unsigned state) {
  switch (state) {
  case 'R': return "rodando";
  case 'E': return "espera";
  case 'S': return "residente";
  case 'A': return "ativo";
  case 'P': return "parado";
  }
  return "?";
}

const char* nome_ordem(int o) {
  static const char* nomes[N_ORDENS] = { "memoria", "nome", "pid", "tipo" };
  return o >= 0 && o < N_ORDENS ? nomes[o] : "?";
}

/* Uma casa decimal com virgula, so quando o numero e pequeno. */
static void fmt_un(char* out, unsigned v, unsigned un, const char* nome) {
  unsigned i = v / un, d = (v % un) * 10 / un;
  if (i < 10 && d) sprintf(out, "%u,%u %s", i, d, nome);
  else sprintf(out, "%u %s", i, nome);
}

void fmt_kb(char* out, unsigned kb) {
  if (kb < 1024) sprintf(out, "%u KB", kb);
  else if (kb < 1024u * 1024u) fmt_un(out, kb, 1024, "MB");
  else fmt_un(out, kb, 1024u * 1024u, "GB");
}

unsigned permil(unsigned kb, unsigned total) {
  if (!total) return 0;
  return (unsigned)((unsigned long long)kb * 1000u / total);
}
