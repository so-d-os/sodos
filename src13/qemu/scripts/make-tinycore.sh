#!/bin/sh
# make-tinycore.sh - Tiny Core Linux 15.0 (x86) para o QEMU do SODOS.
#
# Baixa a ISO "TinyCore" (25 MB: kernel 6.6, BusyBox e o X do projeto X.Org,
# Xvesa, com FLWM e wbar), confere o MD5 publicado e o SHA-256 guardado em
# vendor/SHA256SUMS, e tira o kernel e o initrd para o -kernel/-initrd (o
# LINUX.BAT pula o menu do isolinux e passa noapic: a maquina isapc anuncia
# um IOAPIC sem linhas e o kernel cai ao configura-lo).
# Saida: build/out/VMLINUZ, CORE.GZ e TINYCORE.ISO (as extensoes do X, cde/).
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
G=$S/vendor/guests
O=$S/build/out
V=15.0
ISO=TinyCore-$V.iso
URL=http://tinycorelinux.net/15.x/x86/release/$ISO
mkdir -p "$G" "$O"
cd "$G"
if [ ! -f "$ISO" ]; then
  echo "baixando $URL"
  curl -fL -o "$ISO.part" "$URL"
  curl -fsSL -o "$ISO.md5.txt" "$URL.md5.txt"
  [ "$(cut -d' ' -f1 "$ISO.md5.txt")" = "$(md5sum "$ISO.part" | cut -d' ' -f1)" ] || {
    echo "MD5 nao confere"; exit 1; }
  mv "$ISO.part" "$ISO"
fi
if grep -q " $ISO\$" ../SHA256SUMS 2>/dev/null; then
  grep " $ISO\$" ../SHA256SUMS | sha256sum -c - >/dev/null
else
  sha256sum "$ISO" >> ../SHA256SUMS
fi
python3 -I "$S/scripts/isoextract.py" "$ISO" "$O" /BOOT/VMLINUZ /BOOT/CORE.GZ >/dev/null
mv -f "$O/vmlinuz" "$O/VMLINUZ"
mv -f "$O/core.gz" "$O/CORE.GZ"
cp -f "$ISO" "$O/TINYCORE.ISO"
echo "$O/VMLINUZ $O/CORE.GZ $O/TINYCORE.ISO"
