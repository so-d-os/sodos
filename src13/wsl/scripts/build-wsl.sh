#!/bin/sh
# build-wsl.sh - Compila wsl/build/WSL.EXE (programa LP64 do SODOS).
# Roda no WSL a partir de qualquer pasta:  sh wsl/scripts/build-wsl.sh
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
C=$S/../c_compiler
B=$S/build
mkdir -p "$B/obj"
# libc LP64, link64 e KERNEL.IMP (pega os exports novos do kernel)
make -s -C "$C" build/link64 build/x_crt0_64libc.o out/KERNEL.IMP \
  build/x64_string.o build/x64_ctype.o build/x64_stdlib.o build/x64_stdio.o build/x64_printf.o \
  build/x64_scanf.o build/x64_fpconv.o build/x64_math.o build/x64_time.o build/x64_misc.o \
  build/x64_sigfault64.o build/x64_wchar.o build/x64_setjmp64.o build/x64_ctors64.o
CF="-m64 -O2 -Wall -Wno-unused-function -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
  -mno-red-zone -mcmodel=large -mstackrealign -fno-asynchronous-unwind-tables \
  -fno-tree-loop-distribute-patterns -nostdinc -isystem $C/include"
OBJS=
for f in main wslcore wslcli ntfs vhdx ext4 vmm/vmm vmm/dev vmm/term vmm/nat vmm/chan vmm/p9; do
  o=$B/obj/$(basename "$f").o
  gcc $CF -c "$S/$f.c" -o "$o"
  OBJS="$OBJS $o"
done
gcc -m64 -c "$S/vmm/svm.S" -o "$B/obj/svm.o"
"$C/build/link64" /out:"$B/WSL.EXE" "$C/build/x_crt0_64libc.o" $OBJS "$B/obj/svm.o" \
  "$C"/build/x64_*.o "$C/out/KERNEL.IMP" 2>&1 | grep -v "undef .*sodos_" || true
ls -l "$B/WSL.EXE"
