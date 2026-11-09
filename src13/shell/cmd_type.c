#include "shell.h"
#include "drives.h"
#include "reboot.h"

/* type (e cat): como o TYPE do DOS. Varios arquivos e * e ? no nome; com
 * mais de um, o nome vem antes de cada. Para paginar: type arquivo | more */

static int type_many;
static int type_err;

static int type_one(const char* path, const char* shown, void* ctx) {
  (void)ctx;
  if (type_many) {
    puts_k("\n");
    puts_k(shown);
    puts_k("\n\n");
  }
  if (text_file(path, 0)) type_err = 1;
  return kbd_break_take();
}

static int has_wild(const char* s) {
  for (; *s; s++)
    if (*s == '*' || *s == '?') return 1;
  return 0;
}

int cmd_type(int argc, char** argv) {
  int i;
  if (argc < 2 || name_eq(argv[1], "/?")) {
    puts_k("Mostra o conteudo de arquivos de texto.\n\n");
    puts_k("Uso: ");
    puts_k(argv[0]);
    puts_k(" [unidade:][caminho]arquivo [...]\n\n");
    puts_k("  type leiame.txt\n");
    puts_k("  type D:\\nota.txt U:\\*.bat\n");
    puts_k("  type D:\\nota.txt | more\n");
    return argc < 2;
  }
  type_many = argc > 2;
  for (i = 1; i < argc; i++)
    if (has_wild(argv[i])) type_many = 1;
  type_err = 0;
  for (i = 1; i < argc; i++) {
    int n = drives_glob(argv[i], type_one, 0);
    if (n == -2) {
      puts_k("Caminho invalido: ");
      puts_k(argv[i]);
      puts_k("\n");
      type_err = 1;
    } else if (n == -1) {
      puts_k("File not found: ");
      puts_k(argv[i]);
      puts_k("\n");
      type_err = 1;
    }
  }
  return type_err;
}
