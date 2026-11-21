/* temp.c - Quinta tela: temperatura da CPU e do SSD, carga das CPUs,
 * memoria livre e o historico dos ultimos ~10 minutos.
 * Placas de video: RX 4xx/5xx (Polaris) e NVIDIA Pascal+ (Tesla P40).
 * Le k_hwstat (kernel/thermal.c): CPU e GPUs sao lidas na hora; SSD, carga e
 * historico vem da amostra que o kernel tira a cada 10 s (a mesma que vai
 * para D:\LOG\TEMP.LOG). */
#include "top.h"

#define CR 1
#define CC 0
#define CH (ROWS - 3)
#define CW (COLS - 2)
#define ATUALIZA 1000
#define G_MIN 30            /* faixa do grafico, graus C */
#define G_MAX 100

static const char* const rotulos[10] = {
  "Ajuda", "", "Procs", "", "Atualiz", "", "", "", "", "Sair"
};

static struct k_hwstat h;
static int ok;

static void le(void) {
  memset(&h, 0, sizeof h);
  ok = k_hwstat(&h) == 0 || h.sensor != 0;
}

/* Cor pela temperatura: verde, amarelo, vermelho. */
static int cor_temp(int t) {
  if (t >= 90) return COR(VERMELHO, BRANCO);
  if (t >= 80) return COR(MARROM, AMARELO);
  return COR(VERDE, BRANCO);
}
static int cor_texto(int t) {
  if (t >= 90) return COR(AZUL_CLARO, VERMELHO_CLARO);
  if (t >= 80) return COR(AZUL_CLARO, AMARELO);
  return C_FORTE;
}
static const char* nivel(int t) {
  if (t >= 95) return "MUITO QUENTE";
  if (t >= 90) return "quente";
  if (t >= 80) return "alta";
  return "normal";
}
static const char* nome_sensor(int s) {
  if (s == 1) return "AMD Tctl";
  if (s == 2) return "Intel DTS";
  return "nenhum";
}

static void gb(char* out, unsigned mb) {
  sprintf(out, "%u,%u GB", mb / 1024, mb % 1024 * 10 / 1024);
}

/* Uma linha "NOME  74 C  [barra]  extra". */
static void linha_temp(int r, const char* nome, int t, int escala, const char* extra) {
  char b[64];
  textow(r, CC + 2, nome, 9, C_JANELA);
  if (t == K_HW_NONE) {
    textow(r, CC + 11, "--", 8, C_FRACO);
    textow(r, CC + 39, "sem leitura", CW - 41, C_FRACO);
    return;
  }
  sprintf(b, "%3d C", t);
  textow(r, CC + 11, b, 8, cor_texto(t));
  barra(r, CC + 20, 16, t > 0 ? (unsigned)t : 0, (unsigned)escala);
  textow(r, CC + 39, extra, CW - 41, C_JANELA);
}

/* Historico da CPU: uma coluna por amostra, a mais nova a direita. */
static void grafico(int r0, int altura, int largura) {
  int col, lin, n = h.hist_n;
  char b[16];
  if (n > largura) n = largura;
  for (lin = 0; lin < altura; lin++) {
    int v = G_MAX - (G_MAX - G_MIN) * lin / (altura > 1 ? altura - 1 : 1);
    int r = r0 + lin;
    if (lin == 0 || lin == altura - 1 || lin == altura / 2) {
      sprintf(b, "%3d", v);
      textow(r, CC + 2, b, 4, C_FRACO);
    }
    poe(r, CC + 6, G_VL, C_JANELA);
    for (col = 0; col < largura; col++) {
      int idx = K_HW_HIST - largura + col;   /* encostado a direita */
      int t = idx >= K_HW_HIST - n ? h.cpu_hist[idx] : -128;
      int c = CC + 7 + col;
      if (t != -128 && t >= v) poe(r, c, ' ', cor_temp(t));
      else poe(r, c, lin == altura - 1 ? '_' : ' ', C_JANELA);
    }
  }
}

