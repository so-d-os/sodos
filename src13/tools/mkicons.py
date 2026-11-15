#!/usr/bin/env python3
"""mkicons.py - gera os conjuntos de icones dos temas do SODOS.

Saida: content/icons/<TEMA>/<NOME>.PNG + TEMA.CFG + LEIAME.TXT
  MODERNO  icones de traco fino (cinza ardosia) e prato branco com sombra longa
  ESCURO   os mesmos desenhos em traco claro, prato grafite
  CLASSICO os desenhos de sempre do WIN (quadrados chapados), so de referencia:
           o TEMA.CFG dele diz icones=interno e o kernel continua desenhando

O make disk-files converte cada PNG em .ICN (utils/mkicon.c) e copia a pasta
para \\SODOS\\ICONS\\<TEMA>. Requer Pillow. Rode de src13:
    python3 tools/mkicons.py
"""
from PIL import Image, ImageDraw
import math, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "content" / "icons"
N = 64          # lado final
SS = 4          # supersampling
C = N * SS

# --------------------------------------------------------------- caneta ---
class Pen:
    """Desenha em coordenadas de 0..64 sobre a tela supersampled."""
    def __init__(self, img, rgb, w=3.2):
        self.d = ImageDraw.Draw(img)
        self.rgb = rgb + (255,)
        self.w = w

    def _p(self, pts):
        return [(x * SS, y * SS) for x, y in pts]

    def cap(self, x, y, w=None):
        r = (w or self.w) * SS / 2
        self.d.ellipse([x * SS - r, y * SS - r, x * SS + r, y * SS + r], fill=self.rgb)

    def line(self, *pts, w=None):
        w = w or self.w
        self.d.line(self._p(pts), fill=self.rgb, width=int(round(w * SS)), joint="curve")
        for x, y in pts:
            self.cap(x, y, w)

    def rect(self, x0, y0, x1, y1, r=3, w=None):
        w = w or self.w
        self.d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], radius=r * SS,
                                 outline=self.rgb, width=int(round(w * SS)))

    def frect(self, x0, y0, x1, y1, r=0):
        if r:
            self.d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], radius=r * SS,
                                     fill=self.rgb)
        else:
            self.d.rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], fill=self.rgb)

    def circle(self, cx, cy, r, w=None):
        w = w or self.w
        self.d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS],
                       outline=self.rgb, width=int(round(w * SS)))

    def dot(self, cx, cy, r):
        self.d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS],
                       fill=self.rgb)

    def arc(self, cx, cy, r, a0, a1, w=None):
        w = w or self.w
        self.d.arc([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], a0, a1,
                   fill=self.rgb, width=int(round(w * SS)))
        for a in (a0, a1):
            t = math.radians(a)
            self.cap(cx + r * math.cos(t), cy + r * math.sin(t), w)

    def poly(self, pts, w=None):
        self.line(*(list(pts) + [pts[0]]), w=w)

    def fpoly(self, pts):
        self.d.polygon(self._p(pts), fill=self.rgb)


# ---------------------------------------------------------------- icones ---
# Cada funcao desenha um glifo na grade 64x64 (margem ~6).

def g_relogio(p):          # smartwatch com balao de conversa
    p.rect(14, 20, 38, 50, 5)
    p.line((18, 20), (19, 10), (33, 10), (34, 20))
    p.line((18, 50), (19, 58), (33, 58), (34, 50))
    p.rect(30, 8, 56, 26, 4)
    p.line((34, 26), (32, 32), (40, 26))
    for x in (37, 43, 49):
        p.dot(x, 17, 1.6)

def g_celular(p):
    p.rect(18, 6, 46, 58, 5)
    p.line((18, 14), (46, 14))
    p.line((18, 48), (46, 48))
    p.circle(32, 53, 1.6, w=2.4)

