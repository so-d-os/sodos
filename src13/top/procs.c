/* procs.c - Primeira tela: programas, DLLs, partes do kernel e servicos que
 * estao na memoria, com quanto cada um ocupa. */
#include "top.h"

#define PR 1           /* janela */
#define PC 0
#define PH 22
#define PW 78
#define LINHAS 12      /* linhas da lista */
#define ATUALIZA 2000  /* ms */

static const char* const rotulos[10] = {
  "Ajuda", "Ordem", "Mapa", "", "Atualiz", "", "", "", "", "Sair"
};

static int sel, topo;
static char sel_nome[32];

static int cor_item(const struct proc* p) {
  if (p->k.kind == KP_EXE) return COR(AZUL, VERDE_CLARO);
  if (p->k.kind == KP_DLL) return COR(AZUL, VERDE);
  if (p->k.kind == KP_SVC) return p->k.state == 'A' ? COR(AZUL, CIANO_CLARO) : COR(AZUL, ESCURO);
  return COR(AZUL, BRANCO);
}

static void resumo_topo(void) {
  int r = PR, c = PC;
  char a[16], b[16], d[16], e[16], buf[80];
  unsigned usado = res.ram_kb - res.livre_kb;
  unsigned pm = permil(usado, res.ram_kb);
  fmt_kb(a, res.ram_kb);
  fmt_kb(b, usado);
  fmt_kb(d, res.livre_kb);
  fmt_kb(e, res.cat_kb[KM_RESERV]);
  sprintf(buf, "RAM %s   em uso %s   livre %s   fora da RAM %s", a, b, d, e);
  textow(r + 1, c + 3, buf, PW - 6, C_FORTE);
  texto(r + 2, c + 3, "Ocupada", C_JANELA);
  barra(r + 2, c + 12, 52, usado, res.ram_kb);
  sprintf(buf, "%3u,%u%%", pm / 10, pm % 10);
  texto(r + 2, c + 66, buf, C_FORTE);
  fmt_kb(a, res.kernel_kb);
  fmt_kb(b, res.progs_kb);
  sprintf(buf, "Sistema %s   Programas %s   EXE %d  DLL %d  servicos ativos %d",
          a, b, res.n_exe, res.n_dll, res.n_svc);
  textow(r + 3, c + 3, buf, PW - 6, C_JANELA);
}

/* Colunas da lista, a partir da coluna 2; o cabecalho usa o mesmo formato. */
#define FORMATO "%4s %-12.12s %-7s %-9s %-8s %7s %7s %7s %5s"
/* Coluna da seta de cada ordem: logo antes do titulo. */
static const int col_ordem[N_ORDENS] = { 64, 6, 2, 19 };

static void cabecalho(int r) {
  char buf[96];
  sprintf(buf, FORMATO, "PID", "NOME", "TIPO", "ESTADO", "BASE", "IMAGEM", "DADOS", "TOTAL", "%MEM");
  linha(r, PC + 1, PW - 2, ' ', C_CABECA);
  texto(r, PC + 2, buf, C_CABECA);
  poe(r, col_ordem[ordem], G_DOWN, COR(AZUL, BRANCO));
}

static void item(int r, const struct proc* p, int marcado) {
  char pid[8], base[12], img[12], dad[12], tot[12], pc[8], buf[96];
  int c = PC + 1, cor = marcado ? C_SELECAO : cor_item(p);
  unsigned pm = permil(p->total_kb, res.ram_kb);
  if (p->k.pid) sprintf(pid, "%u", p->k.pid);
  else strcpy(pid, "-");
  if (p->k.base) sprintf(base, "%08X", p->k.base);
  else strcpy(base, "-");
  if (p->k.image_kb) fmt_kb(img, p->k.image_kb);
  else strcpy(img, "-");
  if (p->k.data_kb) fmt_kb(dad, p->k.data_kb);
  else strcpy(dad, "-");
  if (p->total_kb) {
    fmt_kb(tot, p->total_kb);
    sprintf(pc, "%u,%u", pm / 10, pm % 10);
  } else {
    strcpy(tot, "-");
    strcpy(pc, "-");
  }
  sprintf(buf, FORMATO, pid, p->k.name,
          nome_tipo(p->k.kind), nome_estado(p->k.state), base, img, dad, tot, pc);
  textow(r, c + 1, buf, PW - 3, cor);
  poe(r, c, marcado ? G_ARROW : ' ', marcado ? C_SELECAO : cor);
}

