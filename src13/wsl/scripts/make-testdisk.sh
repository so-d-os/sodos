#!/bin/sh
# make-testdisk.sh - Monta wsl/build/windisk.img: um "SSD do Windows" de teste
# com GPT, uma particao NTFS e, dentro dela, o layout do WSL2:
#   Program Files\WSL\tools\kernel                  (o kernel do WSL deste PC)
#   Users\Teste\AppData\Local\wsl\{GUID}\ext4.vhdx  (Ubuntu minimo: busybox)
# Roda no WSL, sem root: ntfs-3g/busybox vem de ~/wsltools (apt-get download).
#   sh wsl/scripts/make-testdisk.sh
# Tambem compila os testes do host (wsl/build/host_wsl, ntfsput).
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
B=$S/build
T=$HOME/wsltools
mkdir -p "$B"
export LD_LIBRARY_PATH=$T/root/lib/x86_64-linux-gnu:$T/root/usr/lib/x86_64-linux-gnu

if [ ! -x "$T/root/sbin/mkntfs" ] || [ ! -f "$T/root/usr/include/ntfs-3g/volume.h" ] || [ ! -x "$T/root/usr/bin/busybox" ]; then
  mkdir -p "$T/deb" && cd "$T/deb"
  apt-get download ntfs-3g libntfs-3g89t64 ntfs-3g-dev busybox-static
  for f in *.deb; do dpkg-deb -x "$f" "$T/root"; done
  cd - >/dev/null
fi

# testes do host
gcc -O1 -g -Wall -o "$B/host_wsl" "$S/tests/host_wsl.c" "$S/wslcore.c" "$S/wslcli.c" \
  "$S/ntfs.c" "$S/vhdx.c" "$S/ext4.c"
gcc -O1 -g -o "$B/ntfsput" -I"$T/root/usr/include" "$S/tests/ntfsput.c" \
  "$T/root/lib/x86_64-linux-gnu/libntfs-3g.so.89"

# rootfs minimo no estilo Ubuntu (/bin -> usr/bin, links relativos)
R=$B/rootfs
rm -rf "$R"
mkdir -p "$R/usr/bin" "$R/usr/sbin" "$R/etc" "$R/proc" "$R/sys" "$R/dev" "$R/tmp" "$R/root" "$R/home/teste"
ln -s usr/bin "$R/bin"
ln -s usr/sbin "$R/sbin"
cp "$T/root/usr/bin/busybox" "$R/usr/bin/busybox"
for a in sh ls cat echo mount uname dmesg ps free df mkdir rm cp mv vi grep head tail sleep poweroff reboot halt top; do
  ln -s busybox "$R/usr/bin/$a"
done
cat > "$R/etc/os-release" <<'EOF'
PRETTY_NAME="Ubuntu 24.04 LTS (teste SODOS)"
NAME="Ubuntu"
VERSION_ID="24.04"
ID=ubuntu
EOF
echo sodos-wsl > "$R/etc/hostname"
cat > "$R/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
echo
echo "== Ubuntu (WSL) no SODOS =="
cat /etc/os-release | head -1
uname -a
echo "WSL-SODOS-BOOT-OK"
exec sh
EOF
chmod +x "$R/init"
echo "ola do ext4" > "$R/home/teste/ola.txt"
# arquivo maior que um bloco e com extents
head -c 300000 /dev/urandom > "$R/home/teste/grande.bin"

rm -f "$B/ext4.img"
mkfs.ext4 -q -F -L wslroot -d "$R" "$B/ext4.img" 128M
rm -f "$B/ext4.vhdx"
qemu-img convert -q -f raw -O vhdx -o subformat=dynamic "$B/ext4.img" "$B/ext4.vhdx"

# particao NTFS
KERNEL=${WSL_KERNEL:-"/mnt/c/Program Files/WSL/tools/kernel"}
rm -f "$B/ntfs.img"
truncate -s 256M "$B/ntfs.img"
"$T/root/sbin/mkntfs" -q -F -Q -L Windows -p 2048 -H 255 -S 63 "$B/ntfs.img"
"$B/ntfsput" "$B/ntfs.img" 'Program Files\WSL\tools\kernel' "$KERNEL"
"$B/ntfsput" "$B/ntfs.img" 'Users\Teste\AppData\Local\wsl\{11111111-2222-3333-4444-555555555555}\ext4.vhdx' "$B/ext4.vhdx"
"$B/ntfsput" "$B/ntfs.img" 'Users\Teste\Documents\leia.txt' "$R/etc/os-release"

# disco GPT: particao basic data em 1 MiB
rm -f "$B/windisk.img"
truncate -s 260M "$B/windisk.img"
printf 'label: gpt\nstart=2048, size=524288, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n' |
  sfdisk -q "$B/windisk.img" >/dev/null
dd if="$B/ntfs.img" of="$B/windisk.img" bs=1M seek=1 conv=notrunc status=none
echo "make-testdisk: $B/windisk.img"
