/* sodos_api.h - Exports do kernel do SODOS que o WSL.EXE usa (KERNEL.IMP).
 * Os que nao estao no so.h ficam declarados aqui. */
#ifndef WSL_SODOS_API_H
#define WSL_SODOS_API_H
#include <so.h>

/* RAM acima de 4 GB, em identidade (endereco == fisico). Teto: SET HIMEM. */
void* k_hmalloc(unsigned long long n, unsigned long long align);
void k_hfree(void* p);
void k_hminfo(unsigned long long* quota, unsigned long long* used, unsigned long long* total);
/* SSD NVMe cru, so leitura (kernel/nvme.c). */
int k_nvme_raw_read(unsigned long long lba, unsigned count, void* buf);
unsigned long long k_nvme_raw_sectors(void);
/* Teclado cru: Ctrl+C nao para o programa e Ctrl+letra chega como 1..26. */
void k_kbd_raw(int on);
/* NAT da VM (net/net.c net_tap_*): pacotes IP para o IP do SODOS. */
int k_net_tap_open(int lo, int hi);
void k_net_tap_close(void);
int k_net_tap_recv(void* buf, int cap);
int k_net_tap_send(const void* ip, int len);
void k_net_addr(unsigned* ip, unsigned* mask, unsigned* gw, unsigned* dns);
/* Memoria sem dono (sobrevive ao programa); a caixa do WSL fica nela. */
void* k_malloc_shared(unsigned n);
void k_free_shared(void* p);
/* Registro de nomes do kernel: o servico publica, o cliente acha. */
int k_svc_set(const char* name, void* p);
void* k_svc_get(const char* name);
/* 1 se este programa esta numa tela de texto ao fundo. */
int k_vt_bg(void);

#endif
