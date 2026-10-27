#include "shell.h"

extern int fat_read_file(const char* filename, void* buffer, uint32_t max_size);

int cmd_cat(int argc, char** argv) {
  if (argc < 2) {
    puts_k("Usage: cat <filename>\n");
    return 1;
  }
  static uint8_t file_buffer[4096];
  int bytes_read = fat_read_file(argv[1], file_buffer, sizeof(file_buffer));
  if (bytes_read < 0) {
    puts_k("Error: File not found or read error\n");
    return 1;
  }
  for (int i = 0; i < bytes_read; i++)
    putc(file_buffer[i]);
  return 0;
}
