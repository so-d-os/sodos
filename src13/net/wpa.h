/* wpa.h - WPA2-PSK (CCMP) para o WiFi: chaves, handshake de 4 vias e CCMP.
 *
 * Nao depende da placa. O driver entrega os quadros EAPOL que chegam
 * (sem o cabecalho 802.11 nem o LLC) e manda os que wpa_eapol_rx devolve.
 * Os dados cifram e decifram com wpa_ccmp_* usando o TK (unicast) ou o GTK
 * (broadcast) que o handshake deixou em wpa_sm. */
#pragma once
#include <stdint.h>

void wpa_sha1(const void* data, int len, uint8_t out[20]);
void wpa_hmac_sha1(const uint8_t* key, int klen, const void* msg, int mlen, uint8_t out[20]);
/* PMK = PBKDF2-HMAC-SHA1(senha, ssid, 4096, 32). */
void wpa_pmk(const char* pass, const uint8_t* ssid, int ssid_len, uint8_t pmk[32]);

typedef struct {
  uint32_t rk[44];
} wpa_aes;
void wpa_aes_init(wpa_aes* c, const uint8_t key[16]);
void wpa_aes_enc(const wpa_aes* c, const uint8_t in[16], uint8_t out[16]);
void wpa_aes_dec(const wpa_aes* c, const uint8_t in[16], uint8_t out[16]);
/* RFC 3394. n = bytes cifrados (multiplo de 8, >= 16); out recebe n-8. 0 = ok. */
int wpa_aes_unwrap(const uint8_t kek[16], const uint8_t* in, int n, uint8_t* out);

/* CCMP sobre um MPDU inteiro: hdr (hlen bytes, ja com o bit Protected),
 * depois os 8 bytes do cabecalho CCMP, depois os dados, depois o MIC de 8.
 * encrypt: mpdu tem hlen+8+plen+8 bytes de espaco; escreve cabecalho CCMP,
 * cifra e poe o MIC. decrypt: plen = tamanho dos dados (sem CCMP e MIC);
 * decifra no lugar. 0 = ok, -1 = MIC errado. */
void wpa_ccmp_encrypt(const uint8_t tk[16], uint64_t pn, int keyid,
                      uint8_t* mpdu, int hlen, int plen);
int wpa_ccmp_decrypt(const uint8_t tk[16], uint8_t* mpdu, int hlen, int plen, uint64_t* pn_out);

/* RSN IE que vai na associacao e no msg 2/4: CCMP/CCMP/PSK. */
extern const uint8_t wpa_rsn_ie[22];

enum { WPA_IDLE = 0, WPA_PTK_START, WPA_PTK_DONE, WPA_DONE };

struct wpa_sm {
  uint8_t pmk[32];
  uint8_t aa[6];   /* MAC do AP */
  uint8_t spa[6];  /* nosso MAC */
  uint8_t anonce[32];
  uint8_t snonce[32];
  uint8_t ptk[48]; /* KCK 0..15, KEK 16..31, TK 32..47 */
  uint8_t gtk[32];
  int gtk_len;
  int gtk_idx;
  uint8_t replay[8];
  int have_replay;
  int state;
  uint64_t tx_pn;  /* proximo PN de unicast */
};

void wpa_sm_init(struct wpa_sm* sm, const uint8_t pmk[32], const uint8_t aa[6],
                 const uint8_t spa[6]);
/* Trata um EAPOL (802.1X) vindo do AP. Quando ha resposta, escreve em out
 * (ate cap bytes) e devolve o tamanho; 0 sem resposta; -1 quadro invalido
 * ou MIC errado (senha errada). */
int wpa_eapol_rx(struct wpa_sm* sm, const uint8_t* p, int len, uint8_t* out, int cap);

/* Vetores de teste (SHA1, PBKDF2 do 802.11i, AES, unwrap, CCMP).
 * 0 = tudo certo; senao, o numero do teste que falhou. */
int wpa_selftest(void);
