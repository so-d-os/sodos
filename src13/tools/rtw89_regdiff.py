# rtw89_regdiff.py REGS_LINUX.BIN REGS.BIN: registradores diferentes entre o
# RTL8852BE no Linux (conectado) e no SODOS, agrupados por bloco.
# Contadores, timers e resultados de calibracao sempre mudam; o que interessa
# sao blocos de configuracao inteiros diferentes.
import struct
import sys

a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
n = min(len(a), len(b)) // 4

BLOCKS = [
    (0x0000, 0x0400, 'SYS/energia'), (0x0400, 0x1000, 'SYS/WCPU'),
    (0x1000, 0x2000, 'PCIe/HCI'), (0x2000, 0x3000, 'WCPU'),
    (0x8000, 0xA000, 'DMAC'), (0xA000, 0xC000, 'DMAC/STA'),
    (0xC000, 0xD000, 'CMAC0'), (0xD000, 0xE000, 'CMAC0 potencia/PTA'),
    (0xE000, 0xE400, 'RF A direto'), (0xF000, 0xF400, 'RF B direto'),
    (0x10000, 0x20000, 'BB (fase 0x10000)'),
]


def block(addr):
    for lo, hi, name in BLOCKS:
        if lo <= addr < hi:
            return name
    return 'outro'


count = {}
lines = []
for i in range(n):
    va = struct.unpack_from('<I', a, i * 4)[0]
    vb = struct.unpack_from('<I', b, i * 4)[0]
    if va == vb:
        continue
    addr = i * 4
    name = block(addr)
    count[name] = count.get(name, 0) + 1
    lines.append('%05X  linux=%08X  sodos=%08X  xor=%08X  %s' % (addr, va, vb, va ^ vb, name))

for name, c in sorted(count.items(), key=lambda x: -x[1]):
    print('%-22s %5d diferentes' % (name, c))
print()
print('\n'.join(lines))
