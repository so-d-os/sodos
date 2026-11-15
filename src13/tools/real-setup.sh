#!/bin/sh
# real-setup.sh - prepara esta maquina para "make real" e "make send" (uma vez so).
#
#   sudo sh tools/real-setup.sh install   # fstab + entrada no GRUB
#   sudo sh tools/real-setup.sh remove    # tira as duas coisas
#
# A particao do SODOS e o FAT32 com rotulo SODOSHD (UUID abaixo) no SSD.
# - fstab: linha "user,noauto" em /mnt/sodoshd, para o make montar e desmontar
#   sem sudo (o Linux nunca monta sozinho no boot).
# - GRUB: entrada "SODOS (SSD)" com atalho F8, que carrega \BOOT\KERNEL.ELF e
#   \BOOT\SODOS.PAK da propria particao. A entrada padrao nao muda, e a entrada
#   antiga (F6, /boot/sodos) continua.
set -e

UUID=${REAL_UUID:-72F8-35A2}
MNT=${REAL_MNT:-/mnt/sodoshd}
ENTRY=/etc/grub.d/42_sodos_ssd
OWNER=${SUDO_UID:-1000}
GROUP=${SUDO_GID:-1000}
TAG="# sodos-real (tools/real-setup.sh)"

[ "$(id -u)" = 0 ] || { echo "Rode com sudo."; exit 1; }

case "$1" in
  install)
    blkid -U "$UUID" >/dev/null || { echo "Nenhuma particao com UUID $UUID."; exit 1; }
    mkdir -p "$MNT"
    if ! grep -q "^UUID=$UUID " /etc/fstab; then
      cp -a /etc/fstab "/etc/fstab.antes-sodos-real"
      printf '%s\nUUID=%s %s vfat noauto,user,uid=%s,gid=%s,umask=022,shortname=mixed,utf8 0 0\n' \
        "$TAG" "$UUID" "$MNT" "$OWNER" "$GROUP" >> /etc/fstab
      systemctl daemon-reload || true
      echo "fstab: $MNT (backup em /etc/fstab.antes-sodos-real)"
    else
      echo "fstab: ja tinha a linha de $UUID"
    fi
    cat > "$ENTRY" <<EOF
#!/bin/sh
exec tail -n +3 \$0
menuentry 'SODOS (SSD, particao SODOSHD)' --hotkey=f8 {
    insmod part_gpt
    insmod fat
    insmod all_video
    insmod multiboot2
    search --no-floppy --fs-uuid --set=root $UUID
    multiboot2 /BOOT/KERNEL.ELF
    if [ -f /BOOT/SODOS.PAK ]; then module2 /BOOT/SODOS.PAK; fi
    boot
}
EOF
    chmod +x "$ENTRY"
    ;;
  remove)
    rm -f "$ENTRY"
    sed -i "\|^$TAG\$|d; \|^UUID=$UUID $MNT |d" /etc/fstab
    systemctl daemon-reload || true
    ;;
  *)
    echo "uso: sudo sh $0 install|remove"; exit 1 ;;
esac

update-grub
echo "Pronto: $1"
