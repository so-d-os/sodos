// hello.c - exemplo de executável
// Função que será chamada quando o executável for carregado
typedef void (*puts_fn)(const char*);

int hello_main(puts_fn puts) {
  puts("Hello, World!\n");
  puts("Este e um executavel carregado dinamicamente!\n");
  puts("Executavel funcionando corretamente.\n");
  return 0;
}
