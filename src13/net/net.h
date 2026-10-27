/* IPv4 sobre NE2000 (QEMU -device ne2k_pci e a mesma placa no v86).
 * IPv6 fica para depois: o mesmo quadro Ethernet, ethertype 0x86DD. */
#ifndef NET_H
#define NET_H

#include <stdint.h>

void net_init(void);

int net_ready(void);
/* 0 quando o PIT nao respondeu: tempos de ping ficam aproximados. */
int net_has_clock(void);
/* Aceita IPv4 em texto ou nome (DNS do SLIRP). 0 = ok. */
int net_resolve(const char* host, uint32_t* ip);
/* ICMP echo. 0 = resposta, -1 = falha ao enviar (ARP), -2 = tempo esgotado. */
int net_ping(uint32_t ip, uint16_t seq, int size, uint32_t timeout_ms,
             uint32_t* rtt_ms, uint8_t* ttl);
/* Espera processando quadros (responde ARP e ICMP no meio tempo). */
void net_delay(uint32_t ms);

/* Uma conexao TCP por vez (cliente). connect: 0 = ok. recv: >0 bytes,
 * 0 = fechada pelo servidor, -1 = erro ou 8 s sem dados. */
int net_tcp_connect(const char* host, int port);
int net_tcp_send(const void* data, int len);
int net_tcp_recv(void* data, int cap);
void net_tcp_close(void);

/* Drena a placa. O servidor HTTP anda junto. */
void net_poll(void);
/* Segunda conexao: escuta. Nao usa o cliente do navegador. */
void net_srv_listen(int port);
/* >0 pedido HTTP completo, 0 ainda nao, -1 estouro. */
int net_srv_take(char* buf, int cap);
/* Sobe a cada conexao nova. A resposta antiga nao deve ser enviada depois. */
unsigned net_srv_gen(void);
/* Envia e, se fin, fecha. 0 = ok. */
int net_srv_write(const void* data, int len, int fin);
/* Envia a resposta e fecha. 0 = ok. */
int net_srv_send(const void* data, int len);
/* Relogio em ms do PIT; so anda enquanto alguem consulta (polling). */
uint32_t net_ms(void);

/* Prova de curl.asm depois de drives_init. Nao faz parte do boot normal. */
void net_demo(void);

#endif