def g_impress(p):
    p.line((20, 22), (20, 8), (44, 8), (44, 22))
    p.rect(8, 22, 56, 44, 4)
    p.line((20, 38), (20, 56), (44, 56), (44, 38), (20, 38))
    p.line((25, 44), (39, 44), w=2.4)
    p.line((25, 50), (39, 50), w=2.4)
    p.dot(48, 29, 1.8)

def g_notebook(p):
    p.rect(12, 12, 52, 42, 3)
    p.line((4, 48), (60, 48))
    p.line((6, 48), (9, 54), (55, 54), (58, 48))
    p.line((28, 51), (36, 51), w=2.4)

def g_fones(p):            # fones intra-auriculares
    p.circle(20, 18, 7)
    p.circle(42, 32, 7)
    p.line((14, 23), (14, 34), (18, 40), (18, 54))
    p.line((36, 37), (34, 46), (30, 52), (30, 58))
    p.line((26, 16), (28, 14))

def g_vr(p):
    p.rect(6, 22, 46, 44, 7)
    p.line((46, 26), (56, 24), (58, 30), (58, 38), (56, 42), (46, 40))
    p.line((20, 44), (26, 40), (32, 44))
    p.line((26, 28), (30, 37), (34, 28), w=2.6)
    p.line((37, 37), (37, 28), (41, 28), (41, 32), (37, 32), (42, 37), w=2.6)

def g_camera(p):
    p.rect(6, 18, 58, 52, 4)
    p.line((22, 18), (25, 11), (39, 11), (42, 18))
    p.circle(33, 35, 11)
    p.circle(33, 35, 5.5)
    p.line((10, 24), (16, 24), w=2.6)
    p.dot(51, 25, 1.8)

def g_som(p):              # caixas de som
    p.rect(6, 12, 22, 54, 2)
    p.rect(42, 12, 58, 54, 2)
    p.rect(22, 26, 42, 54, 2)
    for x in (14, 50):
        p.circle(x, 22, 3.5, w=2.4)
        p.circle(x, 40, 6, w=2.4)
    p.circle(32, 40, 6, w=2.4)
    p.dot(32, 40, 1.6)

def g_teclado(p):
    p.rect(4, 20, 60, 50, 6)
    p.line((32, 20), (32, 14), (40, 12), (46, 6))
    for row, y in enumerate((28, 35)):
        for k in range(8):
            p.frect(9 + k * 6.2, y - 2, 13 + k * 6.2, y + 2, 1)
    p.frect(16, 40, 48, 44, 1.5)

def g_acao(p):             # camera de acao
    p.rect(14, 10, 52, 42, 4)
    p.circle(39, 24, 7.5)
    p.circle(39, 24, 3)
    p.rect(18, 15, 28, 22, 1.5, w=2.4)
    p.line((20, 30), (20, 34), w=2.4)
    p.line((25, 30), (25, 34), w=2.4)
    p.line((28, 42), (28, 50), (40, 50), (40, 56), (24, 56))
    p.circle(15, 50, 3, w=2.4)
    p.line((18, 50), (28, 50))

def g_pendrive(p):
    p.rect(22, 12, 54, 52, 4)
    p.rect(8, 22, 22, 42, 1.5)
    p.line((13, 28), (17, 28), w=2.4)
    p.line((13, 36), (17, 36), w=2.4)
    p.line((32, 22), (32, 30), w=2.4)
    p.line((38, 22), (38, 30), w=2.4)
    p.dot(45, 44, 2)

def g_monitor(p):
    p.rect(6, 8, 58, 44, 3)
    p.line((32, 44), (32, 52))
    p.line((20, 54), (44, 54))
    p.circle(54, 54, 3, w=2.4)

def g_bateria(p):          # power bank com cabo
    p.rect(14, 12, 40, 58, 5)
    p.line((14, 20), (40, 20), w=2.4)
    p.fpoly([(29, 26), (21, 38), (27, 38), (25, 48), (33, 35), (27, 35)])
    p.line((22, 12), (22, 6), (46, 6), (52, 12), (52, 30))
    p.rect(49, 30, 55, 38, 1, w=2.4)

