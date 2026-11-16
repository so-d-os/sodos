/* r8125.h - Realtek RTL8125 2.5GbE (PCI 10EC:8125), a placa onboard do PC
 * real. Polling, sem IRQ. Liga como placa-base da pilha (net_attach_base). */
#ifndef R8125_H
#define R8125_H

#include <stdint.h>

/* Acha a placa, inicializa, espera o link e pede DHCP. Mostra tudo na tela
 * (o PC real nao tem serial). 0 = placa ligada (mesmo sem link). */
int r8125_start(void);

/* Linhas do comando "net eth": modelo, MAC, link, contadores. */
void r8125_status(void);

/* 1 se a placa esta ligada (achada e inicializada). */
int r8125_present(void);

#endif
