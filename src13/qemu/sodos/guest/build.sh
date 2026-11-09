#!/bin/sh
# build.sh - monta os guests de teste (ELF32 multiboot) em build/out:
# HELLO.ELF (escreve e sai) e ECO.ELF (ecoa o teclado pela serial).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$S/build/out"
for g in hello eco; do
  G=$(echo $g | tr a-z A-Z)
  gcc -m32 -c "$S/sodos/guest/$g.S" -o "$S/build/out/$g.o"
  ld -m elf_i386 -T "$S/sodos/guest/hello.ld" -z max-page-size=0x1000 --build-id=none \
    -o "$S/build/out/$G.ELF" "$S/build/out/$g.o"
  echo "$S/build/out/$G.ELF"
done
