#!/usr/bin/env python3
# qmp.py SOCK acao...  - controla o QEMU pelo QMP (teste do NetSurf no SODOS).
# Acoes: size W H | screendump F.ppm | sleep s | delay s | key k1+k2 | type txt
#        move x y | click x y | dclick x y | wheel n
# QEMU: -qmp unix:SOCK,server,nowait -device usb-tablet. O teclado do QEMU e
# US e o SODOS pode estar em ABNT2: em "type" use letras, numeros, '.', '-'.
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
f = s.makefile('rw')


def rd():
    while True:
        m = json.loads(f.readline())
        if 'event' not in m:
            return m


def cmd(c, a=None):
    f.write(json.dumps({'execute': c, **({'arguments': a} if a else {})}) + '\n')
    f.flush()
    return rd()


rd()
cmd('qmp_capabilities')
args = sys.argv[2:]
W, H = 1024, 768
DELAY = 0.06


def mv(x, y):
    cmd('input-send-event', {'events': [
        {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (W - 1))}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (H - 1))}}]})


def btn(b, d):
    cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'down': d, 'button': b}}]})


KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '\n': 'ret', '-': 'minus', '=': 'equal'}
i = 0
while i < len(args):
    a = args[i]
    if a == 'size':
        W, H = int(args[i + 1]), int(args[i + 2])
        i += 3
    elif a == 'screendump':
        cmd('screendump', {'filename': args[i + 1]})
        i += 2
    elif a == 'sleep':
        time.sleep(float(args[i + 1]))
        i += 2
    elif a == 'delay':
        DELAY = float(args[i + 1])
        i += 2
    elif a == 'key':
        cmd('send-key', {'keys': [{'type': 'qcode', 'data': k} for k in args[i + 1].split('+')]})
        time.sleep(0.08)
        i += 2
    elif a == 'type':
        for ch in args[i + 1]:
            k = KEYS.get(ch, ch.lower())
            if ch.isupper():
                k = 'shift+' + ch.lower()
            cmd('send-key', {'keys': [{'type': 'qcode', 'data': x} for x in k.split('+')]})
            time.sleep(DELAY)
        i += 2
    elif a == 'move':
        mv(float(args[i + 1]), float(args[i + 2]))
        i += 3
    elif a in ('click', 'dclick'):
        mv(float(args[i + 1]), float(args[i + 2]))
        time.sleep(0.1)
        for _ in range(2 if a == 'dclick' else 1):
            btn('left', True)
            time.sleep(0.12)
            btn('left', False)
            time.sleep(0.12)
        i += 3
    elif a == 'wheel':
        n = int(args[i + 1])
        b = 'wheel-down' if n > 0 else 'wheel-up'
        for _ in range(abs(n)):
            btn(b, True)
            btn(b, False)
            time.sleep(0.05)
        i += 2
    else:
        print('acao desconhecida:', a)
        i += 1
