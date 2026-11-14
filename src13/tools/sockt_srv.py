#!/usr/bin/env python3
"""Servidor do SOCKT.EXE (c_compiler/examples/sockt.c).

Recebe 4 bytes de tamanho + N bytes, confere o padrao (i*7+3), responde
"S <soma> ok|ERRO\n" e devolve N bytes do mesmo padrao. Uso: sockt_srv.py [porta]
"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9555
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(4)
print("escutando", port, flush=True)
while True:
    c, addr = srv.accept()
    t0 = time.time()
    hdr = b""
    while len(hdr) < 4:
        hdr += c.recv(4 - len(hdr))
    n = int.from_bytes(hdr, "big")
    data = bytearray()
    while len(data) < n:
        d = c.recv(65536)
        if not d:
            break
        data += d
    exp = bytes(((i * 7 + 3) & 255) for i in range(n))
    ok = bytes(data) == exp
    s = sum(data) & 0xFFFFFFFF
    dt = time.time() - t0
    print(f"{addr}: recebi {len(data)}/{n} em {dt:.2f}s, {'ok' if ok else 'ERRO'}", flush=True)
    c.sendall(f"S {s} {'ok' if ok else 'ERRO'}\n".encode() + exp)
    time.sleep(1)
    c.close()
