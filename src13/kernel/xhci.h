/* xhci.h - Mouse USB atras de um controlador xHCI (USB 3), por polling.
 *
 * PCs atuais nao tem porta PS/2 de mouse nem UHCI: o mouse USB fica num xHCI.
 * O driver liga o primeiro mouse HID (protocolo boot) que achar nas portas
 * raiz e acompanha quando ele e tirado ou colocado de novo. Sem hubs. */
#pragma once
#include <stdint.h>

/* Procura os xHCI do PCI e o mouse. ps2_ok != 0 quando um mouse PS/2
 * respondeu: nesse caso um controlador que o firmware ainda usa (emulacao
 * PS/2 de teclado e mouse USB) fica com o firmware. Devolve 0 com mouse pronto.
 * Chamar de novo com o mouse ja ligado nao refaz nada. */
int xhci_mouse_init(int ps2_ok);

/* 1 com um relatorio novo. dx/dy em pixels (dy positivo = para baixo),
 * dz como no HID (positivo = roda para cima), btn: bit 0 esq, 1 dir, 2 meio. */
int xhci_mouse_poll(int* dx, int* dy, int* dz, int* btn);
