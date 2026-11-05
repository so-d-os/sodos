/* crt0.c - Ponto de entrada de todo .EXE em C (vai como CRT0.OBJ no link).
 * Chama os construtores C++ e os destrutores registrados por __cxa_atexit.
 * A LIBCX tem uma versao vazia; o runtime C++ define a forte no .EXE. */
int main(int argc, char** argv);
void exit(int code);
void __so_libc_start(void);
void __sodos_ctors(void);
void __sodos_dtors(void);

/* Topo da pilha para o runtime (pthread_attr_getstack), como no crt0_64.S. */
char* __sodos_stack_top;

void _start(int argc, char** argv) {
  int rc;
  __sodos_stack_top = (char*)__builtin_frame_address(0);
  __so_libc_start();
  __sodos_ctors();
  rc = main(argc, argv);
  __sodos_dtors();
  exit(rc);
}
