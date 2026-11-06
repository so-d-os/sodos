#!/bin/sh
# fetch-clang-headers.sh - headers do clang 18 (sem root) em vendor/tools; o
# libclang do bindgen nao aceita os intrinsecos do gcc.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
V=$S/vendor
[ -d "$V/tools/usr/lib/llvm-18/lib/clang/18/include" ] && exit 0
T=$(mktemp -d /tmp/sodos-clanghdr-XXXXXX)
trap 'rm -rf "$T"' EXIT
(cd "$T" && apt-get download libclang-common-18-dev)
mkdir -p "$V/tools"
dpkg -x "$T"/libclang-common-18-dev_*.deb "$V/tools"
echo "headers do clang em $V/tools/usr/lib/llvm-18/lib/clang/18/include"
