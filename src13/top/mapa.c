/* mapa.c - Segunda tela: a RAM toda num quadro, como o mapa do Doutor de
 * Disco e do desfragmentador. Cada caractere vale mapa_cel_kb; cheio onde o
 * sistema ou os programas ocupam, ponto onde esta livre. */
#include "top.h"

#define MR 1           /* janela */
#define MC 0
#define MH 22
#define MW 78
#define GR (MR + 2)    /* quadro */
#define GC 3
#define GL 15
#define GW 72
#define ATUALIZA 2000

static const char* const rotulos[10] = {
  "Ajuda", "", "Procs", "Destaq", "Atualiz", "", "", "", "", "Sair"
};

static const char* const nomes[KM_CATS] = {
  "Livre", "Kernel", "EXE", "DLL", "Dados", "Buffers", "Nao-RAM", "Destaque"
};

static int cor_cat(int k) {
  switch (k) {
  case KM_KERNEL: return COR(AZUL, BRANCO);
  case KM_EXE: return COR(AZUL, AMARELO);
  case KM_DLL: return COR(AZUL, VERDE_CLARO);
  case KM_DADOS: return COR(AZUL, CIANO_CLARO);
  case KM_BUFFERS: return COR(AZUL, MAGENTA_CLARO);
  case KM_RESERV: return COR(AZUL, ESCURO);
  case KM_DONO: return COR(VERMELHO, AMARELO);
  }
  return COR(AZUL, CIANO);
}

static int cur;
static unsigned dono;

/* Desenho e cor de uma celula. */
static void celula(const unsigned* k, int* ch, int* cor) {
  unsigned ram = 0, usado, maior = 0;
  int i, dom = KM_KERNEL;
  for (i = 0; i < KM_CATS; i++)
    if (i != KM_RESERV && i != KM_DONO) ram += k[i];
  if (ram * 2 < mapa_cel_kb) {
    *ch = G_MAPR;
    *cor = cor_cat(KM_RESERV);
    return;
  }
  if (dono && k[KM_DONO]) {
    *ch = G_MAPU;
    *cor = cor_cat(KM_DONO);
    return;
  }
  usado = ram - k[KM_LIVRE];
  if (!usado) {
    *ch = G_MAPF;
    *cor = cor_cat(KM_LIVRE);
    return;
  }
  for (i = KM_KERNEL; i <= KM_BUFFERS; i++)
    if (k[i] > maior) {
      maior = k[i];
      dom = i;
    }
  *ch = usado * 2 >= ram ? G_MAPU : G_MAPH;
  *cor = cor_cat(dom);
}

static void legenda(int r) {
  static const int ordem_leg[7] = { KM_KERNEL, KM_EXE, KM_DLL, KM_DADOS, KM_BUFFERS, KM_LIVRE, KM_RESERV };
  int i, c = MC + 2;
  linha(r, MC + 1, MW - 2, ' ', C_PAINEL);
  for (i = 0; i < 7; i++) {
    int k = ordem_leg[i];
    int ch = k == KM_LIVRE ? G_MAPF : k == KM_RESERV ? G_MAPR : G_MAPU;
    poe(r, c, ch, cor_cat(k));
    texto(r, c + 2, nomes[k], C_PAINEL);
    c += tam(nomes[k]) + 4;
  }
  poe(r, c, G_MAPH, C_PAINEL);
  texto(r, c + 2, "meio", C_PAINEL);
}

/* "Kernel 2 MB, Livre 6 MB" para a celula sob o cursor. */
static void info_celula(int r) {
  char buf[128], v[16];
  const unsigned* k = mapa_kb + cur * KM_CATS;
  unsigned de = (unsigned)((unsigned long long)cur * mapa_cel_kb / 1024u);
  unsigned ate = (unsigned)((unsigned long long)(cur + 1) * mapa_cel_kb / 1024u);
  int i, n;
  n = sprintf(buf, "Bloco %d (%u a %u MB):", cur, de, ate);
  for (i = 0; i < KM_DONO; i++) {
    if (!k[i]) continue;
    fmt_kb(v, k[i]);
    n += sprintf(buf + n, " %s %s,", nomes[i], v);
  }
  if (buf[n - 1] == ',') buf[n - 1] = 0;
  textow(r, MC + 3, buf, MW - 6, C_FORTE);
}

static const struct proc* acha_pid(unsigned pid) {
  int i;
  for (i = 0; i < n_procs; i++)
    if (procs[i].k.kind == KP_EXE && procs[i].k.pid == pid) return &procs[i];
  return 0;
}

