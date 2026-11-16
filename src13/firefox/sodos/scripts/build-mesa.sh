#!/bin/sh
# build-mesa.sh - Mesa (EGL surfaceless + GLES, gallium em software) para o
# FIREFOX.EXE. Saida: $MESA_OUT (padrao ~/ffsrc/mesa-build/mesa_sodos.o), um
# objeto relocavel com o Mesa inteiro em que so sodos_mesa_eglGetProcAddress
# fica global; o link-firefox64.sh o inclui se existir. Ver docs/07-webgl.md.
#
#   MESA_DRIVER=llvmpipe (padrao; compila tambem o LLVM 18) | softpipe
#
# Fontes e builds em ~/ffsrc (fora do git), como o do Firefox: llvm18/,
# mesa-src/mesa-25.0.7, mesa-src/libdrm-2.4.124, mesa-venv (meson + mako),
# mesa-build/{llvm,llvmpipe,softpipe,drm}. O LLVM leva ~20 min com 16 CPUs.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)        # src13/firefox
F=${FFSRC:-$HOME/ffsrc}
B=$F/mesa-build
DRIVER=${MESA_DRIVER:-llvmpipe}
MESA_V=25.0.7
DRM_V=2.4.124
OUT=${MESA_OUT:-$B/mesa_sodos.o}
TC=$S/toolchain/bin
OV=$S/sodos/mesa/include
GCC13=${GCC13:-$F/gcc-13}

# libc++ 18 pede o g++ 13 (o mesmo do build do Firefox).
export PATH="$GCC13/bin:$F/mesa-venv/bin:$PATH"
export LD_LIBRARY_PATH="$GCC13/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p "$F/dl" "$F/mesa-src" "$B"
[ -x "$F/mesa-venv/bin/meson" ] || {
  python3 -m venv "$F/mesa-venv"
  "$F/mesa-venv/bin/pip" -q install meson mako pyyaml packaging
}
fetch() { [ -f "$F/dl/$2" ] || curl -sSL -o "$F/dl/$2" "$1/$2"; }
if [ ! -d "$F/mesa-src/mesa-$MESA_V" ]; then
  fetch https://archive.mesa3d.org mesa-$MESA_V.tar.xz
  tar xf "$F/dl/mesa-$MESA_V.tar.xz" -C "$F/mesa-src"
  patch -d "$F/mesa-src/mesa-$MESA_V" -p1 < "$S/patches/mesa-0001-sodos.patch"
fi
LLVM_V=18.1.8
LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_V
if [ "$DRIVER" = llvmpipe ] && [ ! -d "$F/llvm18/llvm" ]; then
  mkdir -p "$F/llvm18"
  for c in llvm cmake third-party; do
    fetch "$LLVM_URL" $c-$LLVM_V.src.tar.xz
    tar xf "$F/dl/$c-$LLVM_V.src.tar.xz" -C "$F/llvm18"
    mv "$F/llvm18/$c-$LLVM_V.src" "$F/llvm18/$c"
  done
  patch -d "$F/llvm18/llvm" -p1 < "$S/patches/llvm-0001-sodos.patch"
fi
if [ ! -d "$F/mesa-src/libdrm-$DRM_V" ]; then
  fetch https://dri.freedesktop.org/libdrm libdrm-$DRM_V.tar.xz
  tar xf "$F/dl/libdrm-$DRM_V.tar.xz" -C "$F/mesa-src"
fi

cat > "$B/sodos-cross.ini" <<EOF
[binaries]
c = '$TC/x86_64-sodos-gcc64'
cpp = '$TC/x86_64-sodos-g++64'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
llvm-config = '$S/sodos/mesa/llvm-config-sodos.py'

[host_machine]
system = 'sodos'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
c_args = ['-isystem', '$OV', '-include', '$OV/sodos_mesa_compat.h']
cpp_args = ['-isystem', '$OV', '-include', '$OV/sodos_mesa_compat.h']

[properties]
pkg_config_libdir = ['$B/sysroot/lib/pkgconfig']
EOF

# libdrm: o EGL do Mesa so compila com ele; sem /dev/dri as chamadas falham
# e a plataforma surfaceless cai no swrast.
if [ ! -f "$B/sysroot/lib/libdrm.a" ]; then
  rm -rf "$B/drm"
  meson setup "$B/drm" "$F/mesa-src/libdrm-$DRM_V" --cross-file "$B/sodos-cross.ini" \
    --prefix="$B/sysroot" -Ddefault_library=static -Dintel=disabled -Dradeon=disabled \
    -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled \
    -Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled -Dtegra=disabled \
    -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dtests=false \
    -Dinstall-test-programs=false -Dudev=false >/dev/null
  ninja -C "$B/drm" install >/dev/null
fi