def g_tv(p):               # tela curva
    p.line((4, 14), (32, 18), (60, 14))
    p.line((4, 14), (6, 44))
    p.line((60, 14), (58, 44))
    p.line((6, 44), (32, 47), (58, 44))
    p.line((26, 47), (24, 54), (40, 54), (38, 47))

def g_roteador(p):
    p.rect(6, 36, 58, 52, 4)
    p.line((14, 36), (14, 14))
    p.arc(14, 14, 7, -70, 10)
    p.arc(14, 14, 13, -70, 10)
    for x in (32, 40, 48):
        p.dot(x, 44, 1.8)
    p.line((14, 52), (14, 56))
    p.line((50, 52), (50, 56))

def g_jogos(p):            # controle
    p.line((16, 18), (48, 18))
    p.line((16, 18), (10, 24), (5, 44), (8, 52), (15, 52), (22, 42), (42, 42),
           (49, 52), (56, 52), (59, 44), (54, 24), (48, 18))
    p.line((14, 30), (24, 30))
    p.line((19, 25), (19, 35))
    p.circle(44, 26, 2, w=2.2)
    p.circle(44, 34, 2, w=2.2)
    p.circle(40, 30, 2, w=2.2)
    p.circle(48, 30, 2, w=2.2)

# ---- sistema ---------------------------------------------------------------

def g_dos(p):              # monitor com o prompt
    p.rect(6, 8, 58, 44, 3)
    p.line((32, 44), (32, 52))
    p.line((20, 54), (44, 54))
    p.line((14, 19), (20, 25), (14, 31))
    p.line((24, 31), (34, 31))

def g_cmd(p):              # janela de terminal
    p.rect(6, 10, 58, 54, 4)
    p.line((6, 20), (58, 20))
    for x in (11, 16, 21):
        p.dot(x, 15, 1.4)
    p.line((14, 30), (21, 36), (14, 42))
    p.line((26, 43), (38, 43))

def g_pasta(p):
    p.line((6, 50), (6, 14), (10, 10), (24, 10), (29, 16), (54, 16), (58, 20), (58, 50),
           (54, 54), (10, 54), (6, 50))
    p.line((6, 24), (58, 24))

def g_arq(p):              # pasta com documentos (Explorer)
    p.line((14, 18), (14, 8), (44, 8), (44, 18))
    p.line((20, 14), (38, 14), w=2.4)
    p.line((6, 50), (6, 22), (10, 18), (22, 18), (27, 24), (54, 24), (58, 28), (58, 50),
           (54, 54), (10, 54), (6, 50))

def g_arquivo(p):
    p.line((40, 6), (14, 6), (12, 8), (12, 56), (14, 58), (50, 58), (52, 56), (52, 18),
           (40, 6))
    p.line((40, 6), (40, 18), (52, 18))
    for y in (30, 38, 46):
        p.line((20, y), (44, y), w=2.4)

def g_imagem(p):
    p.rect(6, 10, 58, 54, 4)
    p.circle(22, 24, 5)
    p.line((10, 48), (24, 34), (32, 42), (42, 30), (56, 46))

def g_video(p):
    p.rect(6, 12, 58, 52, 4)
    p.line((26, 22), (26, 42), (42, 32), (26, 22))

def g_atalho(p):
    p.rect(8, 8, 56, 56, 6)
    p.line((22, 44), (22, 32), (26, 26), (34, 24), (44, 24))
    p.line((38, 18), (44, 24), (38, 30))

def g_programa(p):         # janela de aplicativo
    p.rect(6, 8, 58, 56, 4)
    p.line((6, 18), (58, 18))
    for x in (11, 16):
        p.dot(x, 13, 1.4)
    p.rect(14, 26, 28, 38, 2, w=2.6)
    p.rect(36, 26, 50, 38, 2, w=2.6)
    p.line((14, 46), (50, 46), w=2.6)

