#!/usr/bin/env python3
"""isoextract.py ISO DESTINO [CAMINHO...] - extrai arquivos de uma ISO 9660.

Leitor minimo (sem Joliet/Rock Ridge: usa os nomes 8.3 do ISO 9660, sem o
";1"). Sem CAMINHO, lista tudo. Ex.:
    isoextract.py TinyCore-15.0.iso out /boot/vmlinuz /boot/core.gz
"""
import os
import struct
import sys

SECTOR = 2048


def records(f, lba, size):
    """Registros de um diretorio: (nome, lba, tamanho, e_dir)."""
    f.seek(lba * SECTOR)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        n = data[pos]
        if n == 0:                      # resto do setor vazio
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + n]
        r_lba = struct.unpack_from('<I', rec, 2)[0]
        r_size = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += n
        if name in (b'\0', b'\1'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0].rstrip('.')
        yield name, r_lba, r_size, bool(flags & 2)


def walk(f, lba, size, prefix=''):
    for name, r_lba, r_size, is_dir in records(f, lba, size):
        path = prefix + '/' + name
        yield path, r_lba, r_size, is_dir
        if is_dir:
            yield from walk(f, r_lba, r_size, path)


def main():
    iso, dest, want = sys.argv[1], sys.argv[2], sys.argv[3:]
    with open(iso, 'rb') as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[1:6] != b'CD001':
            sys.exit('nao e ISO 9660')
        root = pvd[156:156 + 34]
        r_lba = struct.unpack_from('<I', root, 2)[0]
        r_size = struct.unpack_from('<I', root, 10)[0]
        wanted = {w.lower() for w in want}
        for path, lba, size, is_dir in walk(f, r_lba, r_size):
            if not want:
                print('%10d %s%s' % (size, path, '/' if is_dir else ''))
                continue
            if is_dir or path.lower() not in wanted:
                continue
            out = os.path.join(dest, os.path.basename(path).lower())
            f.seek(lba * SECTOR)
            with open(out, 'wb') as o:
                o.write(f.read(size))
            print(out)


if __name__ == '__main__':
    main()
