#!/bin/sh
# run-guest.sh - Q3: roda o guest de teste (sodos/guest/hello.S) no QEMU
# DENTRO do SODOS, na VM de teste (vm.sh). Grava no D: o QEMU.EXE, o firmware
# (D:\QEMU\bios.bin, multiboot*.bin...) e o HELLO.ELF, e mostra D:\SAIDA.TXT.
#
#   sh scripts/run-guest.sh          # copia tudo e roda
#   sh scripts/run-guest.sh --run    # so roda (o D: ja tem os arquivos)
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
V=${QEMU_VERSION:-9.2.0}
Q=$S/vendor/qemu-$V
B=${QEMU_BUILD_DIR:-$BUILD_ROOT/qemu-$V}
VM="sh $S/scripts/vm.sh"

if [ "$1" != "--run" ]; then
  sh "$S/sodos/guest/build.sh" >/dev/null
  for f in bios.bin multiboot.bin multiboot_dma.bin linuxboot.bin linuxboot_dma.bin; do
    $VM put "$Q/pc-bios/$f" "QEMU/$f" >/dev/null
  done
  $VM put "$S/build/out/HELLO.ELF" >/dev/null
  $VM put "$B/qemu-system-i386" QEMU.EXE >/dev/null
fi
$VM start >/dev/null
P='D:\'
t0=$(date +%s)
SODOS_VM_TIMEOUT=300 $VM exec "${P}QEMU.EXE -L ${P}QEMU -M isapc -m 16 -accel tcg,tb-size=16 \
-kernel ${P}HELLO.ELF -serial file:${P}SAIDA.TXT -device isa-debug-exit,iobase=0xf4,iosize=4 \
-display none -vga none -monitor none -nic none"
t1=$(date +%s)
echo "== ${P}SAIDA.TXT ($((t1 - t0)) s)"
SODOS_VM_TIMEOUT=20 $VM exec "type ${P}SAIDA.TXT"
# 0x21 = 33 = (0x10 << 1) | 1: o guest saiu pelo isa-debug-exit
grep -a "saiu QEMU.EXE" "$BUILD_ROOT/vm/serial.log" | tail -1
