/* cpu.c - Quarta tela: as CPUs (SMP), com o uso e o estado de cada uma.
 * Le k_cpu_count/k_cpu_stat: a BSP (CPU0) esta sempre ativa; os APs ficam
 * em hlt esperando o escalonador por CPU, e por isso aparecem parados. */
#include "top.h"

#define CR 1
#define CC 0
#define CH 22
#define CW 78
#define ATUALIZA 500
#define CPU_MAX 8

static const char* const rotulos[10] = {
  "Ajuda", "", "Procs", "", "Atualiz", "", "", "", "", "Sair"
};

static struct k_cpu_stat c[CPU_MAX];
static int n;

static void le(void) {
  int i;
  n = k_cpu_count();
  if (n < 0) n = 0;
  if (n > CPU_MAX) n = CPU_MAX;
  for (i = 0; i < n; i++) {
    memset(&c[i], 0, sizeof c[i]);
    k_cpu_stat(i, &c[i]);
  }
}

/* Trabalhando / (trabalhando + parado), em %. */
static unsigned pct(const struct k_cpu_stat* s) {
  unsigned long long t = (unsigned long long)s->busy_ms + s->idle_ms;
  if (!t) return 0;
  return (unsigned)((unsigned long long)s->busy_ms * 100u / t);
}

/* "12:34" ou "1:02:03" */
static void hms(char* out, unsigned ms) {
  int s = (int)(ms / 1000);
  if (s >= 3600) sprintf(out, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  else sprintf(out, "%d:%02d", s / 60, s % 60);
}

static const char* estado(const struct k_cpu_stat* s) {
  if (!s->online) return "ausente";
  return s->state ? "ativa" : "hlt";
}

static void desenha(void) {
  char b[80], t[16];
  unsigned carga = 0, total = 0;
  int i, r, online = 0;

  for (i = 0; i < n; i++) {
    unsigned long long tt = (unsigned long long)c[i].busy_ms + c[i].idle_ms;
    if (c[i].online) online++;
    carga += c[i].busy_ms;
    if (tt > 0xFFFFFFFFu) tt = 0xFFFFFFFFu;
    total += (unsigned)tt;
  }

  fundo();
  menu_topo(3);
  teclas(rotulos);
  janela(CR, CC, CH, CW, " CPUs (SMP) ");

  sprintf(b, "CPUs: %d   (1 BSP + %d AP)   online %d/%d   carga media %u%%",
          n, n > 0 ? n - 1 : 0, online, n, total ? carga * 100u / total : 0);
  textow(CR + 1, CC + 2, b, CW - 4, C_FORTE);

  separador(CR + 2, CC, CW);
  textow(CR + 3, CC + 2, "CPU  PAPEL  ESTADO   USO (0-100%)                 TEMPO", CW - 4, C_CABECA);

  for (i = 0; i < n; i++) {
    unsigned p = pct(&c[i]);
    r = CR + 4 + i;
    sprintf(b, "CPU%d", i);
    textow(r, CC + 2, b, 5, C_FORTE);
    textow(r, CC + 7, i == 0 ? "BSP" : "AP", 5, C_JANELA);
    textow(r, CC + 12, estado(&c[i]), 8, c[i].online ? C_FORTE : C_FRACO);
    barra(r, CC + 21, 16, p, 100);
    sprintf(b, "%3u%%", p);
    textow(r, CC + 38, b, 4, C_FORTE);
    hms(t, c[i].busy_ms + c[i].idle_ms);
    sprintf(b, "ha %s", t);
    textow(r, CC + 44, b, 14, C_JANELA);
  }

  if (n == 0) {
    textow(CR + 4, CC + 2, "Nenhuma CPU informada pelo kernel.", CW - 4, C_FRACO);
  } else {
    sprintf(b, "APIC ids:");
    for (i = 0; i < n; i++) {
      char id[16];
      sprintf(id, " %u", c[i].id);
      strcat(b, id);
    }
    textow(CR + 4 + n, CC + 2, b, CW - 4, C_FRACO);
  }

  separador(CR + CH - 6, CC, CW);
  textow(CR + CH - 5, CC + 2, "USO = tempo trabalhando / (trabalhando + parado) desde o boot.",
         CW - 4, C_JANELA);
  if (n > 1)
    textow(CR + CH - 4, CC + 2,
           "Os APs estao em hlt: o escalonador por CPU ainda nao distribui trabalho.",
           CW - 4, C_JANELA);
  else
    textow(CR + CH - 4, CC + 2,
           "Sem SMP: o kernel nao encontrou o MADT do ACPI (1 CPU).",
           CW - 4, C_JANELA);
  textow(CR + CH - 3, CC + 2, "A BSP roda o SO inteiro; um AP so acorda por IPI.",
         CW - 4, C_JANELA);
}

static void ajuda(void) {
  aviso(" CPUs ",
        "O kernel descobre as CPUs no MADT do ACPI e sobe os APs com\n"
        "INIT-SIPI. A CPU0 e a BSP, que roda o SO; as outras sao APs.\n"
        "\n"
        "A barra e o uso desde o boot: tempo trabalhando dividido pelo\n"
        "tempo total. A BSP esta sempre ativa (o SO e polling); os APs\n"
        "aparecem em hlt porque ainda nao ha escalonador por CPU.\n"
        "\n"
        "F3 ou Tab vai aos processos   F5 atualiza   F10 ou ESC sai");
}

int tela_cpu(void) {
  le();
  for (;;) {
    int k;
    desenha();
    k = tecla(ATUALIZA);
    if (k == K_NADA || k == K_F(5) || k == 'r' || k == 'R') {
      le();
    } else if (k == K_F(1) || k == '?') {
      ajuda();
      le();
    } else if (k == K_F(3) || k == K_TAB || k == 'p' || k == 'P') {
      return 1;
    } else if (k == K_F(10) || k == K_ESC || k == K_EOF || k == 'q' || k == 'Q') {
      return 0;
    }
  }
}
