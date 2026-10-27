#include "shell.h"

int cmd_echo(int argc, char** argv) {
  if (argc < 2) {
    putc('\n');
    return 0;
  }
  int redirect_index = -1;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '>' && argv[i][1] == '\0') {
      redirect_index = i;
      break;
    }
  }
  if (redirect_index == -1) {
    for (int i = 1; i < argc; ++i) {
      puts_k(argv[i]);
      if (i + 1 < argc) putc(' ');
    }
    putc('\n');
    return 0;
  }
  char output[512];
  int output_pos = 0;
  for (int i = 1; i < redirect_index; i++) {
    int j = 0;
    while (argv[i][j] != '\0' && output_pos < 511) {
      output[output_pos++] = argv[i][j++];
    }
    if (i + 1 < redirect_index && output_pos < 511)
      output[output_pos++] = ' ';
  }
  if (output_pos < 511)
    output[output_pos++] = '\n';
  output[output_pos] = '\0';
  if (redirect_index + 1 >= argc) {
    puts_k("Error: No filename specified after >\n");
    return 1;
  }
  const char* filename = argv[redirect_index + 1];
  if (write_to_mem_file(filename, output) < 0) {
    puts_k("Error: Cannot write to file\n");
    return 1;
  }
  puts_k("Written to: ");
  puts_k(filename);
  puts_k("\n");
  return 0;
}
