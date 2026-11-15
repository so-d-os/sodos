#!/bin/bash
# Cria /dev/nvme0n1p6 (FAT32, rotulo SODOSHD) no espaco livre de 21,3 GiB
# entre o swap (p4) e o /home (p5). Nao toca nas outras particoes.
# Rodar com: sudo bash tools/mkpart-sodos.sh
set -euo pipefail

DISK=/dev/nvme0n1
START=95952896                 # logo apos o fim da p4 (95952895), alinhado a 1 MiB
END=140683263                  # setor antes do inicio da p5 (140683264)
SIZE=$((END - START + 1))
NEW=${DISK}p6

[ "$(id -u)" = 0 ] || { echo "rode com sudo"; exit 1; }

# Conferencias: p4 termina onde esperado, p5 comeca onde esperado, p6 nao existe
P4END=$(( $(cat /sys/block/nvme0n1/nvme0n1p4/start) + $(cat /sys/block/nvme0n1/nvme0n1p4/size) - 1 ))
[ "$P4END" = $((START - 1)) ] || { echo "fim da p4 inesperado: $P4END"; exit 1; }
[ "$(cat /sys/block/nvme0n1/nvme0n1p5/start)" = $((END + 1)) ] || { echo "inicio da p5 inesperado"; exit 1; }
[ ! -e /sys/block/nvme0n1/nvme0n1p6 ] || { echo "p6 ja existe"; exit 1; }

BK=/root/mkpart-sodos-backup-$(date +%Y%m%d-%H%M%S)
mkdir -p "$BK"
sfdisk -d $DISK > "$BK/nvme0n1.sfdisk"
echo ">> backup da tabela em $BK"

echo ">> 1/3 criando p6 (Microsoft basic data)"
echo "$START,$SIZE,EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" | sfdisk --no-reread -N 6 $DISK
partx -a -n 6 $DISK || partx -u -n 6 $DISK || true
sleep 1
[ -b $NEW ] || { echo "kernel nao viu a p6; reinicie e rode: mkfs.fat -F 32 -S 512 -n SODOSHD $NEW"; exit 1; }

echo ">> 2/3 formatando FAT32"
mkfs.fat -F 32 -S 512 -n SODOSHD $NEW

echo ">> 3/3 resultado"
lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS $DISK
fsck.fat -n $NEW
echo "OK. (nao foi adicionada ao /etc/fstab)"