def g_web(p):              # globo
    p.circle(32, 32, 25)
    p.line((7, 32), (57, 32))
    p.line((32, 7), (32, 57))
    p.d.ellipse([20 * SS, 7 * SS, 44 * SS, 57 * SS], outline=p.rgb, width=int(p.w * SS))
    p.arc(32, 2, 32, 62, 118, w=2.4)
    p.arc(32, 62, 32, 242, 298, w=2.4)

def g_notas(p):
    p.rect(12, 10, 52, 58, 4)
    for x in (20, 28, 36, 44):
        p.line((x, 6), (x, 14), w=2.6)
    for y in (26, 34, 42, 50):
        p.line((20, y), (44 if y != 50 else 34, y), w=2.4)

def g_gerenc(p):           # monitor de atividade
    p.rect(6, 8, 58, 56, 4)
    p.line((12, 36), (20, 36), (25, 24), (32, 46), (38, 18), (44, 36), (52, 36))

def g_config(p):           # engrenagem
    cx, cy, ro, ri = 32, 32, 25, 19
    pts = []
    for k in range(8):
        a = k * math.pi / 4
        for da, r in ((-0.22, ri), (-0.15, ro), (0.15, ro), (0.22, ri)):
            pts.append((cx + r * math.cos(a + da), cy + r * math.sin(a + da)))
    # liga os dentes pela circunferencia interna
    full = []
    for i in range(0, len(pts), 4):
        full += pts[i:i + 4]
        a0 = math.atan2(pts[i + 3][1] - cy, pts[i + 3][0] - cx)
        a1 = a0 + math.pi / 4 - 0.44
        for t in range(1, 4):
            a = a0 + (a1 - a0) * t / 4
            full.append((cx + ri * math.cos(a), cy + ri * math.sin(a)))
    p.poly(full)
    p.circle(32, 32, 8)

def g_sair(p):             # liga/desliga
    p.arc(32, 34, 20, -55, 235)
    p.line((32, 8), (32, 30))

def g_fontes(p):           # Aa
    p.line((6, 52), (20, 12), (34, 52))
    p.line((11, 38), (29, 38))
    p.circle(47, 42, 9)
    p.line((56, 32), (56, 52))

def g_fontadv(p):          # A com controles
    p.line((8, 48), (22, 10), (36, 48))
    p.line((13, 34), (31, 34))
    p.line((42, 14), (58, 14))
    p.line((42, 32), (58, 32))
    p.line((42, 50), (58, 50))
    p.dot(48, 14, 3.4)
    p.dot(54, 32, 3.4)
    p.dot(46, 50, 3.4)

def g_design(p):           # pincel
    p.line((52, 8), (28, 34))
    p.line((56, 12), (32, 38))
    p.line((52, 8), (56, 12))
    p.line((26, 36), (20, 38), (14, 46), (10, 56), (20, 54), (28, 48), (32, 40))

def g_menu(p):
    for y in (16, 32, 48):
        p.dot(12, y, 2.6)
        p.line((22, y), (54, y))

def g_abrircom(p):         # elos de corrente
    def link(cx, cy):
        pts = []
        a = math.radians(-45)
        for t in range(0, 361, 10):
            u = math.radians(t)
            x, y = 13 * math.cos(u), 6.5 * math.sin(u)
            pts.append((cx + x * math.cos(a) - y * math.sin(a),
                        cy + x * math.sin(a) + y * math.cos(a)))
        p.line(*pts)
    link(24, 40)
    link(40, 24)

