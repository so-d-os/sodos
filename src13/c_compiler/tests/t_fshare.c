/* Testa na posix-lite os varios fds no mesmo arquivo (como o SQLite faz com
 * as conexoes do mesmo banco): todos veem o mesmo conteudo, o fsync de um
 * nao apaga o que outro escreveu, O_TRUNC e unlink com o arquivo aberto.
 * (dup fica de fora: o da libc, misc.c, e um stub ENOSYS e vence na ligacao.) */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void check(int ok, const char* what) {
  if (!ok) {
    printf("FALHOU %s\n", what);
    fails++;
  }
}

int main(void) {
  int a, b, c, d;
  char buf[64];
  struct stat st;

  unlink("TFSH.DB");
  a = open("TFSH.DB", O_RDWR | O_CREAT, 0644);
  b = open("TFSH.DB", O_RDWR);
  check(a >= 0 && b >= 0, "open a/b");

  /* a escreve; b le o mesmo conteudo sem fsync */
  check(write(a, "AAAA", 4) == 4, "write a");
  memset(buf, 0, sizeof(buf));
  check(read(b, buf, 4) == 4 && memcmp(buf, "AAAA", 4) == 0, "b ve o que a escreveu");

  /* b escreve mais adiante; o fsync de a grava as duas partes */
  check(lseek(b, 8, SEEK_SET) == 8, "lseek b");
  check(write(b, "BBBB", 4) == 4, "write b");
  check(fstat(a, &st) == 0 && st.st_size == 12, "fstat a ve o tamanho de b");
  check(fsync(a) == 0, "fsync a");
  close(a);
  close(b);

  c = open("TFSH.DB", O_RDONLY);
  memset(buf, 0, sizeof(buf));
  check(read(c, buf, 64) == 12, "tamanho no disco");
  check(memcmp(buf, "AAAA\0\0\0\0BBBB", 12) == 0, "conteudo no disco");

  /* stat pelo caminho com um fd ainda aberto e sujo */
  d = open("TFSH.DB", O_RDWR | O_APPEND);
  check(write(d, "CC", 2) == 2, "append");
  check(stat("TFSH.DB", &st) == 0 && st.st_size == 14, "stat ve o tamanho da memoria");

  close(d);
  close(c);

  /* O_TRUNC num arquivo ja aberto zera para todos */
  a = open("TFSH.DB", O_RDWR);
  b = open("TFSH.DB", O_RDWR | O_TRUNC);
  check(fstat(a, &st) == 0 && st.st_size == 0, "O_TRUNC vale para o outro fd");
  close(b);
  close(a);

  /* unlink com o arquivo aberto: ainda le, e o close nao recria */
  a = open("TFSH.J", O_RDWR | O_CREAT, 0644);
  check(write(a, "journal", 7) == 7, "write journal");
  check(unlink("TFSH.J") == 0, "unlink aberto");
  check(lseek(a, 0, SEEK_SET) == 0 && read(a, buf, 7) == 7, "le depois do unlink");
  close(a);
  check(stat("TFSH.J", &st) != 0, "close nao recriou o apagado");
  b = open("TFSH.J", O_RDONLY);
  check(b < 0 && errno == ENOENT, "open do apagado falha");

  unlink("TFSH.DB");
  if (fails) return 1;
  printf("t_fshare ok\n");
  return 0;
}
