# rtw89_dump.py [saida.bin]: copia os registradores 0x0000-0x1FFFF do RTL8852BE
# (MAC, BB, IQK, RFC e o acesso direto ao RF em 0xE000/0xF000) com o Linux
# conectado, para comparar com o U:\NET\REGS.BIN que o SODOS grava.
# So le (como o mac_reg_dump do debugfs do rtw89). Precisa de root e do boot
# com iomem=relaxed (o kernel bloqueia mapear um BAR que um driver esta usando).
import mmap
import os
import struct
import sys

DEV = '/sys/bus/pci/devices/0000:02:00.0'
out = sys.argv[1] if len(sys.argv) > 1 else 'REGS_LINUX.BIN'

with open(DEV + '/vendor') as f, open(DEV + '/device') as g:
    ids = (f.read().strip(), g.read().strip())
if ids != ('0x10ec', '0xb852'):
    raise SystemExit('nao e o RTL8852BE em 02:00.0: %s %s' % ids)

with open('/proc/cmdline') as f:
    if 'iomem=relaxed' not in f.read().split():
        raise SystemExit('este boot nao tem iomem=relaxed: o kernel recusa mapear o BAR '
                         '(no GRUB, aperte e e acrescente iomem=relaxed na linha "linux")')

fd = os.open(DEV + '/resource2', os.O_RDONLY | os.O_SYNC)
mm = mmap.mmap(fd, 0x20000, mmap.MAP_SHARED, mmap.PROT_READ)
data = bytearray()
for a in range(0, 0x20000, 4):
    data += struct.pack('<I', struct.unpack_from('<I', mm, a)[0])
mm.close()
os.close(fd)
with open(out, 'wb') as f:
    f.write(data)
print('%s: %d bytes' % (out, len(data)))
