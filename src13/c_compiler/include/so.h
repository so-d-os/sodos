/* so.h - Funcoes do kernel (import "KERNEL", ver kexports.c) e de DLL.
 * k_* seguem o kernel: edi, esi, edx, ecx, r8d, r9d. As outras funcoes
 * deste compilador recebem os argumentos na pilha, de 4 em 4 bytes. */
#ifndef _SO_H
#define _SO_H

/* console */
void k_putc(int c);
void k_write(const char* s, int n);
/* Depuracao: escreve direto na serial (sem console/VGA). */
void k_dbg(const char* s, int n);
void k_puts(const char* s);
int k_getc(void);
/* Nao espera: -1 se nao ha tecla. Para jogos e lacos com k_ms(). */
int k_trygetc(void);
int k_readline(char* buf, int max);
void k_clear(void);
/* Cede o turno as outras tarefas do SO (WIN, HTTPD). Ponto de cooperacao
 * para threads de usuario e lacos de espera. */
void k_yield(void);

/* arquivos: "D:NOME.EXT", "NOME.EXT" ou pasta "D:\\temp\\net\\NOME.EXT" */
int k_fsize(const char* path);
int k_fload(const char* path, char* buf, int max);
/* Le len bytes a partir de offset. Devolve quantos leu, ou <0. */
int k_fread(const char* path, unsigned offset, char* buf, int len);
int k_fsave(const char* path, const char* data, int size);
int k_flist(int letter, char* out, int max);
/* Lista pasta: linhas "NOME" ou "NOME\\" (dir), separadas por \\n. <0 erro. */
int k_dirlist(const char* path, char* out, int max);
void k_pwd(char* out, int max);
int k_chdir(const char* path);
int k_drive(void);
int k_set_drive(int letter);
/* Unidade de onde o .EXE atual foi carregado ('C', 'D', ...; 0 = nenhuma). */
int k_exe_drive(void);

/* Uma entrada de pasta. date/time no formato FAT (0 = sem data):
 * dia = date & 31, mes = (date >> 5) & 15, ano = 1980 + (date >> 9);
 * hora = time >> 11, minuto = (time >> 5) & 63. */
struct k_dirent {
  char name[32];
  unsigned size;
  unsigned short date;
  unsigned short time;
  unsigned char dir;
  unsigned char pad[3];
};
/* Pasta "U:\\SRC": quantas entradas (sem . e ..); -1 se nao e pasta. */
int k_dir(const char* path, struct k_dirent* out, int max);
/* Apaga arquivo (aceita * e ?): 0, ou quantos com * e ?. -1 nao achou,
 * -3 e pasta, -4 somente leitura, -5 unidade sem apagar, -2 invalido. */
int k_fdelete(const char* path);
/* 0 ok; mkdir 1 = ja existe; rmdir -2 = nao vazia, -3 = nao e pasta. */
int k_mkdir(const char* path);
int k_rmdir(const char* path);
/* Espaco em KB de um disco FAT; -1 se nao for. */
int k_dfree(int letter, unsigned* total_kb, unsigned* free_kb);
/* Mascara das unidades montadas: bit 0 = A:, bit 1 = B:, ... */
unsigned k_drive_mask(void);

/* tela de texto 80x25: celulas caractere | cor << 8 (cor = fundo << 4 | frente).
 * -1 no WIN ou com a saida desviada: ai so ha k_putc. */
int k_vput(int row, int col, const unsigned short* cells, int n);
/* cursor na linha/coluna; row < 0 esconde */
void k_cursor(int row, int col);
/* Desenho do caractere ch na tela de texto: 16 bytes, um por linha, bit 7 a
 * esquerda. Vale para ch em toda a tela, inclusive o que ja esta escrito.
 * 0 ok; -1 ch invalido. k_font_set tambem da -1 no WIN ou com a saida desviada.
 * k_font_reset volta todos ao padrao; o kernel tambem faz isso quando o
 * programa que mudou a fonte termina. */
