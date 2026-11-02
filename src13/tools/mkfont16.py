#!/usr/bin/env python3
"""Gera video/font8x16_data.c: fonte 8x16 em ordem CP850 a partir de um PSF.

Uso: python3 tools/mkfont16.py /usr/share/consolefonts/Lat15-VGA16.psf.gz video/font8x16_data.c

CP850 e a pagina do DOS em portugues: tem ã õ Á Ê Ó etc., que a CP437
da BIOS nao tem. O kernel usa esta fonte no modo texto VGA e no fbcon.
Glifos que o PSF nao tem viram '?'.
"""
import gzip
import os
import sys

# 0x01..0x1F: simbolos graficos do BIOS (iguais na CP437 e na CP850)
CP437_LO = "☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"


def cp850_char(i):
    if i == 0:
        return " "
    if i < 0x20:
        return CP437_LO[i - 1]
    if i < 0x7F:
        return chr(i)
    if i == 0x7F:
        return "⌂"
    return bytes([i]).decode("cp850")


def load_psf(path):
    data = gzip.open(path).read() if path.endswith(".gz") else open(path, "rb").read()
    if data[:2] == b"\x36\x04":  # PSF1
        mode, size = data[2], data[3]
        n = 512 if mode & 1 else 256
        glyphs = [data[4 + i * size:4 + (i + 1) * size] for i in range(n)]
        table = data[4 + n * size:]
        umap = {}
        pos = 0
        for g in range(n):
            if not (mode & 2):
                break
            while pos + 1 < len(table):
                u = table[pos] | (table[pos + 1] << 8)
                pos += 2
                if u == 0xFFFF:
                    break
                if u != 0xFFFE:
                    umap.setdefault(chr(u), g)
        return size, glyphs, umap
    raise SystemExit("so PSF1 e suportado")


def main():
    src, out = sys.argv[1], sys.argv[2]
    height, glyphs, umap = load_psf(src)
    if height != 16:
        raise SystemExit("fonte precisa ter 16 linhas")
    q = umap.get("?", 0x3F)
    lines = [
        "/* Gerado por tools/mkfont16.py a partir de %s (dominio publico). */" % os.path.basename(src),
        "/* Fonte 8x16 em ordem CP850 (DOS em portugues); modo texto VGA e fbcon. */",
        "#include <stdint.h>",
        "const uint8_t font8x16_cp850[256][16] = {",
    ]
    missing = 0
    for i in range(256):
        g = umap.get(cp850_char(i))
        if g is None:
            g = q
            missing += 1
        row = ", ".join("0x%02X" % b for b in glyphs[g])
        lines.append("  {%s}, /* 0x%02X */" % (row, i))
    lines.append("};")
    open(out, "w").write("\n".join(lines) + "\n")
    print("%s: %d glifos sem correspondencia viraram '?'" % (out, missing))


if __name__ == "__main__":
    main()
