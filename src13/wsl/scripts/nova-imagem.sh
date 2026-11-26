#!/bin/sh
# nova-imagem.sh - grava o disco que a VM do WSL ve agora (a imagem de base
# mais o que foi gravado neste boot) como imagem nova em partes DEBIAN.000,
# DEBIAN.001... numa pasta do SODOS. Roda DENTRO da VM, sem rede.
#
#   sh /d/SOURCE/SODOS/src13/wsl/scripts/nova-imagem.sh [/d/WSL/NOVA]
#
# Serve para levar para a imagem o que so ficaria na camada de escrita em RAM
# (some quando a VM para): hoje, as ferramentas do FERRAM.TGZ.
#   1. se faltar gcc/make/nasm, extrai o /d/WSL/FERRAM.TGZ no / (gcc 12,
#      binutils, libc6-dev, make, nasm, com o banco do dpkg);
#   2. grava /etc/sodos-imagem (data e versoes);
#   3. sync e fsfreeze -f / (o ext4 fica consistente no disco), dd iflag=direct
#      do /dev/vda em partes de PART_MB (padrao 128: o 9p guarda o arquivo
#      inteiro em RAM do WSL.EXE ate o close), sha256 de cada parte,
#      fsfreeze -u /;
#   4. rele as partes pelo 9p e confere o sha256; copia VMLINUZ e INITRD.IMG.
# Enquanto o / esta congelado (1 a 5 min), quem grava nele (fora /tmp, /run e
# as unidades do SODOS) espera. Precisa do WSL.EXE que le partes
# (wsl/main.c, img_scan). Testar: WSL run -D D:\WSL\NOVA (ver wsl/README.md).
set -e
DEST=${1:-/d/WSL/NOVA}
SRC=${SRC:-/d/WSL}
PART_MB=${PART_MB:-128}
[ "$(hostname)" = sodos-wsl ] && [ -b /dev/vda ] || { echo "nova-imagem: rode dentro da VM do WSL do SODOS"; exit 1; }
[ $((PART_MB % 2)) -eq 0 ] && [ "$PART_MB" -ge 2 ] && [ "$PART_MB" -lt 2048 ] || { echo "nova-imagem: PART_MB par, entre 2 e 2046"; exit 1; }
exec 9>/tmp/nova-imagem.lock
flock -n 9 || { echo "nova-imagem: ja esta rodando"; exit 1; }
if [ -e "$DEST/DEBIAN.000" ] && [ -z "$FORCE" ]; then
  echo "nova-imagem: $DEST ja tem uma imagem (FORCE=1 grava por cima; nunca na pasta que a VM esta usando)"
  exit 1
fi
t0=$(date +%s)

# 1. ferramentas
if ! command -v gcc >/dev/null 2>&1 || ! command -v nasm >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
  echo "nova-imagem: extraindo $SRC/FERRAM.TGZ"
  tar -xzf "$SRC/FERRAM.TGZ" -C /
  ldconfig
fi
# o FERRAM.TGZ so tem os arquivos: o /usr/bin/cc (padrao do make) vem do
# postinst do gcc, por update-alternatives
command -v cc >/dev/null 2>&1 || update-alternatives --quiet --install /usr/bin/cc cc /usr/bin/gcc 20
for t in gcc cc make nasm as ld objcopy readelf nm python3 git; do
  command -v $t >/dev/null 2>&1 || { echo "nova-imagem: falta $t"; exit 1; }
done

# 2. marca
{
  echo "SODOS-Debian $(date '+%Y-%m-%d %H:%M') (wsl/scripts/nova-imagem.sh)"
  . /etc/os-release && echo "base: $PRETTY_NAME"
  gcc --version | head -1
  ld --version | head -1
  make --version | head -1
  nasm -v
} > /etc/sodos-imagem
cat /etc/sodos-imagem

# 3. copia com o / congelado
mkdir -p "$DEST"
for f in "$DEST"/DEBIAN.[0-9][0-9][0-9]; do [ -e "$f" ] && rm -f "$f"; done
sectors=$(cat /sys/block/vda/size)
total_mb=$((sectors / 2048))
n=$(((total_mb + PART_MB - 1) / PART_MB))
echo "nova-imagem: /dev/vda $total_mb MB em $n partes de $PART_MB MB para $DEST"
rm -f /tmp/nova-imagem.*.sha
sync
trap 'fsfreeze -u / 2>/dev/null' EXIT INT TERM
fsfreeze -f /
i=0
while [ $i -lt $n ]; do
  p=$(printf 'DEBIAN.%03d' $i)
  dd if=/dev/vda bs=2M skip=$((i * PART_MB / 2)) count=$((PART_MB / 2)) iflag=direct status=none |
    tee "$DEST/$p" | sha256sum | cut -d' ' -f1 > /tmp/nova-imagem.$i.sha
  i=$((i + 1))
done
fsfreeze -u /
trap - EXIT INT TERM
echo "nova-imagem: copia feita em $(( $(date +%s) - t0 )) s; conferindo"

# 4. confere o que o SODOS gravou
i=0
sum=0
while [ $i -lt $n ]; do
  p=$(printf 'DEBIAN.%03d' $i)
  want=$((PART_MB * 1048576))
  [ $i -eq $((n - 1)) ] && want=$((sectors * 512 - i * PART_MB * 1048576))
  got=$(stat -c %s "$DEST/$p")
  [ "$got" -eq "$want" ] || { echo "nova-imagem: $p com $got bytes, esperado $want"; exit 1; }
  [ "$(sha256sum < "$DEST/$p" | cut -d' ' -f1)" = "$(cat /tmp/nova-imagem.$i.sha)" ] ||
    { echo "nova-imagem: $p difere do disco (sha256)"; exit 1; }
  sum=$((sum + got))
  i=$((i + 1))
done
cp "$SRC/VMLINUZ" "$SRC/INITRD.IMG" "$DEST/"
echo "nova-imagem: FIM-OK $n partes, $((sum / 1048576)) MB, em $(( $(date +%s) - t0 )) s"
