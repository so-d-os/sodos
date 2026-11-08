/* video.c - Terceira tela: a placa de video, a VRAM num quadro (como o mapa
 * da RAM) e a RAM que o modo grafico ocupa fora da placa. Le k_vram. */
#include "top.h"

#define VR 1           /* janela */
#define VC 0
#define VH 22
#define VW 78
#define QR (VR + 6)    /* quadro da VRAM */
#define QC 3
#define QL 4
#define QW 72
#define ATUALIZA 2000

/* partes da VRAM no quadro */
#define V_LIVRE 0
#define V_FRENTE 1
#define V_PAG2 2
#define V_TEXTO 3
#define V_PARTES 4

static const char* const rotulos[10] = {
  "Ajuda", "", "Procs", "", "Atualiz", "", "", "", "", "Sair"
};

static const char* const nomes[V_PARTES] = {
  "Livre", "Tela visivel", "Segunda pagina", "Texto e fonte"
};

static struct k_vram v;

static int cor_parte(int p) {
  switch (p) {
  case V_FRENTE: return COR(AZUL, AMARELO);
  case V_PAG2: return COR(AZUL, CIANO_CLARO);
  case V_TEXTO: return COR(AZUL, BRANCO);
  }
  return COR(AZUL, CIANO);
}

static const char* nome_estado_v(unsigned s) {
  switch (s) {
  case 1: return "WIN na tela";
  case 2: return "WIN ao fundo";
  }
  return "modo texto";
}

/* KB de cada parte, na ordem em que ficam na VRAM. */
static void partes(unsigned* kb) {
  kb[V_FRENTE] = v.front_kb;
  kb[V_PAG2] = v.page2_kb;
  kb[V_TEXTO] = v.text_kb;
}

static void cabeca(void) {
  char buf[96], a[16];
  int r = VR + 1;
  if (v.pci_id)
    sprintf(buf, "Placa   %s    PCI %04X:%04X    LFB %08X", v.dev, v.pci_id >> 16,
            v.pci_id & 0xFFFFu, v.lfb);
  else
    sprintf(buf, "Placa   %s    sem placa de video no PCI", v.dev);
  textow(r, VC + 3, buf, VW - 6, C_FORTE);
  if (v.drv[0])
    sprintf(buf, "Driver  %s (aceleracao ligada)", v.drv);
  else
    strcpy(buf, "Driver  nenhum: a CPU desenha tudo (veja docs\\GPU.md)");
  textow(r + 1, VC + 3, buf, VW - 6, C_JANELA);
  if (v.state)
    sprintf(buf, "Modo    %ux%ux%u, %u bytes por linha    %s", v.w, v.h, v.bpp, v.pitch,
            nome_estado_v(v.state));
  else if (v.w)
    sprintf(buf, "Modo    texto %ux%u    WIN fechado", v.w, v.h);
  else
    strcpy(buf, "Modo    texto no framebuffer (fbcon)    WIN fechado");
  textow(r + 2, VC + 3, buf, VW - 6, C_JANELA);
  separador(VR + 4, VC, VW);
  if (v.vram_kb) {
    unsigned usado = v.front_kb + v.page2_kb + v.text_kb;
    unsigned pm;
    char b[16];
    if (usado > v.vram_kb) usado = v.vram_kb;
    pm = permil(usado, v.vram_kb);
    fmt_kb(a, v.vram_kb);
    fmt_kb(b, usado);
    sprintf(buf, "VRAM %s    em uso %s (%u,%u%%)", a, b, pm / 10, pm % 10);
  } else {
    strcpy(buf, "VRAM tamanho desconhecido (a placa nao informa)");
  }
  textow(VR + 5, VC + 3, buf, VW - 6, C_FORTE);
}

/* Cada caractere vale vram_kb / (QL*QW); as partes ocupam a VRAM do inicio. */
static void quadro(void) {
  unsigned kb[V_PARTES];
  unsigned long long cel, ini[V_PARTES], fim[V_PARTES], pos = 0;
  int i, p, n = QL * QW;
  for (i = 0; i < QL; i++) linha(QR + i, VC + 1, VW - 2, ' ', C_PAINEL);
  if (!v.vram_kb) return;
  partes(kb);
  for (p = V_FRENTE; p < V_PARTES; p++) {
    ini[p] = pos;
    pos += kb[p];
    fim[p] = pos;
  }
  cel = ((unsigned long long)v.vram_kb + n - 1) / n;
  if (!cel) cel = 1;
  for (i = 0; i < n && (unsigned long long)i * cel < v.vram_kb; i++) {
    unsigned long long a = (unsigned long long)i * cel, e = a + cel;
    unsigned long long maior = 0, tot = 0;
    int dom = V_LIVRE, ch;
    for (p = V_FRENTE; p < V_PARTES; p++) {
      unsigned long long x0 = ini[p] > a ? ini[p] : a;
      unsigned long long x1 = fim[p] < e ? fim[p] : e;
      if (x1 <= x0) continue;
      tot += x1 - x0;
      if (x1 - x0 > maior) {
        maior = x1 - x0;
        dom = p;
      }
    }
    ch = !tot ? G_MAPF : tot * 2 >= cel ? G_MAPU : G_MAPH;
    poe(QR + i / QW, QC + i % QW, ch, cor_parte(dom));
  }
}

