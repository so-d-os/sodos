/* install.h - a lista de arquivos do SODOS e a funcao install() (install.c).
 *
 * O setup.c so desenha a janela; quem sabe o que vai para onde e o install.c.
 * Os arquivos a instalar ficam em <pasta do SETUP.EXE>\ARQUIVOS, com os nomes
 * da coluna "origem" da lista. */
#ifndef INSTALL_H
#define INSTALL_H

/* Chamado durante a copia: bytes copiados e o total, o arquivo da vez
 * (destino) e quantos arquivos ja foram. Devolve 0 para seguir, 1 para parar. */
typedef int (*inst_progresso_fn)(long feitos, long total, const char* arquivo,
                                 int n, int n_total, void* ud);

typedef struct {
  int copiados;   /* arquivos gravados */
  int faltando;   /* origem nao esta em ARQUIVOS (pulados) */
  int erros;      /* nao conseguiu gravar */
  int mantidos;   /* configuracao do usuario que ja existia (nao sobrescreve) */
  int parado;     /* o usuario parou no meio */
  int path_ok;    /* AUTOEXEC.BAT com D:\SODOS\BIN no PATH */
  char ultimo_erro[160];
} inst_resultado_t;

/* Quantos arquivos a lista tem. */
int install_total(void);

/* Soma o tamanho dos arquivos que estao em origem (bytes). */
long install_bytes(const char* origem);

/* Copia cada arquivo da lista de origem (pasta ARQUIVOS) para o destino.
 * raiz: 0 instala de verdade no D:; "D:\TEMP\TESTE" grava tudo debaixo dessa
 * pasta (D:\SODOS\BIN -> D:\TEMP\TESTE\SODOS\BIN), para testar sem mexer no
 * sistema. No fim poe D:\SODOS\BIN no PATH do AUTOEXEC.BAT (e do sistema, se
 * raiz for 0). Devolve 0 se nao houve erro. */
int install(const char* origem, const char* raiz, inst_progresso_fn cb, void* ud,
            inst_resultado_t* res);

#endif
