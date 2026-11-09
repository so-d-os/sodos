#!/bin/sh
# copy-qemu-disk.sh <disco> - copia o QEMU para D:\UTILS\QEMU.
#
# Chamado por "make disk-files". Sem build do QEMU, avisa e nao falha.
# Vao o QEMU.EXE, o firmware que a maquina isapc usa (o -L aponta para a
# propria pasta: o QEMU procura o firmware junto do executavel), os guests de
# teste HELLO.ELF e ECO.ELF, o disquete FreeDOS, o Tiny Core Linux, o CRASHT.EXE e os .BAT
# de exemplo (sodos/disk/; QTESTE.BAT roda o QEMU por passos para achar uma EXCEPTION).
# Com o pendrive SODOS conectado, a mesma pasta vai tambem para U:\UTILS\QEMU
# (nos .BAT, D:\UTILS\QEMU vira U:\UTILS\QEMU). Sem pendrive, so avisa.
DISK=$(realpath "$1")
S=$(cd "$(dirname "$0")/.." && pwd)
V=${QEMU_VERSION:-9.2.0}
Q=$S/vendor/qemu-$V
B=${QEMU_BUILD_DIR:-$HOME/sodos-build/qemu-$V}
EXE=$B/qemu-system-i386

if [ ! -f "$EXE" ]; then
  echo "QEMU: $EXE nao existe; rode qemu/scripts/build-qemu.sh. Pulando."
  exit 0
fi

mkdir_d() {
  mdir -i "$DISK@@512" "::/$1" >/dev/null 2>&1 || mmd -i "$DISK@@512" "::/$1" || exit 1
}
# Lista "origem|nome" do que foi para o D:, repetida depois no pendrive.
LIST=$(mktemp)
trap 'rm -f "$LIST"' EXIT
put() {
  mcopy -o -i "$DISK@@512" "$1" "::/UTILS/QEMU/$2" || exit 1
  printf '%s|%s\n' "$1" "$2" >>"$LIST"
}

mkdir_d UTILS
mkdir_d UTILS/QEMU
put "$EXE" QEMU.EXE
n=1
# O FAT do SODOS le nomes longos (VFAT): o firmware vai com o nome original.
for f in bios.bin bios-256k.bin vgabios.bin vgabios-stdvga.bin \
         multiboot.bin multiboot_dma.bin linuxboot.bin linuxboot_dma.bin kvmvapic.bin; do
  [ -f "$Q/pc-bios/$f" ] || continue
  put "$Q/pc-bios/$f" "$f"
  n=$((n + 1))
done
if sh "$S/sodos/guest/build.sh" >/dev/null 2>&1; then
  for g in HELLO.ELF ECO.ELF; do
    put "$S/build/out/$g" "$g"
    n=$((n + 1))
  done
fi
# FreeDOS: disquete de 1,44 MB que boota no A:\> pela serial (scripts/make-freedos.sh)
# stderr fica visivel: se o script falhar, o motivo aparece no make disk-files.
if sh "$S/scripts/make-freedos.sh" >/dev/null; then
  for g in FREEDOS.IMG SERCON.BIN; do
    put "$S/build/out/$g" "$g"
    n=$((n + 1))
  done
else
  echo "QEMU: sem FreeDOS (scripts/make-freedos.sh falhou); DOS.BAT nao vai bootar."
fi
# Tiny Core Linux com X (scripts/make-tinycore.sh): 43 MB, LINUX.BAT
if sh "$S/scripts/make-tinycore.sh" >/dev/null; then
  for g in VMLINUZ CORE.GZ TINYCORE.ISO; do
    [ -s "$S/build/out/$g" ] || { echo "QEMU: falta $S/build/out/$g"; exit 1; }
    put "$S/build/out/$g" "$g"
    n=$((n + 1))
  done
else
  echo "QEMU: sem Tiny Core (scripts/make-tinycore.sh falhou); LINUX.BAT nao vai bootar."
fi
# CRASHT.EXE (c_compiler/examples/crasht.c): o "qteste crash" testa o CRASH.LOG
C=$S/../c_compiler/out/CRASHT.EXE
if [ -f "$C" ]; then
  put "$C" CRASHT.EXE
  n=$((n + 1))
fi
for f in "$S"/sodos/disk/*; do
  [ -f "$f" ] || continue
  put "$f" "$(basename "$f" | tr 'a-z' 'A-Z')"
  n=$((n + 1))
done
echo "QEMU: $n arquivos em D:\\UTILS\\QEMU."

. "$S/../tools/usb-mount.sh"
usb_mount QEMU || { echo "QEMU: nada copiado para U:\\UTILS\\QEMU"; exit 0; }
U=$USB_MNT/UTILS/QEMU
rc=0
mkdir -p "$U" || rc=1
while [ $rc -eq 0 ] && IFS='|' read -r src name; do
  case "$name" in
    *.BAT) sed 's/D:\\UTILS\\QEMU/U:\\UTILS\\QEMU/g' "$src" >"$U/$name" || rc=1 ;;
    *) cmp -s "$src" "$U/$name" || cp "$src" "$U/$name" || rc=1 ;;
  esac
done <"$LIST"
usb_umount
if [ $rc -eq 0 ]; then
  echo "QEMU: $n arquivos em U:\\UTILS\\QEMU ($U)."
else
  echo "QEMU: falhou copiar para $U"
fi
exit $rc
