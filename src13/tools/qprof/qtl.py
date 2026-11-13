#!/usr/bin/env python3
"""qtl.py AMOSTRAS KERNEL.elf FFBASE [JANELA_S] - linha do tempo por categoria."""
import bisect, collections, subprocess, sys

out = subprocess.run(["nm", "-n", "--defined-only", sys.argv[2]], capture_output=True, text=True).stdout
a, n = [], []
for l in out.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in "tT":
        a.append(int(p[0], 16)); n.append(p[2])
B = int(sys.argv[3], 16)
W = float(sys.argv[4]) if len(sys.argv) > 4 else 10
cats = collections.defaultdict(collections.Counter)
K = ["disk_wr", "disk_rd", "firefox", "espera", "kern_outro"]
for l in open(sys.argv[1]):
    t, rip, rsp, h = l.split()
    rip = int(rip, 16)
    b = int(float(t) // W)
    if rip >= B and rip < B + 0x0C000000:
        c = "firefox"
    elif a and a[0] <= rip < a[-1] + 0x10000:
        f = n[bisect.bisect_right(a, rip) - 1]
        if "write_sector" in f:
            c = "disk_wr"
        elif "read_sector" in f or "ata_wait" in f:
            c = "disk_rd"
        elif f in ("net_ms", "nic_poll", "nic_poll.part.0", "arp_resolve", "input_poll", "win_wm_run"):
            c = "espera"
        else:
            c = "kern_outro"
    else:
        c = "kern_outro"
    cats[b][c] += 1
for b in sorted(cats):
    c = cats[b]; tot = sum(c.values())
    print("%4d-%4ds  " % (b * W, b * W + W) + "  ".join("%s=%3.0f%%" % (k, 100 * c[k] / tot) for k in K))
