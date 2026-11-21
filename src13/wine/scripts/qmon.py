#!/usr/bin/env python3
# qmon.py SOCKET - manda as linhas da entrada padrao ao monitor do QEMU (unix
# socket) e imprime as respostas. Substitui "nc -U" onde nao ha netcat.
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.settimeout(0.5)
def drain():
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
    except socket.timeout:
        pass
    return out
drain()
for line in sys.stdin:
    s.sendall(line.rstrip("\n").encode() + b"\n")
    time.sleep(0.08)
time.sleep(1.5)
sys.stdout.buffer.write(drain())
