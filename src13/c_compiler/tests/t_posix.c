/* Testa POSIX.DLL (posix-lite) sob soxrun. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  int fd;
  char buf[64];
  char cwd[96];
  struct stat st;
  DIR* d;
  struct dirent* e;
  int n = 0;

  fd = open("TPOSX.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    printf("FALHOU open w errno=%d\n", errno);
    return 1;
  }
  if (write(fd, "ola-posix\n", 10) != 10) {
    printf("FALHOU write\n");
    return 1;
  }
  close(fd);

  fd = open("TPOSX.TXT", O_RDONLY);
  if (fd < 0) {
    printf("FALHOU open r\n");
    return 1;
  }
  n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n != 10 || memcmp(buf, "ola-posix\n", 10) != 0) {
    printf("FALHOU read (%d)\n", n);
    return 1;
  }

  if (stat("TPOSX.TXT", &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 10) {
    printf("FALHOU stat file\n");
    return 1;
  }

  if (!getcwd(cwd, sizeof(cwd))) {
    printf("FALHOU getcwd\n");
    return 1;
  }

  d = opendir(".");
  if (!d) d = opendir("D:\\");
  if (!d) {
    printf("FALHOU opendir\n");
    return 1;
  }
  n = 0;
  while ((e = readdir(d)) != 0) n++;
  closedir(d);
  if (n < 1) {
    printf("FALHOU readdir vazio\n");
    return 1;
  }

  if (st.st_mtime < 946684800) {
    printf("FALHOU st_mtime %ld\n", (long)st.st_mtime);
    return 1;
  }
  if (access("TPOSX.TXT", F_OK) != 0 || access("NAOEXISTE.TXT", F_OK) == 0) {
    printf("FALHOU access\n");
    return 1;
  }

  /* ftruncate + fsync + O_EXCL */
  fd = open("TPOSX.TXT", O_RDWR);
  if (fd < 0 || ftruncate(fd, 3) != 0 || fsync(fd) != 0) {
    printf("FALHOU ftruncate\n");
    return 1;
  }
  close(fd);
  if (stat("TPOSX.TXT", &st) != 0 || st.st_size != 3) {
    printf("FALHOU tamanho depois do ftruncate\n");
    return 1;
  }
  if (open("TPOSX.TXT", O_WRONLY | O_CREAT | O_EXCL, 0644) >= 0 || errno != EEXIST) {
    printf("FALHOU O_EXCL\n");
    return 1;
  }

  if (unlink("TPOSX.TXT") != 0 || stat("TPOSX.TXT", &st) == 0) {
    printf("FALHOU unlink errno=%d\n", errno);
    return 1;
  }

  /* pastas */
  rmdir("TPDIR");
  if (mkdir("TPDIR", 0755) != 0 || stat("TPDIR", &st) != 0 || !S_ISDIR(st.st_mode)) {
    printf("FALHOU mkdir\n");
    return 1;
  }
  if (mkdir("TPDIR", 0755) == 0 || errno != EEXIST) {
    printf("FALHOU mkdir repetido\n");
    return 1;
  }
  if (rmdir("TPDIR") != 0 || stat("TPDIR", &st) == 0) {
    printf("FALHOU rmdir\n");
    return 1;
  }

  printf("ok: posix (%s, %d ents)\n", cwd, n);
  return 0;
}
