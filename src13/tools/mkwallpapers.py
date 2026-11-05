#!/usr/bin/env python3
"""mkwallpapers.py - gera os papeis de parede de exemplo do SODOS.

Saida: content/wallpapers/WALL1.PNG .. WALL4.PNG (1280x720).
Requer Pillow (pip install pillow). Rode da raiz de src13:
    python3 tools/mkwallpapers.py
"""
from PIL import Image, ImageDraw, ImageFilter
import math, pathlib

out = pathlib.Path("/mnt/c/dev/so/sodos/src13/content/wallpapers")
out.mkdir(parents=True, exist_ok=True)
W, H = 1280, 720

def grad(c0, c1, glow=None):
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / (H - 1)
        r = int(c0[0] + (c1[0] - c0[0]) * t)
        g = int(c0[1] + (c1[1] - c0[1]) * t)
        b = int(c0[2] + (c1[2] - c0[2]) * t)
        d.line([(0, y), (W, y)], fill=(r, g, b))
    if glow:
        cx, cy, rad, col = glow
        lay = Image.new("L", (W, H), 0)
        dl = ImageDraw.Draw(lay)
        for i in range(rad, 0, -4):
            v = int(120 * (1 - i / rad))
            dl.ellipse([cx - i, cy - i, cx + i, cy + i], fill=v)
        lay = lay.filter(ImageFilter.GaussianBlur(40))
        im.paste(Image.new("RGB", (W, H), col), (0, 0), lay)
    return im

# 1) teal -> indigo, com brilho
grad((0, 96, 96), (16, 24, 64), (980, 140, 420, (120, 210, 220))).save(out / "WALL1.PNG")
# 2) grafite -> azul profundo
grad((18, 20, 28), (12, 40, 90), (300, 560, 380, (60, 130, 220))).save(out / "WALL2.PNG")
# 3) ambar -> vinho
grad((70, 30, 40), (150, 80, 30), (640, 240, 460, (250, 190, 90))).save(out / "WALL3.PNG")

# 4) "retro": blocos 8-bit
im = Image.new("RGB", (W, H), (0, 0, 0))
d = ImageDraw.Draw(im)
pal = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0),
       (170, 0, 170), (170, 85, 0), (170, 170, 170), (85, 85, 85),
       (85, 85, 255), (85, 255, 85), (85, 255, 255), (255, 85, 85),
       (255, 85, 255), (255, 255, 85), (255, 255, 255)]
for y in range(0, H, 24):
    for x in range(0, W, 24):
        i = ((x ^ y) // 24) % 16
        d.rectangle([x, y, x + 23, y + 23], fill=pal[i])
im.save(out / "WALL4.PNG")

for f in sorted(out.glob("*.PNG")):
    print(f.name, f.stat().st_size // 1024, "KB")