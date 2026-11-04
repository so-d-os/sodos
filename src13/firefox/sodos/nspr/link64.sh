#!/bin/sh
# link64.sh - liga o PRTEST64 (NSPR 4.39 LP64 + libsodos64 + LIBCX64).
#
#   NSPR_SRC=$HOME/ffsrc/firefox-153.6.0esr/nsprpub sh sodos/nspr/link64.sh
#
# O kernel/loader carrega SOX2; o soxrun nao roda 64-bit, entao o teste
# roda no QEMU (copie PRTEST64.EXE para o D:).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
FF=${FIREFOX_DIR:-$S/vendor/firefox-153.6.0esr}
N=${NSPR_SRC:-$S/vendor/nspr-4.35/nspr}
O=$S/build/nspr64

[ -d "$O/obj" ] || {
  echo "NSPR64 nao compilado; rode NSPR_BITS=64 sh sodos/nspr/build.sh" >&2
  exit 1
}

# libc64 estatica + crt0
make -C "$C" hello64c >/dev/null

for f in "$S"/sodos/port/*.c; do
  "$S/sodos/sodos-cc64" -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare \
    -I "$S/sodos/include" -I "$C/include" \
    -c "$f" -o "$O/port_$(basename "${f%.c}").o"
done
"$S/sodos/sodos-cc64" -O2 -g -I "$S/sodos/include" -I "$C/include" \
  -c "$S/../libs/posix-lite/posix.c" -o "$O/posix64.o"

"$S/sodos/sodos-cc64" -O2 -g -DHAVE_LONG_LONG -I "$S/sodos/include" -I "$C/include" \
  -I "$S/sodos/nspr/include" -I "$N/pr/include" -I "$N/pr/include/private" \
  -c "$S/sodos/nspr/tests/t_nspr.c" -o "$O/t_nspr64.o"

"$C/build/link64" /out:"$O/PRTEST64.EXE" "$C/build/x_crt0_64libc.o" \
  "$O/t_nspr64.o" $(find "$O/obj" -name '*.o') $(ls "$O"/port_*.o) \
  "$O/posix64.o" $(ls "$C"/build/x64_*.o) "$C/out/KERNEL.IMP"
echo "PRTEST64.EXE pronto: $O/PRTEST64.EXE"
