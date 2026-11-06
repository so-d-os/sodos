#!/usr/bin/env python3
"""elf2sox2.py - converte um PIE estatico (GNU ld) em SOX2, o executavel LP64 do SODOS.

O PIE ligado no endereco 0 ja traz o que o SOX2 pede:
  - PT_LOAD vira a imagem; o que sobra em memsz vira bss;
  - R_X86_64_RELATIVE vira a lista de relocacoes (o loader soma a base);
  - cada import do kernel tem um slot __sodos_imp_<nome> gerado pelo driver.

Tambem preenche as variaveis __sodos_tls_* (sodos_tls.c) com o bloco de TLS.
Uso: elf2sox2.py entrada.elf saida.exe [tamanho_da_pilha]
"""
import struct
import sys

SOX_MAGIC2 = 0x32584F53
PT_LOAD, PT_DYNAMIC, PT_TLS = 1, 2, 7
SHT_SYMTAB = 2
DT_RELA, DT_RELASZ, DT_JMPREL, DT_PLTRELSZ = 7, 8, 23, 2
R_NONE, R_64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 0, 1, 6, 7, 8
IMP_PREFIX = b"__sodos_imp_"
TLS_VARS = (b"__sodos_tls_rel", b"__sodos_tls_filesz", b"__sodos_tls_memsz", b"__sodos_tls_align")


def die(msg):
    sys.stderr.write("elf2sox2: %s\n" % msg)
    sys.exit(1)


def main():
    if len(sys.argv) < 3:
        die("uso: elf2sox2.py entrada.elf saida.exe [pilha]")
    with open(sys.argv[1], "rb") as f:
        elf = f.read()
    stack = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x1000000
    if elf[:4] != b"\x7fELF" or elf[4] != 2:
        die("entrada nao e ELF64")
    entry, phoff, shoff = struct.unpack_from("<QQQ", elf, 24)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", elf, 54)

    loads, dyn, tls = [], None, None
    for i in range(phnum):
        p_type, _flags, off, vaddr, _paddr, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", elf, phoff + i * phentsize
        )
        if p_type == PT_LOAD:
            loads.append((off, vaddr, filesz, memsz))
        elif p_type == PT_DYNAMIC:
            dyn = (vaddr, filesz)
        elif p_type == PT_TLS:
            tls = (vaddr, filesz, memsz, align)
    if not loads:
        die("sem PT_LOAD")

    image_size = max(v + f for _, v, f, _ in loads)
    total = max(v + m for _, v, _, m in loads)
    if total >= 1 << 32:
        die("imagem maior que 4 GB")
    image = bytearray(image_size)
    for off, vaddr, filesz, _ in loads:
        image[vaddr:vaddr + filesz] = elf[off:off + filesz]

    # Simbolos: slots de import e variaveis de TLS.
    symbols = {}
    imports = []
    for i in range(shnum):
        sh = struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize)
        if sh[1] != SHT_SYMTAB:
            continue
        strsh = struct.unpack_from("<IIQQQQIIQQ", elf, shoff + sh[6] * shentsize)
        stroff = strsh[4]
        for pos in range(sh[4], sh[4] + sh[5], 24):
            name_off, _info, _other, shndx, value, _size = struct.unpack_from("<IBBHQQ", elf, pos)
            if not name_off or not shndx:
                continue
            start = stroff + name_off
            name = elf[start:elf.index(b"\0", start)]
            if name.startswith(IMP_PREFIX):
                imports.append((name[len(IMP_PREFIX):], value))
            elif name in TLS_VARS:
                symbols[name] = value
    if tls and len(symbols) != len(TLS_VARS):
        die("o programa tem TLS, mas nao tem sodos_tls.c")
    if tls:
        vaddr, filesz, memsz, align = tls
        rel = vaddr - symbols[b"__sodos_tls_rel"]
        struct.pack_into("<q", image, symbols[b"__sodos_tls_rel"], rel)
        struct.pack_into("<Q", image, symbols[b"__sodos_tls_filesz"], filesz)
        struct.pack_into("<Q", image, symbols[b"__sodos_tls_memsz"], memsz)
        struct.pack_into("<Q", image, symbols[b"__sodos_tls_align"], max(align, 1))

    # Relocacoes dinamicas.
    relocs = []
    tables = []
    if dyn:
        tags = {}
        for pos in range(dyn[0], dyn[0] + dyn[1], 16):
            tag, val = struct.unpack_from("<qQ", image, pos)
            if tag == 0:
                break
            tags[tag] = val
        if DT_RELA in tags:
            tables.append((tags[DT_RELA], tags.get(DT_RELASZ, 0)))
        if DT_JMPREL in tags:
            tables.append((tags[DT_JMPREL], tags.get(DT_PLTRELSZ, 0)))
    for start, size in tables:
        for off, info, addend in struct.iter_unpack("<QQq", bytes(image[start:start + size])):
            rtype, rsym = info & 0xFFFFFFFF, info >> 32
            if rtype == R_RELATIVE:
                if off + 8 > image_size:
                    die("relocacao fora da imagem: %#x" % off)
                struct.pack_into("<q", image, off, addend)
                relocs.append(off)
            elif rtype == R_NONE:
                continue
            elif rtype in (R_64, R_GLOB_DAT, R_JUMP_SLOT):
                # Sem biblioteca dinamica: so sobra simbolo fraco nao definido.
                struct.pack_into("<q", image, off, addend if rtype == R_64 else 0)
            else:
                die("relocacao nao suportada: tipo %d em %#x (simbolo %d)" % (rtype, off, rsym))

    # Tabelas SOX2.
    strtab = bytearray(b"KERNEL\0")
    imp_table = bytearray()
    for name, slot in imports:
        imp_table += struct.pack("<III", 0, len(strtab), slot)
        strtab += name + b"\0"

    hdr_size = 16 * 4
    off_relocs = hdr_size + image_size
    off_imports = off_relocs + 4 * len(relocs)
    off_strtab = off_imports + len(imp_table)
    hdr = struct.pack(
        "<16I",
        SOX_MAGIC2, 0, image_size, total - image_size, entry, image_size, stack,
        len(relocs), off_relocs, len(imports), off_imports, 0, 0,
        len(strtab), off_strtab, hdr_size,
    )
    with open(sys.argv[2], "wb") as f:
        f.write(hdr)
        f.write(image)
        f.write(struct.pack("<%dI" % len(relocs), *relocs))
        f.write(imp_table)
        f.write(strtab)
    print("elf2sox2: %s image=%d bss=%d imports=%d relocs=%d tls=%s entry=%d" % (
        sys.argv[2], image_size, total - image_size, len(imports), len(relocs),
        tls[2] if tls else 0, entry))


if __name__ == "__main__":
    main()
