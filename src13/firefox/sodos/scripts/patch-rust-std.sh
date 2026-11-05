#!/bin/sh
# patch-rust-std.sh - aplica patches/rust-std-sodos.patch no rust-src do nightly,
# compila o std para x86_64-sodos e instala em rustlib/x86_64-sodos/lib.
#
# Reverter:
#   rustup +nightly component remove rust-src && rustup +nightly component add rust-src
#   rm -rf "$(rustup run nightly rustc --print sysroot)/lib/rustlib/x86_64-sodos"
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
ROOT=$(rustup run nightly rustc --print sysroot)/lib/rustlib
L=$ROOT/src/rust/library
LIB=$ROOT/x86_64-sodos/lib
T=${SODOS_STD_TARGET:-$HOME/ffsrc/rust-std-target}

[ -d "$L/std" ] || { echo "falta rust-src: rustup +nightly component add rust-src" >&2; exit 1; }
if [ ! -f "$L/std/src/sys/alloc/sodos.rs" ]; then
  (cd "$L" && patch -p1 < "$S/patches/rust-std-sodos.patch")
  echo "std do SODOS aplicado em $L"
fi

rm -rf "$T"
export PATH="$S/toolchain/bin:$PATH"
# embed-bitcode: o LTO do jsrust precisa do bitcode do std.
(cd "$S/sodos/rust/stdtest" && SODOS_BUILD_STD=1 CARGO_TARGET_DIR="$T" RUSTFLAGS=-Cembed-bitcode=yes \
  "$S/sodos/rust/tools/cargo" build --release --target x86_64-sodos)
rm -rf "$LIB"
mkdir -p "$LIB"
find "$T/x86_64-sodos/release/deps" -name '*.rlib' -exec cp {} "$LIB/" \;
find "$T/x86_64-sodos/release/deps" -name '*.rmeta' -exec cp {} "$LIB/" \;
echo "std instalado em $LIB ($(ls "$LIB" | wc -l) arquivos)"
