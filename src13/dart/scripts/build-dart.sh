#!/bin/sh
# build-dart.sh - compila o DARTAOT.EXE (dartaotruntime_product do Dart 3.13.2)
# para o SODOS (SOX2/LP64) e, com "sdk", o Dart SDK linux-x64 do mesmo commit
# (gen_snapshot, dart2js_aot.dart.snapshot...) que gera os snapshots.
#
#   sh dart/scripts/build-dart.sh          # so o DARTAOT.EXE
#   sh dart/scripts/build-dart.sh sdk      # tambem o SDK de referencia (~10 min)
#
# Fonte em ~/dartsrc/sdk (scripts/fetch-dart.sh). Saida: dart/build/DARTAOT.EXE.
# Roda no WSL.
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
SDK=${DART_SRC:-$HOME/dartsrc/sdk}
OUT=$SDK/out/SodosX64
export PATH=$HOME/dartsrc/depot_tools:$PATH DEPOT_TOOLS_UPDATE=0

[ -d "$SDK/runtime" ] || { echo "build-dart: $SDK nao existe; rode dart/scripts/fetch-dart.sh"; exit 1; }

# 1. patch do fonte (idempotente)
cd "$SDK"
for p in "$D"/patches/dart-*.patch; do
  if git apply --check "$p" 2>/dev/null; then
    git apply "$p" && echo "build-dart: aplicado $(basename "$p")"
  elif git apply --check -R "$p" 2>/dev/null; then
    :
  else
    echo "build-dart: $(basename "$p") nao aplica nem esta aplicado" >&2
    exit 1
  fi
done

# 2. objetos do port (dart/port/*.c) e da libsodos LP64 (firefox/sodos/port)
sh "$D/../qemu/scripts/build-port64.sh"
sh "$D/scripts/build-port.sh"

# 3. GN: "linux" x64 com o toolchain do SODOS (dart/toolchain/bin/x86_64-sodos64-*)
mkdir -p "$OUT"
cat > "$OUT/args.gn.new" <<EOF
# SODOS LP64 (SOX2): compila "como Linux" com o toolchain do port do Firefox.
target_os = "linux"
host_cpu = "x64"
target_cpu = "x64"
dart_target_arch = "x64"
is_clang = false
x64_toolchain_prefix = "$D/toolchain/bin/x86_64-sodos64-"
dart_sysroot = ""
dart_use_compressed_pointers = false
dart_use_crashpad = false
dart_support_perfetto = false
is_debug = false
is_release = true
is_product = false
dart_debug = false
dart_runtime_mode = "develop"
exclude_kernel_service = true
dart_disable_secure_socket = true
use_rbe = false
verify_sdk_hash = false
dart_version_git_info = true
EOF
if ! cmp -s "$OUT/args.gn.new" "$OUT/args.gn"; then
  mv "$OUT/args.gn.new" "$OUT/args.gn"
  buildtools/gn gen "$OUT"
else
  rm -f "$OUT/args.gn.new"
fi

# 4. o runtime; o passo depois do link (extrair simbolos de um ELF) falha porque
#    a saida e SOX: o que importa e o executavel.
rm -f "$OUT/dartaotruntime_product"
buildtools/ninja/ninja -C "$OUT" dartaotruntime_product >"$D/build/ninja.log" 2>&1 || true
grep -E "^FAILED|error:" "$D/build/ninja.log" | grep -v "dartaotruntime_product.sym\|exe.stripped" | head -20
[ -f "$OUT/dartaotruntime_product" ] || { echo "build-dart: falhou (dart/build/ninja.log)"; exit 1; }
grep elf2sox2 "$D/build/ninja.log" || true
cp "$OUT/dartaotruntime_product" "$D/build/DARTAOT.EXE"
echo "build-dart: $D/build/DARTAOT.EXE"

# 5. SDK de referencia linux-x64 (os snapshots tem de ser deste commit)
if [ "$1" = sdk ]; then
  python3 tools/build.py -m release -a x64 create_sdk
  echo "build-dart: SDK em $SDK/out/ReleaseX64/dart-sdk"
fi
