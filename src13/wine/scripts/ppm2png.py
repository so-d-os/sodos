import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
# cabecalho P6: magic, largura, altura, max (comentarios nao usados pelo QEMU)
parts = d.split(maxsplit=4)
w, h = int(parts[1]), int(parts[2])
pix = parts[4][:w * h * 3]
raw = b''.join(b'\x00' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
print(w, h)
