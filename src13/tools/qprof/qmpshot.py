#!/usr/bin/env python3
"""qmpshot.py SOCK SAIDA.png - screendump do QEMU convertido para PNG (sem PIL)."""
import json, socket, struct, sys, zlib, os

sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile("rwb", buffering=0)
f.readline()
def c(m):
    f.write((json.dumps(m) + "\n").encode())
    while True:
        r = json.loads(f.readline())
        if "event" not in r:
            return r
c({"execute": "qmp_capabilities"})
ppm = out + ".ppm"
print(c({"execute": "screendump", "arguments": {"filename": os.path.abspath(ppm)}}))
d = open(ppm, "rb").read()
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
pix = parts[3]
raw = b"".join(b"\0" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
os.unlink(ppm)
print(out, w, h)
