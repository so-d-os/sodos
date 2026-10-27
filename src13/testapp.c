// testapp.c - outro exemplo de executável
typedef void (*puts_fn)(const char*);

int testapp_main(puts_fn puts) {
  puts("=== Test Application ===\n");
  puts("Este e um modulo de teste.\n");
  puts("O sistema de carregamento de executaveis esta funcionando!\n");
  puts("========================\n");
  return 0;
}
