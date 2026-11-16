#!/usr/bin/env python3
"""tools/dirfs/mtools.py - mcopy/mmd/mdir sobre uma PASTA (FAT32 montada).

O `make send` poe tools/dirfs no PATH e roda o mesmo `disk-files` com
DISK=<ponto de montagem do HDSODOS>. Quando o `-i X@@512` aponta para uma
pasta, estes comandos copiam direto nela; quando aponta para uma imagem,
chamam o mtools de verdade (/usr/bin), entao o `make disk-files` normal
continua igual.

Copia so o que mudou: arquivo com o mesmo tamanho e a mesma data (FAT guarda
de 2 em 2 s) ou com o mesmo conteudo fica como esta. A FAT nao diferencia
maiusculas: o vfat do Linux acha "BROWSER" mesmo se a pasta se chama "browser".
"""
import filecmp
import os
import shutil
import sys

REAL = {"mcopy": "/usr/bin/mcopy", "mmd": "/usr/bin/mmd", "mdir": "/usr/bin/mdir"}
STATS = {"copiados": 0, "iguais": 0}


def parse(argv):
    flags, args, img = set(), [], None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-i":
            img = argv[i + 1]
            i += 2
            continue
        if a.startswith("-") and len(a) > 1 and not a.startswith("::"):
            flags.update(a[1:])
            i += 1
            continue
        args.append(a)
        i += 1
    return flags, args, img


def target(root, p):
    if p.startswith("::"):
        p = p[2:]
    return os.path.join(root, p.lstrip("/"))


def same(src, dst):
    try:
        s, d = os.stat(src), os.stat(dst)
    except OSError:
        return False
    if s.st_size != d.st_size:
        return False
    if abs(s.st_mtime - d.st_mtime) <= 2:
        return True
    return filecmp.cmp(src, dst, shallow=False)


def copy_file(src, dst, overwrite):
    if os.path.isdir(dst):
        dst = os.path.join(dst, os.path.basename(src))
    if os.path.exists(dst):
        if same(src, dst):
            STATS["iguais"] += 1
            return
        if not overwrite:
            return
    tmp_ok = True
    try:
        shutil.copyfile(src, dst)
        try:
            shutil.copystat(src, dst)  # a data igual deixa o proximo send pular
        except OSError:
            pass
    except OSError as e:
        tmp_ok = False
        sys.stderr.write("mcopy(dirfs): %s -> %s: %s\n" % (src, dst, e))
    if not tmp_ok:
        sys.exit(1)
    STATS["copiados"] += 1


def copy_tree(src, dst, overwrite):
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        if os.path.isdir(s):
            copy_tree(s, d, overwrite)
        else:
            copy_file(s, d, overwrite)


def mcopy(root, flags, args):
    *srcs, dest = args
    dst = target(root, dest)
    into = dest.endswith("/") or len(srcs) > 1 or os.path.isdir(dst)
    overwrite = True  # o disk-files ja testa (mdir) o que nao deve sobrescrever
    for s in srcs:
        if os.path.isdir(s):
            if "s" not in flags:
                continue
            copy_tree(s, os.path.join(dst, os.path.basename(s.rstrip("/"))) if into else dst, overwrite)
        else:
            if into:
                os.makedirs(dst, exist_ok=True)
            copy_file(s, dst, overwrite)
    if STATS["copiados"] or len(srcs) > 1 or "s" in flags:
        sys.stderr.write("dirfs: %d copiados, %d iguais (%s)\n" % (STATS["copiados"], STATS["iguais"], dest))
    return 0


def main():
    prog = os.path.basename(sys.argv[0])
    flags, args, img = parse(sys.argv[1:])
    root = img.split("@@")[0] if img else None
    if not root or not os.path.isdir(root):
        os.execv(REAL[prog], [REAL[prog]] + sys.argv[1:])
    if prog == "mcopy":
        return mcopy(root, flags, args)
    if prog == "mmd":
        for a in args:
            os.makedirs(target(root, a), exist_ok=True)
        return 0
    if prog == "mdir":
        p = target(root, args[0] if args else "::/")
        if not os.path.exists(p):
            return 1
        if "w" in flags:
            print("  ".join(sorted(os.listdir(p))) if os.path.isdir(p) else p)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
