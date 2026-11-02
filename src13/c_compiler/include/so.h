/* so.h - Funcoes do kernel (import "KERNEL", ver kexports.c) e de DLL.
 * k_* seguem o kernel: edi, esi, edx, ecx, r8d, r9d. As outras funcoes
 * deste compilador recebem os argumentos na pilha, de 4 em 4 bytes. */
#ifndef _SO_H
#define _SO_H

/* console */
void k_putc(int c);
void k_write(const char* s, int n);
void k_puts(const char* s);
int k_getc(void);
int k_readline(char* buf, int max);
void k_clear(void);

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

/* tela de texto 80x25: celulas caractere | cor << 8 (cor = fundo << 4 | frente).
 * -1 no WIN ou com a saida desviada: ai so ha k_putc. */
int k_vput(int row, int col, const unsigned short* cells, int n);
/* cursor na linha/coluna; row < 0 esconde */
void k_cursor(int row, int col);

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
void k_random(unsigned char* out, int n);

/* rede: uma conexao TCP por vez */
int k_net_ready(void);
int k_dns(const char* host, unsigned* ip);
int k_tcp_connect(const char* host, int port);
int k_tcp_send(const void* data, int len);
int k_tcp_recv(void* data, int cap);
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
/* rgba = R,G,B,A por pixel; A < 128 nao desenha. stride em bytes. */
void k_gui_pixels(int x, int y, int w, int h, const void* rgba, int stride);

/* No cc, __export em uma funcao ou variavel global a coloca na tabela de
 * exports da DLL. No gcc (bootstrap) use link /export:nome ou /exportall. */
#ifdef __GNUC__
#define __export
#endif

#endif