def g_temas(p):            # paleta
    p.line((32, 6), (20, 8), (10, 16), (6, 28), (8, 42), (18, 54), (32, 58), (40, 56),
           (42, 50), (38, 44), (40, 38), (48, 36), (56, 36), (58, 28), (54, 16), (44, 8),
           (32, 6))
    for x, y in ((20, 22), (32, 16), (44, 20), (17, 36)):
        p.circle(x, y, 3.6, w=2.4)

def g_rede(p):             # Wi-Fi
    p.arc(32, 46, 30, 225, 315)
    p.arc(32, 46, 20, 225, 315)
    p.arc(32, 46, 10, 225, 315)
    p.dot(32, 48, 3)

GLYPHS = {
    # conjunto da referencia
    "RELOGIO": g_relogio, "CELULAR": g_celular, "IMPRESS": g_impress,
    "NOTEBOOK": g_notebook, "FONES": g_fones, "VR": g_vr, "CAMERA": g_camera,
    "SOM": g_som, "TECLADO": g_teclado, "ACAO": g_acao, "PENDRIVE": g_pendrive,
    "MONITOR": g_monitor, "BATERIA": g_bateria, "TV": g_tv, "ROTEADOR": g_roteador,
    "JOGOS": g_jogos,
    # desktop
    "DOS": g_dos, "ARQ": g_arq, "WEB": g_web, "CMD": g_cmd,
    "PASTA": g_pasta, "ARQUIVO": g_arquivo, "IMAGEM": g_imagem, "VIDEO": g_video,
    "ATALHO": g_atalho, "PROGRAMA": g_programa,
    # menu Iniciar e Configuracoes
    "NOTAS": g_notas, "GERENC": g_gerenc, "EXPLORER": g_arq, "CONFIG": g_config,
    "NAVEGA": g_web, "COMMAND": g_cmd, "SAIR": g_sair, "TELA": g_monitor,
    "FONTES": g_fontes, "FONTADV": g_fontadv, "DESIGN": g_design, "MENU": g_menu,
    "ABRIRCOM": g_abrircom, "TEMAS": g_temas, "REDE": g_rede,
    # programas conhecidos (NOME.EXE no desktop usa NOME.ICN)
    "FIREFOX": g_web, "NETSURF": g_web, "IMAGEVW": g_imagem, "VIDEOCFG": g_monitor,
    "TOP": g_gerenc, "MC": g_pasta, "NET": g_rede, "QEMU": g_monitor,
}


def glyph(fn, rgb, w=3.2):
    img = Image.new("RGBA", (C, C), (0, 0, 0, 0))
    fn(Pen(img, rgb, w))
    return img.resize((N, N), Image.LANCZOS)


def plate(face, shadow, edge):
    """Prato redondo com sombra longa a 45 graus (como na referencia)."""
    img = Image.new("RGBA", (C, C), (0, 0, 0, 0))
    px = img.load()
    cx = cy = 29 * SS
    r = 25 * SS
    L = 40 * SS
    k = 1 / math.sqrt(2)
    for y in range(C):
        for x in range(C):
            dx, dy = x - cx, y - cy
            t = (dx + dy) * k            # ao longo da sombra
            s = (dx - dy) * k            # atravessado
            if t > 0 and abs(s) <= r:
                f = 1 - t / L
                if f > 0:
                    # borda lateral macia
                    e = min(1.0, (r - abs(s)) / (3 * SS))
                    a = int(shadow[3] * f ** 1.4 * e)
                    px[x, y] = shadow[:3] + (a,)
    d = ImageDraw.Draw(img)
    # sombrinha curta logo abaixo do prato
    for i in range(6, 0, -1):
        a = int(18 * (7 - i) / 6)
        d.ellipse([cx - r - i * SS / 2, cy - r + i * SS / 3, cx + r + i * SS / 2,
                   cy + r + i * SS], fill=shadow[:3] + (a,))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=face + (255,))
    if edge:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=edge + (255,), width=SS)
    return img.resize((N, N), Image.LANCZOS)


