#!/bin/sh
# Copia os READMEs para \SODOS do pendrive (rotulo SODOS, que o SO ve como U:).
# Chamado pelo make disk-files. Sem pendrive, so avisa: nao e erro.
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
. "$ROOT/src13/tools/usb-mount.sh"

usb_mount usb-docs || { echo "usb-docs: READMEs nao copiados para U:\\SODOS"; exit 0; }

rc=0
mkdir -p "$USB_MNT/SODOS" &&
  cp "$ROOT/README.md" "$USB_MNT/SODOS/README.MD" &&
  cp "$ROOT/README.pt-BR.md" "$USB_MNT/SODOS/README.PT-BR.MD" || rc=1
if [ $rc -eq 0 ]; then
  echo "usb-docs: README.MD e README.PT-BR.MD em U:\\SODOS ($USB_MNT/SODOS)"
else
  echo "usb-docs: falhou copiar para $USB_MNT/SODOS"
fi
usb_umount
exit $rc
