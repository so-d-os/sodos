#!/bin/sh
# Copia os READMEs para \SODOS do pendrive (rotulo SODOS, que o SO ve como U:).
# Chamado pelo make disk-files. Sem pendrive, so avisa: nao e erro.
# Monta com udisksctl (sem sudo) e, se fomos nos que montamos, desmonta no fim.
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
DEV=/dev/disk/by-label/SODOS

if [ ! -e "$DEV" ]; then
  echo "usb-docs: pendrive SODOS nao conectado; READMEs nao copiados para U:\\SODOS"
  exit 0
fi
DEV=$(readlink -f "$DEV")

MNT=$(findmnt -n -o TARGET --source "$DEV" | head -n 1)
OURS=0
if [ -z "$MNT" ]; then
  if ! command -v udisksctl >/dev/null 2>&1; then
    echo "usb-docs: sem udisksctl para montar $DEV; READMEs nao copiados"
    exit 0
  fi
  # O caminho vem da saida do udisksctl: /media/usuario/SODOS pode ser uma
  # pasta velha do disco interno e o pendrive cair em SODOS1.
  OUT=$(udisksctl mount -b "$DEV" 2>&1) || {
    echo "usb-docs: nao montou $DEV: $OUT"
    exit 0
  }
  MNT=$(findmnt -n -o TARGET --source "$DEV" | head -n 1)
  OURS=1
fi
if [ -z "$MNT" ] || [ ! -d "$MNT" ]; then
  echo "usb-docs: $DEV sem ponto de montagem; READMEs nao copiados"
  exit 0
fi

rc=0
mkdir -p "$MNT/SODOS" &&
  cp "$ROOT/README.md" "$MNT/SODOS/README.MD" &&
  cp "$ROOT/README.pt-BR.md" "$MNT/SODOS/README.PT-BR.MD" || rc=1
sync
if [ $rc -eq 0 ]; then
  echo "usb-docs: README.MD e README.PT-BR.MD em U:\\SODOS ($MNT/SODOS)"
else
  echo "usb-docs: falhou copiar para $MNT/SODOS"
fi
[ $OURS -eq 1 ] && udisksctl unmount -b "$DEV" >/dev/null 2>&1
exit $rc
