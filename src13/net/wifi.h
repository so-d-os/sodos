/* wifi.h - WiFi do SODOS: WIFI.CFG, a placa (rtw89) e o WPA2 (wpa).
 *
 * net wifi start le U:\NET\WIFI.CFG (ssid=, psk=, bssid=, channel=, ip=dhcp)
 * e o firmware U:\NET\RTW8852B.BIN, liga a placa e segue as etapas que o
 * driver ja tem. Cada etapa sai na tela; o log vai para WIFI.LOG. */
#pragma once
#include <stdint.h>

#define WIFI_CFG_PATH "U:\\NET\\WIFI.CFG"
#define WIFI_FW_PATH  "U:\\NET\\RTW8852B.BIN"
#define WIFI_LOG_PATH "U:\\NET\\WIFI.LOG"

struct wifi_cfg {
  char ssid[33];
  int ssid_len;
  char psk[64];
  uint8_t bssid[6];
  int has_bssid;
  int channel;
  int dhcp;
  uint32_t ip, mask, gw, dns;  /* ip= fixo, quando nao e dhcp */
};

/* 0 = ok. Erro de leitura -1, sem ssid/psk -2. */
int wifi_cfg_load(const char* path, struct wifi_cfg* c);
/* 0 = ok; <0 parou numa etapa (a mensagem ja saiu na tela). */
int wifi_start(const char* cfg_path);
void wifi_stop(void);
void wifi_status(void);
