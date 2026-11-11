/* rtw89.h - Realtek RTL8852BE (PCIe, 802.11ax), porte do rtw89 do Linux.
 *
 * Etapa 1: acha a placa no PCI, liga a energia (pwr_on do 8852B), le a
 * versao do chip e o efuse (MAC gravado, tipo de RFE, cristal) e reconhece
 * o firmware lido do pendrive.
 * Etapa 2: prepara a DMAC e os aneis de DMA e envia o firmware pelo CH12.
 * Etapa 3: termina a MAC e confere a resposta do firmware (C2H) no RX.
 * Etapa 4: tabelas do radio (BB/RF), canal e recepcao de beacons. Ainda sem
 * as calibracoes (DACK, IQK, DPK, TSSI) e sem os ganhos do efuse.
 * Etapa 5: estacao no firmware e envio de quadros de gerencia (autenticacao).
 * O log ([rtw]) vai para a COM1 e para um buffer (rtw_log_text). */
#pragma once
#include <stdint.h>

struct rtw_info {
  int bus, dev, fun;
  uint16_t vid, did;
  uint64_t bar;
  int cv;         /* versao do chip: 0 = A, 1 = B, ... */
  int acv;        /* versao do lado analogico */
  int was_on;     /* a MAC ja estava ligada (outro sistema deixou assim) */
  int efuse_ok;   /* autoload do efuse valido */
  uint8_t mac[6];
  int rfe;
  int xtal;
  /* firmware escolhido dentro do arquivo (multi-firmware do rtw89) */
  int fw_major, fw_minor, fw_sub, fw_idx;
  int fw_cv, fw_type;
  uint32_t fw_off, fw_size;
};

/* 0 achou e mapeou o BAR; -1 sem a placa. */
int rtw_probe(struct rtw_info* inf);
/* 1 se a RTL8852BE esta no barramento (sem ligar nada). */
int rtw_present(void);
/* Liga a MAC, le versao e efuse. 0 = ok; <0 = etapa que falhou (rtw_error). */
int rtw_power_on(struct rtw_info* inf);
void rtw_power_off(void);
/* Escolhe o firmware certo para o cv do chip. 0 = ok. */
int rtw_fw_pick(struct rtw_info* inf, const uint8_t* fw, uint32_t len);
/* Etapa 2: DLE/HFC, aneis de DMA do PCIe e envio do firmware escolhido por
 * rtw_fw_pick (file = o arquivo inteiro). 0 = firmware rodando. */
int rtw_fw_load(struct rtw_info* inf, const uint8_t* file);
/* Etapa 3: resto da MAC (DMAC/CMAC), libera os canais de DMA e manda um H2C
 * esperando o ack do firmware pelo anel de RX. 0 = firmware respondeu. */
int rtw_mac_init(void);

/* Etapa 4: tabelas BB/RF, NCTL, caminhos de RX e o canal (20 MHz); deixa o
 * filtro aberto para beacons de qualquer rede. 0 = radio ligado. */
int rtw_radio_init(int channel);
/* So troca o canal (BB/RF e filtro), sem recalibrar: e o que o scan usa
 * depois do primeiro canal. 0 = radio no canal. */
int rtw_radio_channel(int channel);

/* RCK e DACK (calibracoes de inicio; rtw_radio_init ja chama). */
void rtw_rfk_init(void);

/* Etapa 5: porta 0, potencia de TX, o macid 0 como estacao no firmware e a
 * CAM de enderecos (nosso MAC e o BSSID do AP, para a placa dar e receber ACK). */
int rtw_sta_init(const uint8_t* mac, const uint8_t* bssid);
/* Depois da associacao: CAM, firmware e porta no modo infraestrutura. */
int rtw_sta_connect(const uint8_t* mac, const uint8_t* bssid, uint32_t aid);
/* Desfaz o rtw_sta_connect (NO_LINK) para associar de novo depois de uma queda. */
int rtw_sta_disconnect(const uint8_t* mac, const uint8_t* bssid);
/* Quadro de gerencia completo (sem FCS) pela fila B0MG. 0 = entregue a placa. */
int rtw_tx_mgmt(const uint8_t* frame, uint32_t len);
/* Quadro de dados completo (sem FCS) pela fila de melhor esforco (ACH0). */
int rtw_tx_data(const uint8_t* frame, uint32_t len);
int rtw_tx_mgmt_done(void);

/* Um buffer recebido. data/desc apontam para o anel e valem ate a proxima
 * chamada. type: 0 = quadro 802.11, 1 = status de PPDU, 10 = C2H. */
struct rtw_rx {
  int ring;  /* 0 = RXQ, 1 = RPQ */
  int fs, ls, type;
  uint32_t len;
  const uint8_t* desc;
  const uint8_t* data;
};
int rtw_rx_poll(struct rtw_rx* out);
/* Registradores 0x0000-0x1FFFF em 'out' (RTW_DUMP_SIZE bytes). */
#define RTW_DUMP_SIZE 0x20000u
void rtw_dump_regs(uint8_t* out);
/* Indices crus (placa << 16 | nosso) de RXQ, RPQ e ACH0, para o log. */
/* Registradores de erro e enchimento dos aneis, no log. */
void rtw_log_stall(void);
/* Codigo do halt se o firmware parou o DMA esperando o driver (SER); 0 se nao.
 * Ai so religando a placa. */
uint32_t rtw_fw_halted(void);
/* Leitura crua de um registrador (0 sem placa), para o log. */
uint32_t rtw_reg(uint32_t a);
void rtw_ring_state(uint32_t* rxq, uint32_t* rpq, uint32_t* ach0);
/* Texto da ultima falha. */
const char* rtw_error(void);

/* Log do driver (tambem na COM1). wifi grava em U:\NET\WIFI.LOG. */
void rtw_log(const char* s);
void rtw_log_hex(uint32_t v);
const char* rtw_log_text(int* len);
void rtw_log_clear(void);
