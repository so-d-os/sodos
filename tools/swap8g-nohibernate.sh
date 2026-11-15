#!/bin/bash
# Desativa hibernacao e reduz o swap /dev/nvme0n1p4 de 29,3 GB para 8 GiB.
# O espaco liberado (~21,3 GiB) fica NAO ALOCADO logo apos o swap,
# para criar depois a particao do SODOS.
# Rodar com: sudo bash tools/swap8g-nohibernate.sh
set -euo pipefail

DISK=/dev/nvme0n1
PART=${DISK}p4
SWAP_UUID=f502e77d-b774-4300-84dd-53a7b434a0b5
START=79175680                 # setor inicial atual da p4 (nao muda)
SIZE=$((8 * 1024 * 1024 * 2))  # 8 GiB em setores de 512 bytes
END=$((START + SIZE - 1))

[ "$(id -u)" = 0 ] || { echo "rode com sudo"; exit 1; }

# Conferencias de seguranca: a p4 tem que ser exatamente o swap esperado
[ "$(blkid -s UUID -o value $PART)" = "$SWAP_UUID" ] || { echo "UUID da p4 inesperado"; exit 1; }
[ "$(blkid -s TYPE -o value $PART)" = swap ] || { echo "p4 nao e swap"; exit 1; }
[ "$(cat /sys/block/nvme0n1/nvme0n1p4/start)" = "$START" ] || { echo "inicio da p4 mudou"; exit 1; }

STAMP=$(date +%Y%m%d-%H%M%S)
BK=/root/swap8g-backup-$STAMP
mkdir -p "$BK"
sfdisk -d $DISK > "$BK/nvme0n1.sfdisk"
cp -a /etc/default/grub /etc/initramfs-tools/conf.d/resume "$BK/"
echo ">> backup da tabela e configs em $BK"

echo ">> 1/5 desativando hibernacao (systemd)"
systemctl mask hibernate.target hybrid-sleep.target suspend-then-hibernate.target
mkdir -p /etc/systemd/sleep.conf.d
cat > /etc/systemd/sleep.conf.d/nohibernate.conf <<'EOF'
[Sleep]
AllowHibernation=no
AllowHybridSleep=no
AllowSuspendThenHibernate=no
EOF

echo ">> 2/5 removendo resume= do initramfs e do GRUB"
echo "RESUME=none" > /etc/initramfs-tools/conf.d/resume
sed -i 's|^GRUB_CMDLINE_RESUME=.*|GRUB_CMDLINE_RESUME=""  # hibernacao desativada '"$STAMP"'|' /etc/default/grub

echo ">> 3/5 desligando swap e redimensionando p4 para 8 GiB"
swapoff $PART
# mantem o mesmo inicio, so muda o tamanho (o resto do disco nao e tocado)
echo "$START,$SIZE" | sfdisk --no-reread -N 4 $DISK
# avisa o kernel so sobre a p4 (as outras estao montadas)
partx -u -n 4 $DISK || partprobe $DISK || true
sleep 1
mkswap -U $SWAP_UUID -L swap $PART
swapon $PART

echo ">> 4/5 regenerando initramfs e grub"
update-initramfs -u -k all
update-grub

echo ">> 5/5 resultado"
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINTS $DISK
swapon --show
parted -s $DISK unit GiB print free
echo "OK. Reinicie quando puder para confirmar o boot sem resume=."
