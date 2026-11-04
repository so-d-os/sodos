#!/bin/sh
# build.sh - compila o NSPR contra a libsodos (alvo SODOS x32).
#
#   sh sodos/nspr/build.sh [-v]
#   NSPR_SRC=.../firefox-153.6.0esr/nsprpub sh sodos/nspr/build.sh
#
# Sem NSPR_SRC usa vendor/nspr-4.35; com NSPR_SRC compila o nsprpub da
# arvore do Firefox (4.39 na 153).
# Saida: build/nspr/obj/**/*.o e build/nspr/libnspr4.a (ar).
# O link com o linker do SODOS e feito pelo Makefile (alvo nspr).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
N=${NSPR_SRC:-$S/vendor/nspr-4.35/nspr}
BITS=${NSPR_BITS:-32}
if [ "$BITS" = 64 ]; then
  O=$S/build/nspr64/obj
  CC=$S/sodos/sodos-cc64
  LOG=$S/build/nspr64/build.log
else
  O=$S/build/nspr/obj
  CC=$S/sodos/sodos-cc
  LOG=$S/build/nspr/build.log
fi

if [ ! -d "$N/pr/src" ]; then
  echo "NSPR nao encontrado em $N (rode ./setup.sh)" >&2
  exit 1
fi

CFLAGS="-O2 -g -Wall -Wno-unused-parameter -Wno-unused-but-set-variable \
 -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-sign-compare \
 -Wno-parentheses -Wno-missing-field-initializers -Wno-format \
 -I$S/sodos/nspr/include -I$S/sodos/include -I$C/include \
 -I$N/pr/include -I$N/pr/include/private"

DEFS="-D_PR_PTHREADS -DXP_UNIX -DLINUX -DSODOS -DHAVE_LONG_LONG"

mkdir -p "$O"
rm -rf "$O"/*
: > "$LOG"

SRCS=""
# No modo pthreads o NSPR nao compila prdir/prfile/prio/prsocket: quem
# implementa a camada de descritores e pr/src/pthreads/ptio.c.
for d in linking malloc memory pthreads; do
  SRCS="$SRCS $(ls "$N"/pr/src/$d/*.c 2>/dev/null)"
done
# dtoa.c e incluido por prdtoa.c; pripcsem.c e do modo sem pthreads.
for f in $(ls "$N"/pr/src/misc/*.c 2>/dev/null); do
  b=$(basename "$f")
  case "$b" in dtoa.c|pripcsem.c) continue ;; esac
  SRCS="$SRCS $f"
done
for f in prfdcach prmwait priometh pripv6 prmapopt prlayer prlog prmmap prpolevt prprf prscanf prstdio; do
  SRCS="$SRCS $N/pr/src/io/$f.c"
done
for f in prcmon prrwlock prtpd; do
  SRCS="$SRCS $N/pr/src/threads/$f.c"
done
SRCS="$SRCS $N/pr/src/md/prosdep.c"
for f in unix unix_errors uxproces uxrng uxshm uxwrap linux; do
  SRCS="$SRCS $N/pr/src/md/unix/$f.c"
done
SRCS="$SRCS $S/sodos/nspr/shim.c"

n=0
bad=0
for src in $SRCS; do
  case "$src" in
    "$N"/*) rel=${src#"$N"/pr/src/} ;;
    *) rel=${src#"$S"/} ;;
  esac
  obj="$O/${rel%.c}.o"
  mkdir -p "$(dirname "$obj")"
  n=$((n + 1))
  if ! $CC $CFLAGS $DEFS -c "$src" -o "$obj" >>"$LOG" 2>&1; then
    bad=$((bad + 1))
    echo "ERRO: $rel"
  fi
done

echo "compilados $n arquivos, $bad com erro (log: $LOG)"
if [ "$bad" -gt 0 ]; then
  echo "--- primeiros erros ---"
  grep -E "error:" "$LOG" | sed "s|$N/pr/src/||" | sort -u | head -40
  exit 1
fi
