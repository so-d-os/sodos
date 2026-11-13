#!/usr/bin/env python3
"""qprof.py - profiler por amostragem do guest via QMP do QEMU.

Le RIP/RSP/RFLAGS da vCPU (via 'info registers' do HMP) em laco e grava uma
linha por amostra. Nao muda nada no guest: so para a vCPU um instante.

Uso: qprof.py SOCK SAIDA SEGUNDOS [CPU]
"""
import json, re, socket, sys, time

sock_path, out_path, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
cpu = int(sys.argv[4]) if len(sys.argv) > 4 else 0

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
f = s.makefile("rwb", buffering=0)

def recv():
    while True:
        line = f.readline()
        if not line:
            raise SystemExit("QMP fechou")
        m = json.loads(line)
        if "event" in m:
            continue
        return m

def cmd(c, **a):
    msg = {"execute": c}
    if a:
        msg["arguments"] = a
    f.write((json.dumps(msg) + "\n").encode())
    return recv()

recv()  # greeting
cmd("qmp_capabilities")
if cpu:
    cmd("human-monitor-command", **{"command-line": "cpu %d" % cpu})

rx = re.compile(r"RIP=([0-9a-f]+) RFL=([0-9a-f]+).*?\nCS =([0-9a-f]+).*?RSP=([0-9a-f]+)", re.S)
rx_rip = re.compile(r"RIP=([0-9a-f]+)")
rx_rsp = re.compile(r"RSP=([0-9a-f]+)")
rx_cr3 = re.compile(r"CR3=([0-9a-f]+)")
n = 0
t0 = time.time()
with open(out_path, "w") as out:
    while time.time() - t0 < secs:
        r = cmd("human-monitor-command", **{"command-line": "info registers"})
        txt = r.get("return", "")
        rip = rx_rip.search(txt)
        rsp = rx_rsp.search(txt)
        hlt = " HLT=1" in txt
        if rip:
            out.write("%.4f %s %s %d\n" % (time.time() - t0, rip.group(1),
                                           rsp.group(1) if rsp else "0", hlt))
            n += 1
dt = time.time() - t0
print("amostras: %d em %.1f s (%.0f Hz)" % (n, dt, n / dt))
