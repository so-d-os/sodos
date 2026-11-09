#!/bin/sh
# make-freedos.sh - disquete FreeDOS 1.3 que boota direto no prompt A:\>.
#
# Parte do x86BOOT.img oficial (FD13-FloppyEdition, 1,44 MB): mesmo setor de
# boot, KERNEL.SYS e utilitarios, mas sem o menu de idioma e sem o instalador.
# Saida: build/out/FREEDOS.IMG e build/out/SERCON.BIN (porta da COM1 para o
# sercon do SeaBIOS: tela de texto e teclado do BIOS pela serial).
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
G=$S/vendor/guests
Z=FD13-FloppyEdition.zip
URL=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/$Z
O=$S/build/out
mkdir -p "$G" "$O"
cd "$G"
if [ ! -f "$Z" ]; then
  echo "baixando $URL"
  curl -fL -o "$Z.part" "$URL"
  mv "$Z.part" "$Z"
fi
if grep -q " $Z\$" ../SHA256SUMS 2>/dev/null; then
  grep " $Z\$" ../SHA256SUMS | sha256sum -c - >/dev/null
else
  sha256sum "$Z" >> ../SHA256SUMS
fi
python3 -I -c "import zipfile; zipfile.ZipFile('$Z').extract('144m/x86BOOT.img', 'fd13')"

I=$O/FREEDOS.IMG
cp "fd13/144m/x86BOOT.img" "$I"
mdel -i "$I" ::/SETUP.BAT ::/FDAUTO.BAT ::/FDCONFIG.SYS
mdeltree -i "$I" ::/FREEDOS/CONFIGS 2>/dev/null || true

T=$(mktemp -d)
printf '%s\r\n' \
  'LASTDRIVE=Z' 'BUFFERS=20' 'FILES=40' \
  'SHELL=\FREEDOS\BIN\COMMAND.COM \FREEDOS\BIN /E:1024 /P=\AUTOEXEC.BAT' > "$T/FDCONFIG.SYS"
printf '%s\r\n' \
  '@echo off' \
  'set PATH=A:\FREEDOS\BIN' \
  'set DOSDIR=A:\FREEDOS' \
  'echo.' \
  'echo FreeDOS 1.3 no QEMU dentro do SODOS (tela e teclado pela serial).' \
  'echo Programas que escrevem direto na memoria de video nao aparecem.' \
  'echo.' > "$T/AUTOEXEC.BAT"
mcopy -o -i "$I" "$T/FDCONFIG.SYS" ::/FDCONFIG.SYS
mcopy -o -i "$I" "$T/AUTOEXEC.BAT" ::/AUTOEXEC.BAT
rm -rf "$T"

# etc/sercon-port: inteiro little-endian lido pelo SeaBIOS (0x3F8 = COM1).
printf '\370\003' > "$O/SERCON.BIN"
echo "$I"
