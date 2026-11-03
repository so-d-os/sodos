#!/bin/sh
# Compila a arvore do NetSurf (tree/, ver setup.sh) com o gcc do SODOS.
# O link final do Makefile do NetSurf (nsfb, com o ld do Linux) sempre falha:
# quem junta o NETSURF.EXE e o link.sh, com o linker do SODOS.
N=$(cd "$(dirname "$0")/.." && pwd)
LOG=$N/build-sodos.log
export PATH=$PATH:$N/hosttools/root/usr/bin
cd "$N/tree" && make TARGET=framebuffer CC="$N/sodos/sodos-cc" -k -j8 "$@" > "$LOG" 2>&1
if grep -E "error:|Error [0-9]" "$LOG" | grep -vE "collect2|\[Makefile:[0-9]+: nsfb\]|build-stamp\]" \
    | grep -q .; then
  grep -E "error:|Error [0-9]" "$LOG" | sed 's|.*/tree/||' | sort -u | head -40
  echo "falhou (log: $LOG)"
  exit 1
fi
echo "ok: objetos prontos, agora link.sh"
