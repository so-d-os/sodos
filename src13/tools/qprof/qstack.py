#!/usr/bin/env python3
"""qstack.py SOCK KERNEL.elf FIREFOX.elf FFBASE N [FILTRO]

Amostra N vezes RIP + 512 palavras da pilha e imprime, para as amostras cujo
RIP esta numa funcao que contem FILTRO, a cadeia de enderecos de retorno
provaveis (palavras da pilha que caem no texto do kernel ou do FIREFOX.EXE),
agregada.
"""
import bisect, collections, json, re, socket, subprocess, sys

sock, kelf, felf, ffbase, N = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16), int(sys.argv[5])
filt = sys.argv[6] if len(sys.argv) > 6 else ""

def syms(elf):
    out = subprocess.run(["nm", "-C", "-n", "--defined-only", elf], capture_output=True, text=True).stdout
    a, n = [], []
    for l in out.splitlines():
        p = l.split(" ", 2)
        if len(p) == 3 and p[1] in "tTwW":
            a.append(int(p[0], 16)); n.append(p[2])
    return a, n
ka, kn = syms(kelf); fa, fn = syms(felf)
klo, khi = ka[0], ka[-1] + 0x1000
flo, fhi = ffbase, ffbase + fa[-1] + 0x1000

def name(x):
    if klo <= x < khi:
        return "k:" + kn[bisect.bisect_right(ka, x) - 1]
    if flo <= x < fhi:
        return re.sub(r"\(.*", "", fn[bisect.bisect_right(fa, x - ffbase) - 1])[:90]
    return None

s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile("rwb", buffering=0); f.readline()
def hmp(c):
    f.write((json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": c}}) + "\n").encode())
    while True:
        r = json.loads(f.readline())
        if "event" not in r:
            return r.get("return", "")
f.write(b'{"execute":"qmp_capabilities"}\n'); f.readline()

chains = collections.Counter(); got = 0
for i in range(N):
    regs = hmp("info registers")
    rip = int(re.search(r"RIP=([0-9a-f]+)", regs).group(1), 16)
    top = name(rip) or "?"
    if filt not in top:
        continue
    rsp = int(re.search(r"RSP=([0-9a-f]+)", regs).group(1), 16)
    mem = hmp("x/512gx 0x%x" % rsp)
    words = [int(w, 16) for w in re.findall(r"0x([0-9a-f]{16})", mem.split(":", 1)[-1] if False else " ".join(l.split(":", 1)[1] for l in mem.splitlines() if ":" in l))]
    ch = [top]
    for w in words:
        nm = name(w)
        if nm and nm != ch[-1]:
            ch.append(nm)
        if len(ch) >= 40:
            break
    chains[" <- ".join(ch)] += 1; got += 1
print("amostras com filtro:", got)
for c, v in chains.most_common(15):
    print("%4d  %s\n" % (v, c))