# ---------------------------------------------------------- tema classico ---
def classic():
    """Os desenhos do WIN de sempre, em 32x32, so para referencia/edicao."""
    out = {}
    S = 32
    def base(bg):
        im = Image.new("RGBA", (S, S), bg + (255,))
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, S - 1, S - 1], outline=(0, 0, 0, 255))
        return im, d
    im, d = base((192, 192, 192)); out["DOS"] = im
    im, d = base((0, 0, 128)); out["ARQ"] = im
    im, d = base((0, 0, 128))
    d.ellipse([4, 4, 27, 27], outline=(255, 255, 255, 255))
    d.line([(4, 16), (27, 16)], fill=(255, 255, 255, 255))
    d.ellipse([10, 4, 21, 27], outline=(255, 255, 255, 255))
    out["WEB"] = im
    im, d = base((0, 0, 0))
    d.rectangle([0, 0, S - 1, S - 1], outline=(255, 255, 255, 255))
    d.line([(6, 11), (11, 16), (6, 21)], fill=(255, 255, 255, 255), width=2)
    d.line([(14, 21), (22, 21)], fill=(255, 255, 255, 255), width=2)
    out["CMD"] = im
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rectangle([0, 6, 10, 12], fill=(220, 176, 32, 255))
    d.rectangle([0, 12, S - 1, S - 1], fill=(220, 176, 32, 255), outline=(0, 0, 0, 255))
    out["PASTA"] = im
    im, d = base((255, 255, 255)); out["ARQUIVO"] = im
    im, d = base((24, 24, 28))
    d.rectangle([6, 6, 8, 25], fill=(220, 48, 48, 255))
    d.rectangle([9, 9, 14, 22], fill=(220, 48, 48, 255))
    out["VIDEO"] = im
    return out


THEMES = {
    "MODERNO": dict(stroke=(43, 58, 74), face=(255, 255, 255),
                    shadow=(110, 118, 132, 80), edge=None),
    "ESCURO": dict(stroke=(222, 230, 242), face=(44, 51, 64),
                   shadow=(0, 0, 0, 120), edge=(64, 74, 92)),
}

CFG = {
    "MODERNO": """\
; Tema Moderno: claro, cantos redondos, icones de traco fino.
; Cores 0xRRGGBB (sem o 0x). Chave ausente = valor do tema classico.
nome=Moderno
estilo=moderno
icones=arquivo
prato=FUNDO
desk=E9ECF1
rotulo=2B3440
rotulosombra=none
barra=FFFFFF
barraalfa=225
painel=FFFFFF
painelalfa=245
titulo=F7F8FA
tituloin=ECEEF2
texto=1F2933
textotitulo=1F2933
fraco=6B7785
destaque=2F6FEB
borda=D5DAE1
fechar=E5484D
realce=E8F0FE
realcetexto=1F2933
botaotitulo=E3E7EC
raio=10
icone=36
; widgets dos programas (GUI.DLL)
w_face=FFFFFF
w_bg=F3F5F8
w_panel=FFFFFF
w_texto=1F2933
w_fraco=6B7785
w_desab=A8B1BD
w_sel=2F6FEB
w_seltexto=FFFFFF
w_campo=FFFFFF
w_borda=D5DAE1
w_destaque=2F6FEB
w_raio=8
w_btn=30
w_linha=26
""",
    "ESCURO": """\
; Tema Escuro: o Moderno em grafite, traco claro.
nome=Escuro
estilo=moderno
icones=arquivo
prato=FUNDO
desk=161A21
rotulo=E6EDF5
rotulosombra=000000
barra=1C2129
barraalfa=230
painel=232933
painelalfa=245
titulo=232933
tituloin=1C2129
texto=E6EDF5
textotitulo=E6EDF5
fraco=8C98A8
destaque=4C8DFF
borda=343C49
fechar=E5484D
realce=2F3B52
realcetexto=FFFFFF
botaotitulo=343C49
raio=10
icone=36
w_face=2A313C
w_bg=1C2129
w_panel=232933
w_texto=E6EDF5
w_fraco=8C98A8
w_desab=5A6575
w_sel=4C8DFF
w_seltexto=FFFFFF
w_campo=161A21
w_borda=3A4250
w_destaque=4C8DFF
w_raio=8
w_btn=30
w_linha=26
""",
    "CLASSICO": """\
; Tema Classico: o WIN de sempre (vidro escuro, desktop teal, icones
; desenhados pelo kernel). E o padrao quando nao ha D:\\SODOS\\TEMA.CFG.
; Os PNG desta pasta sao so referencia: com icones=arquivo o kernel passa a
; usar os .ICN daqui (gere com o make disk-files ou pelo TEMAS.EXE).
nome=Classico
estilo=classico
icones=interno
""",
}

