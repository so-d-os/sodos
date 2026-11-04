#!/usr/bin/env python3
"""soxdump.py - lista imports/exports de um .EXE/.DLL SOX (debug do port).

    python3 soxdump.py build/out/SODOS.DLL
"""
import struct
import sys


def dump(path):
    data = open(path, "rb").read()
    (magic, flags, image_size, bss, entry, text, stack, nrel, offrel,
     nimp, offimp, nexp, offexp, strsz, offstr, offimg) = struct.unpack_from("<16I", data, 0)
    if magic != 0x31584F53:
        raise SystemExit("%s: nao e SOX" % path)

    def name(off):
        end = data.index(b"\0", offstr + off)
        return data[offstr + off:end].decode("ascii", "replace")

    print("%s: image=%d bss=%d entry=%08X relocs=%d" %
          (path, image_size, bss, entry, nrel))
    print("imports:")
    for i in range(nimp):
        lib, nm, slot = struct.unpack_from("<III", data, offimp + i * 12)
        print("  %-12s %-32s slot=%08X" % (name(lib), name(nm), slot))
    print("exports:")
    for i in range(nexp):
        nm, val = struct.unpack_from("<II", data, offexp + i * 8)
        print("  %-32s %08X" % (name(nm), val))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        dump(p)