static void desenha(void) {
  char b[96], x[48], y[24], z[24];
  int r, gl, ga, i, pico = K_HW_NONE;

  fundo();
  menu_topo(4);
  teclas(rotulos);
  janela(CR, CC, CH, CW, " Temperatura ");

  if (!ok) {
    textow(CR + 2, CC + 2, "O kernel nao tem k_hwstat (kernel antigo) ou nao ha amostras.", CW - 4, C_FRACO);
    return;
  }
  for (i = K_HW_HIST - h.hist_n; i < K_HW_HIST; i++)
    if (h.cpu_hist[i] != -128 && h.cpu_hist[i] > pico) pico = h.cpu_hist[i];

  sprintf(b, "Sensor: %s   CPUs %d   amostra a cada %d s   log em D:\\LOG\\TEMP.LOG",
          nome_sensor(h.sensor), h.ncpu, h.step_s ? h.step_s : 10);
  textow(CR + 1, CC + 2, b, CW - 4, C_FORTE);
  separador(CR + 2, CC, CW);

  r = CR + 3;
  if (h.cpu_c != K_HW_NONE)
    sprintf(x, "%s   min %d  max %d", nivel(h.cpu_c), h.cpu_min_c, h.cpu_max_c);
  else
    x[0] = 0;
  linha_temp(r++, "CPU", h.cpu_c, 105, x);
  if (h.ssd_max_c != K_HW_NONE) sprintf(x, "max %d", h.ssd_max_c);
  else x[0] = 0;
  linha_temp(r++, "SSD", h.ssd_c, 85, x);
  for (i = 0; i < h.gpu_n && i < K_HW_GPUS; i++) {
    char nome[8];
    sprintf(nome, "GPU%d", i);
    if (h.gpu_max_c[i] != K_HW_NONE) sprintf(x, "%.15s   max %d", h.gpu_name[i], h.gpu_max_c[i]);
    else sprintf(x, "%.15s", h.gpu_name[i]);
    linha_temp(r++, nome, h.gpu_c[i], 105, x);
  }

  textow(r, CC + 2, "Carga", 9, C_JANELA);
  sprintf(b, "%3d %%", h.load_pct);
  textow(r, CC + 11, b, 8, C_FORTE);
  barra(r, CC + 20, 16, (unsigned)h.load_pct, 100);
  textow(r++, CC + 39, "todas as CPUs, ultimos 10 s", CW - 41, C_JANELA);

  textow(r, CC + 2, "Memoria", 9, C_JANELA);
  gb(y, h.mem_free_mb);
  gb(z, h.mem_total_mb);
  sprintf(b, "%s livres de %s", y, z);
  barra(r, CC + 20, 16, h.mem_total_mb - h.mem_free_mb, h.mem_total_mb ? h.mem_total_mb : 1);
  textow(r, CC + 11, h.mem_total_mb ? "" : "--", 8, C_FRACO);
  textow(r++, CC + 39, b, CW - 41, C_JANELA);

  if (h.hot_count)
    sprintf(b, "Acima de 90 C: %u amostras (~%u s)", h.hot_count, h.hot_count * (unsigned)(h.step_s ? h.step_s : 10));
  else
    sprintf(b, "Nenhuma amostra acima de 90 C desde o boot");
  textow(r++, CC + 2, b, CW - 4, h.hot_count ? COR(AZUL_CLARO, VERMELHO_CLARO) : C_JANELA);

  separador(r++, CC, CW);
  gl = CW - 10;
  if (gl > K_HW_HIST) gl = K_HW_HIST;
  ga = CR + CH - 2 - r;
  if (ga >= 3) {
    sprintf(b, "CPU nos ultimos %d min (1 coluna = %d s)", gl * (h.step_s ? h.step_s : 10) / 60,
            h.step_s ? h.step_s : 10);
    if (pico != K_HW_NONE) {
      sprintf(x, "   pico %d C", pico);
      strcat(b, x);
    }
    textow(r++, CC + 2, b, CW - 4, C_CABECA);
    grafico(r, ga - 1, gl);
  }
}

static void ajuda(void) {
  aviso(" Temperatura ",
        "CPU: sensor da propria CPU (AMD Tctl ou Intel DTS), lido na hora.\n"
        "SSD: temperatura do NVMe pelo log SMART.\n"
        "GPU: RX 4xx/5xx (SMC da placa) e NVIDIA Pascal+ como a Tesla P40.\n"
        "O kernel tira uma amostra a cada 10 s e grava D:\\LOG\\TEMP.LOG a\n"
        "cada minuto (a cada 10 s acima de 90 C). O arquivo vem de boots\n"
        "anteriores: se o PC desligar, as ultimas linhas antes do\n"
        "'===== boot' mostram a temperatura antes de apagar.\n"
        "\n"
        "Verde < 80 C, amarelo 80-89 C, vermelho >= 90 C.\n"
        "\n"
        "F3 ou Tab vai aos processos   F5 atualiza   F10 ou ESC sai");
}

int tela_temp(void) {
  le();
  for (;;) {
    int k;
    desenha();
    k = tecla(ATUALIZA);
    if (k == K_NADA || k == K_RESIZE || k == K_F(5) || k == 'r' || k == 'R') {
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
