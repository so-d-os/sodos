/* hello64.c - primeiro programa SOX2/LP64 do SODOS.
 *
 * Compilado com gcc -m64 -mcmodel=large e ligado pelo link64. Usa so
 * k_write/k_exit (imports do KERNEL); nao depende de LIBCX64 ainda. */
#include <so.h>

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  k_write("HELLO64: ABI LP64 no SODOS\n", 27);
  k_dbg("HELLO64: ABI LP64 no SODOS\n", 27);
  return 0;
}
