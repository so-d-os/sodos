/* top.h - TOP do SODOS: programas, servicos e mapa da memoria no modo texto.
 * Telas no padrao do SPECIAL (Doutor de Disco): janelas com sombra, bordas e
 * blocos desenhados com k_font_set; barras de cima e de teclas como no MC.
 * Compila com o CC do proprio sistema:
 *   cc -o TOP.EXE top.c tela.c dados.c procs.c mapa.c video.c cpu.c */
#ifndef TOP_H
#define TOP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

/* ---------- desenhos trocados na fonte (0xC0-0xDF, como no SPECIAL) ---------- */
#define G_BL 0xC0
#define G_BR 0xC1
#define G_HB 0xC2
#define G_VL 0xC3
#define G_VR 0xC4
#define G_CLOSE 0xC5
#define G_SEPL 0xC6
#define G_SEPH 0xC7
#define G_SEPR 0xC8
#define G_BTNL 0xC9
#define G_BTNR 0xCA
#define G_CHK 0xCB
#define G_CHKON 0xCC
#define G_RAD 0xCD
#define G_RADON 0xCE
#define G_TICK 0xCF
#define G_PB0 0xD0     /* barra: G_PB0 + 0..4 quartos */
#define G_MAPF 0xD5    /* bloco livre */
#define G_MAPU 0xD6    /* bloco ocupado */
#define G_MAPB 0xD7    /* bloco marcado */
#define G_ARROW 0xD8
#define G_DESK 0xD9
#define G_MAPH 0xDA    /* bloco meio ocupado */
#define G_MAPR 0xDB    /* nao e RAM */
#define G_DOWN 0xDC    /* coluna da ordem */

#define G_BASE 0xC0
#define N_GLIFOS 29

/* ---------- cores (fundo << 4 | frente); k_blink(0) libera fundos 8-15 ---------- */
#define COR(fundo, frente) (((fundo) << 4) | (frente))
#define PRETO 0
#define AZUL 1
#define VERDE 2
#define CIANO 3
#define VERMELHO 4
#define MAGENTA 5
#define MARROM 6
#define CINZA 7
#define ESCURO 8
#define AZUL_CLARO 9
#define VERDE_CLARO 10
#define CIANO_CLARO 11
#define VERMELHO_CLARO 12
#define MAGENTA_CLARO 13
#define AMARELO 14
#define BRANCO 15

#define C_JANELA COR(AZUL_CLARO, BRANCO)
#define C_BORDA COR(AZUL_CLARO, BRANCO)
#define C_TITULO COR(BRANCO, AZUL)
#define C_FORTE COR(AZUL_CLARO, BRANCO)
#define C_FRACO COR(AZUL_CLARO, AZUL)
#define C_SELECAO COR(BRANCO, AZUL)
#define C_FUNDO COR(PRETO, AZUL)
/* barras do MC: menu em cima, teclas embaixo */
#define C_MENU COR(CIANO, PRETO)
#define C_MENUSEL COR(PRETO, BRANCO)
#define C_MENUHOT COR(CIANO, AMARELO)
#define C_TECLA COR(PRETO, CINZA)
#define C_ROTULO COR(CIANO, PRETO)
/* painel azul do MC, onde ficam a lista e o mapa */
#define C_PAINEL COR(AZUL, CINZA)
#define C_CABECA COR(AZUL, AMARELO)

/* ---------- teclas de k_getc ---------- */
#define K_UP 0x100
#define K_DOWN 0x101
#define K_LEFT 0x102
#define K_RIGHT 0x103
#define K_PGUP 0x104
#define K_PGDN 0x105
#define K_HOME 0x106
#define K_END 0x107
#define K_F(n) (0x140 + (n))
#define K_ESC 27
#define K_ENTER '\n'
#define K_TAB '\t'
#define K_EOF 26
#define K_NADA (-1)     /* o tempo de atualizar venceu */

/* ---------- tela.c ---------- */
int tela_inicia(void);   /* -1 fora da tela de texto (WIN, saida desviada) */
void tela_fim(void);
void tela_mostra(void);
void poe(int r, int c, int ch, int cor);
void texto(int r, int c, const char* s, int cor);
/* s em w colunas: corta ou completa com espacos */
void textow(int r, int c, const char* s, int w, int cor);
void linha(int r, int c, int n, int ch, int cor);
void pinta(int r, int c, int n, int cor);   /* troca so a cor */
int tam(const char* s);
void fundo(void);
/* barra de cima: abas e relogio; aba = 0 processos, 1 mapa, 2 video, 3 cpu */
void menu_topo(int aba);
/* barra de teclas do MC: rotulos[0] e o F1 */
void teclas(const char* const* rotulos);
void janela(int r, int c, int h, int w, const char* titulo);
void separador(int r, int c, int w);
void botao(int r, int c, const char* s, int sel);
void barra(int r, int c, int w, unsigned feito, unsigned total);
/* Espera tecla ate 'ms' (0 = sem limite); K_NADA se o tempo venceu. */
int tecla(int ms);
/* Caixa com linhas separadas por \n e um botao OK sobre a tela atual. */
void aviso(const char* titulo, const char* msg);

/* ---------- dados.c ---------- */
#define MAX_PROC 48
#define MAPA_MAX 2048

struct proc {
  struct k_proc k;
  unsigned total_kb;   /* imagem + dados */
};

/* Ordens da lista */
#define ORD_MEM 0
#define ORD_NOME 1
#define ORD_PID 2
#define ORD_TIPO 3
#define N_ORDENS 4

struct resumo {
  unsigned ram_kb;        /* RAM de verdade (sem as areas reservadas) */
  unsigned livre_kb;
  unsigned kernel_kb;     /* kernel + buffers */
  unsigned progs_kb;      /* EXE + DLL + dados */
  unsigned cat_kb[KM_CATS];
  int n_exe, n_dll, n_svc;
};

extern struct proc procs[MAX_PROC];
extern int n_procs;
extern int ordem;
extern struct resumo res;
/* mapa: KM_CATS valores por celula */
extern unsigned mapa_kb[MAPA_MAX * KM_CATS];
extern int mapa_n;
extern unsigned mapa_cel_kb;

void dados_le(void);
/* Le o mapa com celulas que cabem em 'cells'; pid > 0 marca o que e dele. */
void mapa_le(int cells, unsigned pid);
void dados_ordena(void);
const char* nome_tipo(unsigned kind);
const char* nome_estado(unsigned state);
const char* nome_ordem(int o);
/* "123 KB", "4,5 MB", "2,9 GB" */
void fmt_kb(char* out, unsigned kb);
/* KB em decimos de porcento do total */
unsigned permil(unsigned kb, unsigned total);

/* ---------- procs.c ---------- */
/* Devolve 1 para ir ao mapa; *destaque recebe o pid escolhido (0 = nenhum). */
int tela_procs(unsigned* destaque);

/* ---------- mapa.c ---------- */
/* Devolve 1 para ir a tela de video, 0 para sair. */
int tela_mapa(unsigned* destaque);

/* ---------- video.c ---------- */
/* Placa, VRAM e RAM do modo grafico. Devolve 1 para voltar aos processos, 0 para sair. */
int tela_video(void);

/* ---------- cpu.c ---------- */
/* CPUs do SMP: quantas, uso e estado de cada uma. Devolve 1 para os processos. */
int tela_cpu(void);

#endif
