#!/usr/bin/env python3
"""winshot.py - captura a tela do WIN (SODOS) e grava PNG.

Usa o endpoint GET /win/printscreen do httpd (BMP 24 bits) e converte para
PNG usando so a biblioteca padrao (zlib), sem Pillow.

Uso:
    python3 tools/winshot.py saida.png                 # http://127.0.0.1:8797
    python3 tools/winshot.py saida.png --host 10.0.2.2 --port 8797
    python3 tools/winshot.py saida.png --bmp entrada.bmp   # arquivo local
    python3 tools/winshot.py saida.png --crop 200,60,1100,560
"""
import argparse
import struct
import sys
import urllib.request
import zlib


def read_bmp(data):
    if data[:2] != b"BM":
        raise SystemExit("nao e BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if bpp != 24 or comp != 0:
        raise SystemExit("BMP nao suportado: %d bpp comp=%d" % (bpp, comp))
    stride = (w * 3 + 3) & ~3
    top_down = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        sy = y if top_down else (h - 1 - y)
        start = off + sy * stride
        line = data[start:start + w * 3]
        if len(line) < w * 3:
            raise SystemExit("BMP truncado")
        # BGR (de baixo para cima) -> RGB
        rgb = bytearray(w * 3)
        rgb[0::3] = line[2::3]
        rgb[1::3] = line[1::3]
        rgb[2::3] = line[0::3]
        rows.append(bytes(rgb))
    return w, h, rows


def crop(rows, w, box):
    x0, y0, x1, y1 = box
    x0 = max(0, min(w, x0))
    x1 = max(0, min(w, x1))
    out = []
    for y in range(y0, min(len(rows), y1)):
        out.append(rows[y][x0 * 3:x1 * 3])
    return x1 - x0, len(out), out


def write_png(path, w, h, rows):
    raw = bytearray()
    for line in rows:
        raw.append(0)          # filtro "none"
        raw += line

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8797)
    ap.add_argument("--bmp", help="le um BMP local em vez de buscar no httpd")
    ap.add_argument("--crop", help="x0,y0,x1,y1")
    a = ap.parse_args()

    if a.bmp:
        with open(a.bmp, "rb") as f:
            data = f.read()
    else:
        url = "http://%s:%d/win/printscreen" % (a.host, a.port)
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                data = r.read()
        except Exception as e:
            raise SystemExit("falha ao buscar %s: %s" % (url, e))

    w, h, rows = read_bmp(data)
    if a.crop:
        box = [int(v) for v in a.crop.split(",")]
        w, h, rows = crop(rows, w, box)
    write_png(a.out, w, h, rows)
    print("%s: %dx%d" % (a.out, w, h))


if __name__ == "__main__":
    main()
