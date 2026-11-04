#!/bin/sh
# build.sh - compila um objeto Rust no_std e liga num EXE SODOS (SOX2).
#
#   sh sodos/rust/build.sh
#
# Alvo x86_64-unknown-none (core pre-compilado): prova o caminho Rust ->
# objeto -> link64 -> SODOS. Ainda nao e o alvo x86_64-sodos do Firefox.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
R=$S/sodos/rust
O=$S/build/rust
export PATH="$HOME/.cargo/bin:$PATH"

mkdir -p "$O"
rustc --target x86_64-unknown-none --crate-type=lib --emit=obj -O \
  "$R/rustlib.rs" -o "$O/rustlib.o"
"$S/sodos/sodos-cc64" -O2 -g -I "$S/sodos/include" -I "$C/include" \
  -c "$R/t_rust64.c" -o "$O/t_rust64.o"
"$C/build/link64" /out:"$O/RUST64.EXE" "$C/build/x_crt0_64libc.o" \
  "$O/rustlib.o" "$O/t_rust64.o" $(ls "$C"/build/x64_*.o) "$C/out/KERNEL.IMP"
echo "RUST64.EXE pronto: $O/RUST64.EXE"
