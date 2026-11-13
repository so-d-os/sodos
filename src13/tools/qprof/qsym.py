#!/usr/bin/env python3
"""qsym.py AMOSTRAS SERIAL.log KERNEL.elf FIREFOX.elf [t0 t1]

Agrupa as amostras do qprof.py por funcao (kernel e FIREFOX.EXE) e por
"area" (pasta do fonte, via prefixo do nome C++). A base do FIREFOX.EXE vem
da linha '[mod] exec FIREFOX.EXE base=' da serial (a ultima).
"""
import bisect, collections, re, subprocess, sys

samples, serial, kelf, felf = sys.argv[1:5]
t0 = float(sys.argv[5]) if len(sys.argv) > 5 else 0
t1 = float(sys.argv[6]) if len(sys.argv) > 6 else 1e18

def syms(elf):
    out = subprocess.run(["nm", "-C", "-n", "--defined-only", elf],
                         capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split(" ", 2)
        if len(p) == 3 and p[1] in "tTwWiI":
            addrs.append(int(p[0], 16)); names.append(p[2])
    return addrs, names

ka, kn = syms(kelf)
fa, fn = syms(felf)
fstart = int(subprocess.run(["sh", "-c", "nm %s | awk '$3==\"__executable_start\"{print $1}'" % felf],
                            capture_output=True, text=True).stdout.strip() or "0", 16)
fend = fa[-1] + 0x100000 if fa else 0
mods = []
for m in re.finditer(r"\[mod\] exec (\S+) base=([0-9A-Fa-f]+) entry=\S+ size=([0-9A-Fa-f]+)",
                     open(serial, errors="replace").read()):
    mods.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
ffbase = [b for n, b, s in mods if n.upper() == "FIREFOX.EXE"]
ffbase = ffbase[-1] if ffbase else None
print("base FIREFOX.EXE:", hex(ffbase) if ffbase else None, " __executable_start:", hex(fstart))

def look(addrs, names, a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else "?"

func = collections.Counter(); area = collections.Counter(); where = collections.Counter()
n = 0
for line in open(samples):
    p = line.split()
    t = float(p[0])
    if t < t0 or t > t1:
        continue
    rip = int(p[1], 16); n += 1
    if ffbase and ffbase <= rip < ffbase + (fend - fstart):
        name = look(fa, fn, rip - ffbase + fstart)
        w = "FIREFOX"
    elif ka and ka[0] <= rip < ka[-1] + 0x10000:
        name = "k:" + look(ka, kn, rip)
        w = "kernel"
    else:
        name = "?%x" % (rip & ~0xfff)
        w = "outro"
        for mn, b, s in mods:
            if b <= rip < b + s:
                name = "%s+%x" % (mn, rip - b); w = mn
    func[name] += 1; where[w] += 1
    if w == "FIREFOX":
        # area: namespace/classe do C++ ou prefixo C
        s = re.sub(r"\(.*", "", name)
        s = re.sub(r"<.*?>", "", s)
        parts = s.split("::")
        a = "::".join(parts[:2]) if len(parts) > 2 else parts[0]
        area[a] += 1
print("amostras:", n)
print("\n== onde ==")
for k, v in where.most_common():
    print("%6.2f%%  %6d  %s" % (100.0 * v / n, v, k))
print("\n== funcoes (top 60) ==")
for k, v in func.most_common(60):
    print("%6.2f%%  %6d  %s" % (100.0 * v / n, v, k[:150]))
print("\n== areas do Firefox (top 40) ==")
for k, v in area.most_common(40):
    print("%6.2f%%  %6d  %s" % (100.0 * v / n, v, k[:120]))