LEIAME = """\
Icones do tema {nome}
=====================

Cada icone e NOME.ICN (o kernel le) com o NOME.PNG de origem ao lado.
O .ICN e "ICN1", largura e altura (16 bits) e os pixels RGBA. O TEMAS.EXE
converte os PNG que nao tem .ICN quando o tema e aplicado: para trocar um
icone basta por o PNG novo aqui e apagar o .ICN antigo.

Ordem de procura de um icone do desktop: NOME.ICN do programa (FIREFOX.EXE
-> FIREFOX.ICN), depois o do tipo (PASTA, ARQUIVO, IMAGEM, VIDEO, ATALHO,
PROGRAMA). Sem nenhum, o kernel desenha o icone classico.

Um tema novo e uma pasta nova em \\SODOS\\ICONS com TEMA.CFG (copie o de
MODERNO) e os icones. Ele aparece sozinho em Configuracoes > Temas.
"""


def main():
    for name, t in THEMES.items():
        d = OUT / name
        d.mkdir(parents=True, exist_ok=True)
        for g, fn in GLYPHS.items():
            glyph(fn, t["stroke"]).save(d / f"{g}.PNG", optimize=True)
        plate(t["face"], t["shadow"], t["edge"]).save(d / "FUNDO.PNG", optimize=True)
        (d / "TEMA.CFG").write_text(CFG[name].replace("\n", "\r\n"))
        (d / "LEIAME.TXT").write_text(LEIAME.format(nome=name.title()).replace("\n", "\r\n"))
    d = OUT / "CLASSICO"
    d.mkdir(parents=True, exist_ok=True)
    for g, im in classic().items():
        im.save(d / f"{g}.PNG", optimize=True)
    (d / "TEMA.CFG").write_text(CFG["CLASSICO"].replace("\n", "\r\n"))
    (d / "LEIAME.TXT").write_text(LEIAME.format(nome="Classico").replace("\n", "\r\n"))
    # folha de contato para conferir
    names = list(GLYPHS) + ["FUNDO"]
    cols = 8
    rows = (len(names) + cols - 1) // cols
    for name in THEMES:
        bg = (233, 236, 241) if name == "MODERNO" else (22, 26, 33)
        sheet = Image.new("RGBA", (cols * 80, rows * 80), bg + (255,))
        pl = Image.open(OUT / name / "FUNDO.PNG")
        for i, g in enumerate(names):
            x, y = (i % cols) * 80 + 8, (i // cols) * 80 + 8
            sheet.alpha_composite(pl, (x, y))
            if g != "FUNDO":
                ic = Image.open(OUT / name / f"{g}.PNG").resize((34, 34), Image.LANCZOS)
                sheet.alpha_composite(ic, (x + 29 - 17, y + 29 - 17))
        sd = ROOT / "c_compiler" / "build" / "icons"
        sd.mkdir(parents=True, exist_ok=True)
        sheet.save(sd / f"_{name}_sheet.png")
    print("icones em", OUT)


if __name__ == "__main__":
    main()
