/* p384.h - verificacao ECDSA na curva secp384r1 (sem geracao de chave). */
#ifndef P384_H
#define P384_H

#include <stddef.h>
#include <stdint.h>

/* 0 = assinatura valida. sig e r||s (48+48), pub e X||Y (48+48), sem o 0x04.
 * hash pode ter 32 ou 48 bytes: o inteiro usa os bits da esquerda. */
int p384_ecdsa_verify(const uint8_t sig[96], const uint8_t pub[96],
                      const uint8_t* hash, size_t hlen);

#endif
