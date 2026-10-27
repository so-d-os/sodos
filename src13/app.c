// app.c (binário flat 32-bit) - placeholder module
typedef void (*puts_fn)(const char*);

int app_main(puts_fn puts) {
  puts("Hello from app.bin module!\n");
  return 0;
}
