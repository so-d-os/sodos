#!/bin/sh
# check.sh - valida o driver x86_64-sodos-gcc/g++.
#
# Compila C e C++, liga SOX com o `link`, roda no soxrun e confere que o
# preprocessador ve SODOS (e nao Linux). Requer c_compiler compilado.
set -e
D=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$D/.." && pwd)
C=$R/../c_compiler
T=$(mktemp -d /tmp/sodos-tc-XXXXXX)
trap 'rm -rf "$T"' EXIT

for f in "$C/build/link" "$C/build/x_crt0.o" "$C/out/LIBCX.DLL" "$C/out/KERNEL.IMP"; do
  [ -e "$f" ] || { echo "falta $f; rode make -C c_compiler" >&2; exit 1; }
done

printf '#include <stdio.h>\nint main(void){printf("C ok\\n");return 0;}\n' > "$T/t.c"
"$D/bin/x86_64-sodos-gcc" "$T/t.c" -o "$T/t"
cp "$C/out/LIBCX.DLL" "$C/out/KERNEL.IMP" "$T/"
"$C/build/soxrun" -C "$T" t

printf '#include <string>\nint main(){std::string s="C++ ok";return s.size()==6?0:1;}\n' > "$T/t.cpp"
"$D/bin/x86_64-sodos-g++" "$T/t.cpp" -o "$T/tpp"
[ -f "$R/build/libcxx/LIBCXX.LIB" ] && cp "$R/build/libcxx/LIBCXX.LIB" "$T/" || true
[ -f "$R/build/out/SODOS.DLL" ] && cp "$R/build/out/SODOS.DLL" "$T/" || true
[ -f "$C/out/POSIXX.DLL" ] && cp "$C/out/POSIXX.DLL" "$T/" || true
"$C/build/soxrun" -C "$T" tpp

macros=$(printf 'int x;\n' | "$D/bin/x86_64-sodos-gcc" -E -dM -)
echo "$macros" | grep -q '__SODOS__' || { echo "sem __SODOS__" >&2; exit 1; }
if echo "$macros" | grep -q '__linux__'; then
  echo "__linux__ ainda definido no alvo" >&2
  exit 1
fi

"$D/bin/x86_64-sodos-gcc" -Wl,--version | grep -q 'GNU ld' || exit 1
[ "$("$D/bin/x86_64-sodos-gcc" -dumpmachine)" = "x86_64-pc-sodos" ] || exit 1

# LP64/SOX2: compila o hello64 e confere o magic (roda no QEMU, nao aqui).
"$D/bin/x86_64-sodos-gcc64" "$R/../c_compiler/lib/hello64.c" -o "$T/h64"
[ "$(dd if="$T/h64" bs=1 count=4 2>/dev/null)" = "SOX2" ] || { echo "SOX2 invalido" >&2; exit 1; }

# C++64: precisa da libc++64 ja construida (make libcxx64).
if [ -f "$R/build/libcxx64/cxx_runtime64.o" ]; then
  printf '#include <string>\nint main(){std::string s="64";return s.size()==2?0:1;}\n' > "$T/h.cpp"
  "$D/bin/x86_64-sodos-g++64" "$T/h.cpp" -o "$T/hpp64"
  [ "$(dd if="$T/hpp64" bs=1 count=4 2>/dev/null)" = "SOX2" ] || { echo "SOX2 C++ invalido" >&2; exit 1; }
fi

echo "toolchain SODOS ok (x32 + SOX2)"
