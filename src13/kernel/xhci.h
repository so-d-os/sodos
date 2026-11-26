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

/* Teclado USB (protocolo boot) como scancodes set 1 na fila do PS/2.
 * xhci_kbd_init: liga os controladores e procura o teclado (boot UEFI).
 * xhci_kbd_pump: chamado por ps2in_pump; recolhe teclas e repete a segurada. */
int xhci_kbd_init(void);
int xhci_kbd_ready(void);
void xhci_kbd_pump(void);

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

/* Webcam USB (UVC, classe 0E): formatos e tamanhos, um por linha. */
int xhci_cam_describe(char* out, int max);

/* Um quadro da webcam. want_w: largura maxima (0 = ate 640), ou tamanho
 * exato com a altura nos 16 bits de cima (largura | altura << 16); skip: quadros
 * descartados antes (a exposicao automatica leva alguns). *kind: 1 = MJPEG
 * (JPEG sem as tabelas de Huffman, as vezes), 2 = YUY2 (w * h * 2 bytes).
 * *data e do driver e vale ate a proxima captura. 0 = ok; senao err diz. */
int xhci_cam_grab(int want_w, int skip, const uint8_t** data, uint32_t* len, int* kind, int* w,
                  int* h, char* err, int emax);

/* Mapeia um BAR de PCI em identidade, sem cache (outros drivers usam). 0 = ok. */
int pci_mmio_map(uint64_t phys, uint64_t len);

/* RIT (video/visual.c): le os relatorios sem esperar nada; 0 = nada ou o
 * driver ocupado. xhci_mouse_maint faz o resto (reset, portas) pelo mundo. */
int xhci_rit_poll(int* dx, int* dy, int* dz, int* btn);
void xhci_mouse_maint(void);
int xhci_rit_wanted(void);
/* MSI ao LAPIC apic no vetor vec (0 = sem MSI); xhci_rit_ack na interrupcao. */
int xhci_rit_irq(uint32_t apic, uint8_t vec);
void xhci_rit_ack(void);
