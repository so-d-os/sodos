/* i2chid.h - touchpad (ou mouse) HID sobre I2C, nos notebooks sem PS/2.
 *
 * Notebooks atuais ligam o touchpad num controlador I2C (DesignWare) e nao
 * no 8042: a porta auxiliar PS/2 existe, mas nada responde nela. O driver acha
 * o dispositivo pelo ACPI (PNP0C50 com I2cSerialBus e GpioInt no _CRS), le o
 * descritor HID e usa a coleção de mouse do report descriptor (o modo que o
 * touchpad tem depois do reset: deslocamento relativo, botoes e roda).
 * Por enquanto: controladores I2C e GPIO da AMD (AMDI0010 / AMDI0030). */
#pragma once

/* Procura e liga o touchpad. 0 = pronto; chamar de novo nao refaz nada. */
int i2chid_init(void);

/* 1 com um relatorio novo, no formato do xhci_mouse_poll: dx/dy em pixels
 * (dy positivo = para baixo), dz como no HID (positivo = roda para cima),
 * btn: bit 0 esq, 1 dir, 2 meio. Nao bloqueia: 0 se nada ou ocupado. */
int i2chid_poll(int* dx, int* dy, int* dz, int* btn);

/* 1 com o touchpad ligado: a RIT deve olhar a cada 1 ms (sem IRQ). */
int i2chid_wanted(void);

/* Uma linha sobre o estado (comando mouse, diagnostico). */
const char* i2chid_status(void);
