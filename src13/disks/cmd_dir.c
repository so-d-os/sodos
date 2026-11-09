#include "shell.h"
#include "drives.h"
#include "kexports.h"
#include "reboot.h"

/* dir /p: para a cada tela cheia. A saida passa por page_out, que manda o
 * caractere para a saida de antes e conta as linhas (80 colunas). */
#define PAGE_LINES 24
static void (*page_prev)(char c);
static int page_lines, page_col, page_quit;

static void page_out(char c) {
  static const char msg[] = "Pressione uma tecla para continuar . . .";
  int k;
  if (page_quit) return;
  shell_set_out(page_prev);
  putc(c);
  if (c == '\n' || ++page_col >= 80) {
    page_col = 0;
    page_lines++;
  }
  if (page_lines >= PAGE_LINES) {
    puts_k(msg);
    do k = shell_get_char(); while (k == 0);
    if (shell_break() || k == 27) page_quit = 1;
    putc('\r');
    for (k = 0; msg[k]; k++) putc(' ');
    putc('\r');
    page_lines = 0;
  }
  shell_set_out(page_out);
}

/* dir [unidade:][pasta\][padrao] [/s] [/w] [/d] */
int cmd_dir(int argc, char** argv) {
  const char* spec = "";
  int flags = 0;
  int paged = 0;
  int i, j;
  for (i = 1; i < argc; i++) {
    if (argv[i][0] != '/') {
      spec = argv[i];
      continue;
    }
    for (j = 1; argv[i][j]; j++) {
      char c = argv[i][j];
      if (c == '/') continue;
      if (c == 's' || c == 'S') flags |= DIR_SUB;
      else if (c == 'w' || c == 'W') flags |= DIR_WIDE;
      else if (c == 'd' || c == 'D') flags |= DIR_DIRS;
      else if (c == 'p' || c == 'P') paged = 1;
      else {
        puts_k("Uso: dir [pasta\\][*.ext] [/s] [/w] [/d] [/p]\n");
        puts_k("  /s  inclui as subpastas\n");
        puts_k("  /w  lista larga, em linhas\n");
        puts_k("  /d  so as pastas\n");
        puts_k("  /p  para a cada tela cheia\n");
        puts_k("  * e ? valem no nome: dir *.exe   dir ???.txt\n");
        return 1;
      }
    }
  }
  page_prev = shell_get_out();
  /* So na tela (ou na janela do WIN); redirecao e pipe nao param. */
  if (paged && (!page_prev || page_prev == kexp_get_console_hook())) {
    page_lines = 1;   /* a linha do comando */
    page_col = page_quit = 0;
    shell_set_out(page_out);
    drives_dir(spec, flags);
    shell_set_out(page_prev);
    return 0;
  }
  drives_dir(spec, flags);
  return 0;
}

int cmd_cd(int argc, char** argv) {
  if (argc < 2) return drives_chdir(0);
  return drives_chdir(argv[1]);
}

int cmd_md(int argc, char** argv) {
  int rc;
  if (argc < 2) {
    puts_k("Uso: md pasta\n");
    return 1;
  }
  rc = drives_mkdir(argv[1]);
  if (rc == 1) puts_k("ja existe\n");
  else if (rc == -5) puts_k("md so funciona em disco FAT\n");
  else if (rc != 0) puts_k("nao criou a pasta\n");
  return rc == 0 ? 0 : 1;
}

int cmd_rd(int argc, char** argv) {
  int rc;
  if (argc < 2) {
    puts_k("Uso: rd pasta\n");
    return 1;
  }
  rc = drives_rmdir(argv[1]);
  if (rc == -2) puts_k("pasta nao esta vazia\n");
  else if (rc == -3) puts_k("nao e pasta\n");
  else if (rc == -5) puts_k("rd so funciona em disco FAT\n");
  else if (rc != 0) puts_k("nao removeu a pasta\n");
  return rc == 0 ? 0 : 1;
}

/* del arquivo | del *.tmp. *.* pergunta antes. */
int cmd_del(int argc, char** argv) {
  const char* s;
  const char* leaf;
  int rc;
  if (argc < 2) {
    puts_k("Uso: del arquivo   (aceita * e ?: del *.tmp)\n");
    return 1;
  }
  leaf = argv[1];
  for (s = argv[1]; *s; s++)
    if (*s == '\\' || *s == '/' || *s == ':') leaf = s + 1;
  if ((leaf[0] == '*' && leaf[1] == 0) ||
      (leaf[0] == '*' && leaf[1] == '.' && leaf[2] == '*' && leaf[3] == 0)) {
    int c;
    puts_k("Apagar todos os arquivos da pasta (s/n)? ");
    do c = shell_get_char(); while (c == 0);
    putc((char)c);
    putc('\n');
    if (c != 's' && c != 'S') return 1;
  }
  rc = drives_del(argv[1]);
  if (rc > 0) {
    char n[12];
    uint32_to_string((uint32_t)rc, n, sizeof(n));
    puts_k(n);
    puts_k(" arquivo(s) apagado(s)\n");
    return 0;
  }
  if (rc == 0) return 0;
  if (rc == -1) puts_k("arquivo nao encontrado\n");
  else if (rc == -3) puts_k("e pasta; use rd\n");
  else if (rc == -4) puts_k("arquivo somente leitura\n");
  else if (rc == -5) puts_k("del so funciona em disco FAT e em D: na RAM\n");
  else puts_k("unidade ou pasta invalida\n");
  return 1;
}
