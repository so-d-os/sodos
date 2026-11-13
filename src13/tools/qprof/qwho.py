#!/usr/bin/env python3
"""qwho.py STK.txt [JANELA_S] [FILTRO_TOPO]

Para as amostras do qstk.py cujo topo contem FILTRO_TOPO (padrao
"write_sector"), mostra por janela de tempo o "responsavel": o primeiro nome
da cadeia que nao e da libc/port (rename, fsync, close, k_*, malloc ...).
"""
import collections, re, sys

path = sys.argv[1]
W = float(sys.argv[2]) if len(sys.argv) > 2 else 20
filt = sys.argv[3] if len(sys.argv) > 3 else "write_sector"
SKIP = re.compile(r"^(rename|fsync|close|write|open|malloc|free|memcpy|stat|path_is_dir|"
                  r"pt_\w+|PR_\w+|_PR_\w+|nsTSubstring|nsTString|void std::|std::sys::fs|"
                  r"std::fs::|<std::path|<hashbrown|<alloc::|nscstring_|nsIFile::|sodos_|"
                  r"sqlite3Os|unix\w+|robust_|posix|syscall|gettid|resched|k_)")
tot = collections.Counter()
win = collections.defaultdict(collections.Counter)
allw = collections.Counter()
for line in open(path):
    p = line.rstrip("\n").split("\t")
    if len(p) < 3:
        continue
    t = float(p[0]); b = int(t // W)
    tot[b] += 1
    if filt not in p[1]:
        continue
    who = "?"
    for nm in p[2].split(" <- "):
        if nm and not SKIP.match(nm):
            who = nm
            break
    win[b][who] += 1
    allw[who] += 1
n = sum(tot.values())
print("== total (%% de todas as amostras) ==")
for k, v in allw.most_common(15):
    print("%5.1f%%  %s" % (100.0 * v / n, k))
print("\n== por janela ==")
for b in sorted(tot):
    s = sum(win[b].values())
    top = ", ".join("%s %.0f%%" % (k[:60], 100.0 * v / tot[b]) for k, v in win[b].most_common(3))
    print("%4d-%4ds escrita=%3.0f%%  %s" % (b * W, b * W + W, 100.0 * s / tot[b], top))