/* Duas linhas sobre o item escolhido. */
static void detalhe(int r, const struct proc* p) {
  char a[16], b[16], l1[96], l2[96];
  fmt_kb(a, p->k.image_kb);
  fmt_kb(b, p->k.data_kb);
  l2[0] = 0;
  if (p->k.kind == KP_EXE) {
    sprintf(l1, "%s, pid %u: %s", p->k.name, p->k.pid,
            p->k.state == 'R' ? "rodando agora" : "esperando o programa que ele chamou terminar");
    sprintf(l2, "Imagem %s em %08X; malloc e pilha %s. Enter mostra no mapa.", a, p->k.base, b);
  } else if (p->k.kind == KP_DLL) {
    sprintf(l1, "%s: biblioteca usada por %u modulo%s", p->k.name, p->k.refs,
            p->k.refs == 1 ? "" : "s");
    sprintf(l2, "Imagem %s em %08X; sai da memoria quando ninguem mais usa.", a, p->k.base);
  } else if (!strcmp(p->k.name, "KERNEL")) {
    sprintf(l1, "Kernel: codigo, dados, shell, FAT, rede e video em %08X (%s)", p->k.base, a);
    sprintf(l2, "Heap do kernel em uso: %s. A area baixa e o boot entram no mapa.", b);
  } else if (!strcmp(p->k.name, "BUFFERS")) {
    sprintf(l1, "Buffers: memoria alta pedida pelo kernel e drivers (%s)", b);
    strcpy(l2, "Discos, rede, video e arquivos abertos pelo carregador.");
  } else if (!strcmp(p->k.name, "MEM>4GB")) {
    sprintf(l1, "RAM acima de 4 GB em uso: %s", b);
    strcpy(l2, "So o kernel alcanca estes enderecos; programas x32 nao.");
  } else if (!strcmp(p->k.name, "HTTPD")) {
    if (p->k.state == 'A') sprintf(l1, "Servico HTTP escutando na porta %u", p->k.refs);
    else strcpy(l1, "Servico HTTP parado: api start sobe quando houver rede");
    strcpy(l2, "Faz parte do kernel; a memoria dele esta na linha KERNEL.");
  } else if (!strcmp(p->k.name, "REDE")) {
    strcpy(l1, p->k.state == 'A' ? "Rede ligada: pilha TCP/IP com endereco" : "Rede parada: sem placa ou sem DHCP");
    strcpy(l2, "Faz parte do kernel; a memoria dele esta na linha KERNEL.");
  } else {
    sprintf(l1, "%s: %s %s", p->k.name, nome_tipo(p->k.kind), nome_estado(p->k.state));
  }
  textow(r, PC + 3, l1, PW - 6, C_FORTE);
  textow(r + 1, PC + 3, l2, PW - 6, C_JANELA);
}

static void desenha(void) {
  int r = PR, c = PC, i;
  char titulo[48];
  fundo();
  menu_topo(0);
  teclas(rotulos);
  sprintf(titulo, " Programas e servicos em memoria (%d) ", n_procs);
  janela(r, c, PH, PW, titulo);
  resumo_topo();
  separador(r + 4, c, PW);
  cabecalho(r + 5);
  if (sel >= n_procs) sel = n_procs - 1;
  if (sel < 0) sel = 0;
  if (sel < topo) topo = sel;
  if (sel >= topo + LINHAS) topo = sel - LINHAS + 1;
  for (i = 0; i < LINHAS; i++) {
    int y = r + 6 + i;
    if (topo + i < n_procs) item(y, &procs[topo + i], topo + i == sel);
    else linha(y, c + 1, PW - 2, ' ', C_PAINEL);
  }
  separador(r + 18, c, PW);
  if (n_procs) {
    detalhe(r + 19, &procs[sel]);
    strcpy(sel_nome, procs[sel].k.name);
  }
}

/* Depois de reler ou reordenar, volta ao mesmo nome. */
static void acha_sel(void) {
  int i;
  for (i = 0; i < n_procs; i++)
    if (!strcmp(procs[i].k.name, sel_nome)) {
      sel = i;
      return;
    }
}

static void ajuda(void) {
  aviso(" Ajuda ",
        "TOP: o que esta ocupando a memoria\n"
        "\n"
        "Setas, PgUp, PgDn  escolhem         F2 ou O  troca a ordem\n"
        "Enter              mapa com destaque F3 ou Tab  mapa da memoria\n"
        "F5 ou R            atualiza agora    F10 ou ESC  sai\n"
        "\n"
        "IMAGEM e o codigo carregado; DADOS e malloc e pilha.\n"
        "A lista se atualiza sozinha a cada 2 segundos.");
}

int tela_procs(unsigned* destaque) {
  dados_le();
  acha_sel();
  for (;;) {
    int k;
    desenha();
    k = tecla(ATUALIZA);
    if (k == K_NADA || k == K_F(5) || k == 'r' || k == 'R') {
      dados_le();
      acha_sel();
    } else if (k == K_UP) {
      sel--;
    } else if (k == K_DOWN) {
      sel++;
    } else if (k == K_PGUP) {
      sel -= LINHAS;
    } else if (k == K_PGDN) {
      sel += LINHAS;
    } else if (k == K_HOME) {
      sel = 0;
    } else if (k == K_END) {
      sel = n_procs - 1;
    } else if (k == K_F(2) || k == 'o' || k == 'O') {
      ordem = (ordem + 1) % N_ORDENS;
      dados_ordena();
      acha_sel();
    } else if (k == K_F(1) || k == '?') {
      ajuda();
    } else if (k == K_F(3) || k == K_TAB || k == 'm' || k == 'M') {
      return 1;
    } else if (k == K_ENTER) {
      if (n_procs && procs[sel].k.kind == KP_EXE) *destaque = procs[sel].k.pid;
      return 1;
    } else if (k == K_F(10) || k == K_ESC || k == K_EOF || k == 'q' || k == 'Q') {
      return 0;
    }
    if (sel < 0) sel = 0;
    if (sel >= n_procs) sel = n_procs - 1;
  }
}
