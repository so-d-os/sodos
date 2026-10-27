#include "shell.h"
#include "drives.h"

int cmd_type(int argc, char** argv) {
  static char buf[2048];
  int n;
  int i;
  if (argc < 2) {
    puts_k("Usage: type <arquivo>\n");
    puts_k("  type readme.txt\n");
    puts_k("  type D:\\nota.txt\n");
    return 1;
  }
  n = file_load(argv[1], buf, sizeof(buf));
  if (n < 0) {
    if (n == -2)
      puts_k("Unidade nao montada. Use: mount D:\n");
    else {
      puts_k("File not found: ");
      puts_k(argv[1]);
      puts_k("\n");
    }
    return 1;
  }
  for (i = 0; i < n; i++)
    putc(buf[i]);
  return 0;
}
