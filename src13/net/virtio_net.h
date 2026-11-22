/* virtio_net.h - placa virtio-net do QEMU/KVM (PCI 1AF4:1000, interface
 * legada por portas de E/S). Polling, sem IRQ. Os quadros vao e voltam por
 * DMA em dois aneis (vring): uma saida para o QEMU por lote, contra centenas
 * por quadro na NE2000. Liga como placa-base da pilha (net_attach_base). */
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

/* Acha a placa e liga. 0 = ligada; -1 = nao ha virtio-net (ou falhou). */
int virtio_net_start(void);

/* 1 se a placa esta ligada. */
int virtio_net_present(void);

/* Linhas do comando "net": MAC e contadores. */
void virtio_net_status(void);

#endif
