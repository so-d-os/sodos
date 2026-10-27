/* crt0.c - Ponto de entrada de todo .EXE em C (vai como CRT0.OBJ no link). */
int main(int argc, char** argv);
void exit(int code);
void __so_libc_start(void);

void _start(int argc, char** argv) {
  __so_libc_start();
  exit(main(argc, argv));
}
