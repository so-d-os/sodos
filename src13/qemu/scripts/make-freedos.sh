#!/bin/sh
# make-freedos.sh - disquete FreeDOS 1.3 que boota direto no prompt A:\>.
#
# Parte do x86BOOT.img oficial (FD13-FloppyEdition, 1,44 MB): mesmo setor de
# boot, KERNEL.SYS e utilitarios, mas sem o menu de idioma e sem o instalador,
# e com o EDIT (pacote edit.zip do repositorio 1.3).
# Saida: build/out/FREEDOS.IMG e build/out/SERCON.BIN (porta da COM1 para o
# sercon do SeaBIOS: tela de texto e teclado do BIOS pela serial, DOSSER.BAT).
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
G=$S/vendor/guests
FD=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files
O=$S/build/out
mkdir -p "$G" "$O"
cd "$G"

# fetch NOME URL: baixa uma vez e confere o SHA-256 (vendor/SHA256SUMS)
fetch() {
  if [ ! -f "$1" ]; then
    echo "baixando $2"
    curl -fL -o "$1.part" "$2"
    mv "$1.part" "$1"
  fi
  if grep -q " $1\$" ../SHA256SUMS 2>/dev/null; then
    grep " $1\$" ../SHA256SUMS | sha256sum -c - >/dev/null
  else
    sha256sum "$1" >> ../SHA256SUMS
  fi
}
fetch FD13-FloppyEdition.zip $FD/distributions/1.3/official/FD13-FloppyEdition.zip
fetch edit.zip $FD/repositories/1.3/base/edit.zip
python3 -I -c "import zipfile; zipfile.ZipFile('FD13-FloppyEdition.zip').extract('144m/x86BOOT.img', 'fd13')"
rm -rf edit && python3 -I -c "import zipfile; zipfile.ZipFile('edit.zip').extractall('edit')"

I=$O/FREEDOS.IMG
cp "fd13/144m/x86BOOT.img" "$I"
mdel -i "$I" ::/SETUP.BAT ::/FDAUTO.BAT ::/FDCONFIG.SYS
mdeltree -i "$I" ::/FREEDOS/CONFIGS 2>/dev/null || true

# EDIT (GPL): executavel, ajuda e a licenca
mcopy -o -i "$I" edit/BIN/EDIT.EXE ::/FREEDOS/BIN/EDIT.EXE
mcopy -o -i "$I" edit/BIN/EDIT.HLP ::/FREEDOS/BIN/EDIT.HLP
mmd -i "$I" ::/FREEDOS/DOC 2>/dev/null || true
mcopy -o -i "$I" edit/DOC/EDIT/COPYING ::/FREEDOS/DOC/EDIT.TXT

T=$(mktemp -d)
printf '%s\r\n' \
  'LASTDRIVE=Z' 'BUFFERS=20' 'FILES=40' \
  'SHELL=\FREEDOS\BIN\COMMAND.COM \FREEDOS\BIN /E:1024 /P=\AUTOEXEC.BAT' > "$T/FDCONFIG.SYS"
printf '%s\r\n' \
  '@echo off' \
  'set PATH=A:\FREEDOS\BIN' \
  'set DOSDIR=A:\FREEDOS' \
  'echo.' \
  'echo FreeDOS 1.3 no QEMU dentro do SODOS.' \
  'echo Experimente: dir, mem, edit TESTE.TXT' \
  'echo No SODOS: F11 = Ctrl e F12 = Alt na proxima tecla; Ctrl+C sai.' \
  'echo.' > "$T/AUTOEXEC.BAT"
mcopy -o -i "$I" "$T/FDCONFIG.SYS" ::/FDCONFIG.SYS
mcopy -o -i "$I" "$T/AUTOEXEC.BAT" ::/AUTOEXEC.BAT
rm -rf "$T"

# etc/sercon-port: inteiro little-endian lido pelo SeaBIOS (0x3F8 = COM1).
printf '\370\003' > "$O/SERCON.BIN"
echo "$I"
