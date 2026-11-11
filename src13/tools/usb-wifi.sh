#!/bin/sh
# Copia o configurador WIFI para o pendrive (rotulo SODOS, que o SO ve como U:):
# os fontes em U:\SRC\WIFI e o WIFI.EXE na raiz, se ja tiver sido compilado.
# Chamado pelo make disk-files. Sem pendrive, so avisa: nao e erro.
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
. "$ROOT/src13/tools/usb-mount.sh"

usb_mount usb-wifi || { echo "usb-wifi: WIFI nao copiado para U:\\SRC\\WIFI"; exit 0; }

rc=0
mkdir -p "$USB_MNT/SRC/WIFI" &&
  cp "$ROOT/src13/wifi/wifi.cpp" "$USB_MNT/SRC/WIFI/WIFI.CPP" &&
  cp "$ROOT/src13/wifi/BUILD.BAT" "$USB_MNT/SRC/WIFI/BUILD.BAT" || rc=1
if [ -f "$ROOT/src13/c_compiler/out/WIFI.EXE" ]; then
  cp "$ROOT/src13/c_compiler/out/WIFI.EXE" "$USB_MNT/WIFI.EXE" || rc=1
else
  echo "usb-wifi: WIFI.EXE ainda nao compilado (rode make disk-files inteiro)"
fi
if [ $rc -eq 0 ]; then
  echo "usb-wifi: fontes em U:\\SRC\\WIFI e WIFI.EXE na raiz ($USB_MNT)"
else
  echo "usb-wifi: falhou copiar para $USB_MNT"
fi
usb_umount
exit $rc
