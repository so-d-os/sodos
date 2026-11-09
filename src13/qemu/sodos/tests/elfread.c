/* elfread.c - o padrao de E/S do load_elf do QEMU: open, read, lseek, read. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
  const char* f = argc > 1 ? argv[1] : "D:\HELLO.ELF";
  unsigned char id[16], eh[52], ph[32];
  unsigned phoff, off, filesz;
  long r;
  int fd = open(f, O_RDONLY);
  printf("open(%s) = %d\n", f, fd);
  if (fd < 0) return 1;
  r = read(fd, id, 16);
  printf("read(16) = %ld magic=%02x%c%c%c\n", r, id[0], id[1], id[2], id[3]);
  r = lseek(fd, 0, SEEK_SET);
  printf("lseek(0) = %ld\n", r);
  r = read(fd, eh, 52);
  memcpy(&phoff, eh + 28, 4);
  printf("read(52) = %ld phoff=%u\n", r, phoff);
  r = lseek(fd, phoff, SEEK_SET);
  printf("lseek(%u) = %ld\n", phoff, r);
  r = read(fd, ph, 32);
  memcpy(&off, ph + 4, 4);
  memcpy(&filesz, ph + 16, 4);
  printf("read(32) = %ld p_offset=%u p_filesz=%u\n", r, off, filesz);
  r = lseek(fd, off, SEEK_SET);
  printf("lseek(%u) = %ld\n", off, r);
  {
    static unsigned char data[65536];
    r = read(fd, data, filesz);
    printf("read(%u) = %ld primeiro=%02x%02x%02x%02x\n", filesz, r, data[0], data[1], data[2], data[3]);
  }
  printf("lseek(0,SEEK_END) = %ld\n", (long)lseek(fd, 0, SEEK_END));
  close(fd);
  return 0;
}
