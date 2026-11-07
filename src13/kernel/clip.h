/* clip.h - Area de transferencia do kernel: uma so para o WIN e o DOS. */
#pragma once

#define CLIP_MAX 4096

/* Guarda n bytes de s (corta em CLIP_MAX - 1). n < 0: ate o '\0'. */
void clip_set(const char* s, int n);
/* Copia o texto para out (com '\0') e devolve quantos bytes copiou. */
int clip_get(char* out, int max);
/* Tamanho do texto guardado. */
int clip_len(void);
