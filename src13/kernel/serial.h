/* serial.h - COM1 (0x3F8): log para QEMU -serial file:... ou stdio */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char* s);
void serial_hex32(uint32_t v);
/* Tudo o que ja saiu na COM1 neste boot (ate 192 KB), para gravar em disco. */
const char* serial_log(uint32_t* n);

#endif
