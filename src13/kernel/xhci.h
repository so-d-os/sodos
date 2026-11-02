/* xhci.h - Mouse USB e pendrive atras de um controlador xHCI (USB 3), por polling.
 *
 * PCs atuais nao tem porta PS/2 de mouse nem UHCI: mouse e pendrive ficam num
 * xHCI, nas portas do PC ou atras de um hub USB 2 (muitos notebooks tem um
 * hub soldado na placa). O driver liga o primeiro mouse HID (protocolo boot)
 * e o primeiro disco mass storage (bulk-only, SCSI) que achar e acompanha
 * quando eles sao tirados ou colocados de novo. */
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

/* Liga os controladores (se ainda nao estiverem) e procura um pendrive.
 * 0 com o disco pronto; chamar de novo com ele ligado nao refaz nada. */
int xhci_disk_init(void);

/* Setores de 512 bytes; 0 sem disco (nunca ligado, ou tirado). */
uint32_t xhci_disk_sectors(void);

/* Muda a cada disco ligado: outro pendrive (ou o mesmo de novo) pede remontar. */
uint32_t xhci_disk_gen(void);

/* Fabricante e produto do INQUIRY, ex.: "General UDisk". */
const char* xhci_disk_name(void);

/* Um setor de 512 bytes; buf pode estar em qualquer endereco. 0 = ok. */
int xhci_disk_read(uint32_t lba, void* buf);
int xhci_disk_write(uint32_t lba, const void* buf);

/* Controladores e dispositivos, um por linha (comando usb). */
int xhci_describe(char* out, int max);
