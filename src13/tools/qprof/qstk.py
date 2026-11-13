#!/usr/bin/env python3
"""qstk.py SOCK KERNEL.elf FIREFOX.elf FFBASE SEGUNDOS SAIDA

Como o qstack.py, mas grava toda amostra com o tempo: "t<TAB>topo<TAB>cadeia".
A cadeia so tem nomes do FIREFOX.EXE (o kernel aparece so no topo).
"""
import bisect, json, re, socket, subprocess, sys, time

sock, kelf, felf, ffbase, secs, outp = sys.argv[1:7]
ffbase = int(ffbase, 16); secs = float(secs)

def syms(elf):
    out = subprocess.run(["nm", "-C", "-n", "--defined-only", elf], capture_output=True, text=True).stdout
    a, n = [], []
    for l in out.splitlines():
        p = l.split(" ", 2)
        if len(p) == 3 and p[1] in "tTwW":
            a.append(int(p[0], 16)); n.append(re.sub(r"\(.*", "", p[2])[:100])
    return a, n
ka, kn = syms(kelf); fa, fn = syms(felf)
klo, khi = ka[0], ka[-1] + 0x1000
fhi = ffbase + fa[-1] + 0x1000

def kname(x):
    return kn[bisect.bisect_right(ka, x) - 1] if klo <= x < khi else None
def fname(x):
    return fn[bisect.bisect_right(fa, x - ffbase) - 1] if ffbase <= x < fhi else None

s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile("rwb", buffering=0); f.readline()
def hmp(c):
    f.write((json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": c}}) + "\n").encode())
    while True:
        r = json.loads(f.readline())
        if "event" not in r:
            return r.get("return", "")
f.write(b'{"execute":"qmp_capabilities"}\n'); f.readline()

t0 = time.time(); n = 0
with open(outp, "w") as out:
    while time.time() - t0 < secs:
        regs = hmp("info registers")
        rip = int(re.search(r"RIP=([0-9a-f]+)", regs).group(1), 16)
        rsp = int(re.search(r"RSP=([0-9a-f]+)", regs).group(1), 16)
        top = kname(rip)
        top = ("k:" + top) if top else (fname(rip) or "?")
        mem = hmp("x/400gx 0x%x" % rsp)
        ch = []
        for l in mem.splitlines():
            if ":" not in l:
                continue
            for w in re.findall(r"0x([0-9a-f]{16})", l.split(":", 1)[1]):
                nm = fname(int(w, 16))
                if nm and (not ch or ch[-1] != nm):
                    ch.append(nm)
        out.write("%.2f\t%s\t%s\n" % (time.time() - t0, top, " <- ".join(ch[:40])))
        n += 1
print("amostras:", n)
