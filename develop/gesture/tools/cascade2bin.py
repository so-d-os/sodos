#!/usr/bin/env python3
"""cascade2bin.py - cascata Haar da OpenCV (XML novo, "opencv-cascade-classifier")
para o formato binario simples que haar.c le (.CAS).

    python3 cascade2bin.py entrada.xml saida.cas

Formato (little-endian, int32 = i, float32 = f):
    i magic 'CAS1'  i janela_w  i janela_h  i n_estagios  i n_features
    features: i n_rects, depois 3 x (i x, i y, i w, i h, f peso)  (sobras zeradas)
    estagios: i n_fracos, f limiar
      fraco:  i n_nos, n_nos x (i esq, i dir, i feature, f limiar),
              i n_folhas, n_folhas x f
    esq/dir > 0: proximo no; <= 0: folha -esq.

So features retas (sem "tilted"): a cascata de oculos tem inclinadas e e
recusada aqui.
"""
import struct
import sys
import xml.etree.ElementTree as ET


def nums(text):
    return text.split()


def main(src, dst):
    root = ET.parse(src).getroot()
    cas = root[0]
    w = int(cas.find('width').text)
    h = int(cas.find('height').text)
    feats = cas.find('features')
    out = bytearray()
    flist = list(feats)
    stages = list(cas.find('stages'))
    out += struct.pack('<4siiii', b'CAS1', w, h, len(stages), len(flist))
    for f in flist:
        t = f.find('tilted')
        if t is not None and t.text.strip() == '1':
            sys.exit('%s: feature inclinada (tilted) nao suportada' % src)
        rects = [nums(r.text) for r in f.find('rects')]
        if len(rects) > 3:
            sys.exit('mais de 3 retangulos')
        out += struct.pack('<i', len(rects))
        for k in range(3):
            if k < len(rects):
                x, y, rw, rh, wt = rects[k]
                out += struct.pack('<iiiif', int(x), int(y), int(rw), int(rh), float(wt))
            else:
                out += struct.pack('<iiiif', 0, 0, 0, 0, 0.0)
    nweak_total = 0
    for st in stages:
        weak = list(st.find('weakClassifiers'))
        out += struct.pack('<if', len(weak), float(st.find('stageThreshold').text))
        for wk in weak:
            nodes = nums(wk.find('internalNodes').text)
            leaves = [float(v) for v in nums(wk.find('leafValues').text)]
            nn = len(nodes) // 4
            out += struct.pack('<i', nn)
            for k in range(nn):
                l, r, fi, thr = nodes[k * 4:k * 4 + 4]
                out += struct.pack('<iiif', int(l), int(r), int(fi), float(thr))
            out += struct.pack('<i', len(leaves))
            out += struct.pack('<%df' % len(leaves), *leaves)
            nweak_total += 1
    open(dst, 'wb').write(out)
    print('%s: janela %dx%d, %d estagios, %d fracos, %d features -> %s (%d bytes)' %
          (src, w, h, len(stages), nweak_total, len(flist), dst, len(out)))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