static void info_dono(int r) {
  char buf[96], v[16];
  const struct proc* p = acha_pid(dono);
  unsigned kb = 0;
  int i, n = 0;
  if (!p) {
    textow(r, MC + 3, "Sem destaque. F4 marca no mapa um programa de cada vez.", MW - 6, C_JANELA);
    return;
  }
  for (i = 0; i < mapa_n; i++)
    if (mapa_kb[i * KM_CATS + KM_DONO]) {
      kb += mapa_kb[i * KM_CATS + KM_DONO];
      n++;
    }
  fmt_kb(v, kb);
  sprintf(buf, "Destaque: %s (pid %u) ocupa %s em %d bloco%s", p->k.name, dono, v, n,
          n == 1 ? "" : "s");
  textow(r, MC + 3, buf, MW - 9, C_FORTE);
  poe(r, MC + MW - 5, G_MAPU, cor_cat(KM_DONO));
}

static void desenha(void) {
  int i;
  char buf[96], a[16], b[16];
  fundo();
  menu_topo(1);
  teclas(rotulos);
  janela(MR, MC, MH, MW, " Mapa da memoria ");
  fmt_kb(a, mapa_cel_kb);
  fmt_kb(b, (unsigned)((unsigned long long)mapa_n * mapa_cel_kb > 0xFFFFFFFFu ? 0xFFFFFFFFu
                       : (unsigned long long)mapa_n * mapa_cel_kb));
  sprintf(buf, "Cada caractere = %s    %d blocos, de 0 a %s", a, mapa_n, b);
  textow(MR + 1, MC + 3, buf, MW - 6, C_FORTE);
  for (i = 0; i < GL; i++) linha(GR + i, MC + 1, MW - 2, ' ', C_PAINEL);
  for (i = 0; i < GL * GW && i < mapa_n; i++) {
    int ch, cor;
    celula(mapa_kb + i * KM_CATS, &ch, &cor);
    if (i == cur) cor = COR(BRANCO, AZUL);
    poe(GR + i / GW, GC + i % GW, ch, cor);
  }
  legenda(GR + GL);
  separador(GR + GL + 1, MC, MW);
  info_celula(GR + GL + 2);
  info_dono(GR + GL + 3);
}

/* Proximo programa para destacar; depois do ultimo, nenhum. */
static void proximo_dono(void) {
  unsigned melhor = 0;
  int i;
  for (i = 0; i < n_procs; i++) {
    unsigned p = procs[i].k.pid;
    if (procs[i].k.kind != KP_EXE || p <= dono) continue;
    if (!melhor || p < melhor) melhor = p;
  }
  dono = melhor;
}

static void le(void) {
  dados_le();
  if (dono && !acha_pid(dono)) dono = 0;
  mapa_le(GL * GW, dono);
  if (cur >= mapa_n) cur = mapa_n ? mapa_n - 1 : 0;
}

static void ajuda(void) {
  aviso(" Mapa ",
        "Cada caractere e um pedaco da RAM fisica, do endereco 0 ao fim.\n"
        "\n"
        "Cheio: mais da metade ocupada. Meio: menos. Ponto: livre.\n"
        "A cor diz quem ocupa mais: kernel, EXE, DLL, dados ou buffers.\n"
        "Riscado: nao e RAM (firmware, video, buraco abaixo de 4 GB).\n"
        "\n"
        "Setas andam no mapa   F4 ou + destaca um programa\n"
        "F3 ou Tab volta       F5 atualiza   F10 ou ESC sai");
}

int tela_mapa(unsigned* destaque) {
  dono = *destaque;
  le();
  for (;;) {
    int k;
    desenha();
    k = tecla(ATUALIZA);
    if (k == K_NADA || k == K_F(5) || k == 'r' || k == 'R') {
      le();
    } else if (k == K_LEFT) {
      cur--;
    } else if (k == K_RIGHT) {
      cur++;
    } else if (k == K_UP) {
      cur -= GW;
    } else if (k == K_DOWN) {
      cur += GW;
    } else if (k == K_HOME) {
      cur = 0;
    } else if (k == K_END) {
      cur = mapa_n - 1;
    } else if (k == K_F(4) || k == '+' || k == ' ') {
      proximo_dono();
      le();
    } else if (k == '-' || k == '0') {
      dono = 0;
      le();
    } else if (k == K_F(1) || k == '?') {
      ajuda();
    } else if (k == K_F(3) || k == K_TAB || k == 'p' || k == 'P') {
      *destaque = dono;
      return 1;
    } else if (k == K_F(10) || k == K_ESC || k == K_EOF || k == 'q' || k == 'Q') {
      return 0;
    }
    if (cur < 0) cur += GW;
    if (cur < 0) cur = 0;
    if (cur >= mapa_n) cur = mapa_n - 1;
    if (cur < 0) cur = 0;
  }
}
