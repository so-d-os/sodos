/*
 * troca.h - Troca do kernel e dos DLLs do sistema sem reiniciar (TROCA.EXE,
 * docs/TROCA.md).
 *
 * Kernel: o TROCA.EXE le o ELF novo (ligado com ld -q, que guarda as
 * relocacoes), copia o .text+.rodata dele para um bloco do kernel abaixo de
 * 2 GB, aponta as referencias a variaveis para as que ja estao na memoria
 * (o estado fica o mesmo) e pede aqui um jmp no comeco de cada funcao antiga
 * para a nova. Programas, janelas e threads continuam rodando.
 *
 * Enquanto os jmps sao escritos as outras CPUs ficam paradas na IPI
 * TROCA_VETOR, e nenhuma CPU nem thread interrompida pode estar no meio dos 5
 * primeiros bytes de uma funcao trocada (troca_apply confere: TROCA_EOCUPADO,
 * e o TROCA.EXE tenta de novo).
 *
 * As variaveis deste lado (troca.c) tem tamanho fixo: o kernel novo que a
 * propria troca aplica usa as mesmas.
 */
#pragma once
#include <stdint.h>

#define TROCA_ABI 1
#define TROCA_VETOR 0xF6          /* IPI que estaciona as outras CPUs */
#define TROCA_BLOCOS_MAX 32       /* kernels aplicados desde o boot */
#define TROCA_TETO 0x7F000000ull  /* codigo e dados novos abaixo de 2 GB (-mcmodel=small) */

#define TROCA_EINVAL   -22
#define TROCA_ENOMEM   -12
#define TROCA_EOCUPADO -16        /* CPU ou thread no meio de uma entrada trocada */
#define TROCA_ETEMPO   -110       /* uma CPU nao parou a tempo (50 ms) */

struct k_troca_patch {
  uint64_t at;                    /* comeco da funcao antiga */
  uint64_t to;                    /* a nova (dentro de text_lo..text_hi) */
};

struct k_troca_copy {             /* feita com as CPUs paradas: variavel que cresceu (/F) */
  uint64_t dst, src, len;
};

struct k_troca_req {
  uint32_t abi;                   /* TROCA_ABI */
  uint32_t npatch;
  const struct k_troca_patch* patch;
  uint32_t ncopy;
  uint32_t build;                 /* build do kernel novo */
  const struct k_troca_copy* copy;
  uint64_t text_lo, text_hi;      /* codigo novo (bloco do k_troca_alloc) */
  void* mapa;                     /* simbolos do kernel novo, para a proxima troca */
  uint32_t mapa_size;
  uint32_t pad;
  uint64_t depois;                /* troca_depois do kernel novo; 0 = nao chama */
  char origem[96];                /* ELF de onde veio */
  /* saida */
  uint64_t rip;                   /* quem impediu (TROCA_EOCUPADO) */
  int32_t cpu;                    /* CPU que impediu ou nao parou; -1 = thread fora da CPU */
  uint32_t us;                    /* microssegundos com as outras CPUs paradas */
};

struct k_troca_info {
  uint32_t abi;
  uint32_t geracao;               /* kernels aplicados desde o boot */
  uint32_t build_boot;            /* build que o GRUB carregou */
  uint32_t build;                 /* build rodando agora (o ultimo aplicado) */
  uint64_t boot_lo, boot_hi;      /* imagem do boot: 0x01000000 .. __bss_end */
  void* mapa;                     /* simbolos atuais; 0 na geracao 0 (use o ELF do boot) */
  uint32_t mapa_size;
  uint32_t cpus;
  char origem[96];                /* ELF da ultima troca ("" na geracao 0) */
};

/* ---- kernel ---- */
/* isr.c, vetor TROCA_VETOR: a CPU fica aqui enquanto o kernel e trocado. */
void troca_ipi(uint64_t rip);
/* 1 se a e codigo do kernel: a imagem do boot ou um bloco de troca. */
int troca_texto(uint64_t a);
/* Bloco de troca que contem a (1..n), 0 se nenhum. */
int troca_bloco(uint64_t a);
uint64_t troca_bloco_lo(int n);

int troca_info(struct k_troca_info* out);
/* Memoria do kernel (dono 0) abaixo de TROCA_TETO, alinhada a 4 KB e zerada. */
void* troca_alloc(uint32_t bytes);
void troca_free(void* p);
int troca_apply(struct k_troca_req* r);
/* Texto para o TROCA sem argumentos: kernel, WINE.DLL e drivers. */
int troca_estado(char* out, int max);
/* Troca de um DLL do sistema: WINE.DLL, FAT32.DLL, LINKEXT.DLL. msg recebe o
 * resultado; 0 = trocou, < 0 = nao. */
int troca_dll(const char* nome, char* msg, int max);
/* O kernel novo chama depois de aplicado (o TROCA.EXE passa o endereco). */
void troca_depois(uint32_t build_antes, uint32_t geracao_nova);
