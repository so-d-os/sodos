#!/bin/sh
# build_custom.sh - alvo Rust customizado x86_64-sodos (core + no_std).
#
#   sh sodos/rust/build_custom.sh
#
# Requer nightly + rust-src (fetch-rust.sh + rustup ... --component rust-src).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
R=$S/sodos/rust
O=$S/build/rust/custom
export PATH="$HOME/.cargo/bin:$S/toolchain/bin:$PATH"

cd "$R/custom"
cargo +nightly build -Z build-std=core,alloc,compiler_builtins -Z json-target-spec \
  --target x86_64-sodos.json --release

rm -rf "$O"; mkdir -p "$O/obj"
ld -r --whole-archive "$R/custom/target/x86_64-sodos/release/libsodos_rust.a" \
  -o "$O/rust_all.o"

"$S/sodos/sodos-cc64" -O2 -g -I "$S/sodos/include" -I "$C/include" \
  -c "$R/t_rust64.c" -o "$O/t_rust64.o"
"$C/build/link64" /out:"$O/RUSTSODOS.EXE" "$C/build/x_crt0_64libc.o" \
  "$O/t_rust64.o" "$O/rust_all.o" $(ls "$C"/build/x64_*.o) "$C/out/KERNEL.IMP"
echo "RUSTSODOS.EXE pronto: $O/RUSTSODOS.EXE"
