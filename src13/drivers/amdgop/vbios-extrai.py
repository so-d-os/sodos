#!/usr/bin/env python3
"""vbios-extrai.py - remonta o VBIOS da RX 570 a partir do SERIAL.LOG.

O AMDSMC/AMDSDMA.DLL (drivers/amdgop/vbios.c, etapa M) copia o VBIOS para o
log em linhas "[vbios] OOOOO <64 bytes em hex>" ou "[vbios] OOOOO =FF" (64
bytes iguais) e fecha com "[vbios] fim da copia: bytes N fnv1a H".

Uso: python3 vbios-extrai.py E:/LOG/SERIAL.LOG vbios.rom
"""
import re
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    texto = open(sys.argv[1], 'rb').read().decode('latin-1')
    linha = re.compile(r'\[vbios\] ([0-9A-F]{5}) (=([0-9A-F]{2})|([0-9A-F]{128}))\s*$')
    fim = re.compile(r'\[vbios\] fim da copia: bytes ([0-9A-F]{8}) fnv1a ([0-9A-F]{8})')
    img = {}
    tam = fnv = None
    for l in texto.splitlines():
        m = linha.search(l)
        if m:
            o = int(m.group(1), 16)
            img[o] = bytes([int(m.group(3), 16)]) * 64 if m.group(3) else bytes.fromhex(m.group(4))
            continue
        m = fim.search(l)
        if m:
            tam, fnv = int(m.group(1), 16), int(m.group(2), 16)
    if tam is None:
        print('sem a linha "fim da copia" no log')
        return 1
    falta = [o for o in range(0, tam, 64) if o not in img]
    if falta:
        print('faltam %d linhas (primeira em %05X)' % (len(falta), falta[0]))
        return 1
    rom = b''.join(img[o] for o in range(0, tam, 64))
    h = 2166136261
    for b in rom:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    if h != fnv:
        print('fnv1a %08X, o log diz %08X: copia corrompida' % (h, fnv))
        return 1
    open(sys.argv[2], 'wb').write(rom)
    print('%s: %d bytes, fnv1a %08X confere' % (sys.argv[2], tam, h))
    return 0


if __name__ == '__main__':
    sys.exit(main())