static void legenda(int r) {
  static const int ordem_leg[V_PARTES] = { V_FRENTE, V_PAG2, V_TEXTO, V_LIVRE };
  int i, c = VC + 3;
  char buf[24];
  unsigned cel = v.vram_kb ? (v.vram_kb + QL * QW - 1) / (QL * QW) : 0;
  linha(r, VC + 1, VW - 2, ' ', C_JANELA);
  for (i = 0; i < V_PARTES; i++) {
    int p = ordem_leg[i];
    poe(r, c, p == V_LIVRE ? G_MAPF : G_MAPU, cor_parte(p));
    texto(r, c + 2, nomes[p], C_JANELA);
    c += tam(nomes[p]) + 4;
  }
  if (cel) {
    fmt_kb(buf, cel);
    texto(r, c, "1 car = ", C_FRACO);
    texto(r, c + 8, buf, C_FRACO);
  }
}

/* Linha da RAM: nome, tamanho e um detalhe. */
static void item_ram(int r, const char* nome, unsigned kb, const char* det) {
  char a[16];
  linha(r, VC + 1, VW - 2, ' ', C_JANELA);
  texto(r, VC + 3, nome, C_JANELA);
  if (kb) {
    fmt_kb(a, kb);
    texto(r, VC + 24 + 8 - tam(a), a, C_FORTE);
  } else {
    texto(r, VC + 24 + 7, "-", C_FRACO);
  }
  textow(r, VC + 36, det, VW - 39, C_FRACO);
}

static void ram(int r) {
  char det[48], a[16];
  unsigned tot = v.back_kb + v.wall_kb + v.win_kb + v.gpu_kb;
  texto(r, VC + 3, "Na RAM por causa do video", C_FORTE);
  if (v.back_kb) sprintf(det, "quadro montado em %08X", v.back_addr);
  else strcpy(det, v.page2_kb ? "nao usa: desenha na segunda pagina" : "nao usa");
  item_ram(r + 1, "Back buffer", v.back_kb, det);
  item_ram(r + 2, "Papel de parede", v.wall_kb, v.wall_kb ? "ja escalado para a tela" : "sem imagem");
  sprintf(det, "%u janela%s de programa", v.win_n, v.win_n == 1 ? "" : "s");
  item_ram(r + 3, "Superficies", v.win_kb, v.win_n ? det : "nenhuma janela aberta");
  item_ram(r + 4, "Driver da placa", v.gpu_kb, v.drv[0] ? v.drv : "sem driver: nada reservado");
  fmt_kb(a, tot);
  linha(r + 5, VC + 1, VW - 2, ' ', C_JANELA);
  texto(r + 5, VC + 3, "Total", C_FORTE);
  if (tot) texto(r + 5, VC + 24 + 8 - tam(a), a, C_FORTE);
  else texto(r + 5, VC + 24 + 7, "-", C_FRACO);
}

static void nota(int r) {
  const char* s;
  if (v.state == 2)
    s = "WIN parado ao fundo: o modo grafico continua ocupando esta memoria.";
  else if (v.state == 1)
    s = "Rode o TOP numa tela do DOS (Ctrl+F1) para ver o WIN aberto ao fundo.";
  else
    s = "WIN fechado: so a tela de texto usa a placa. Abra o WIN e use Ctrl+F1.";
  textow(r, VC + 3, s, VW - 6, C_JANELA);
}

static void desenha(void) {
  fundo();
  menu_topo(2);
  teclas(rotulos);
  janela(VR, VC, VH, VW, " Placa de video ");
  cabeca();
  quadro();
  legenda(QR + QL);
  separador(QR + QL + 1, VC, VW);
  ram(QR + QL + 2);
  separador(QR + QL + 8, VC, VW);
  nota(QR + QL + 9);
}

static void le(void) {
  if (k_vram(&v) != 0) memset(&v, 0, sizeof v);
}

static void ajuda(void) {
  aviso(" Video ",
        "A memoria da placa de video (VRAM) e o que o modo grafico gasta.\n"
        "\n"
        "Quadro: a VRAM do inicio ao fim. Amarelo e a tela visivel;\n"
        "ciano, a segunda pagina (pagina dupla do Bochs); ponto, livre.\n"
        "No GOP (UEFI) a placa so mostra o quadro visivel.\n"
        "\n"
        "Embaixo: RAM comum usada por causa do video (back buffer,\n"
        "papel de parede, janelas). Sem driver da placa, o driver nao\n"
        "reserva nada.\n"
        "\n"
        "F3 ou Tab volta aos processos   F5 atualiza   F10 ou ESC sai");
}

int tela_video(void) {
  le();
  for (;;) {
    int k;
    desenha();
    k = tecla(ATUALIZA);
    if (k == K_NADA || k == K_F(5) || k == 'r' || k == 'R') {
      le();
    } else if (k == K_F(1) || k == '?') {
      ajuda();
    } else if (k == K_F(3) || k == K_TAB || k == 'p' || k == 'P') {
      return 1;
    } else if (k == K_F(10) || k == K_ESC || k == K_EOF || k == 'q' || k == 'Q') {
      return 0;
    }
  }
}
