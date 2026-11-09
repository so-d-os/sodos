#!/bin/sh
# test.sh - compila os testes de sodos/tests e roda na VM de teste (vm.sh).
#   sh scripts/test.sh             ZTEST, CTXTEST e GTEST
#   sh scripts/test.sh gtest       so um
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
sh "$S/scripts/build-port64.sh"
mkdir -p "$S/build/out"
T=${*:-ztest ctxtest gtest}
for t in $T; do
  E=$(echo "$t" | tr a-z A-Z).EXE
  $CC64 -O2 $(x86_64-sodos-pkg-config --cflags glib-2.0 zlib) "$S/sodos/tests/$t.c" \
    $(x86_64-sodos-pkg-config --libs glib-2.0 zlib) -o "$S/build/out/$E" >/dev/null
  sh "$S/scripts/vm.sh" put "$S/build/out/$E" >/dev/null
done
sh "$S/scripts/vm.sh" start
for t in $T; do
  E=$(echo "$t" | tr a-z A-Z).EXE
  P='D:\'
  echo "== $P$E"
  SODOS_VM_TIMEOUT=120 sh "$S/scripts/vm.sh" exec "$P$E"
done
