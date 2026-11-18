/* host_wsl.c - (host, teste) Os comandos de leitura do WSL.EXE contra uma
 * imagem de disco no Linux:  host_wsl DISCO.img list|info|ls|cat|cp ... */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../wslcore.h"
#include "../wslcli.h"

static int fd;

static int rd(void* ctx, uint64_t lba, uint32_t count, void* buf) {
  (void)ctx;
  return pread(fd, buf, (size_t)count * 512, (off_t)(lba * 512)) == (ssize_t)count * 512 ? 0 : -1;
}

static int save(const char* path, const void* data, uint32_t len) {
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  fwrite(data, 1, len, f);
  return fclose(f);
}

int main(int argc, char** argv) {
  struct stat sb;
  wsl_io disk;
  if (argc < 3) {
    fprintf(stderr, "uso: host_wsl DISCO.img comando...\n");
    return 2;
  }
  fd = open(argv[1], O_RDONLY);
  if (fd < 0 || fstat(fd, &sb)) {
    perror(argv[1]);
    return 1;
  }
  wsl_disk_io(&disk, rd, 0, (uint64_t)sb.st_size / 512);
  argv[1] = "wsl";
  return wsl_cli_read(argc - 1, argv + 1, &disk, save);
}
