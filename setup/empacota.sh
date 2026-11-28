#!/bin/sh
# setup/empacota.sh - monta o pacote do SETUP: SETUP.EXE + ARQUIVOS\
#
#   sh setup/empacota.sh [origem] [pacote]
#
#   origem   de onde vem cada arquivo da lista do install.c (padrao: /d, a
#            raiz do D: como esta hoje). SYS\X.H vem de <origem>/SYS/X.H.
#   pacote   pasta do pacote (padrao: /d/SETUP). Recebe ARQUIVOS/ e, se ja
#            compilado, o SETUP.EXE de setup/.
#
# A lista e a do install.c: cada linha {"ORIGEM", ...}. O AUTOEXEC.BAT vem de
# setup/AUTOEXEC.BAT (o modelo), nao da origem.
set -u
AQUI=$(cd "$(dirname "$0")" && pwd)
ORIG=${1:-/d}
PAC=${2:-/d/SETUP}
mkdir -p "$PAC/ARQUIVOS/SYS" || exit 1

ok=0; falta=0
for nome in $(sed -n 's/^ *{"\([^"]*\)", .*/\1/p' "$AQUI/install.c"); do
  rel=$(printf '%s' "$nome" | tr '\\' '/')
  case "$nome" in
    AUTOEXEC.BAT) src="$AQUI/AUTOEXEC.BAT" ;;
    *) src="$ORIG/$rel" ;;
  esac
  dst="$PAC/ARQUIVOS/$rel"
  if [ -f "$src" ]; then
    if [ ! -f "$dst" ] || ! cmp -s "$src" "$dst"; then cp "$src" "$dst" || exit 1; fi
    ok=$((ok + 1))
  else
    echo "empacota: falta $src"
    falta=$((falta + 1))
  fi
done
[ -f "$AQUI/SETUP.EXE" ] && cp "$AQUI/SETUP.EXE" "$PAC/SETUP.EXE"
echo "empacota: $ok arquivo(s) em $PAC/ARQUIVOS, $falta faltando"
