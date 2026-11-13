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
/* Bytes ja recebidos e nao lidos; -1 sem conexao ou fluxo encerrado. */
int net_tcp_pending(void);
void net_tcp_close(void);

/* Soquetes TCP nao bloqueantes, varios ao mesmo tempo (IPs em ordem de host).
 * connect: 1..64 ou < 0. poll: bits 1 ler, 2 escrever, 4 erro, 8 conectado.
 * send: aceitos, -1 EAGAIN, -2 erro. recv: lidos, 0 fim, -1 EAGAIN, -2 erro.
 * err: errno do Linux. pump: retransmite e anda as filas (o poll ja chama). */
int net_sock_connect(uint32_t ip, int port, uint32_t owner);
int net_sock_poll(int h);
int net_sock_err(int h);
int net_sock_send(int h, const void* data, int len);
int net_sock_recv(int h, void* data, int cap, int peek);
void net_sock_close(int h);
/* Fecha todos os soquetes de um programa (pid). */
void net_sock_release_owner(uint32_t owner);
int net_sock_info(int h, uint32_t* lip, int* lport, uint32_t* rip, int* rport);
void net_sock_pump(void);

/* Drena a placa. O servidor HTTP anda junto. */
void net_poll(void);
/* Segunda conexao: escuta. Nao usa o cliente do navegador. */
void net_srv_listen(int port);
/* Fecha a conexao em andamento e para de escutar. */
void net_srv_stop(void);
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

/* Outra placa no lugar da NE2000 (o WiFi). tx manda um quadro Ethernet
 * (0 = ok); rx copia o proximo quadro recebido e devolve o tamanho, 0 sem
 * nada. mac fica valido enquanto a placa estiver ligada. NULL desliga.
 * tick (pode ser NULL) roda a cada net_poll, fora das esperas da pilha:
 * ali a placa vigia o link e reconecta (pode chamar net_dhcp_reboot). */
struct net_link {
  const char* name;
  const uint8_t* mac;
  int (*tx)(const uint8_t* frame, int len);
  int (*rx)(uint8_t* frame, int cap);
  void (*tick)(void);
};
void net_attach(const struct net_link* l);
/* "NE2000", o nome da placa ligada por net_attach, ou NULL sem placa. */
const char* net_link_name(void);
/* Pede endereco por DHCP. 0 = ok; -1 sem placa; -2 ninguem respondeu. */
int net_dhcp(void);
/* INIT-REBOOT: pede de novo o ultimo IP do DHCP (depois de reconectar).
 * 0 = o servidor confirmou; -1 nunca houve DHCP; -2 nao respondeu;
 * -3 recusou (NAK) ou deu outro IP: ai e net_dhcp do zero.
 * O aluguel tambem e renovado sozinho no meio do prazo (net_poll). */
int net_dhcp_reboot(void);
/* Segundos ate renovar o aluguel; 0 sem aluguel (IP fixo ou sem DHCP). */
uint32_t net_dhcp_renew_in(void);
void net_get_addr(uint32_t* ip, uint32_t* mask, uint32_t* gw, uint32_t* dns);
/* Falha de ARP/DNS/TCP com o WiFi ligado (wifi.c grava em U:\NET\WIFI.LOG). */
void wifi_note(const char* what, uint32_t v);

/* Prova de curl.asm depois de drives_init. Nao faz parte do boot normal. */
void net_demo(void);

#endif
