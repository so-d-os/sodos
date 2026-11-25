/*
 * audio.h - Som do SODOS (docs/AUDIO.md).
 *
 * Opcional, como o WINE e o LINKEXT: SET AUDIO=1 liga, SET AUDIO=0 (ou sem a
 * variavel) desliga. Desligado, o driver atual e o nulo, que fica no kernel:
 * nenhum DLL carregado, nenhuma memoria, nenhum acesso a placa e nada no
 * IRQ0 alem de testar uma variavel. Os programas chamam k_audio_* do mesmo
 * jeito; o nulo aceita e joga fora (como o NUL do DOS).
 *
 * SET AUDIO=1 carrega PATHSODOS\DRV\AUDIO.DLL (depois D:\SODOS\DRV e
 * Z:\SODOS\DRV): o driver HD Audio (Intel HDA, o som dos PCs desde 2004,
 * tambem o do QEMU e do VirtualBox). SET AUDIO=X.DLL carrega outro driver da
 * mesma pasta. SET AUDIO=0 para o som e descarrega o DLL na hora.
 *
 * O DLL roda no contexto do kernel (como FAT32.DLL e LINKEXT.DLL: -m64, sem
 * SSE, sem libc, sem importar KERNEL); tudo vem em audio_host. Ele recebe
 * sempre PCM de 16 bits com sinal, 2 canais: o kernel converte 8 bits, mono e
 * taxas que a placa nao tem.
 *
 * A placa toca um buffer circular. Quem escreve e o programa (k_audio_write);
 * o que ja tocou o kernel zera (service, a cada ~10 ms pelo IRQ0, so enquanto
 * ha som), entao um programa que para de mandar amostras fica em silencio e,
 * depois de um tempo parado, a placa para.
 */
#pragma once
#include <stdint.h>

#define AUDIO_ABI 1
#define AUDIO_SYM "audio_driver"
#define AUDIO_PATH_MAX 128

struct audio_host {
  uint32_t abi;                      /* AUDIO_ABI */
  void (*log)(const char* s);        /* serial, prefixo [audio] */
  void (*log_hex)(uint32_t v);
  uint32_t (*pci_read32)(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off);
  void (*pci_write32)(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off, uint32_t v);
  /* BAR de memoria em identidade, sem cache. 0 = ok. */
  int (*mmio_map)(uint64_t phys, uint64_t len);
  /* Memoria zerada com endereco fisico = virtual, abaixo de 4 GB; align
   * potencia de 2 (0 = 16). E do kernel: nao some com o fim de um programa. */
  void* (*alloc)(uint32_t bytes, uint32_t align);
  void (*free)(void* p);
  uint32_t (*ms)(void);              /* relogio em ms, para timeouts */
};

struct audio_ops {
  uint32_t abi;                      /* AUDIO_ABI */
  const char* name;                  /* "HDA" */
  /* Acha a placa e liga o codec. 0 = pronto; <0 = sem placa (o kernel chama
   * fini e descarrega). Nao toca nada ainda. */
  int (*init)(const struct audio_host* h);
  /* Para tudo e solta a memoria. Depois dele o DLL e descarregado. */
  void (*fini)(void);
  /* Abre o fluxo: 16 bits, 2 canais, rate Hz. 0 ok; -1 taxa que a placa nao
   * tem (o kernel tenta 48000 e converte); outro <0 = erro. */
  int (*start)(uint32_t rate);
  /* Copia ate bytes (multiplo de 4) para a fila; devolve quantos aceitou
   * (0 = fila cheia agora). A placa comeca a tocar no primeiro write. */
  int (*write)(const void* buf, int bytes);
  /* Bytes na fila ainda nao tocados. */
  int (*pending)(void);
  /* Zera o que ja tocou e para a placa quando nada mais toca ha um tempo.
   * 1 = ainda ha som (chamar de novo); 0 = parada. */
  int (*service)(void);
  /* Para na hora (o que estava na fila se perde). */
  void (*stop)(void);
  /* Volume da saida, 0..100. pct < 0: so le. Devolve o atual. */
  int (*volume)(int pct);
  /* Placa, codec, saidas e estado em texto (comando AUDIO). Bytes escritos. */
  int (*info)(char* out, int max);
};

typedef const struct audio_ops* (*audio_entry_fn)(void);

/* ---- lado do kernel (kernel/audio.c) ---- */
/* env.c chama quando AUDIO ou PATHSODOS mudam: carrega ou descarrega. */
void audio_env_changed(void);
/* IRQ0 (irq.c): so com audio_tick_on != 0, ja com o BKL do mundo. */
extern volatile int audio_tick_on;
void audio_irq_tick(uint64_t ms);
/* Fim de um processo (module.c): se o som era dele, para. */
void audio_pid_gone(uint32_t pid);

/* Exports para os programas (kexports.c, c_compiler/include/so.h). */
/* Abre o som: rate Hz (8000..192000), channels 1 ou 2, bits 8 (sem sinal) ou
 * 16 (com sinal). 0 ok; -1 parametros; -2 outro programa esta tocando;
 * -3 a placa recusou. Com AUDIO=0 abre o driver nulo (0). */
int k_audio_open(int rate, int channels, int bits);
/* Manda amostras no formato do open; espera ter lugar na fila. Devolve bytes
 * aceitos (todos), ou <0 (sem open, placa parou de andar). */
int k_audio_write(const void* buf, int bytes);
/* wait != 0: espera tocar o que esta na fila. Depois solta o som. */
int k_audio_close(int wait);
/* Bytes (no formato do open) ainda na fila. 0 com o nulo. */
int k_audio_pending(void);
/* Volume 0..100; pct < 0 so le. -1 sem driver (AUDIO=0). */
int k_audio_volume(int pct);
/* Tom de hz por ms, sem esperar (o IRQ0 alimenta). 0 ok; -1 desligado ou
 * a placa esta com um programa. */
int k_audio_beep(int hz, int ms);
/* Texto do comando AUDIO. Bytes escritos. */
int k_audio_info(char* out, int max);
