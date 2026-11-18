/* elf.h (port do Dart) - o do port do Firefox + o que o glibc tem a mais e a
 * VM usa (os_linux.cc: build id do snapshot). */
#ifndef SODOS_DART_ELF_H
#define SODOS_DART_ELF_H
#include_next <elf.h>
#ifndef ELF_NOTE_GNU
#define ELF_NOTE_GNU "GNU"
#endif
#endif
