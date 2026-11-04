/* sodos.h - API da camada de plataforma do port do Firefox para o SODOS.
 *
 * Tudo o que o Gecko/NSPR precisa e que o kernel nao oferece diretamente passa
 * por aqui. O nome do arquivo que o port gera e SODOS.DLL (SOX, x32).
 *
 * Estado (M0): threads cooperativos, mmap anonimo, tempo, log e uma conexao
 * TCP por vez. A lista de lacunas e as fases estao em ../docs/.
 */
#ifndef SODOS_H
#define SODOS_H

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SODOS_VERSION "0.1.0-m0"

enum {
  SODOS_LOG_ERROR = 0,
  SODOS_LOG_WARN = 1,
  SODOS_LOG_INFO = 2,
  SODOS_LOG_DEBUG = 3
};

/* Chame uma vez no comeco do programa; DllMain ja chama na carga da DLL. */
void sodos_init(void);
const char* sodos_log_path(void);

void sodos_log(int level, const char* fmt, ...);
void sodos_vlog(int level, const char* fmt, va_list ap);
void sodos_log_flush(void);
/* Registra e encerra o processo com codigo 3 (nao volta). */
void sodos_panic(const char* fmt, ...);

/* Tempo: sodos_now_ms e monotonico (64 bits, sem o wrap de 49 dias do k_ms);
 * sodos_wall_ms e o relogio UTC do RTC com os ms do PIT por cima. */
uint64_t sodos_now_ms(void);
uint64_t sodos_wall_ms(void);
/* Dorme cedendo a CPU para as outras threads e para o WIN. */
void sodos_sleep_ms(unsigned ms);
void sodos_yield(void);
void sodos_mem_info(uint32_t* total, uint32_t* freeb);

/* Rede: o kernel tem UMA conexao TCP por vez. O handle devolvido por
 * sodos_net_connect e >= SODOS_SOCK_FD_BASE e pode ir para poll(). */
#define SODOS_SOCK_FD_BASE 1000

int sodos_net_connect(const char* host, int port);
int sodos_net_send(const void* buf, int len);
/* >0 bytes, 0 fim de fluxo, -1 erro, -2 sem conexao, -3 interrompido (tecla). */
int sodos_net_recv(void* buf, int cap);
/* Bytes ja recebidos e nao lidos; -1 sem conexao. */
int sodos_net_pending(void);
int sodos_net_connected(void);
void sodos_net_close(void);

#ifdef __cplusplus
}
#endif

#endif
