#!/bin/sh
# crashlog.sh - mostra o crash mais recente do FIREFOX.LOG com os nomes das
# funcoes da pilha.
#
# Uso: sh firefox/sodos/scripts/crashlog.sh [FIREFOX.LOG] [N]
#   FIREFOX.LOG  padrao: D:\LOG\FIREFOX.LOG do disk.img (via mtype)
#   N            qual registro (1 = o mais recente, que fica no topo)
#
# O port grava o log em D:\LOG\FIREFOX.LOG (ou U:\LOG no pendrive) a cada
# crash. Os offsets sao relativos a base do .EXE; o ELF com simbolos e o
# FIREFOX.EXE.elf do mesmo link (SODOS_KEEP_ELF=1 make firefox-browser-link).
# Um ELF de outro link da nomes errados.
set -e
HERE=$(cd "$(dirname "$0")/../../.." && pwd)
LOG=${1:-}
N=${2:-1}
ELF=${FIREFOX_ELF:-$HOME/ffsrc/obj-browser64/dist/bin/FIREFOX.EXE.elf}
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
if [ -z "$LOG" ]; then
  mtype -i "$HERE/disk.img@@512" ::/LOG/FIREFOX.LOG > "$TMP"
else
  cp "$LOG" "$TMP"
fi
[ -f "$ELF" ] || { echo "sem $ELF (link com SODOS_KEEP_ELF=1)"; exit 1; }
# __executable_start do ELF: o offset do log soma com ele
B=$(nm "$ELF" | awk '$3 == "__executable_start" { print $1; exit }')
[ -n "$B" ] || B=0
awk -v n="$N" '/^==== / { k++ } k == n' "$TMP" | tr -d '\r' | while IFS= read -r line; do
  case "$line" in
    "  +"*)
      off=${line#  +}
      addr=$(printf '%x' $((0x$B + 0x$off)))
      fn=$(addr2line -f -C -e "$ELF" "$addr" | head -1)
      printf '  +%-9s %s\n' "$off" "$fn"
      ;;
    *) printf '%s\n' "$line" ;;
  esac
done