# LLVM so com o necessario ao llvmpipe (MCJIT/ORC + X86), estatico, sem
# RTTI/excecoes, alvo "linux" (o JIT gera ELF x86-64 na memoria; o SODOS so
# precisa de mmap executavel, que o JIT do SpiderMonkey ja usa).
if [ "$DRIVER" = llvmpipe ] && [ ! -f "$B/llvm/lib/libLLVMX86CodeGen.a" ]; then
  {
    echo "set(CMAKE_SYSTEM_NAME Linux)"
    echo "set(CMAKE_SYSTEM_PROCESSOR x86_64)"
    echo "set(CMAKE_C_COMPILER $TC/x86_64-sodos-gcc64)"
    echo "set(CMAKE_CXX_COMPILER $TC/x86_64-sodos-g++64)"
    echo "set(CMAKE_AR /usr/bin/ar)"
    echo "set(CMAKE_RANLIB /usr/bin/ranlib)"
    echo "set(CMAKE_C_FLAGS_INIT \"-isystem $OV -include $OV/sodos_mesa_compat.h\")"
    echo "set(CMAKE_CXX_FLAGS_INIT \"-isystem $OV -include $OV/sodos_mesa_compat.h\")"
    echo "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)"
    echo "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)"
    echo "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)"
    echo "set(CMAKE_FIND_ROOT_PATH $B/sysroot)"
  } > "$B/llvm-toolchain.cmake"
  cmake -G Ninja -S "$F/llvm18/llvm" -B "$B/llvm" -DCMAKE_TOOLCHAIN_FILE="$B/llvm-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_HOST_TRIPLE=x86_64-unknown-linux-gnu -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-gnu \
    -DLLVM_ENABLE_THREADS=ON -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_PIC=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_BACKTRACES=OFF \
    -DLLVM_ENABLE_CRASH_OVERRIDES=OFF -DLLVM_ENABLE_UNWIND_TABLES=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_BINDINGS=OFF >/dev/null
  ninja -C "$B/llvm" >"$B/llvm.log" 2>&1 || { grep -A5 "^FAILED" "$B/llvm.log" | head -30 >&2; exit 1; }
fi

LLVM_OPT="-Dllvm=disabled -Ddraw-use-llvm=false"
[ "$DRIVER" = llvmpipe ] && LLVM_OPT="-Dllvm=enabled -Dshared-llvm=disabled -Ddraw-use-llvm=true"
W=$B/$DRIVER
if [ ! -f "$W/build.ninja" ]; then
  meson setup "$W" "$F/mesa-src/mesa-$MESA_V" --cross-file "$B/sodos-cross.ini" \
    -Dbuildtype=release -Ddefault_library=static -Dplatforms= \
    -Dgallium-drivers=$DRIVER -Dvulkan-drivers= -Degl=enabled -Dgbm=disabled \
    -Dglx=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true $LLVM_OPT \
    -Dshader-cache=disabled -Dxmlconfig=disabled -Dzstd=disabled -Dzlib=disabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dtools= \
    -Dbuild-tests=false -Dexpat=disabled -Dcpp_rtti=false
fi
# As .so finais nao ligam (nao ha linker ELF do alvo); os objetos bastam.
ninja -C "$W" -k 0 >"$B/$DRIVER.log" 2>&1 || true
if grep -q "^FAILED: .*\.o *$" "$B/$DRIVER.log"; then
  grep -A3 "^FAILED: .*\.o *$" "$B/$DRIVER.log" | head -30 >&2
  exit 1
fi

cd "$W"
G=libgallium-$MESA_V.so
EGLO=$(ninja -t commands src/egl/libEGL.so.1.0.0 | tail -1 | tr ' ' '\n' | grep -E '\.o$')
GA=$(ninja -t commands src/gallium/targets/dri/$G | tail -1 | tr ' ' '\n' | grep -E '\.a$' | grep -v libdri.a)
LLVMLIBS=
[ "$DRIVER" = llvmpipe ] && LLVMLIBS=$("$S/sodos/mesa/llvm-config-sodos.py" --libfiles)
# Um objeto so, como se fossem libEGL + libGLESv2 + libgallium; o pipe-loader
# dinamico do libEGL fica de fora (o estatico do libgallium serve aos dois).
ld -r -o "$OUT.tmp" $EGLO "src/mapi/es2api/libGLESv2.so.2.0.0.p/.._entry.c.o" \
  "src/gallium/targets/dri/$G.p/dri_target.c.o" \
  --whole-archive src/gallium/frontends/dri/libdri.a --no-whole-archive \
  --start-group $GA src/loader/libloader.a $LLVMLIBS --end-group
# So a porta de entrada fica global: nada do Mesa/LLVM colide com o Firefox.
# Simbolos fracos/COMDAT (templates e inlines da libc++) continuam globais: o
# ld junta as copias iguais do libxul; localizados, o grupo COMDAT do Mesa
# seria descartado com referencias para dentro dele.
nm --defined-only "$OUT.tmp" | awk '$2 ~ /^[TDBRGS]$/ && $3 != "eglGetProcAddress" {print $3}' \
  > "$OUT.local"
objcopy --localize-symbols="$OUT.local" \
  --redefine-sym eglGetProcAddress=sodos_mesa_eglGetProcAddress "$OUT.tmp" "$OUT"
rm -f "$OUT.local"
rm -f "$OUT.tmp"
ls -la "$OUT"
