/* nat.h - Rede da VM do WSL: NAT para o IP do SODOS (ver nat.c). */
#ifndef WSL_NAT_H
#define WSL_NAT_H
#include <stdint.h>

#define NAT_GUEST_IP 0x0A00030Fu /* 10.0.3.15 */
#define NAT_GW_IP 0x0A000302u    /* 10.0.3.2 */
#define NAT_MASK 0xFFFFFF00u

extern const uint8_t nat_guest_mac[6];

/* Liga o desvio no kernel. 0 = rede ok (dns preenchido); <0 sem rede. */
int nat_init(uint32_t* dns);
void nat_close(void);
/* Quadro Ethernet que a VM mandou. */
void nat_from_guest(const uint8_t* frame, int len);
/* Proximo quadro Ethernet para a VM; 0 se nao ha. */
int nat_to_guest(uint8_t* frame, int cap);

#endif
