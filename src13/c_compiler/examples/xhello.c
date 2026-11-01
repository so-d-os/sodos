/* xhello.c - hello via gcc -mx32 (SysV). So chama k_* do KERNEL (mesma ABI). */
#include <so.h>

int main(int argc, char** argv) {
  int i;
  k_puts("ola gcc-x32");
  k_putc('\n');
  for (i = 0; i < argc; i++) {
    k_puts("  ");
    k_puts(argv[i] ? argv[i] : "(null)");
    k_putc('\n');
  }
  return 0;
}