int k_font_get(int ch, unsigned char* glyph);
int k_font_set(int ch, const unsigned char* glyph);
void k_font_reset(void);
/* 0: o bit 7 da cor deixa de piscar e da fundo claro (cores 8-15). 1: volta a
 * piscar; o kernel tambem faz isso quando o programa termina. -1 no WIN. */
int k_blink(int on);

/* memoria do programa (liberada sozinha quando ele termina) */
void* k_malloc(unsigned n);
void k_free(void* p);
void* k_realloc(void* p, unsigned n);
unsigned k_msize(void* p);
void k_meminfo(unsigned* total, unsigned* freeb);

/* processos e DLLs */
void k_exit(int code);
int k_exec(const char* path, int argc, char** argv);
/* Uma linha do prompt: comando interno, .EXE ou .BAT. Nao grava historico.
 * A linha do chamador nao e alterada. Erro de carga vira 1. */
int k_shell(char* line);
/* Como k_shell, mas a saida vai para out (com 0 no fim). *len = bytes
 * gravados, negativo se cortou em max-1. Devolve o codigo do comando. */
int k_shell_capture(char* line, char* out, int max, int* len);
const char* k_last_error(void);
int k_dll_load(const char* name);
void* k_dll_sym(int handle, const char* name);
void k_dll_free(int handle);

/* tempo e aleatoriedade */
unsigned k_rdtsc(unsigned* hi);
unsigned k_ms(void);
/* data UTC AAAAMMDD, ou 0 se o relogio nao for confiavel */
int k_date(void);
/* hora UTC HHMMSS, ou -1 */
int k_time(void);
void k_random(unsigned char* out, int n);

/* rede: uma conexao TCP por vez */
int k_net_ready(void);
int k_dns(const char* host, unsigned* ip);
int k_tcp_connect(const char* host, int port);
int k_tcp_send(const void* data, int len);
int k_tcp_recv(void* data, int cap);
/* Bytes no buffer de recepcao, -1 se nao ha conexao (ou o par fechou). */
int k_net_pending(void);
void k_tcp_close(void);

/* janela canvas: so funciona num .EXE aberto pelo WIN. Cores 0xRRGGBB.
 * Use gui.h (GUI.DLL), que tem as constantes de eventos e teclas. */
int k_gui_open(const char* title);
void k_gui_close(void);
void k_gui_title(const char* title);
void k_gui_size(int* w, int* h);
void k_gui_font(int* cw, int* ch);
void k_gui_fill(int x, int y, int w, int h, unsigned rgb);
void k_gui_text(int x, int y, const char* s, int n, unsigned fg, unsigned bg, int flags);
void k_gui_present(void);
int k_gui_event(int* ev, int timeout_ms);
/* rgba = R,G,B,A por pixel. No modo retro A < 128 nao desenha; no modo
 * avancado o pixel e misturado com o que ja esta na janela. */
void k_gui_pixels(int x, int y, int w, int h, const void* rgba, int stride);

/* video: 0 = retro (alfa binario, baixo consumo), 1 = avancado (mistura).
 * k_gui_quality devolve o valor anterior; com mode < 0 so consulta. */
int k_gui_quality(int mode);
/* cor de fundo do desktop (0xRRGGBB); 0 volta ao padrao */
int k_win_bg(unsigned rgb);
/* papel de parede: rgba w*h*4 (R,G,B,A); rgba NULL limpa a imagem */
int k_win_wallpaper(const void* rgba, int w, int h);

/* interface (VIDEO.CFG): posicao do menu Iniciar (0 esq, 1 centro, 2 dir),
 * menu translucido (0/1) e painel de IA (0/1). Com valor < 0 so consulta. */
int k_ui_menu_pos(int pos);
int k_ui_glass(int on);
int k_ui_ia(int on);

/* No cc, __export em uma funcao ou variavel global a coloca na tabela de
 * exports da DLL. No gcc (bootstrap) use link /export:nome ou /exportall. */
#ifdef __GNUC__
#define __export
#endif

#endif
