#!/bin/sh
# probe-deps.sh - lista TODAS as dependencias obrigatorias do configure.
# O meson para na primeira que falta; aqui cada uma ganha um .pc falso (em
# build/probe-pc, nunca no sysroot) e o configure roda de novo, ate passar.
# Resultado: build/deps-faltando.txt. Nao gera nada utilizavel para compilar.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
P=$S/build/probe-pc
rm -rf "$P"
mkdir -p "$P/include"
# glib.h de sonda: so o que o meson testa (o glib.h do c_compiler e outro).
cat > "$P/include/glib.h" <<'H'
#define GLIB_SIZEOF_SIZE_T 8
#define GLIB_SIZEOF_VOID_P 8
H
: > "$S/build/deps-faltando.txt"
i=0
while [ $i -lt 30 ]; do
  i=$((i + 1))
  SODOS_PROBE_PC="$P" sh "$S/scripts/configure-qemu.sh" "$@" >/dev/null 2>&1 || true
  dep=$(sed -n 's/.*Dependency "\([^"]*\)" not found.*/\1/p; s/.*Dependency lookup for \([^ ]*\) with.*/\1/p' "$S/build/configure.log" | head -1)
  if [ -z "$dep" ]; then
    grep -E "ERROR" "$S/build/configure.log" | head -3 >> "$S/build/deps-faltando.txt" || true
    break
  fi
  echo "$dep" >> "$S/build/deps-faltando.txt"
  cat > "$P/$dep.pc" <<PC
Name: $dep
Description: sonda
Version: 2.80.0
Cflags: -I$P/include
Libs:
PC
done
cat "$S/build/deps-faltando.txt"
