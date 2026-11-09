#!/bin/sh
# build.sh - monta o guest de teste (ELF32 multiboot) em build/out/HELLO.ELF.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$S/build/out"
gcc -m32 -c "$S/sodos/guest/hello.S" -o "$S/build/out/hello.o"
ld -m elf_i386 -T "$S/sodos/guest/hello.ld" -z max-page-size=0x1000 --build-id=none \
  -o "$S/build/out/HELLO.ELF" "$S/build/out/hello.o"
echo "$S/build/out/HELLO.ELF"
