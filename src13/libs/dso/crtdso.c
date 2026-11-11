/* crtdso.c - entra em todo executavel dinamico e em toda .so.
 *
 * __dso_handle identifica o modulo para __cxa_atexit: o g++ registra os
 * destrutores de objetos globais com o endereco dele. E oculto, um por modulo. */
void* __dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;
