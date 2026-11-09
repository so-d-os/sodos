# usb-mount.sh - funcoes para achar e montar o pendrive SODOS (o SO o ve como U:).
# Uso (com ". tools/usb-mount.sh"):
#   usb_mount "prefixo" || exit 0   # define USB_MNT; sem pendrive, avisa e retorna 1
#   ...copia para $USB_MNT...
#   usb_umount                      # desmonta so se fomos nos que montamos
# Monta com udisksctl (sem sudo).
USB_DEV=/dev/disk/by-label/SODOS
USB_MNT=
USB_OURS=0

usb_mount() {
  if [ ! -e "$USB_DEV" ]; then
    echo "$1: pendrive SODOS nao conectado"
    return 1
  fi
  USB_DEV=$(readlink -f "$USB_DEV")
  USB_MNT=$(findmnt -n -o TARGET --source "$USB_DEV" | head -n 1)
  USB_OURS=0
  if [ -z "$USB_MNT" ]; then
    if ! command -v udisksctl >/dev/null 2>&1; then
      echo "$1: sem udisksctl para montar $USB_DEV"
      return 1
    fi
    # O caminho vem do findmnt: /media/usuario/SODOS pode ser uma pasta
    # velha do disco interno e o pendrive cair em SODOS1.
    _out=$(udisksctl mount -b "$USB_DEV" 2>&1) || {
      echo "$1: nao montou $USB_DEV: $_out"
      return 1
    }
    USB_MNT=$(findmnt -n -o TARGET --source "$USB_DEV" | head -n 1)
    USB_OURS=1
  fi
  if [ -z "$USB_MNT" ] || [ ! -d "$USB_MNT" ]; then
    echo "$1: $USB_DEV sem ponto de montagem"
    usb_umount
    return 1
  fi
  return 0
}

usb_umount() {
  sync
  [ "$USB_OURS" -eq 1 ] && udisksctl unmount -b "$USB_DEV" >/dev/null 2>&1
  USB_OURS=0
}
