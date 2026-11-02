/* rtw89.h - Realtek RTL8852BE (PCIe, 802.11ax), porte do rtw89 do Linux.
 *
 * Etapa 1: acha a placa no PCI, liga a energia (pwr_on do 8852B), le a
 * versao do chip e o efuse (MAC gravado, tipo de RFE, cristal) e reconhece
 * o firmware lido do pendrive.
 * Etapa 2: prepara a DMAC e os aneis de DMA e envia o firmware pelo CH12.
 * Ainda nao liga o radio (BB/RF).
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
/* Liga a MAC, le versao e efuse. 0 = ok; <0 = etapa que falhou (rtw_error). */
int rtw_power_on(struct rtw_info* inf);
void rtw_power_off(void);
/* Escolhe o firmware certo para o cv do chip. 0 = ok. */
int rtw_fw_pick(struct rtw_info* inf, const uint8_t* fw, uint32_t len);
/* Etapa 2: DLE/HFC, aneis de DMA do PCIe e envio do firmware escolhido por
 * rtw_fw_pick (file = o arquivo inteiro). 0 = firmware rodando. */
int rtw_fw_load(struct rtw_info* inf, const uint8_t* file);
/* Texto da ultima falha. */
const char* rtw_error(void);

/* Log do driver (tambem na COM1). wifi grava em U:\NET\WIFI.LOG. */
void rtw_log(const char* s);
void rtw_log_hex(uint32_t v);
const char* rtw_log_text(int* len);
void rtw_log_clear(void);
