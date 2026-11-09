#!/bin/sh
# fetch-uapi.sh - headers UAPI do kernel Linux (linux-libc-dev) em
# sodos/sysroot/include-uapi. O QEMU (host_os linux) inclui linux/types.h,
# linux/fs.h, linux/vfio.h... So constantes e tipos; nada disso chama o kernel.
# Entram com -idirafter: os headers do SODOS tem prioridade.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
U=$SYSROOT/include-uapi
[ -f "$U/scsi/sg.h" ] && { echo "ok: $U"; exit 0; }
rm -rf "$U"
T=$(mktemp -d /tmp/sodos-uapi-XXXXXX)
trap 'rm -rf "$T"' EXIT
(cd "$T" && apt-get download linux-libc-dev libc6-dev >/dev/null)
mkdir -p "$T/x" "$U"
for d in "$T"/*.deb; do dpkg -x "$d" "$T/x"; done
I=$T/x/usr/include
# netfilter*: nomes que so diferem na caixa (xt_MARK.h/xt_mark.h) nao cabem
# no NTFS do /mnt/c, e o QEMU nao os usa.
(cd "$I" && tar cf - --exclude='linux/netfilter*' linux asm-generic) | (cd "$U" && tar xf -)
cp -r "$I/x86_64-linux-gnu/asm" "$U/asm"
# scsi/sg.h da glibc: so structs e constantes do ioctl SG_IO.
mkdir -p "$U/scsi"
cp "$I/scsi/sg.h" "$U/scsi/"
echo "ok: $U"
